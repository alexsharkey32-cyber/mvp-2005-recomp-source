#!/bin/bash
# Makes "MVP Baseball 2005.app" in the project folder: double-click it to open the launcher
# (import your disc, pick settings, Play). The app runs build/mvp2005 from this folder, so mods/,
# textures/, memcards/ and mvp2005.ini stay where they are. Run again after moving the folder.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$ROOT/MVP Baseball 2005.app"
[ -x "$ROOT/build/mvp2005" ] || { echo "build first: make -j8"; exit 1; }
rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cat > "$APP/Contents/MacOS/MVPBaseball2005" <<SH
#!/bin/bash
cd "$ROOT" || exit 1
exec ./build/mvp2005 "\$@" >> mvp2005.log 2>&1
SH
chmod +x "$APP/Contents/MacOS/MVPBaseball2005"
cat > "$APP/Contents/Info.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>MVP Baseball 2005</string>
  <key>CFBundleDisplayName</key><string>MVP Baseball 2005</string>
  <key>CFBundleIdentifier</key><string>local.mvp2005.recomp</string>
  <key>CFBundleExecutable</key><string>MVPBaseball2005</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
</dict></plist>
PL
echo "made $APP"
