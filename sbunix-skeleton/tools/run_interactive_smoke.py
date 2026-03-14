#!/usr/bin/env python3
import argparse
import os
import pty
import re
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path


PROMPT = "sbush$ "


def run(cmd, cwd, env=None):
    result = subprocess.run(cmd, cwd=cwd, env=env, text=True)
    if result.returncode != 0:
        raise SystemExit(result.returncode)


class ConsoleSession:
    def __init__(self, master_fd, live):
        self.master_fd = master_fd
        self.live = live
        self._buf = bytearray()
        self._lock = threading.Lock()
        self._stop = False
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    def _reader(self):
        while not self._stop:
            try:
                data = os.read(self.master_fd, 4096)
            except OSError:
                break
            if not data:
                break
            with self._lock:
                self._buf.extend(data)
            if self.live:
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()

    def close(self):
        self._stop = True
        self._thread.join(timeout=1)

    def write(self, data):
        os.write(self.master_fd, data)

    def text(self):
        with self._lock:
            return self._buf.decode("latin1", errors="replace")

    def position(self):
        with self._lock:
            return len(self._buf)

    def slice_from(self, start):
        with self._lock:
            return self._buf[start:].decode("latin1", errors="replace")

    def wait_for(self, pattern, timeout, start=0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            text = self.slice_from(start)
            if pattern in text:
                return text
            time.sleep(0.05)
        raise TimeoutError(f"Timed out waiting for {pattern!r}")

    def wait_for_regex(self, pattern, timeout, start=0):
        rx = re.compile(pattern, re.MULTILINE)
        deadline = time.time() + timeout
        while time.time() < deadline:
            text = self.slice_from(start)
            match = rx.search(text)
            if match:
                return match, text
            time.sleep(0.05)
        raise TimeoutError(f"Timed out waiting for regex {pattern!r}")


def pass_line(name):
    print(f"[PASS] interactive: {name}")


def fail_line(name, reason):
    print(f"[FAIL] interactive: {name} ({reason})")


def scenario_ctrl_u(sess):
    name = "ctrl-u line kill"
    start = sess.position()
    sess.write(b"junk")
    time.sleep(0.2)
    sess.write(b"\x15")
    sess.write(b"echo CTRL_U_OK\n")
    sess.wait_for(PROMPT, 5, start)
    text = sess.slice_from(start)
    if "CTRL_U_OK" in text and "command not found" not in text:
        pass_line(name)
        return True
    fail_line(name, "line kill did not preserve clean command entry")
    return False


def scenario_ctrl_c(sess):
    name = "ctrl-c interrupt"
    start = sess.position()
    sess.write(b"sleep 5\n")
    time.sleep(0.5)
    sess.write(b"\x03")
    sess.wait_for(PROMPT, 5, start)
    sess.write(b"echo CTRL_C_OK\n")
    text = sess.wait_for("CTRL_C_OK", 5, start)
    if "CTRL_C_OK" in text:
        pass_line(name)
        return True
    fail_line(name, "shell did not recover after SIGINT")
    return False


def _stop_job(sess, signal_byte):
    start = sess.position()
    sess.write(b"sleep 5\n")
    time.sleep(0.5)
    sess.write(signal_byte)
    sess.wait_for(PROMPT, 5, start)
    match, _ = sess.wait_for_regex(r"\[(\d+)\]\s+Stopped\t([^\r\n]+)", 5, start)
    return match.group(1), match.group(2)


def scenario_ctrl_z(sess):
    name = "ctrl-z stop"
    try:
        _, cmd = _stop_job(sess, b"\x1a")
    except Exception as exc:
        fail_line(name, str(exc))
        return False
    if "sleep 5" in cmd:
        pass_line(name)
        return True
    fail_line(name, f"unexpected stopped command: {cmd}")
    return False


def scenario_jobs(sess):
    name = "jobs builtin"
    try:
        _, _ = _stop_job(sess, b"\x1a")
    except Exception as exc:
        fail_line(name, str(exc))
        return False
    start = sess.position()
    sess.write(b"jobs\n")
    sess.wait_for(PROMPT, 5, start)
    text = sess.slice_from(start)
    if "Stopped" in text and "sleep 5" in text:
        pass_line(name)
        return True
    fail_line(name, "jobs output did not show stopped sleep command")
    return False


def scenario_fg(sess):
    name = "fg resume stopped job"
    try:
        jobid, _ = _stop_job(sess, b"\x1a")
    except Exception as exc:
        fail_line(name, str(exc))
        return False
    start = sess.position()
    sess.write(f"fg {jobid}\n".encode())
    time.sleep(0.5)
    sess.write(b"\x03")
    sess.wait_for(PROMPT, 5, start)
    sess.write(b"echo FG_OK\n")
    text = sess.wait_for("FG_OK", 5, start)
    if "FG_OK" in text:
        pass_line(name)
        return True
    fail_line(name, "foreground resume did not return shell control")
    return False


def scenario_ctrl_backslash(sess):
    name = "ctrl-\\\\ sigquit"
    start = sess.position()
    sess.write(b"sleep 5\n")
    time.sleep(0.5)
    sess.write(b"\x1c")
    sess.wait_for(PROMPT, 5, start)
    sess.write(b"echo CTRL_BACKSLASH_OK\n")
    text = sess.wait_for("CTRL_BACKSLASH_OK", 5, start)
    if "CTRL_BACKSLASH_OK" in text:
        pass_line(name)
        return True
    fail_line(name, "shell did not recover after SIGQUIT")
    return False


def scenario_ctrl_d(sess):
    name = "ctrl-d eof at prompt"
    start = sess.position()
    sess.write(b"\x04")
    sess.wait_for("init: shell exited, respawning...", 5, start)
    sess.wait_for(PROMPT, 10, start)
    sess.write(b"echo CTRL_D_OK\n")
    text = sess.wait_for("CTRL_D_OK", 5, start)
    if "CTRL_D_OK" in text:
        pass_line(name)
        return True
    fail_line(name, "shell did not respawn cleanly after EOF")
    return False


SCENARIOS = {
    "ctrl-u": scenario_ctrl_u,
    "ctrl-c": scenario_ctrl_c,
    "ctrl-z": scenario_ctrl_z,
    "jobs": scenario_jobs,
    "fg": scenario_fg,
    "ctrl-backslash": scenario_ctrl_backslash,
    "ctrl-d": scenario_ctrl_d,
}


def selected_scenarios(mode):
    if mode == "all":
        return list(SCENARIOS.items())
    if mode not in SCENARIOS:
        raise SystemExit(
            "usage: run_interactive_smoke.py [all|ctrl-u|ctrl-c|ctrl-z|jobs|fg|ctrl-backslash|ctrl-d]"
        )
    return [(mode, SCENARIOS[mode])]


def main():
    parser = argparse.ArgumentParser(description="Best-effort interactive smoke runner for sbush.")
    parser.add_argument("mode", nargs="?", default="all")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--live", action="store_true")
    args = parser.parse_args()

    project_dir = Path(__file__).resolve().parent.parent

    with tempfile.NamedTemporaryFile("w", delete=False, dir=project_dir) as rc_file:
        rc_file.write("#!/bin/sh\n")
        rc_file.write("echo [INTERACTIVE] smoke start\n")
        rc_path = rc_file.name

    env = os.environ.copy()
    env["RC_SCRIPT"] = rc_path
    env["BUILD_TESTS"] = "1"

    try:
        run(["bash", "-lc", "source tools/sourceme.sh && make build/kernel.elf"], cwd=project_dir, env=env)
        run(["bash", "-lc", "source tools/sourceme.sh && make build/disk.img"], cwd=project_dir, env=env)

        master_fd, slave_fd = pty.openpty()
        qemu_cmd = [
            "qemu-system-riscv64",
            "-machine", "virt",
            "-bios", "default",
            "-kernel", "build/kernel.elf",
            "-drive", "file=build/disk.img,format=raw,if=none,id=hd0",
            "-device", "virtio-blk-pci-non-transitional,drive=hd0",
            "-device", "virtio-gpu-pci",
            "-device", "virtio-net-pci-non-transitional,netdev=net0",
            "-netdev", "user,id=net0",
            "-nographic",
        ]
        proc = subprocess.Popen(
            qemu_cmd,
            cwd=project_dir,
            stdin=slave_fd,
            stdout=slave_fd,
            stderr=slave_fd,
            close_fds=True,
        )
        os.close(slave_fd)

        sess = ConsoleSession(master_fd, args.live)
        try:
            sess.wait_for(PROMPT, min(30, args.timeout))
        except TimeoutError as exc:
            proc.kill()
            proc.wait()
            raise SystemExit(f"interactive smoke failed to reach prompt: {exc}")

        passed = 0
        failed = 0
        for _, scenario in selected_scenarios(args.mode):
            try:
                if scenario(sess):
                    passed += 1
                else:
                    failed += 1
            except Exception as exc:
                fail_line(scenario.__name__, str(exc))
                failed += 1
                try:
                    sess.wait_for(PROMPT, 5)
                except Exception:
                    break

        print(f"interactive-smoke: {passed} passed, {failed} failed")
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        sess.close()
        os.close(master_fd)
        return 0
    finally:
        try:
            os.unlink(rc_path)
        except FileNotFoundError:
            pass


if __name__ == "__main__":
    raise SystemExit(main())
