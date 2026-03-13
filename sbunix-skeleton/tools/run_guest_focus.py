#!/usr/bin/env python3
import argparse
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def run(cmd, cwd, env=None):
    result = subprocess.run(cmd, cwd=cwd, env=env, text=True)
    if result.returncode != 0:
        raise SystemExit(result.returncode)


def main():
    parser = argparse.ArgumentParser(description="Build and boot SBUnix with a temporary rc script.")
    parser.add_argument("--label", required=True)
    parser.add_argument("--command", action="append", required=True,
                        help="Command line to place in the temporary /etc/rc")
    parser.add_argument("--require", action="append", default=[],
                        help="Substring that must appear in guest output")
    parser.add_argument("--forbid", action="append", default=[],
                        help="Substring that must not appear in guest output")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--disk", default="build/disk.img")
    parser.add_argument("--build-tests", action="store_true")
    parser.add_argument("--output")
    args = parser.parse_args()

    project_dir = Path(__file__).resolve().parent.parent
    build_tests = "1" if args.build_tests else ""

    with tempfile.NamedTemporaryFile("w", delete=False, dir=project_dir) as rc_file:
        rc_file.write("#!/bin/sh\n")
        rc_file.write(f"echo [FOCUS] {args.label} start\n")
        for command in args.command:
            rc_file.write(f"{command}\n")
        rc_file.write("poweroff\n")
        rc_path = rc_file.name

    output_path = Path(args.output) if args.output else Path(tempfile.mktemp(prefix="sbunix-focus-", suffix=".log"))
    env = os.environ.copy()
    env["RC_SCRIPT"] = rc_path
    if build_tests:
        env["BUILD_TESTS"] = build_tests

    try:
        run(["bash", "-lc", "source tools/sourceme.sh && make build/kernel.elf"], cwd=project_dir, env=env)
        run(["bash", "-lc", f"source tools/sourceme.sh && make {args.disk}"], cwd=project_dir, env=env)

        qemu_cmd = [
            "qemu-system-riscv64",
            "-machine", "virt",
            "-bios", "default",
            "-kernel", "build/kernel.elf",
            "-drive", f"file={args.disk},format=raw,if=none,id=hd0",
            "-device", "virtio-blk-pci-non-transitional,drive=hd0",
            "-device", "virtio-gpu-pci",
            "-device", "virtio-net-pci-non-transitional,netdev=net0",
            "-netdev", "user,id=net0",
            "-nographic",
        ]

        with output_path.open("w") as output_file:
            proc = subprocess.Popen(
                qemu_cmd,
                cwd=project_dir,
                stdout=output_file,
                stderr=subprocess.STDOUT,
                text=True,
            )
            deadline = time.time() + args.timeout
            while proc.poll() is None and time.time() < deadline:
                time.sleep(1)
            if proc.poll() is None:
                proc.kill()
                proc.wait()
                print(f"Timed out after {args.timeout}s. Output: {output_path}", file=sys.stderr)
                return 1

        text = output_path.read_text()
        missing = [pattern for pattern in args.require if pattern not in text]
        forbidden = [pattern for pattern in args.forbid if pattern in text]

        if missing or forbidden:
            if missing:
                print("Missing required output patterns:", file=sys.stderr)
                for pattern in missing:
                    print(f"  {pattern}", file=sys.stderr)
            if forbidden:
                print("Found forbidden output patterns:", file=sys.stderr)
                for pattern in forbidden:
                    print(f"  {pattern}", file=sys.stderr)
            print(f"Guest output log: {output_path}", file=sys.stderr)
            return 1

        print(f"Guest run succeeded. Output: {output_path}")
        return 0
    finally:
        try:
            os.unlink(rc_path)
        except FileNotFoundError:
            pass


if __name__ == "__main__":
    raise SystemExit(main())
