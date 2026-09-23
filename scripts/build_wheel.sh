#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

python -m pip install --upgrade pip
python -m pip install "scikit-build-core>=0.10.2" "pybind11>=3.0.1" build

# Eigen3 (and the other vcpkg-managed deps quantModeling links against) are
# only findable through the vcpkg toolchain file — pyproject.toml's
# [tool.scikit-build.cmake.define] can't reference $VCPKG_ROOT (TOML has no
# env interpolation), so pass it via CMAKE_ARGS, which scikit-build-core
# forwards to the CMake configure step.
if [ -n "${VCPKG_ROOT:-}" ]; then
  export CMAKE_ARGS="${CMAKE_ARGS:-} -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
fi

python -m build --wheel

echo "Wheel built in dist/"
