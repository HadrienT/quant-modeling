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

# Editable, incrementally-rebuilding install of the C++ bindings. Idempotent:
# after the first run this only recompiles the .cpp files that changed.
scripts/dev_install.sh

exec uvicorn api.app.main:app --reload
