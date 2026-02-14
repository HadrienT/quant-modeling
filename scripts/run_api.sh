#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if [[ ! -d .venv ]]; then
  python3 -m venv .venv
fi

source .venv/bin/activate

python -m pip install --upgrade pip
python -m pip install -r api/requirements.txt

# Build and install the pybind wheel if not present
if ! ls dist/quantmodeling-*.whl >/dev/null 2>&1; then
  scripts/build_wheel.sh
fi

python -m pip install --force-reinstall dist/quantmodeling-*.whl

exec uvicorn api.app.main:app --reload
