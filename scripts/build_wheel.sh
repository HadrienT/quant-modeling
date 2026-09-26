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

# GPU backend (blueprint/wp/19-gpu.md): QM_ENABLE_CUDA=1 scripts/build_wheel.sh.
# nvcc's host compiler defaults to g++-13 -- CUDA 12.4 rejects gcc 14, the
# server's default; override with QM_CUDA_HOST_COMPILER.
if [ "${QM_ENABLE_CUDA:-0}" = "1" ]; then
  export CMAKE_ARGS="${CMAKE_ARGS:-} -DQM_ENABLE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=70 -DCMAKE_CUDA_HOST_COMPILER=${QM_CUDA_HOST_COMPILER:-g++-13}"
fi

python -m build --wheel

echo "Wheel built in dist/"
