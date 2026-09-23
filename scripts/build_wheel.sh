#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

python -m pip install --upgrade pip
python -m pip install "scikit-build-core>=0.10.2" "pybind11>=3.0.1" build

python -m build --wheel

echo "Wheel built in dist/"
