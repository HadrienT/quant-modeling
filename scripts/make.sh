set -euo pipefail

# ctest runs each TEST()/TEST_F() as its own process (gtest_discover_tests),
# so -j parallelizes across independent processes, not threads sharing
# state -- safe by construction. 16 measured at ~1.7 GB above whatever else
# is already running on a 47 GB box with a good margin left; raise it if
# you know the box is otherwise idle, or override: `NPROC=8 scripts/make.sh`.
NPROC="${NPROC:-16}"

# rm -rf build
cmake --preset default
cmake --build build
GTEST_COLOR=1 ctest --test-dir build --output-on-failure -j "$NPROC"