#!/usr/bin/env python3
"""Check real axalloc OOM and axlibc's C allocation boundary in isolated processes."""
import argparse
import json
from pathlib import Path
import resource
import signal
import subprocess

ROOT = Path(__file__).resolve().parent
MODES = ["fallible", "small", "trait-oom", "malloc-oom", "size-max",
         "layout-limit", "max-valid", "rust-vec-oom"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release", action="store_true")
    parser.add_argument("--allocator", choices=["tlsf", "buddy", "slab"], default="tlsf")
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    profile = "release" if args.release else "debug"
    target_dir = ROOT / "target" / args.allocator
    command = ["cargo", "build", "--locked", "--no-default-features",
               "--features", args.allocator, "--target-dir", str(target_dir)]
    if args.release:
        command.append("--release")
    subprocess.run(command, cwd=ROOT, check=True)
    rows = []
    for mode in MODES:
        proc = subprocess.run([str(target_dir / profile / "arceos-oom-probe"), mode],
                              capture_output=True, text=True, timeout=15)
        if mode == "rust-vec-oom":
            matched = (proc.returncode == -signal.SIGABRT and
                       "memory allocation of 1073741832 bytes failed" in proc.stderr)
        else:
            matched = proc.returncode == 0 and "PASS:" in proc.stdout
        rows.append({"mode": mode, "returncode": proc.returncode,
                     "passed": matched, "stdout": proc.stdout, "stderr": proc.stderr})
        print(f"{'PASS' if matched else 'FAIL'} {mode}: exit {proc.returncode}")
        if not matched:
            print(proc.stdout + proc.stderr)
    result = {"rustc": subprocess.check_output(["rustc", "--version"], text=True).strip(),
              "allocator": args.allocator, "profile": profile, "results": rows}
    (target_dir / f"oom-{profile}.json").write_text(json.dumps(result, indent=2) + "\n")
    if not all(row["passed"] for row in rows):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
