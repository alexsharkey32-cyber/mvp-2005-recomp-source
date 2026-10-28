#!/bin/bash
# MVP Baseball 2005.app with SDL2 and MoltenVK inside (Contents/Frameworks), signed ad hoc, zipped.
# usage: tools/release/package_mac.sh BUILD/mvp2005 OUT_DIR
set -e
EXE="$1"; OUT="$2"; ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$OUT"; STAGE="$(mktemp -d)"; APP="$STAGE/MVP Baseball 2005.app"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources"
cp "$EXE" "$APP/Contents/MacOS/mvp2005"
BREW="$(brew --prefix)"
SDL="$(otool -L "$EXE" | awk '/libSDL2/ {print $1}' | head -1)"
SDLREAL="$SDL"; [[ "$SDL" == @rpath/* ]] && SDLREAL="$BREW/lib/$(basename "$SDL")"
cp -L "$SDLREAL" "$APP/Contents/Frameworks/"
install_name_tool -change "$SDL" "@executable_path/../Frameworks/$(basename "$SDLREAL")" "$APP/Contents/MacOS/mvp2005"
install_name_tool -id "@executable_path/../Frameworks/$(basename "$SDLREAL")" "$APP/Contents/Frameworks/$(basename "$SDLREAL")"
# Homebrew's sdl2 is sdl2-compat: the SDL2 API over SDL3, which it opens at start from next to itself
for s3 in "$BREW/lib/libSDL3.0.dylib" "$BREW/opt/sdl3/lib/libSDL3.0.dylib"; do
  if [ -f "$s3" ]; then                       # sdl2-compat looks for @loader_path/libSDL3.dylib first
    cp -L "$s3" "$APP/Contents/Frameworks/libSDL3.dylib"; install_name_tool -id "@loader_path/libSDL3.dylib" "$APP/Contents/Frameworks/libSDL3.dylib"
    break
  fi
done
# Vulkan through MoltenVK (opened at run time; Metal is the default renderer anyway)
[ -f "$BREW/lib/libMoltenVK.dylib" ] && cp -L "$BREW/lib/libMoltenVK.dylib" "$APP/Contents/Frameworks/"
bash "$ROOT/tools/release/seed.sh" "$APP/Contents/Resources"
mv "$APP/Contents/Resources/README.md" "$STAGE/README.md"
VER="${GITHUB_REF_NAME:-}"; case "$VER" in v[0-9]*) ;; *) VER="v$(sed -n 's/^#define MVP_VERSION "\(.*\)"/\1/p' "$(dirname "$0")/../../runtime/include/version.h")";; esac
cat > "$APP/Contents/Info.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>MVP Baseball 2005</string>
  <key>CFBundleDisplayName</key><string>MVP Baseball 2005</string>
  <key>CFBundleIdentifier</key><string>io.github.mvp2005recomp</string>
  <key>CFBundleExecutable</key><string>mvp2005</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>${VER#v}</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.sports-games</string>
</dict></plist>
PL
codesign --force --deep -s - "$APP"
cat > "$STAGE/HOW TO START.txt" <<T
1. Move "MVP Baseball 2005.app" to Applications (or anywhere).
2. The first time: right-click it and choose Open, then Open again (it isn't signed by Apple).
3. Choose your MVP Baseball 2005 (USA) disc image (.iso) and the features you want.
Your settings, saves and features are kept in ~/Library/Application Support/MVP Baseball 2005 Recomp.
T
(cd "$STAGE" && ditto -c -k --sequesterRsrc . "$OLDPWD/$OUT/MVPBaseball2005-${VER}-mac.zip")
echo "made $OUT/MVPBaseball2005-${VER}-mac.zip"
