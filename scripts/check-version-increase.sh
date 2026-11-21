#!/usr/bin/env bash
# Version-increase check for pull requests (SPEC.md §22): the version in
# CMakeLists.txt (project(zproject VERSION x.y.z)) must be greater than the
# base branch's. A base without a CMakeLists.txt (the initial commit) counts
# as 0.0.0. Dependabot PRs are exempt; ci.yml does not run this for them.
#   scripts/check-version-increase.sh <base-ref>     e.g. origin/main
set -euo pipefail
cd "$(dirname "$0")/.."

base="${1:?usage: check-version-increase.sh <base-ref>}"

extract() { sed -nE 's/^project\(zproject VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' | head -n1; }

head_ver=$(extract < CMakeLists.txt)
[ -n "$head_ver" ] || { echo "check-version-increase: no project(zproject VERSION x.y.z) in CMakeLists.txt" >&2; exit 1; }
if git cat-file -e "${base}:CMakeLists.txt" 2>/dev/null; then
  base_ver=$(git show "${base}:CMakeLists.txt" | extract)
  [ -n "$base_ver" ] || { echo "check-version-increase: no version in ${base}:CMakeLists.txt" >&2; exit 1; }
else
  base_ver="0.0.0"
fi

if [ "$head_ver" != "$base_ver" ] \
   && [ "$(printf '%s\n%s\n' "$base_ver" "$head_ver" | sort -V | tail -n1)" = "$head_ver" ]; then
  echo "check-version-increase: OK ($base_ver on $base -> $head_ver)"
else
  echo "check-version-increase: version $head_ver is not greater than $base_ver on $base." >&2
  echo "Every pull request bumps project(zproject VERSION ...) in CMakeLists.txt: patch for fixes and documentation, minor for features (SPEC.md §22)." >&2
  exit 1
fi
