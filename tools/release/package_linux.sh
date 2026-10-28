#!/bin/bash
# A folder with the program, libSDL2 next to it (found through $ORIGIN) and the seed files, as .tar.gz.
# usage: tools/release/package_linux.sh BUILD/mvp2005 OUT_DIR
set -e
EXE="$1"; OUT="$2"; ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VER="${GITHUB_REF_NAME:-}"; case "$VER" in v[0-9]*) ;; *) VER="v$(sed -n 's/^#define MVP_VERSION "\(.*\)"/\1/p' "$(dirname "$0")/../../runtime/include/version.h")";; esac; NAME="MVPBaseball2005-${VER}-linux-x64"
mkdir -p "$OUT"; STAGE="$(mktemp -d)/$NAME"; mkdir -p "$STAGE/lib"
cp "$EXE" "$STAGE/mvp2005"
SDL="$(ldd "$EXE" | awk '/libSDL2/ {print $3}' | head -1)"
[ -n "$SDL" ] && cp -L "$SDL" "$STAGE/lib/"
patchelf --set-rpath '$ORIGIN/lib' "$STAGE/mvp2005"
bash "$ROOT/tools/release/seed.sh" "$STAGE"
cat > "$STAGE/mvp2005.desktop" <<T
[Desktop Entry]
Type=Application
Name=MVP Baseball 2005
Exec=sh -c 'cd "\$(dirname "%k")" && ./mvp2005'
Categories=Game;SportsGame;
T
cat > "$STAGE/HOW TO START.txt" <<T
1. Unpack this folder anywhere you can write to.
2. Run ./mvp2005 (needs a 64-bit distribution from 2022 or later; Vulkan is used when a driver is installed).
3. Choose your MVP Baseball 2005 (USA) disc image (.iso) and the features you want.
Settings, saves and features are kept in ~/.local/share/mvp2005-recomp, so a new version can simply replace this
folder (the first start of a new version also picks up an older one unpacked next to it). To keep everything in
this folder instead, put an empty file named "portable" next to the program.
T
tar -C "$(dirname "$STAGE")" -czf "$OUT/$NAME.tar.gz" "$NAME"
echo "made $OUT/$NAME.tar.gz"
