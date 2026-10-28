#!/bin/bash
# A folder with mvp2005.exe, the DLLs it needs (SDL2) and the seed files, zipped. Run in MSYS2 (UCRT64).
# usage: tools/release/package_windows.sh BUILD/mvp2005.exe OUT_DIR
set -e
EXE="$1"; OUT="$2"; ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VER="${GITHUB_REF_NAME:-dev}"; NAME="MVPBaseball2005-${VER}-windows"
mkdir -p "$OUT"; STAGE="$(mktemp -d)/$NAME"; mkdir -p "$STAGE"
cp "$EXE" "$STAGE/MVP Baseball 2005.exe"
# every DLL it loads from the MSYS2 prefix (not Windows' own)
ntldd -R "$EXE" | awk '{print $3}' | tr '\\' '/' | grep -i "/ucrt64/" | sort -u | while read -r d; do cp "$(cygpath -u "$d")" "$STAGE/"; done
bash "$ROOT/tools/release/seed.sh" "$STAGE"
cat > "$STAGE/HOW TO START.txt" <<T
1. Unzip this folder anywhere you can write to (Documents, a games drive...), not Program Files.
2. Start "MVP Baseball 2005.exe". If Windows SmartScreen warns, choose More info > Run anyway.
3. Choose your MVP Baseball 2005 (USA) disc image (.iso) and the features you want.
Settings, saves and features are kept in this folder. Needs Windows 10 (1803) or later.
T
(cd "$(dirname "$STAGE")" && zip -qr "$OLDPWD/$OUT/$NAME.zip" "$NAME")
echo "made $OUT/$NAME.zip"
