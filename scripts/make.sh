set -euo pipefail


# rm -rf build
cmake --preset default
cmake --build build
GTEST_COLOR=1 ctest --test-dir build --output-on-failure