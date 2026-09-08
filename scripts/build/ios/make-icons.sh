#!/bin/bash
# CorsixTH-iOS @build 2026-09-06 Generate the iOS app icon set from CorsixTH's own icon.
#
# Produces, under the output directory:
#   AppIcon.xcassets/AppIcon.appiconset/  every @1x/@2x/@3x PNG the icon set declares
#   Assets.car                            the compiled catalog (CFBundleIconName = AppIcon)
#   AppIcon60x60@2x.png                   loose fallbacks that SpringBoard always honours
#   AppIcon76x76@2x.png                   for developer-signed sideloads, even when it is
#   AppIcon83.5x83.5@2x.png               still serving a cached Assets.car icon
#
# Nothing here is committed: the icons are build products, regenerated from
# CorsixTH/Icon.icns (or CorsixTH.ico) whenever the source or the parameters change.

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)

SOURCE=${CORSIXTH_IOS_ICON_SOURCE:-$ROOT/CorsixTH/Icon.icns}
OUT=${1:-$ROOT/build/ios-icons}
# The source artwork is a rounded rectangle with a black border and transparent corners.
# iOS applies its own mask, so crop that frame off (inset) and flatten what is left onto an
# opaque colour (iOS icons may not carry an alpha channel).
BG=${CORSIXTH_IOS_ICON_BG:-111111}
INSET=${CORSIXTH_IOS_ICON_INSET:-0.05}
MIN_OS=${CORSIXTH_IOS_MIN_OS:-16.0}

[[ -f $SOURCE ]] || { echo "make-icons: icon source not found: $SOURCE" >&2; exit 1; }

STAMP="$OUT/.stamp"
WANT="source=$SOURCE bg=$BG inset=$INSET mtime=$(stat -f %m "$SOURCE")"
if [[ -f $STAMP && -f $OUT/Assets.car ]] && [[ $(cat "$STAMP") == "$WANT" ]]; then
  echo "  icons: up to date ($OUT)"
  exit 0
fi

SET="$OUT/AppIcon.xcassets/AppIcon.appiconset"
rm -rf "$OUT"
mkdir -p "$SET"

# Render the largest size once with the Swift compositor, then downscale with sips: the
# compositor costs a ~5 s swift compile per invocation and sips is instant.
MASTER="$OUT/icon-1024.png"
swift "$ROOT/scripts/build/ios/composite-icon.swift" "$SOURCE" "$MASTER" 1024 "$BG" "$INSET"

emit() { # emit <pixels> <filename>
  if [[ $1 -eq 1024 ]]; then cp "$MASTER" "$SET/$2"
  else sips -Z "$1" "$MASTER" --out "$SET/$2" >/dev/null; fi
}

# idiom size, scale, target device, pixel size, file name
ENTRIES="
iphone 20x20 2x 40 AppIcon20x20@2x.png
iphone 20x20 3x 60 AppIcon20x20@3x.png
iphone 29x29 2x 58 AppIcon29x29@2x.png
iphone 29x29 3x 87 AppIcon29x29@3x.png
iphone 40x40 2x 80 AppIcon40x40@2x.png
iphone 40x40 3x 120 AppIcon40x40@3x.png
iphone 60x60 2x 120 AppIcon60x60@2x.png
iphone 60x60 3x 180 AppIcon60x60@3x.png
ipad 20x20 1x 20 AppIcon20x20~ipad.png
ipad 20x20 2x 40 AppIcon20x20@2x~ipad.png
ipad 29x29 1x 29 AppIcon29x29~ipad.png
ipad 29x29 2x 58 AppIcon29x29@2x~ipad.png
ipad 40x40 1x 40 AppIcon40x40~ipad.png
ipad 40x40 2x 80 AppIcon40x40@2x~ipad.png
ipad 76x76 2x 152 AppIcon76x76@2x~ipad.png
ipad 83.5x83.5 2x 167 AppIcon83.5x83.5@2x~ipad.png
ios-marketing 1024x1024 1x 1024 AppIcon1024x1024.png
"

{
  echo '{'
  echo '  "images" : ['
  first=1
  while read -r idiom size scale px file; do
    [[ -z ${idiom:-} ]] && continue
    emit "$px" "$file"
    [[ $first -eq 1 ]] || echo '    },'
    first=0
    echo '    {'
    echo "      \"filename\" : \"$file\","
    echo "      \"idiom\" : \"$idiom\","
    echo "      \"scale\" : \"$scale\","
    echo "      \"size\" : \"$size\""
  done <<< "$ENTRIES"
  echo '    }'
  echo '  ],'
  echo '  "info" : { "author" : "xcode", "version" : 1 }'
  echo '}'
} > "$SET/Contents.json"

cat > "$OUT/AppIcon.xcassets/Contents.json" <<'JSON'
{
  "info" : { "author" : "xcode", "version" : 1 }
}
JSON

xcrun actool "$OUT/AppIcon.xcassets" \
  --compile "$OUT" \
  --app-icon AppIcon \
  --output-partial-info-plist "$OUT/AppIcon-partial.plist" \
  --platform iphoneos \
  --minimum-deployment-target "$MIN_OS" \
  --target-device iphone --target-device ipad \
  --output-format human-readable-text >/dev/null

[[ -f $OUT/Assets.car ]] || { echo "make-icons: actool produced no Assets.car" >&2; exit 1; }

# Loose PNGs for the CFBundleIconFiles path (see the header comment).
cp "$SET/AppIcon60x60@2x.png"        "$OUT/AppIcon60x60@2x.png"
cp "$SET/AppIcon76x76@2x~ipad.png"   "$OUT/AppIcon76x76@2x.png"
cp "$SET/AppIcon83.5x83.5@2x~ipad.png" "$OUT/AppIcon83.5x83.5@2x.png"

for f in "$OUT"/AppIcon*.png; do
  if sips -g hasAlpha "$f" | grep -q "hasAlpha: yes"; then
    echo "make-icons: $f still has an alpha channel; iOS rejects that" >&2
    exit 1
  fi
done

echo "$WANT" > "$STAMP"
echo "  icons: generated from $(basename "$SOURCE") -> $OUT"
