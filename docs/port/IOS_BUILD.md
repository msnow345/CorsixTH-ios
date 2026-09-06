# Building CorsixTH for iOS / iPadOS

This document covers **Task 1** of the iOS port: obtaining every native dependency for
`arm64` iOS (device and simulator) and configuring the CorsixTH tree against them.
Compiling, linking, bundling and signing the app itself is covered by later tasks.

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
