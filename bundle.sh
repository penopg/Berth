#!/usr/bin/env bash
# Собирает Berth.app — чтобы приложение запускалось из Launchpad и жило в доке
# с нормальной иконкой, а не как безымянный процесс из терминала.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${OUT:-$ROOT/build}"
APP="$OUT/Berth.app"

say() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }

"$ROOT/build.sh"

say "собираю иконку"
ICONSET="$OUT/berth.iconset"
rm -rf "$ICONSET" && mkdir -p "$ICONSET"
python3 "$ROOT/assets/make_icon.py" "$OUT/berth-1024.png" >/dev/null

# Набор размеров, который ждёт iconutil.
for size in 16 32 128 256 512; do
    sips -z $size $size "$OUT/berth-1024.png" --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
    double=$((size * 2))
    sips -z $double $double "$OUT/berth-1024.png" --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$OUT/berth.icns"

say "собираю Berth.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$OUT/berth" "$APP/Contents/MacOS/berth"
cp "$OUT/berth.icns" "$APP/Contents/Resources/berth.icns"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>              <string>Berth</string>
    <key>CFBundleDisplayName</key>       <string>Berth</string>
    <key>CFBundleExecutable</key>        <string>berth</string>
    <key>CFBundleIdentifier</key>        <string>dev.novikov.berth</string>
    <key>CFBundleIconFile</key>          <string>berth</string>
    <key>CFBundlePackageType</key>       <string>APPL</string>
    <key>CFBundleShortVersionString</key><string>0.1</string>
    <key>CFBundleVersion</key>           <string>0.1</string>
    <key>LSMinimumSystemVersion</key>    <string>11.0</string>
    <key>NSHighResolutionCapable</key>   <true/>
</dict>
</plist>
PLIST

# Подпись «для себя»: без неё macOS ругается на неопознанный бинарь при
# каждом запуске. Это не нотаризация, для распространения нужна настоящая.
codesign --force --deep --sign - "$APP" 2>/dev/null || true

say "готово: $APP"
say "поставить в систему: cp -R \"$APP\" /Applications/"
