set -euo pipefail

# Same idea as make.sh, at a lower default: ASan/UBSan roughly triples a
# process's memory footprint, and each of these tests runs as its own
# process, so parallelism multiplies that overhead directly. 8 measured at
# ~8.4 GB above whatever else is already running on a 47 GB box -- a real
# chunk of headroom, unlike make.sh's default preset, so this stays more
# conservative unless you know the box is otherwise idle:
# `NPROC=16 scripts/make_asan.sh`.
NPROC="${NPROC:-8}"

cmake --preset asan -B build-asan
cmake --build build-asan
GTEST_COLOR=1 ctest --test-dir build-asan --output-on-failure -j "$NPROC"
