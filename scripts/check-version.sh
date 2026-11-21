#!/usr/bin/env bash
# Version-drift check (SPEC.md §22): the CMake project() version, the top
# CHANGELOG heading, the README header and the SPEC.md version line agree.
#   scripts/check-version.sh               # every PR / push (ci.yml)
#   scripts/check-version.sh --tag vX.Y.Z  # tag builds: the tag must also be
#                                          # v$VERSION and the heading dated
set -euo pipefail
cd "$(dirname "$0")/.."

tag=""
if [ "${1:-}" = "--tag" ]; then
  tag="${2:?--tag needs a value}"
fi

fail=0
err() { echo "check-version: $*" >&2; fail=1; }

version=$(sed -nE 's/^project\(zproject VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' CMakeLists.txt | head -n1)
[ -n "$version" ] || { echo "check-version: no project(zproject VERSION x.y.z) in CMakeLists.txt" >&2; exit 1; }

heading=$(grep -m1 -E '^## [0-9]' CHANGELOG.md || true)
head_ver=$(sed -nE 's/^## ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' <<<"$heading")
[ "$head_ver" = "$version" ] || err "top CHANGELOG heading '$heading' is not $version (CMakeLists.txt)"

readme_ver=$(grep -m1 -oE '^\*\*[0-9]+\.[0-9]+\.[0-9]+\*\*' README.md | tr -d '*' || true)
[ "$readme_ver" = "$version" ] || err "README header says '${readme_ver:-none}', CMakeLists.txt says $version"

spec_ver=$(grep -m1 -oE '^This specification describes zproject \*\*[0-9]+\.[0-9]+\.[0-9]+\*\*' SPEC.md \
             | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' || true)
[ "$spec_ver" = "$version" ] || err "SPEC.md describes '${spec_ver:-none}', CMakeLists.txt says $version"

# "Current release" names the newest tagged release: this version or (while it
# is still in a PR) the newest other v* tag; "none yet" until the first tag.
# Needs the tags (actions/checkout with fetch-depth: 0).
current=$(grep -m1 -oE '^Current release: (\*\*[0-9]+\.[0-9]+\.[0-9]+\*\*|none yet)' README.md \
            | sed -E 's/^Current release: //; s/\*//g' || true)
latest_other=$(git tag -l 'v[0-9]*' 2>/dev/null | sed 's/^v//' | grep -vxF "$version" | sort -V | tail -n1 || true)
if [ -n "$latest_other" ]; then
  if [ "$current" != "$version" ] && [ "$current" != "$latest_other" ]; then
    err "README 'Current release' says '${current:-none}', expected $latest_other (newest tag) or $version"
  fi
elif [ -z "$current" ]; then
  err "README has no 'Current release: **x.y.z**' (or 'none yet') line"
fi

if [ -n "$tag" ]; then
  [ "$tag" = "v$version" ] || err "tag $tag does not match CMakeLists.txt version v$version"
  grep -qE "^## $version — [0-9]{4}-[0-9]{2}-[0-9]{2}\$" <<<"$heading" \
    || err "top CHANGELOG heading '$heading' must be dated (## $version — YYYY-MM-DD)"
fi

[ "$fail" -eq 0 ] || exit 1
echo "check-version: OK ($version${tag:+, tag $tag})"
