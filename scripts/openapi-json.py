#!/usr/bin/env python3
"""Regenerate api/openapi.json from api/openapi.yaml (the source of truth).

zprojectd embeds the JSON and serves it at GET /v1/openapi.json. CI runs
`scripts/openapi-json.py --check`, which fails when the committed JSON is out
of date. Needs PyYAML (Debian/Ubuntu: python3-yaml); build-time only.
"""
import json
import pathlib
import sys

import yaml

root = pathlib.Path(__file__).resolve().parent.parent
src = root / "api" / "openapi.yaml"
dst = root / "api" / "openapi.json"
doc = yaml.safe_load(src.read_text(encoding="utf-8"))
text = json.dumps(doc, indent=1, ensure_ascii=True, sort_keys=False) + "\n"
if "--check" in sys.argv[1:]:
    if not dst.exists() or dst.read_text(encoding="utf-8") != text:
        print("api/openapi.json is out of date: run scripts/openapi-json.py", file=sys.stderr)
        sys.exit(1)
    print("api/openapi.json is up to date")
else:
    dst.write_text(text, encoding="utf-8")
    print(f"wrote {dst.relative_to(root)}")
