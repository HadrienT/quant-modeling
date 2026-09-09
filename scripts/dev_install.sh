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
    python3 -m venv .venv
  fi
  # shellcheck disable=SC1091
  source .venv/bin/activate
fi

python -m pip install --upgrade pip

# Build backend + tools installed into THIS environment so the install below can
# run with --no-build-isolation (no throwaway build venv on every rebuild).
python -m pip install \
  "scikit-build-core>=0.10.2" \
  "pybind11>=3.0.1" \
  "cmake>=3.20" \
  ninja

# Editable install. editable.rebuild (pyproject.toml) makes `import quantmodeling`
# re-run `cmake --build build/pypkg` when sources change.
python -m pip install --no-build-isolation -ve .

echo
echo "quantmodeling installed editable."
echo "  -> 'import quantmodeling' now recompiles only changed C++ files."
