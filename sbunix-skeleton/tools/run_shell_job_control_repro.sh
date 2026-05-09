#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
artifact_dir=${SBUNIX_FG_REPRO_ARTIFACT_DIR:-/home/adebayo/test-findings/test-findings-38-artifacts/shell-fg/$(date +%Y%m%d-%H%M%S)}
timeout_s=${TIMEOUT:-300}
make_jobs=${MAKE_JOBS:-1}
qemu_mem=${QEMU_MEM:-128M}

mkdir -p "$artifact_dir"

cd "$repo_root"
# shellcheck disable=SC1091
source tools/sourceme.sh

{
  echo "artifact_dir=$artifact_dir"
  echo "timeout=$timeout_s"
  echo "qemu_mem=$qemu_mem"
  echo "make_jobs=$make_jobs"
  SKIP_BOOT_TESTS=1 MAKEFLAGS= make -j"$make_jobs" build/kernel.elf build/disk.img
} >"$artifact_dir/build.log" 2>&1

python3 - "$artifact_dir" "$timeout_s" "$qemu_mem" <<'PY'
import json
import os
import pty
import re
import select
import signal
import sys
import time
import tty

artifact_dir = sys.argv[1]
timeout_s = int(sys.argv[2])
qemu_mem = sys.argv[3]

raw_path = os.path.join(artifact_dir, "qemu.raw.log")
norm_path = os.path.join(artifact_dir, "qemu.normalized.log")
events_path = os.path.join(artifact_dir, "events.jsonl")
summary_path = os.path.join(artifact_dir, "summary.txt")
failure_path = os.path.join(artifact_dir, "failure-segment.log")

raw = bytearray()
norm = ""
qemu_pid = None
master_fd = None

FAIL_TOKENS = [
    "panic",
    "Page fault",
    "pte-update violation",
    "kernel PTE repair",
    "WARNING",
]


def event(kind, **fields):
    fields["kind"] = kind
    fields["time"] = time.time()
    with open(events_path, "a", encoding="utf-8") as f:
        f.write(json.dumps(fields, sort_keys=True) + "\n")


def write_logs():
    with open(raw_path, "wb") as f:
        f.write(raw)
    with open(norm_path, "w", encoding="utf-8", errors="replace") as f:
        f.write(norm)


def fail(message, start=0):
    write_logs()
    segment = norm[start:]
    with open(failure_path, "w", encoding="utf-8", errors="replace") as f:
        f.write(segment)
    with open(summary_path, "w", encoding="utf-8") as f:
        f.write("FAIL: " + message + "\n")
    event("fail", message=message)
    cleanup()
    raise SystemExit(1)


def cleanup():
    global qemu_pid, master_fd
    if master_fd is not None:
        try:
            os.close(master_fd)
        except OSError:
            pass
        master_fd = None
    if qemu_pid:
        try:
            os.kill(qemu_pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            os.waitpid(qemu_pid, 0)
        except ChildProcessError:
            pass
        qemu_pid = None


def append_output(data):
    global norm
    raw.extend(data)
    norm = raw.decode("utf-8", errors="replace").replace("\r", "")


def pump_once(wait=0.1):
    if master_fd is None:
        return False
    r, _, _ = select.select([master_fd], [], [], wait)
    if not r:
        return False
    try:
        data = os.read(master_fd, 4096)
    except OSError:
        return False
    if not data:
        return False
    append_output(data)
    return True


def check_fail_tokens(start=0):
    segment = norm[start:]
    for token in FAIL_TOKENS:
        if token in segment:
            fail("failure token in QEMU output: " + token, start)


def wait_for(pattern, timeout, start=0, label="pattern"):
    deadline = time.time() + timeout
    compiled = re.compile(pattern, re.S)
    while time.time() < deadline:
        pump_once(0.1)
        check_fail_tokens(start)
        segment = norm[start:]
        match = compiled.search(segment)
        if match:
            return match
        if qemu_pid:
            done, _ = os.waitpid(qemu_pid, os.WNOHANG)
            if done:
                fail("QEMU exited while waiting for " + label, start)
    fail("timeout waiting for " + label, start)


def send(data, label):
    os.write(master_fd, data)
    event("send", label=label, bytes=list(data))


def send_line(line):
    send(line.encode("utf-8") + b"\n", line)


def send_ctrl_z():
    send(b"\x1a", "ctrl-z")


def send_ctrl_d():
    send(b"\x04", "ctrl-d")


def wait_prompt(start, timeout=10):
    return wait_for(r"sbush\$ ", timeout, start, "sbush prompt")


def run_marked(name, command, timeout=10):
    start = len(norm)
    marker = "__SBUSH_REPRO_%s__" % name
    send_line("echo %s_BEGIN" % marker)
    send_line(command)
    send_line("echo %s_END:$?" % marker)
    wait_for(re.escape(marker) + r"_END:[0-9]+[\s\S]*?sbush\$ ", timeout, start, name)
    segment = norm[start:]
    if re.search(re.escape(marker) + r"_END:0", segment) is None:
        fail("command failed: " + command, start)
    return start, segment


def assert_jobs_only_stopped_cat(segment, start):
    job_lines = [m.group(0) for m in re.finditer(r"\[[0-9]+\][^\n]*", segment)]
    if any("sleep" in line for line in job_lines):
        fail("stale sleep job remained visible", start)
    stopped_cat = [line for line in job_lines if "Stopped" in line and re.search(r"\bcat\b", line)]
    if len(stopped_cat) != 1:
        fail("jobs did not show exactly one stopped cat", start)


def assert_jobs_empty(segment, start):
    job_lines = [m.group(0) for m in re.finditer(r"\[[0-9]+\][^\n]*", segment)]
    if any(("sleep" in line or "cat" in line) for line in job_lines):
        fail("job table kept a stale sleep/cat entry", start)


def suspend_cat(label):
    start = len(norm)
    send_line("cat")
    time.sleep(0.3)
    pump_once(0.1)
    send_ctrl_z()
    wait_for(r"\[[0-9]+\] Stopped\s+cat[\s\S]*?sbush\$ ", 5, start, label)
    segment = norm[start:]
    if "Stopped" not in segment or "cat" not in segment:
        fail("Ctrl-Z did not stop cat in " + label, start)
    return start, segment


def foreground_cat(label):
    start = len(norm)
    send_line("fg")
    deadline = time.time() + 0.7
    while time.time() < deadline:
        pump_once(0.1)
        check_fail_tokens(start)
        if "fg: no such job" in norm[start:]:
            fail("fg reported no job in " + label, start)
        if re.search(r"\bsleep\b", norm[start:]):
            fail("fg selected stale sleep in " + label, start)
        if "sbush$ " in norm[start:]:
            fail("fg returned to prompt before Ctrl-D in " + label, start)
    send_ctrl_d()
    wait_prompt(start, 5)
    segment = norm[start:]
    if "fg: no such job" in segment:
        fail("fg reported no job in " + label, start)
    if re.search(r"\bsleep\b", segment):
        fail("fg selected stale sleep in " + label, start)
    return start, segment


def run_cycle(i):
    event("cycle", index=i)
    run_marked("sleep_%d" % i, "sleep 1 &", 10)
    time.sleep(1.4)
    run_marked("wait_%d" % i, "wait", 10)

    suspend_cat("cycle %d first cat" % i)
    jobs_start, jobs_segment = run_marked("jobs_a_%d" % i, "jobs", 10)
    assert_jobs_only_stopped_cat(jobs_segment, jobs_start)
    foreground_cat("cycle %d first fg" % i)
    empty_start, empty_segment = run_marked("jobs_after_a_%d" % i, "jobs", 10)
    assert_jobs_empty(empty_segment, empty_start)

    suspend_cat("cycle %d second cat" % i)
    run_marked("ps_%d" % i, "ps", 10)
    jobs_start, jobs_segment = run_marked("jobs_b_%d" % i, "jobs", 10)
    assert_jobs_only_stopped_cat(jobs_segment, jobs_start)
    foreground_cat("cycle %d second fg" % i)
    empty_start, empty_segment = run_marked("jobs_after_b_%d" % i, "jobs", 10)
    assert_jobs_empty(empty_segment, empty_start)


def main():
    global qemu_pid, master_fd
    args = [
        "qemu-system-riscv64",
        "-machine", "virt",
        "-m", qemu_mem,
        "-bios", "default",
        "-kernel", "build/kernel.elf",
        "-drive", "file=build/disk.img,format=raw,if=none,id=hd0",
        "-device", "virtio-blk-pci-non-transitional,drive=hd0",
        "-device", "virtio-gpu-pci",
        "-device", "virtio-net-pci-non-transitional,netdev=net0",
        "-netdev", "user,id=net0",
        "-nographic",
    ]
    qemu_pid, master_fd = pty.fork()
    if qemu_pid == 0:
        tty.setraw(0)
        os.execvp(args[0], args)

    event("spawn", pid=qemu_pid)
    wait_prompt(0, min(timeout_s, 180))
    for i in range(1, 6):
        run_cycle(i)
    start = len(norm)
    send_line("echo __SBUSH_FG_REPRO_DONE__")
    wait_for(r"__SBUSH_FG_REPRO_DONE__", 5, start, "end marker")
    send_line("poweroff")
    time.sleep(1)
    write_logs()
    with open(summary_path, "w", encoding="utf-8") as f:
        f.write("PASS: shell fg job-control repro completed\n")
    cleanup()


try:
    main()
except KeyboardInterrupt:
    fail("interrupted")
PY
