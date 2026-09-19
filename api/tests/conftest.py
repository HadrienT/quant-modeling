import sys
from pathlib import Path

# `app` lives in api/, which is not on the path when pytest runs from the repo root.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
