#!/usr/bin/env python3
"""Write the product library the site shows, from api/app/product_library.

The .qms files are the single source (api/app/product_templates.py reads
them to price a term sheet); the front end reads this generated, committed
JSON for the pricing catalog and the scripting page's library. CI regenerates
it and fails on drift, like web/openapi.json.

Usage:
    python scripts/gen_product_library.py web/src/shared/products/scripted.gen.json
"""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main(out: str) -> None:
    path = ROOT / "api/app/product_templates.py"
    spec = importlib.util.spec_from_file_location("product_templates", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules["product_templates"] = module
    spec.loader.exec_module(module)
    products = module.catalog()
    Path(out).write_text(json.dumps(products, indent="\t", ensure_ascii=True) + "\n")
    print(f"wrote {out} — {len(products)} products")


if __name__ == "__main__":
    main(
        sys.argv[1]
        if len(sys.argv) > 1
        else "web/src/shared/products/scripted.gen.json"
    )
