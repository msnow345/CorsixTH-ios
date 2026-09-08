# Building CorsixTH for iOS / iPadOS

This document covers the dependency and configure step of the iOS port: obtaining every native
dependency for `arm64` iOS (device and simulator) and configuring the CorsixTH tree against them.
Compiling, linking, bundling and signing the app itself is covered further down, under
[Packaging, signing and installing](#packaging-signing-and-installing).

## Prerequisites

| Tool | Notes |
| --- | --- |
| Xcode + iOS SDK | verified with Xcode 26.6 (build 17F113), iOS SDK 26.5 |
| `cmake` | 3.16+ (verified with the Homebrew build) |
| `ninja` | the iOS presets use the Ninja generator |
| `vcpkg` | bootstrapped checkout; `VCPKG_ROOT` must point at it |

`autoconf` / `automake` are **not** required — no dependency in the iOS set uses autotools
(see "Deliberate omissions" below for why `libsndfile` is excluded).

Everything is installed out of the source tree, under `build/ios-deps/` (git-ignored by the
existing `/build*/` rule). No dependency source or binary is committed.

## 1. Cross-build the dependencies

```sh
export VCPKG_ROOT=/path/to/vcpkg          # already set on the reference host
scripts/build/ios/fetch-deps.sh device    # or: simulator / all
```

The script is a thin wrapper around one `vcpkg install` in manifest mode plus an artefact
verification pass. The equivalent explicit command for the device triplet is:

```sh
"$VCPKG_ROOT/vcpkg" install \
  --triplet arm64-ios-min16 \
  --host-triplet arm64-osx \
  --overlay-triplets "$PWD/CMake/ios/triplets" \
  --x-manifest-root="$PWD" \
  --x-install-root="$PWD/build/ios-deps/device"
```

(The simulator equivalent uses `--triplet arm64-ios-simulator-min16` and
`--x-install-root="$PWD/build/ios-deps/simulator"`. Each triplet needs its **own** install
root: vcpkg manifest mode owns a root exclusively and uninstalls anything outside the current
install plan, so a shared root makes the two triplets delete each other.)

Dependency versions come from the registry baseline already pinned in
`vcpkg-configuration.json` (`microsoft/vcpkg@bee87c32fcf25e81b0d9c312144475b5e34181a8`), so
the set is reproducible without pinning anything further.

After it finishes the script asserts, for every installed `.a`:

* `lipo -info` reports `arm64` and nothing else;
* `otool -l` reports `platform 2` (`PLATFORM_IOS`; `7` for the simulator) and `minos 16.0`
  in every `LC_BUILD_VERSION` record;
* `include/SDL3/SDL_events.h` exports `SDL_EVENT_PINCH_BEGIN`
  (`CorsixTH/Src/sdl_core.cpp` needs the pinch gesture events);
* the eight libraries CorsixTH links directly are present.

## 2. Configure CorsixTH

```sh
cmake --preset ios-device       # build/ios-device,  Ninja
cmake --preset ios-simulator    # build/ios-simulator, Ninja
cmake --preset ios-device-xcode # build/ios-device-xcode, Xcode generator
```

All three presets set `CMAKE_SYSTEM_NAME=iOS`, `CMAKE_OSX_ARCHITECTURES=arm64`,
`CMAKE_OSX_DEPLOYMENT_TARGET=16.0`, the matching sysroot and vcpkg triplet, and turn off
everything the v1 iOS build does not need:
`WITH_MOVIES`, `WITH_UPDATE_CHECK`, `WITH_MIDI_DEVICE`, `WITH_LUAJIT`, `WITH_TRACY`,
`BUILD_ANIMVIEW`, `BUILD_TOOLS`, `ENABLE_UNIT_TESTS`, `ENABLE_SANITIZERS`,
`USE_SOURCE_DATADIRS`; and set `SEARCH_LOCAL_DATADIRS=ON`.

`VCPKG_INSTALLED_DIR` points each preset at the same per-triplet tree `fetch-deps.sh`
populates (`build/ios-deps/device` or `build/ios-deps/simulator`), so configure does not
rebuild anything.

`ios-device-xcode` exists for the packaging/signing work; the Ninja presets give faster and
cleaner compiler diagnostics.

Two things worth knowing about configure:

* It still runs `vcpkg install` in manifest mode through the toolchain file, so `VCPKG_ROOT`
  must be set even though nothing needs building. With `build/ios-deps/<triplet>` already
  populated it reports every package as "already installed" and costs a couple of seconds.
* `CMAKE_SYSTEM_PROCESSOR` (set to `arm64` by `ios-base`) is baked into
  `CMakeFiles/<ver>/CMakeSystem.cmake` on the **first** configure of a binary directory.
  Re-running a preset over an existing cache will not change it; delete the binary directory
  instead. Without it `CORSIX_TH_ARCH` is empty and `th_lua.cpp` fails to compile.

## Why an overlay triplet

`CMake/ios/triplets/arm64-ios-min16.cmake` and `arm64-ios-simulator-min16.cmake` are copies
of vcpkg's community `arm64-ios` / `arm64-ios-simulator` triplets plus two settings:

* `-DCMAKE_OSX_DEPLOYMENT_TARGET=16.0` — the community triplets leave the deployment target
  unset, so ports inherit the SDK default and every static library ends up stamped
  `LC_BUILD_VERSION minos 26.5`. That silently raises the app's real minimum OS.
* `-DCMAKE_MACOSX_BUNDLE=OFF` — CMake defaults `CMAKE_MACOSX_BUNDLE` to `ON` when
  `CMAKE_SYSTEM_NAME` is `iOS`, which turns each dependency's helper executable into a
  `.app` and breaks any port that `install()`s one with only a `RUNTIME DESTINATION`.
  fluidsynth's CLI fails to configure without this.

The triplets are deliberately named differently from the built-in ones so that forgetting
`--overlay-triplets` fails loudly instead of quietly producing SDK-minimum binaries.

## Deliberate omissions

* **`lua[tools]`** — the `lua`/`luac` executables are declared unsupported on iOS by the
  port and are not needed in the app. `vcpkg.json` now requests the `tools` feature only on
  `!ios`.
* **`fluidsynth[sndfile]`** — `libsndfile[external-libs]` pulls `mp3lame`, whose autotools
  `configure` cannot cross-compile to iOS (vcpkg-make passes `--build` equal to `--host`, so
  configure tries to run an iOS binary and dies with "cannot run C compiled programs").
  Dropping the feature costs SF3 (Vorbis-compressed SoundFont) support and
  render-to-file; plain SF2 playback, which is what the port needs, is unaffected.
  `vcpkg.json` requests the `sndfile` feature only on `!ios`.
* **ffmpeg, curl, rtmidi, wxWidgets, Catch2, Tracy** — the corresponding CMake options are
  off, so their `find_package` calls never run and their manifest features are never
  activated.

## Verifying by hand

```sh
cd build/ios-deps/device/arm64-ios-min16
lipo -info lib/libSDL3.a
otool -l lib/libSDL3.a | grep -A3 LC_BUILD_VERSION | head
grep -n SDL_EVENT_PINCH_BEGIN include/SDL3/SDL_events.h
```

`vtool -show-build` refuses to read a static archive ("file is not mach-o"); use `otool -l`
for `.a` files and `vtool` for the linked executable.

---

# Packaging, signing and installing

`scripts/build/ios/package-ios.sh` is the whole pipeline. It builds the CMake iOS app
target, stages the bundle with the tree's own `install()` rules, generates and stages the app
icon, re-signs the finished bundle and — on request — installs, seeds and launches it on a
device.

There is **no shell app and no inside-out re-signing**: everything CorsixTH links on iOS is
static, so the CMake app target is the shipping bundle (see
`.superpowers/sdd/IOS_PORT_PLAN/task-2-report.md` §1 for the reasoning).

## The environment contract

No team id, signing identity or bundle identifier is committed. The script reads them from
the environment, and sources `scripts/build/ios/ios-signing.env` (git-ignored; copy
`ios-signing.env.example`) if that file exists. Variables already exported in your shell win
over the file.

| Variable | Required | Meaning |
| --- | --- | --- |
| `CORSIXTH_IOS_TEAM_ID` | yes | Apple Developer team id; becomes `DEVELOPMENT_TEAM`. |
| `CORSIXTH_IOS_CODESIGN_IDENTITY` | yes | The **full** `codesign -s` identity string. |
| `CORSIXTH_IOS_BUNDLE_ID` | yes | `CFBundleIdentifier`; also `-DCORSIXTH_IOS_BUNDLE_ID`. |
| `CORSIXTH_IOS_DEVICE` | for `--install` | devicectl identifier (`xcrun devicectl list devices`). |
| `CORSIXTH_IOS_DEVICE_UDID` | first run on a new team | **Hardware** UDID for `xcodebuild -destination`. Not the same value as `CORSIXTH_IOS_DEVICE`. |
| `CORSIXTH_IOS_SHORT_VERSION` | no | `CFBundleShortVersionString` (default `0.70`). |
| `CORSIXTH_IOS_BUNDLE_VERSION` | no | `CFBundleVersion` (default `1`). Bump it to beat SpringBoard's icon cache. |
| `CORSIXTH_IOS_ICON_SOURCE` | no | Icon artwork (default `CorsixTH/Icon.icns`). |
| `CORSIXTH_IOS_ICON_BG` | no | Six hex digits behind the icon (default `111111`). |
| `CORSIXTH_IOS_ICON_INSET` | no | Fraction cropped off each icon edge (default `0.05`). |
| `CORSIXTH_IOS_ENV_FILE` | no | Alternative path for the file above. |
| `VCPKG_ROOT` | yes (except `--resign-only`) | Bootstrapped vcpkg checkout. |

Worked example (the reference host's values — these are personal and belong here as
documentation, not in a committed script):

```sh
export CORSIXTH_IOS_TEAM_ID=ABCDE12345
export CORSIXTH_IOS_CODESIGN_IDENTITY="Apple Development: Your Name (XXXXXXXXXX)"
export CORSIXTH_IOS_BUNDLE_ID=com.example.corsixth
export CORSIXTH_IOS_DEVICE=AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE   # iPad Pro 11-inch (M5)
export CORSIXTH_IOS_DEVICE_UDID=00008XXX-XXXXXXXXXXXXXXXX          # same iPad, hardware UDID
export VCPKG_ROOT=$HOME/vcpkg
```

Two traps worth stating plainly:

* **The team id and the identity's parenthetical are different values** (`ABCDE12345` vs
  `XXXXXXXXXX`) and that is normal — one identifies the team, the other the certificate.
* **Never shorten the identity to `"Apple Development"`.** Once a machine holds certificates
  from more than one team the prefix matches several and `codesign` fails. The script checks
  the string against `security find-identity` and refuses an ambiguous one.

## Usage

```sh
scripts/build/ios/fetch-deps.sh device        # once, to fetch the native dependencies

# full build + package + install + launch, from nothing
scripts/build/ios/package-ios.sh --clean --install --launch

# the fast loop: refresh the profile, re-stage, re-sign, reinstall (~6 s including install)
scripts/build/ios/package-ios.sh --resign-only --install

# seed a container with your own Theme Hospital data
scripts/build/ios/package-ios.sh --no-build --push-data ~/CorsixTH-testdata/full/HOSP
```

| Flag | Effect |
| --- | --- |
| *(none)* | Reconfigure, incremental build, wipe and re-stage, sign. |
| `--clean` | Delete `build/ios-device-xcode` and `build/ios-stage` first. It does **not** touch `build/ios-deps` (run `fetch-deps.sh device` yourself first; a missing dependency tree surfaces as a CMake configure failure, not a friendly message) and it does **not** touch `build/ios-icons`, so the icon cache survives and the run prints `icons: up to date`. |
| `--resign-only` | Skip the configure and the stage wipe; still runs the incremental `xcodebuild` that refreshes the provisioning profile. Refuses to run if the warm build tree was configured for a different bundle id. |
| `--no-build` | Stage and sign what is already built; does **not** refresh the profile. |
| `--install` | `devicectl device install app`. |
| `--launch` | `devicectl device process launch --console --terminate-existing`. |
| `--device <id>` | Override `CORSIXTH_IOS_DEVICE`. |
| `--push-data <dir>` | Copy a data folder to `Documents/CorsixTH/<basename>` in the container. |

devicectl operations retry three times: a Wi-Fi-paired device drops its tunnel when it
auto-locks, and a locked device refuses launches outright
(`FBSOpenApplicationErrorDomain error 7 … Locked`). Unlock the iPad before `--launch`.

**`--remove-existing-content` is never passed to `devicectl device copy to`.** It wipes the
entire app data container, not the destination path.

## What the pipeline actually does

1. `cmake --preset ios-device-xcode -DCORSIXTH_IOS_BUNDLE_ID=… -DCORSIXTH_IOS_SHORT_VERSION=…
   -DCORSIXTH_IOS_BUNDLE_VERSION=…`
2. `xcodebuild -project build/ios-device-xcode/CorsixTH_Top_Level.xcodeproj -target CorsixTH
   -configuration Release -allowProvisioningUpdates -allowProvisioningDeviceRegistration
   [-destination "platform=iOS,id=$CORSIXTH_IOS_DEVICE_UDID"] DEVELOPMENT_TEAM=…
   CODE_SIGN_STYLE=Automatic PRODUCT_BUNDLE_IDENTIFIER=…` — this signs the bare app and
   mints or refreshes the provisioning profile. Full output goes to
   `build/ios-xcodebuild.log`. **`-allowProvisioningDeviceRegistration` is what lets a team
   with no registered devices produce a profile at all**; without it the first build under a
   fresh team fails with "Your team has no devices from which to generate a provisioning
   profile".
3. `cmake --install build/ios-device-xcode --config Release --prefix build/ios-stage` —
   populates the flat bundle from the tree's existing `install()` rules, because
   `CORSIX_TH_DATADIR` is `CorsixTH.app` on iOS. It also copies the fresh
   `embedded.mobileprovision` out of the Xcode product, so nothing caches a profile.
4. `scripts/build/ios/make-icons.sh` (see below), then the icon files are copied in.
5. A manifest check: binary, `CorsixTH.lua`, `re.lua`, `Info.plist`, `LICENSE.txt`,
   `embedded.mobileprovision`, `Assets.car`, the three loose icon PNGs,
   `CorsixTHUnicode.ttf`, a `.sf2` SoundFont, `Lua/`, `Bitmap/`, `Levels/`, `Campaigns/`,
   `Graphics/`, plus spot checks inside each, and `CFBundleIdentifier` equals the requested
   bundle id. **The script exits non-zero rather than shipping an incomplete bundle.**
6. `codesign -d --entitlements - --xml` on the Xcode product to capture the entitlements
   Xcode itself produced, then `codesign --force --sign "$IDENTITY" --entitlements …
   --timestamp=none build/ios-stage/CorsixTH.app`, then `codesign --verify --deep --strict`.
   The re-sign is not optional: `cmake --install` adds ~360 files after Xcode signed, which
   invalidates the seal.
7. `devicectl` install / `copy to` / `process launch`.

## App icon

`scripts/build/ios/make-icons.sh` builds the icon set from CorsixTH's own artwork; nothing
generated is committed. `scripts/build/ios/composite-icon.swift` does the one thing `sips`
cannot: composite the alpha-bearing source onto an opaque canvas, because **iOS app icons
may not have an alpha channel**. It also over-draws the source by `CORSIXTH_IOS_ICON_INSET`
(5 % per edge) to crop off the artwork's own rounded border, which would otherwise show as a
double frame inside iOS's icon mask. The 1024 px master is rendered once by the compositor
and every other size is a `sips -Z` downscale.

The script emits both icon mechanisms, and the pipeline stages both:

* `Assets.car`, compiled by `actool` from a generated `AppIcon.appiconset`, selected by
  `CFBundleIconName` in `Info.plist` — the modern, correct mechanism.
* Loose `AppIcon60x60@2x.png`, `AppIcon76x76@2x.png`, `AppIcon83.5x83.5@2x.png` in the
  bundle root, named by `CFBundleIcons` / `CFBundleIcons~ipad`. **SpringBoard caches icons
  aggressively for developer-signed installs and does not always read `Assets.car`**; the
  loose PNGs are always honoured. If an icon change still does not appear, bump
  `CORSIXTH_IOS_BUNDLE_VERSION` and, failing that, restart the device.

Icon keys live in `CMake/ios/Info.plist.in`, which remains the single source of truth for
bundle metadata.

## Profile lifetime

A paid Apple Developer team mints 365-day profiles; a free personal team mints 7-day ones.
Check any time with:

```sh
security cms -D -i build/ios-stage/CorsixTH.app/embedded.mobileprovision \
  | plutil -p - | grep -iE "TimeToLive|ExpirationDate|TeamName"
```

When the profile does age out, `scripts/build/ios/package-ios.sh --resign-only --install`
refreshes it; the incremental `xcodebuild` is what talks to Apple.

---

# Reproducing from a clean tree

Both documented paths were re-run from a **deleted** binary directory on the reference host, in
that state, and both are reproduced below verbatim. `build/ios-deps` was deliberately left alone:
rebuilding dependencies is expensive and `fetch-deps.sh` already verifies its own output.

## The app build and package

```
$ time ./scripts/build/ios/package-ios.sh --clean
==> signing environment from .../scripts/build/ios/ios-signing.env
==> team ABCDE12345, bundle id com.example.corsixth, identity Apple Development: Your Name (XXXXXXXXXX)
==> clean: removing .../build/ios-device-xcode and .../build/ios-stage
==> cmake --preset ios-device-xcode
Note: FFmpeg video is disabled
Note: Update check is disabled
Building common libraries
Building CorsixTH
Linking lua modules
==> xcodebuild (signs the bare app and refreshes the provisioning profile)
    Provisioning Profile: "iOS Team Provisioning Profile: *"
** BUILD SUCCEEDED **
==> cmake --install -> .../build/ios-stage
==> app icon
  icons: up to date (.../build/ios-icons)
==> codesign
.../build/ios-stage/CorsixTH.app: replacing existing signature
.../build/ios-stage/CorsixTH.app: valid on disk
.../build/ios-stage/CorsixTH.app: satisfies its Designated Requirement
==> 371 files,  64M in .../build/ios-stage/CorsixTH.app
        30.331 total
```

(The team id, bundle id and identity above are the placeholders this document uses throughout;
the real values come from the git-ignored `scripts/build/ios/ios-signing.env`.)

Two expected messages, neither of them a problem: a CMake warning that you cannot run CorsixTH
*from Xcode* without `-DUSE_SOURCE_DATADIRS` (irrelevant — the product is installed to a device,
not run on the host), and `Cannot locate Doxygen or Lua, 'doc' target is not available`.

Artifact verification of what that produced:

```
$ lipo -info build/ios-stage/CorsixTH.app/CorsixTH
Non-fat file: ... is architecture: arm64

$ vtool -show-build build/ios-stage/CorsixTH.app/CorsixTH
 platform IOS
    minos 16.0
      sdk 26.5

$ otool -L build/ios-stage/CorsixTH.app/CorsixTH | tail -n +2 | grep -v "/System/Library\|/usr/lib"
   (no output: nothing but system frameworks — every dependency is statically linked)

$ codesign --verify --deep --strict --verbose=2 build/ios-stage/CorsixTH.app
build/ios-stage/CorsixTH.app: valid on disk
build/ios-stage/CorsixTH.app: satisfies its Designated Requirement

$ plutil -p build/ios-stage/CorsixTH.app/Info.plist | grep -E "MinimumOSVersion|CFBundleIconName|UIRequiresFullScreen"
  "CFBundleIconName" => "AppIcon"
  "MinimumOSVersion" => "16.0"
  "UIRequiresFullScreen" => true

$ shasum -a 256 build/ios-stage/CorsixTH.app/*.sf2
9575028c7a1f589f5770fccc8cff2734566af40cd26ed836944e9a5152688cfe  .../GeneralUser-GS.sf2
```

## The Ninja developer build

```
$ rm -rf build/ios-device
$ export VCPKG_ROOT=$HOME/vcpkg
$ time (cmake --preset ios-device && cmake --build build/ios-device)
...
[40/40] Linking CXX executable CorsixTH/CorsixTH.app/CorsixTH
ld: warning: ignoring duplicate libraries: '.../build/ios-deps/device/arm64-ios-min16/lib/liblua.a'
        7.282 total

$ lipo -info build/ios-device/CorsixTH/CorsixTH.app/CorsixTH
Non-fat file: ... is architecture: arm64
$ vtool -show-build build/ios-device/CorsixTH/CorsixTH.app/CorsixTH | grep -E "platform|minos"
 platform IOS
    minos 16.0
$ grep -n "CORSIX_TH_OS\|CORSIX_TH_ARCH" build/ios-device/CorsixTH/Src/config.h
110:#define CORSIX_TH_OS "ios"
113:#define CORSIX_TH_ARCH "arm64"
```

The duplicate-`liblua.a` warning is pre-existing and harmless: `CorsixTH/CMakeLists.txt` links
`${LUA_LIBRARIES}`, which lists it twice. It is not iOS-specific.
