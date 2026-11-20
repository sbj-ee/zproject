#!/usr/bin/env bash
# Render assets/icon/zproject.svg (icon concept B, SPEC.md §23) to
# assets/icon/zproject-<size>.png. Needs rsvg-convert (`sudo apt install
# librsvg2-bin` / `brew install librsvg`). The PNGs are committed, so builds
# don't need this; run it after changing the SVG.
set -euo pipefail
cd "$(dirname "$0")/.."
for s in 16 24 32 48 64 128 256 512 1024; do
  rsvg-convert -w "$s" -h "$s" assets/icon/zproject.svg -o "assets/icon/zproject-$s.png"
done
echo "rendered: $(ls assets/icon | tr '\n' ' ')"
