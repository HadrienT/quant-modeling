#!/usr/bin/env python3
"""Follow a test run from another terminal, without touching it.

    scripts/test_watch.py                # the latest run (scripts/test_run.sh)
    scripts/test_watch.py path/to.log    # any test_affected.py log
    scripts/test_watch.py --once         # print the summary once and exit

Every few seconds: the phases test_affected.py announced, the one running
with its progress (ctest's n/m, pytest's percentage), the time since the
run started, and every failure as soon as it is printed. It only reads the
log; it stops by itself when the run's "exit <status>" line appears.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CTEST = re.compile(r"^\s*(\d+)/(\d+) Test\s+#\d+: (\S+) \.+\s*(\*\*\*)?(\w+)")
PYTEST_PCT = re.compile(r"\[\s*(\d+)%\]\s*$")
PHASE = re.compile(r"^== ([^:]+)$")
PLANNED = re.compile(r"^== ([^:]+): ")
FAIL = re.compile(
    r"(\*\*\*Failed|\*\*\*Exception|\*\*\*Timeout|^FAILED |^ERROR |Failure$|^\s*✗|"
    r"\d+ failed|tests failed out of|error:)"
)


def default_log() -> Path:
    for build in ("build-cuda", "build"):
        p = ROOT / build / "test-runs" / "latest.log"
        if p.exists():
            return p
    sys.exit("no run yet: start one with scripts/test_run.sh")


def summary(log: Path) -> tuple[str, bool]:
    lines = log.read_text(errors="replace").splitlines()
    planned, phases, failures = [], [], []
    progress: dict[str, str] = {}
    done = None
    header = lines[0] if lines and lines[0].startswith("#") else ""
    m = re.search(r"started (\S+)", header)
    started = datetime.fromisoformat(m.group(1)).timestamp() if m else None
    for line in lines:
        if m := PLANNED.match(line):
            planned.append(m.group(1))
        elif m := PHASE.match(line):
            phases.append(m.group(1))
        elif phases and (m := CTEST.match(line)):
            progress[phases[-1]] = f"{m.group(1)}/{m.group(2)}"
        elif phases and (m := PYTEST_PCT.search(line)):
            progress[phases[-1]] = f"{m.group(1)} %"
        elif line.startswith("exit "):
            done = line.split()[1]
        if FAIL.search(line) and "0 failed" not in line:
            failures.append(line.strip())
        if re.search(r"tests passed out of|passed|Tests\s+\d+ passed", line) and phases:
            if re.search(
                r"(\d+% tests passed out of \d+|\d+ passed|Tests\s+\d+ passed)", line
            ):
                progress[phases[-1]] = line.strip()
    clock = "elapsed ?"
    if started is not None:
        end = log.stat().st_mtime if done else time.time()
        elapsed = int(end - started)
        clock = f"elapsed {elapsed // 60} min {elapsed % 60:02d} s"
    out = [header, clock + (f" -- finished, exit {done}" if done else "")]
    for name in planned or phases:
        if name in phases[:-1] or (done and name in phases):
            mark = "done"
        elif phases and name == phases[-1]:
            mark = "running"
        else:
            mark = "waiting"
        out.append(f"  {mark:8s} {name:22s} {progress.get(name, '')}")
    if failures:
        out.append(f"failures ({len(failures)}):")
        out += ["  " + f for f in failures[-15:]]
    return "\n".join(out), done is not None


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("log", nargs="?", type=Path)
    ap.add_argument("--once", action="store_true")
    ap.add_argument(
        "--every", type=float, default=5.0, help="seconds between refreshes"
    )
    args = ap.parse_args()
    log = args.log or default_log()
    while True:
        text, finished = summary(log.resolve())
        if not args.once and sys.stdout.isatty():
            os.system("clear")
        print(text, flush=True)
        if args.once or finished:
            return 0
        time.sleep(args.every)


if __name__ == "__main__":
    sys.exit(main())
