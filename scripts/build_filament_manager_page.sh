#!/usr/bin/env bash
# Builds the Filament Manager web page and replaces resources/web/filament_manager/
# with the build output. Stops on the first error.
#
#   scripts/build_filament_manager_page.sh
#
# Needs node and npm; `npm ci` fetches from the npm registry.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PAGE="$ROOT/src/slic3r/GUI/FilamentManager/page"
OUT="$ROOT/resources/web/filament_manager"

cd "$PAGE"
npm ci --no-audit --no-fund
npm run build

[ -f dist/index.html ] || { echo "build produced no dist/index.html" >&2; exit 1; }
[ -d dist/assets ] || { echo "build produced no dist/assets" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT"
cp dist/index.html "$OUT/index.html"
cp -r dist/assets "$OUT/assets"

echo "Bundle in $OUT:"
find "$OUT" -type f | sort | while read -r f; do
  printf '%10d  %s\n' "$(wc -c < "$f")" "${f#"$ROOT"/}"
done
