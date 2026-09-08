#!/bin/bash
# CorsixTH-iOS @build 2026-09-06 One-command build -> stage -> sign -> install pipeline
# for the iOS/iPadOS app bundle. See docs/port/IOS_BUILD.md for the environment contract.
#
# The bundle is produced by CMake's own iOS app target and populated by the tree's existing
# install() rules; there is no shell app and no inside-out re-signing, because everything
# CorsixTH links on iOS is static (see .superpowers/sdd/IOS_PORT_PLAN/task-2-report.md).
#
#   configure -> xcodebuild (signs the bare app, mints/refreshes the profile)
#             -> cmake --install (adds ~360 resource files, invalidating that signature)
#             -> stage the app icon
#             -> codesign --force (re-seal the complete bundle)
#             -> devicectl install / launch
#
# No personal team id, signing identity or bundle id is committed. All three come from the
# environment, optionally via the git-ignored file named by CORSIXTH_IOS_ENV_FILE.

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
PRESET=ios-device-xcode
BUILD_DIR="$ROOT/build/$PRESET"
STAGE_DIR="$ROOT/build/ios-stage"
ICON_DIR="$ROOT/build/ios-icons"
APP="$STAGE_DIR/CorsixTH.app"
XCODE_APP="$BUILD_DIR/CorsixTH/Release-iphoneos/CorsixTH.app"
PROJECT="$BUILD_DIR/CorsixTH_Top_Level.xcodeproj"

usage() {
  cat <<'USAGE'
usage: scripts/build/ios/package-ios.sh [options]

  --resign-only      Skip the CMake configure and reuse the warm build and stage trees.
                     Still runs the incremental xcodebuild that refreshes the provisioning
                     profile, then re-stages, re-signs and (with --install) reinstalls.
  --clean            Delete the Xcode build tree and the stage first, then full rebuild.
  --no-build         Do not run xcodebuild; sign and stage whatever is already built.
                     (Does not refresh the provisioning profile.)
  --install          Install the signed bundle on the device with devicectl.
  --launch           Launch it afterwards with --console (implies --install unless
                     --no-install-launch semantics are wanted; use with --install).
  --device <id>      devicectl identifier to install to. Overrides CORSIXTH_IOS_DEVICE.
  --push-data <dir>  Copy a Theme Hospital data folder into the app container at
                     Documents/CorsixTH/<basename>. Never uses --remove-existing-content.
  -h, --help         This message.

Environment (see docs/port/IOS_BUILD.md):
  CORSIXTH_IOS_TEAM_ID            required  Apple Developer team id  (DEVELOPMENT_TEAM)
  CORSIXTH_IOS_CODESIGN_IDENTITY  required  full codesign identity string, quoted
  CORSIXTH_IOS_BUNDLE_ID          required  CFBundleIdentifier
  CORSIXTH_IOS_DEVICE             optional  devicectl identifier for --install/--launch
  CORSIXTH_IOS_DEVICE_UDID        optional  hardware UDID for xcodebuild -destination
  CORSIXTH_IOS_SHORT_VERSION      optional  CFBundleShortVersionString (default 0.70)
  CORSIXTH_IOS_BUNDLE_VERSION     optional  CFBundleVersion (default 1)
  CORSIXTH_IOS_ICON_SOURCE/_BG/_INSET       app icon source and compositing parameters
  CORSIXTH_IOS_ENV_FILE           optional  file sourced for the above; defaults to
                                            scripts/build/ios/ios-signing.env if it exists
  VCPKG_ROOT                      required for the CMake configure step
USAGE
}

# ---------------------------------------------------------------- argument parsing
MODE=full
DO_INSTALL=0
DO_LAUNCH=0
DEVICE_ARG=
PUSH_DATA=
while [[ $# -gt 0 ]]; do
  case "$1" in
    --resign-only) MODE=resign ;;
    --clean) MODE=clean ;;
    --no-build) MODE=nobuild ;;
    --install) DO_INSTALL=1 ;;
    --launch) DO_LAUNCH=1 ;;
    --device) DEVICE_ARG=${2:?--device needs an identifier}; shift ;;
    --push-data) PUSH_DATA=${2:?--push-data needs a directory}; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "package-ios: unknown option '$1'" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

# ---------------------------------------------------------------- environment contract
# Values already exported in the caller's shell win over the env file.
ENV_FILE=${CORSIXTH_IOS_ENV_FILE:-$ROOT/scripts/build/ios/ios-signing.env}
if [[ -f $ENV_FILE ]]; then
  saved=
  for v in CORSIXTH_IOS_TEAM_ID CORSIXTH_IOS_CODESIGN_IDENTITY CORSIXTH_IOS_BUNDLE_ID \
           CORSIXTH_IOS_DEVICE CORSIXTH_IOS_DEVICE_UDID CORSIXTH_IOS_SHORT_VERSION \
           CORSIXTH_IOS_BUNDLE_VERSION CORSIXTH_IOS_ICON_SOURCE CORSIXTH_IOS_ICON_BG \
           CORSIXTH_IOS_ICON_INSET VCPKG_ROOT; do
    eval "cur=\${$v:-}"
    [[ -n $cur ]] && saved="$saved $v=$(printf %q "$cur")"
  done
  # shellcheck disable=SC1090
  . "$ENV_FILE"
  [[ -n $saved ]] && eval "export $saved"
  echo "==> signing environment from $ENV_FILE"
fi

require() {
  eval "val=\${$1:-}"
  [[ -n $val ]] || { echo "package-ios: $1 is not set (see --help)" >&2; exit 1; }
}
require CORSIXTH_IOS_TEAM_ID
require CORSIXTH_IOS_CODESIGN_IDENTITY
require CORSIXTH_IOS_BUNDLE_ID

TEAM_ID=$CORSIXTH_IOS_TEAM_ID
IDENTITY=$CORSIXTH_IOS_CODESIGN_IDENTITY
BUNDLE_ID=$CORSIXTH_IOS_BUNDLE_ID
SHORT_VERSION=${CORSIXTH_IOS_SHORT_VERSION:-0.70}
BUNDLE_VERSION=${CORSIXTH_IOS_BUNDLE_VERSION:-1}
DEVICE=${DEVICE_ARG:-${CORSIXTH_IOS_DEVICE:-}}
DEVICE_UDID=${CORSIXTH_IOS_DEVICE_UDID:-}

# There is more than one "Apple Development" certificate on a machine that has belonged to
# two teams, and a prefix match against several is fatal, not ambiguous-but-fine.
matches=$(security find-identity -v -p codesigning | grep -c -F "$IDENTITY" || true)
if [[ $matches -eq 0 ]]; then
  echo "package-ios: no codesigning identity matches '$IDENTITY'" >&2
  security find-identity -v -p codesigning >&2
  exit 1
elif [[ $matches -gt 1 ]]; then
  echo "package-ios: '$IDENTITY' matches $matches identities; give the full string" >&2
  security find-identity -v -p codesigning >&2
  exit 1
fi

echo "==> team $TEAM_ID, bundle id $BUNDLE_ID, identity $IDENTITY"

# ---------------------------------------------------------------- configure
if [[ $MODE == clean ]]; then
  echo "==> clean: removing $BUILD_DIR and $STAGE_DIR"
  rm -rf "$BUILD_DIR" "$STAGE_DIR"
fi

if [[ $MODE == resign || $MODE == nobuild ]]; then
  [[ -d $BUILD_DIR ]] || {
    echo "package-ios: $BUILD_DIR does not exist; run without --resign-only first" >&2
    exit 1
  }
  cached=$(sed -n 's/^CORSIXTH_IOS_BUNDLE_ID:STRING=//p' "$BUILD_DIR/CMakeCache.txt")
  if [[ $cached != "$BUNDLE_ID" ]]; then
    echo "package-ios: the warm build tree is configured for bundle id '$cached', not" >&2
    echo "             '$BUNDLE_ID'. Re-run without --resign-only to reconfigure." >&2
    exit 1
  fi
else
  : "${VCPKG_ROOT:?VCPKG_ROOT must point at a bootstrapped vcpkg checkout}"
  echo "==> cmake --preset $PRESET"
  cmake --preset "$PRESET" \
    -DCORSIXTH_IOS_BUNDLE_ID="$BUNDLE_ID" \
    -DCORSIXTH_IOS_SHORT_VERSION="$SHORT_VERSION" \
    -DCORSIXTH_IOS_BUNDLE_VERSION="$BUNDLE_VERSION" >/dev/null
fi

# ---------------------------------------------------------------- build and sign the app
if [[ $MODE != nobuild ]]; then
  echo "==> xcodebuild (signs the bare app and refreshes the provisioning profile)"
  dest=()
  if [[ -n $DEVICE_UDID ]]; then
    # A team with no registered devices cannot mint a profile until xcodebuild is told
    # which device to register; -destination wants the hardware UDID, not the devicectl id.
    dest=(-destination "platform=iOS,id=$DEVICE_UDID")
  fi
  XCODEBUILD_LOG="$ROOT/build/ios-xcodebuild.log"
  set +e
  xcodebuild -project "$PROJECT" -target CorsixTH -configuration Release \
    -allowProvisioningUpdates -allowProvisioningDeviceRegistration \
    "${dest[@]+"${dest[@]}"}" \
    DEVELOPMENT_TEAM="$TEAM_ID" \
    CODE_SIGN_STYLE=Automatic \
    PRODUCT_BUNDLE_IDENTIFIER="$BUNDLE_ID" > "$XCODEBUILD_LOG" 2>&1
  rc=$?
  set -e
  grep -E "Signing Identity|Provisioning Profile|BUILD (SUCCEEDED|FAILED)" "$XCODEBUILD_LOG" || true
  if [[ $rc -ne 0 ]]; then
    echo "package-ios: xcodebuild failed (exit $rc). Full log: $XCODEBUILD_LOG" >&2
    grep -E "error:" "$XCODEBUILD_LOG" | head -40 >&2
    exit $rc
  fi
  [[ -x $XCODE_APP/CorsixTH ]] || { echo "package-ios: xcodebuild produced no binary" >&2; exit 1; }
fi

# ---------------------------------------------------------------- stage
if [[ $MODE == full || $MODE == clean ]]; then
  rm -rf "$STAGE_DIR"
fi
echo "==> cmake --install -> $STAGE_DIR"
cmake --install "$BUILD_DIR" --config Release --prefix "$STAGE_DIR" >/dev/null

echo "==> app icon"
"$ROOT/scripts/build/ios/make-icons.sh" "$ICON_DIR"
cp "$ICON_DIR/Assets.car" "$APP/Assets.car"
cp "$ICON_DIR/AppIcon60x60@2x.png" "$ICON_DIR/AppIcon76x76@2x.png" \
   "$ICON_DIR/AppIcon83.5x83.5@2x.png" "$APP/"

# ---------------------------------------------------------------- manifest check
# Never ship a bundle that is quietly missing part of the game.
missing=0
for required in CorsixTH CorsixTH.lua re.lua Info.plist LICENSE.txt embedded.mobileprovision \
                Assets.car "AppIcon60x60@2x.png" "AppIcon76x76@2x.png" "AppIcon83.5x83.5@2x.png" \
                CorsixTHUnicode.ttf Lua Bitmap Levels Campaigns Graphics; do
  if [[ ! -e $APP/$required ]]; then
    echo "package-ios: staged bundle is missing $required" >&2
    missing=1
  fi
done
if ! ls "$APP"/*.sf2 >/dev/null 2>&1; then
  echo "package-ios: staged bundle has no SoundFont (.sf2); MIDI would be silent" >&2
  missing=1
fi
for required in Lua/app.lua Lua/dialogs Lua/languages Bitmap/aux_ui.tab Levels/original05.level Campaigns/ChrizmanTV.campaign Graphics/file_mapping.txt; do
  if [[ ! -e $APP/$required ]]; then
    echo "package-ios: staged bundle is missing $required" >&2
    missing=1
  fi
done
[[ $missing -eq 0 ]] || exit 1

plist_id=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$APP/Info.plist")
[[ $plist_id == "$BUNDLE_ID" ]] || {
  echo "package-ios: staged Info.plist says $plist_id, expected $BUNDLE_ID" >&2
  exit 1
}

# ---------------------------------------------------------------- sign
# Adding ~360 resource files invalidated Xcode's signature, so re-seal the whole bundle
# with the entitlements Xcode's own signing produced (never hand-written).
ENTITLEMENTS="$ROOT/build/ios-stage-entitlements.plist"
codesign -d --entitlements - --xml "$XCODE_APP" > "$ENTITLEMENTS" 2>/dev/null
echo "==> codesign"
codesign --force --sign "$IDENTITY" --entitlements "$ENTITLEMENTS" --timestamp=none "$APP"
codesign --verify --deep --strict --verbose=2 "$APP"

echo "==> $(find "$APP" -type f | wc -l | tr -d ' ') files, $(du -sh "$APP" | cut -f1) in $APP"

# ---------------------------------------------------------------- device
need_device() {
  [[ -n $DEVICE ]] || {
    echo "package-ios: no device; pass --device or set CORSIXTH_IOS_DEVICE" >&2
    exit 1
  }
}

# A Wi-Fi-paired device drops its tunnel when it auto-locks, which fails a long transfer
# partway through. Retrying is normal operation, not an error worth aborting on.
devicectl_retry() {
  local attempt=1
  while true; do
    if xcrun devicectl "$@"; then return 0; fi
    if [[ $attempt -ge 3 ]]; then
      echo "package-ios: devicectl $1 $2 failed after $attempt attempts" >&2
      echo "             (a locked device refuses launches; unlock it and retry)" >&2
      return 1
    fi
    echo "    devicectl attempt $attempt failed; retrying in 5s" >&2
    attempt=$((attempt + 1))
    sleep 5
  done
}

if [[ $DO_INSTALL -eq 1 ]]; then
  need_device
  echo "==> devicectl install"
  devicectl_retry device install app --device "$DEVICE" "$APP"
fi

if [[ -n $PUSH_DATA ]]; then
  need_device
  [[ -d $PUSH_DATA ]] || { echo "package-ios: no such data directory: $PUSH_DATA" >&2; exit 1; }
  name=$(basename "$PUSH_DATA")
  echo "==> pushing $name into the app container (Documents/CorsixTH/$name)"
  # --remove-existing-content wipes the WHOLE container, not the destination. Never.
  devicectl_retry device copy to --device "$DEVICE" \
    --domain-type appDataContainer --domain-identifier "$BUNDLE_ID" \
    --source "$PUSH_DATA" --destination "Documents/CorsixTH/$name"
fi

if [[ $DO_LAUNCH -eq 1 ]]; then
  need_device
  echo "==> devicectl launch"
  devicectl_retry device process launch --console --terminate-existing \
    --device "$DEVICE" "$BUNDLE_ID"
fi
