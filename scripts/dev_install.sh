#!/usr/bin/env bash
#
# Editable, incrementally-rebuilding install of the `quantmodeling` extension.
#
# Runs once to set things up. Afterwards `import quantmodeling` recompiles only
# the C++ translation units that changed (scikit-build-core's editable.rebuild),
# reusing the persistent CMake build directory build/pypkg. No wheel packaging,
# no --force-reinstall round-trip.
#
#   - Fast inner loop for API / notebook work: edit a .cpp, restart the process,
#     next import rebuilds that file only (~1-3 s).
#   - Use scripts/build_wheel.sh instead to produce a distributable wheel in
#     dist/ (isolated build, for releases).
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

# Reuse an already-active virtualenv; otherwise fall back to .venv.
if [[ -z "${VIRTUAL_ENV:-}" ]]; then
  if [[ ! -d .venv ]]; then
    python3 -m venv .venv 2>/dev/null || uv venv .venv
  fi
  # shellcheck disable=SC1091
  source .venv/bin/activate
fi

# Package installer: uv (fast, and what typically creates .venv here) if present,
# otherwise pip. A uv-created venv has no pip, so uv must win when available.
if command -v uv >/dev/null 2>&1; then
  PIP=(uv pip)
elif python -m pip --version >/dev/null 2>&1; then
  PIP=(python -m pip)
  python -m pip install --upgrade pip
else
  echo "✗ neither uv nor pip is available in this environment." >&2
  echo "  Install uv (https://docs.astral.sh/uv/) or add pip with:" >&2
  echo "    python -m ensurepip --upgrade   # needs the python3-venv system package" >&2
  exit 1
fi

# Build backend + tools installed into THIS environment so the install below can
# run with --no-build-isolation (no throwaway build venv on every rebuild).
"${PIP[@]}" install \
  "scikit-build-core>=0.10.2" \
  "pybind11>=3.0.1" \
  "cmake>=3.20" \
  ninja

# If vcpkg is set up, point the CMake configure at its toolchain so the wheel
# build finds the same Eigen / pybind11 as ./scripts/make.sh. Without vcpkg the
# build falls back to system packages (e.g. libeigen3-dev).
CONFIG_ARGS=()
TOOLCHAIN="${VCPKG_ROOT:-}/scripts/buildsystems/vcpkg.cmake"
if [[ -n "${VCPKG_ROOT:-}" && -f "$TOOLCHAIN" ]]; then
  echo "→ using vcpkg toolchain: $TOOLCHAIN"
  CONFIG_ARGS+=("--config-settings=cmake.define.CMAKE_TOOLCHAIN_FILE=$TOOLCHAIN")
fi

# Editable install. editable.rebuild (pyproject.toml) makes `import quantmodeling`
# re-run `cmake --build build/pypkg` when sources change.
"${PIP[@]}" install --no-build-isolation -ve . "${CONFIG_ARGS[@]}"

echo
echo "quantmodeling installed editable."
echo "  -> 'import quantmodeling' now recompiles only changed C++ files."
