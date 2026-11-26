#!/usr/bin/env bash
#
# Run scripts/test_affected.py with its output in a log that
# scripts/test_watch.py can follow from another terminal:
#
#   scripts/test_run.sh [test_affected.py arguments, e.g. --pr]
#   scripts/test_watch.py            # in a second terminal
#
# Logs go to build-cuda/test-runs/ (build-cuda/ if it exists, else build/),
# one per run, timestamped; latest.log points at the newest. The last line
# is "exit <status>". The exit status is test_affected.py's.
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT_DIR/build-cuda"
[[ -d "$BUILD" ]] || BUILD="$ROOT_DIR/build"
RUNS="$BUILD/test-runs"
mkdir -p "$RUNS"
LOG="$RUNS/$(date +%Y%m%d-%H%M%S).log"
ln -sfn "$(basename "$LOG")" "$RUNS/latest.log"

{
    echo "# test_affected.py $* -- started $(date --iso-8601=seconds) on $(git -C "$ROOT_DIR" rev-parse --abbrev-ref HEAD)"
    "$ROOT_DIR/scripts/test_affected.py" "$@" 2>&1
    status=$?
    echo "exit $status"
    exit $status
} >"$LOG" 2>&1
status=$?
echo "$LOG: exit $status"
exit $status
