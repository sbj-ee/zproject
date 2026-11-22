#!/bin/bash
# Usage: check-cli-tls.sh <path/to/zproject.app>
# Runs the bundled zproject-cli as it runs on a Mac without Homebrew Qt and
# checks that it has a TLS backend (doctor: "tls: yes"). Homebrew's Qt is
# hidden for the run by renaming its Cellar kegs, because Qt falls back to
# its build-time plugin directory there and would mask a bundle that can't
# find its own Contents/PlugIns. The kegs are restored on exit.
set -euo pipefail
app=${1:?usage: check-cli-tls.sh <zproject.app>}
cli="$app/Contents/MacOS/zproject-cli"
[ -x "$cli" ] || { echo "check-cli-tls: no $cli" >&2; exit 1; }

hidden=()
restore() {
  local d
  for d in "${hidden[@]+"${hidden[@]}"}"; do mv "$d.zp-hidden" "$d"; done
}
trap restore EXIT

if command -v brew >/dev/null 2>&1; then
  cellar=$(brew --cellar)
  for d in "$cellar"/qt*; do
    [ -d "$d" ] || continue
    mv "$d" "$d.zp-hidden"
    hidden+=("$d")
  done
  if [ "${CI:-}" = "true" ] && [ ${#hidden[@]} -eq 0 ]; then
    echo "check-cli-tls: expected Homebrew Qt kegs to hide in $cellar" >&2
    exit 1
  fi
  echo "check-cli-tls: hid ${#hidden[@]} Homebrew Qt keg(s)"
fi

out=$(env -u QT_PLUGIN_PATH -u QT_QPA_PLATFORM_PLUGIN_PATH -u DYLD_LIBRARY_PATH -u DYLD_FRAMEWORK_PATH \
        "$cli" doctor 2>&1) || { echo "$out"; echo "check-cli-tls: doctor failed" >&2; exit 1; }
echo "$out"
grep -qE '^tls: +yes$' <<<"$out" || { echo "check-cli-tls: no TLS backend in $app" >&2; exit 1; }
grep -qE '^tlsBackend: +[a-z]' <<<"$out" || { echo "check-cli-tls: no TLS backend name" >&2; exit 1; }
echo "check-cli-tls: OK, the bundled CLI has TLS without Homebrew Qt"
