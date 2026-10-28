#!/bin/bash
# Files every download carries (copied into the game folder at the first start): the major leagues' rates by
# season (simulated games), the era logos, and the Python tools behind "Build a season" and "My League".
# usage: tools/release/seed.sh DEST   (DEST/seed is made)
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
D="$1/seed"; mkdir -p "$D/mods" "$D/tools"
cp "$ROOT/mods/league_rates.txt" "$D/mods/" 2>/dev/null || true
[ -d "$ROOT/mods/era-logos" ] && cp -R "$ROOT/mods/era-logos" "$D/mods/"
cp "$ROOT"/tools/*.py "$D/tools/"
for d in eras teammaker heads; do [ -d "$ROOT/tools/$d" ] && cp -R "$ROOT/tools/$d" "$D/tools/"; done
find "$D" -name __pycache__ -prune -exec rm -rf {} + 2>/dev/null || true
cp "$ROOT/README.md" "$1/README.md" 2>/dev/null || true
