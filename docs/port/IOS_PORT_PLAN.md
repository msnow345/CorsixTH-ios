# CorsixTH → iOS / iPadOS port plan

Branch: `ios-port`. Target device: iPad (arm64, iOS 26 installed, deployment target 16.0),
iPhone as a secondary target. Host: macOS arm64, Xcode 26.6, iOS SDK 26.5.

This port follows the methodology in the sibling GeneralsX iOS port
(`../GeneralsX/docs/port/PORTING_PATTERNS.md` and `PORTING_PLAYBOOK.md` relative to this
repo's parent directory — read them for the shell-app pattern, the deferred-tap touch state
machine, and the devicectl crib sheet). Reuse their patterns; do not copy code blindly.

Unlike Generals, CorsixTH needs **no graphics translation layer**: it renders through
`SDL_Renderer`, which is Metal on iOS. The work is dependency cross-building, bundle/filesystem
layout, audio (including MIDI music), touch input, lifecycle, and packaging.

## Context: what the codebase already gives us

- `CorsixTH/Src/th_gfx_sdl.cpp` — SDL3 window + renderer, letterbox/viewport handling,
  `hidpi` support, zoom buffer. No platform-specific rendering code.
- `CorsixTH/Src/sdl_core.cpp:322` — main loop is a blocking `SDL_WaitEvent` pump that
  dispatches to Lua. Every event passes `SDL_ConvertEventToRenderCoordinates`
  (`sdl_core.cpp:326`), so renderer scaling and input coordinates cannot desync.
- `SDL_EVENT_PINCH_BEGIN/UPDATE/END` are already handled (`sdl_core.cpp:396-411`) and wired
  to `App:onPinchBegin/Update/End` (`CorsixTH/Lua/app.lua:68-70`) → `GameUI` zoom.
- `CorsixTH/Src/iso_fs.cpp` reads original game data directly out of `.iso`/`.dmg` images;
  `CorsixTH/Lua/dialogs/resizables/directory_browser.lua` already offers them in the in-game
  data-file browser (`filesystem.lua:84`).
- `CorsixTH/Lua/config_finder.lua` reads a `config.path.txt` sitting next to the Lua tree and
  uses its contents as the config directory. Saves, Levels, Campaigns, Logs and Screenshots
  all default to directories beside `config.txt` (`app.lua:456-500`), so redirecting the
  config path redirects all user-writable state with no code change.
- `CORSIX_TH_SEARCH_LOCAL_DATADIRS` + `libs/whereami` locate `CorsixTH.lua` relative to the
  executable, searching `./`, `CorsixTH/`, `Contents/Resources/`, `../Resources/`
  (`CorsixTH/Src/main.cpp:74-118`). An iOS bundle root satisfies the first entry.
- `CorsixTH/Lua/audio.lua:79` supports a user-supplied waveform music folder
  (`audio_music` config) as an alternative to the original XMI/MIDI tracks.

## Global Constraints

These bind every task. A reviewer treats a violation as a defect.

1. **Branch and commits.** All work on `ios-port` in this repository. One commit per category
   of change (build plumbing / audio / input / lifecycle / packaging); never mix platform
   plumbing with game-logic changes in one commit. Commit messages: conventional-commit style
   (`feat(ios): …`, `fix(ios): …`, `build(ios): …`).
2. **Upstreamability.** Keep the diff minimal and reviewable. Guard iOS-specific C++ with
   `#ifdef CORSIX_TH_IOS` (define it from CMake when `IOS`), or Apple's `TARGET_OS_IOS` where
   a system header is already included. Never wrap game logic in platform conditionals. Never
   reformat untouched code; the repo has `.clang-format` — format only lines you add.
3. **Annotation convention.** Every non-obvious platform change carries a one-line comment
   `// CorsixTH-iOS @<bugfix|feature|build|refactor> <YYYY-MM-DD> <description>`.
4. **No LuaJIT, ever.** iOS forbids JIT. Vanilla Lua only; never set `WITH_LUAJIT`. The Lua
   version follows this repo's own pinned vcpkg baseline, which currently resolves to **5.5.0**
   — the same version every vcpkg desktop build of this tree gets, so it is not an iOS-specific
   divergence. Do not add a version override. Known consequence to document, not to fix:
   CorsixTH's savegame persistence (`CorsixTH/Src/persist_lua.cpp`) writes Lua bytecode, which
   is not portable across Lua versions, so saves may not interchange with a desktop build
   linked against 5.4. iOS-to-iOS saves are unaffected.
5. **No dynamic Lua C modules.** `luafilesystem` and `lpeg` must be statically linked into the
   binary and registered from C (`CorsixTH/Src/main.cpp` already does this under
   `CORSIX_TH_LINK_LUA_MODULES`). Runtime `.so`/`.dylib` module loading does not work in an
   iOS app.
6. **No game assets in the repo, ever.** Original Theme Hospital data is user-supplied. Never
   commit, download, or redistribute it. Test data lives outside the repo.
7. **Artifact verification, not exit codes.** After any build, verify what was produced:
   `lipo -info` for architecture, `otool -L` for link dependencies, `nm`/`strings` for symbols
   you expect. A green build command that silently fell back to a host-arch or stub dependency
   is a failure. State the command and its output in your report.
8. **Deployment target `IPHONEOS_DEPLOYMENT_TARGET=16.0`**, `TARGETED_DEVICE_FAMILY "1,2"`,
   landscape-only, `arm64`.
9. **User data locations.** Read-only game data and the Lua tree live in the signed bundle.
   Writable state (config.txt, hotkeys.txt, Saves, Levels, Campaigns, Logs, Screenshots) lives
   under the app container's `Documents/CorsixTH/` so it is visible in the Files app. Nothing
   writable is ever expected inside the bundle.
10. **Report evidence.** Every report states the exact commands run and their real output.
    Never claim a behaviour you did not observe. If you could not test something on device,
    say so explicitly and say why.
11. **Do not dispatch subagents.** You are the implementer; review comes from the controller.

## Reference environment (already verified on this host)

- `cmake`, `ninja`, `meson`, `pkg-config`, `xcodegen`, `libtool` present via Homebrew.
  `autoconf`/`automake` are **not** installed.
- `VCPKG_ROOT=$HOME/vcpkg` exists.
- Test devices (both paired to this host, iOS 26/"27.0"):
  - **iPad Pro 11-inch (M5)** (`iPad17,1`) — hardware UDID `00008XXX-XXXXXXXXXXXXXXXX`,
    devicectl identifier `AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE`. 2420x1668 native @2x
    (1210x834 pt). Primary target.
  - **iPhone 17 Pro Max** (`iPhone18,2`) — devicectl identifier
    `11111111-2222-3333-4444-555555555555`, hardware UDID `00008YYY-YYYYYYYYYYYYYYYY`.
    2868x1320 native @3x (956x440 pt) — this is the device that exercises the UI-scale cap
    edge case in Task 6 (1320/480 = 2.75, so the cap is 2 while the display scale is 3).
  - Physical device availability depends on the user having it plugged in; ask via the
    controller rather than assuming, and say in your report which device you actually tested on.
- A code-signing identity for automatic signing exists on this host; discover it with
  `security find-identity -v -p codesigning` (the value in parentheses is the team id) rather
  than hard-coding one. The controller will also supply it in your dispatch.
- Simulators available (iPad Pro 11-inch (M5) 26.5, iPad Air, iPad mini) for fast iteration.
- Build outputs go under `build/ios*` (already git-ignored patterns: verify and extend
  `.gitignore` if not).

---

## Task 1: iOS dependency toolchain

Produce a reproducible way to obtain every native dependency CorsixTH needs, built for
`arm64` iOS device **and** `arm64` iOS simulator, plus a CMake preset that configures the
CorsixTH tree against them.

### Dependencies required for the v1 iOS build

Needed: `SDL3`, `SDL3_mixer`, `Lua 5.4`, `luafilesystem`, `lpeg`, `freetype`, `libpng`, `zlib`.

Explicitly **not** needed for v1 (leave the corresponding CMake options OFF so their
`find_package` calls never run): `ffmpeg` (`WITH_MOVIES=OFF`), `curl`
(`WITH_UPDATE_CHECK=OFF`), `rtmidi` (`WITH_MIDI_DEVICE=OFF`), `wxWidgets` (AnimView),
`catch2`, `tracy`. `fluidsynth` is handled in Task 3, not here — but if your chosen
dependency mechanism can produce a working iOS `fluidsynth`, note that in your report, since
it decides Task 3's approach.

### Critical constraint: SDL3 version

`CorsixTH/Src/sdl_core.cpp` uses `SDL_EVENT_PINCH_BEGIN`, `SDL_EVENT_PINCH_UPDATE`,
`SDL_EVENT_PINCH_END` and `e.pinch.scale`; `th_gfx_sdl.cpp` references behaviour from
"SDL 3.6.10". Whatever SDL3 you obtain **must** export those symbols. Verify by grepping the
installed `SDL3/SDL_events.h` for `SDL_EVENT_PINCH_BEGIN` before declaring the dependency
satisfied. If a released/packaged SDL3 lacks them, build SDL3 from source at a pinned commit
that has them and record the exact commit SHA in your report.

`SDL3_mixer` must be the version matching your SDL3 (the tree currently references
`release-3.2.4` under `FETCH_SDL_MIXER`); if that tag is incompatible with the SDL3 revision
you pin, pin a compatible `SDL_mixer` commit instead and record it.

### Approach — decide empirically, in this order

1. **Try vcpkg `arm64-ios` first** (timebox ~45 minutes of wall clock). `VCPKG_ROOT` is set.
   This is the preferred outcome because `CORSIX_TH_LINK_LUA_MODULES` is gated on
   `VCPKG_TARGET_TRIPLET` (`CorsixTH/CMakeLists.txt:139`), so a vcpkg build satisfies
   constraint 5 with no CMake change, and because it may also give us `fluidsynth`.
   Test with the actual manifest features this port needs.
2. **If vcpkg `arm64-ios` cannot produce the set** (missing ports, patches needed, host-arch
   leakage), fall back to a **CMake superbuild** using `FetchContent`/`ExternalProject` with
   every dependency pinned to an exact tag or SHA, driven by an `ios-deps` CMake project under
   `CMake/ios/` or a script under `scripts/build/ios/`. `zlib` and `libpng` may come from the
   iOS SDK (`libz` is in the SDK; `libpng` is not) — check rather than assume. `luafilesystem`
   and `lpeg` ship as plain C with no usable CMake build; write minimal `CMakeLists.txt`
   wrappers for them under `CMake/ios/` producing static libs plus the
   `unofficial::luafilesystem::lfs` / `unofficial::lpeg::lpeg` targets the tree expects, or
   provide equivalent targets and note the divergence for Task 2.
   In this branch you must also arrange for `CORSIX_TH_LINK_LUA_MODULES` to be ON for iOS —
   propose the change but leave the CMakeLists edit to Task 2 if it is cleaner there; say which
   you did.

Do not spend time on both approaches once one works. Record the decision, the reason, and the
exact versions/SHAs in your report — Task 2 and Task 3 depend on knowing them.

### Deliverables

- A documented, repeatable command sequence (script under `scripts/build/ios/` preferred, e.g.
  `scripts/build/ios/fetch-deps.sh`) that produces the dependency set from a clean checkout.
- A `CMakePresets.json` configure preset named `ios-device` (and `ios-simulator` if it costs
  little) added alongside the existing presets, setting the iOS system name, arm64 architecture,
  deployment target 16.0, `WITH_MOVIES=OFF`, `WITH_UPDATE_CHECK=OFF`, `WITH_MIDI_DEVICE=OFF`,
  `BUILD_ANIMVIEW=OFF`, `ENABLE_UNIT_TESTS=OFF`, `SEARCH_LOCAL_DATADIRS=ON`, and whatever
  toolchain/triplet wiring your chosen approach needs. Do not disturb the existing presets.
- Notes in `docs/port/IOS_BUILD.md` (create it) covering prerequisites and the command sequence.

### Acceptance

- `cmake --preset ios-device` configures the CorsixTH tree to completion with no missing
  dependency. Compilation/link of the game itself is Task 2's gate — configure-only is enough
  here, but if configure succeeds it is worth running the build to see how far it gets and
  reporting the first real compile error for Task 2.
- Every produced static library verified `arm64` and iOS-platform via `lipo -info` and
  `otool -l | grep -A3 LC_BUILD_VERSION` (or `vtool -show`), with the output quoted in the report.
- Installed `SDL3/SDL_events.h` contains `SDL_EVENT_PINCH_BEGIN` (quote the grep).

---

## Task 2: Compile, link and launch an iOS app bundle

Take the configured tree from Task 1 to a signed, launching `.app` that reaches CorsixTH's
"cannot find game data" state on device — i.e. the engine initialises, SDL creates a Metal
renderer, Lua boots, and the game asks for its data files.

### CMake work

- Add an iOS branch to the build. Where the tree branches on `APPLE`, it currently assumes
  macOS bundle layout (`CorsixTH/CMakeLists.txt:40-43` sets
  `CORSIX_TH_DATADIR CorsixTH.app/Contents/Resources/`). For iOS the Lua tree and data live at
  the **bundle root**, so `CORSIX_TH_DATADIR` and `CORSIX_TH_INTERPRETER_PATH` must reflect a
  flat bundle. Prefer `if(IOS)` branches over editing the macOS paths.
- Define `CORSIX_TH_IOS` (compile definition) when `IOS`, for use by later tasks.
- Ensure `CORSIX_TH_LINK_LUA_MODULES` is ON for iOS regardless of `VCPKG_TARGET_TRIPLET`
  (constraint 5), keeping the existing behaviour for other platforms untouched.
- Build the game as an iOS application target producing `CorsixTH.app` with a valid
  `Info.plist`, or — if that fights CMake's iOS support — build a static/`MACOSX_BUNDLE`-free
  binary and let Task 4's shell-app packaging assemble the bundle (this is the GeneralsX
  pattern: CMake builds the binary, XcodeGen builds a signing shell, a script splices them).
  **Decide and state which**; Task 4 builds on your choice. The GeneralsX split is the known-good
  route and is recommended unless CMake's iOS app target works cleanly on the first try.
- `RtMidi`, `curl`, `ffmpeg` and AnimView paths must not be reached; verify by reading the
  configure output.

### Source work expected (keep minimal)

- Whatever compile errors arise from the iOS SDK. Expect issues around: `std::filesystem`
  availability (fine on iOS 16), `SDL_main` (already included via
  `CorsixTH/SrcUnshared/main.cpp:28` — SDL3 provides the UIKit entry point, so `main()` should
  need no change; confirm rather than assume), and any POSIX-only calls.
- Do **not** implement touch, scaling, lifecycle or audio-session work here. Those are Tasks
  3, 6, 7, 8. Stay in scope.

### Bundle content (minimum for this task)

Stage into the bundle root: `CorsixTH.lua`, `Lua/`, `Bitmap/`, `Levels/`, `Campaigns/`,
`Graphics/`, and a Unicode TTF (use `FETCH_UNICODE_FONT=ON`'s GoNoto, or stage an equivalent —
`freetype` needs a real font file for non-Latin text and the config's `unicode_font`). Lua
files must keep their relative directory structure; `config_finder.lua` derives the bundle root
by stripping `/Lua/config_finder.lua` from its own source path
(`CorsixTH/Lua/config_finder.lua:49`), so `Lua/` must sit directly under the bundle root.

### Signing / install

Use automatic signing. Discover the development team from the host (`security find-identity -v
-p codesigning`, or the GeneralsX approach in
`../GeneralsX/ios/project.yml` and `../GeneralsX/scripts/build/ios/package-ios-zh.sh`).
Bundle id: `com.example.corsixth` unless a conflict forces otherwise. Install to the connected
iPad with `xcrun devicectl device install app --device 00008XXX-XXXXXXXXXXXXXXXX <abs path>`
and launch with `xcrun devicectl device process launch --console --device <udid> <bundle-id>`.

Read the GeneralsX playbook's devicectl crib sheet before using devicectl.
**Never** use `xcrun devicectl device copy to --remove-existing-content true` — it wipes the
entire app data container.

### Acceptance

- `.app` built, `lipo -info` shows arm64 only, `otool -L` shows no dependency on Homebrew or
  other host-only paths (quote both).
- App installs and launches on the iPad. Capture the console output of the launch. The gate is:
  SDL initialises, a window/renderer is created (log the renderer name — expect `metal`), Lua
  boots, and the game reaches its missing-data-files path rather than crashing. A crash log or
  the exact failure output is an acceptable *report*, but not an acceptable *result*: iterate
  until it launches.

---

## Task 3: Audio — complete fidelity (sound effects, speech, and MIDI music)

**This is the highest-priority subsystem for this port.** The requirement is that everything the
desktop build plays, the iOS build plays: sound effects, announcer/character speech, and the
original in-game music. "Music can come later" is not an acceptable outcome.

### What CorsixTH's audio actually consists of

- **Sound effects and speech** come out of the original data (`SOUND-0.DAT`) and are played as
  SDL_mixer chunks — see `CorsixTH/Src/th_sound.cpp`, `CorsixTH/Src/sdl_audio.cpp`,
  `CorsixTH/Lua/audio.lua`. No extra dependency beyond `SDL3_mixer`.
- **Music** is the hard part. The original tracks are XMI (MIDI) files;
  `CorsixTH/Src/xmi2mid.cpp` converts XMI → standard MIDI, and the result is handed to
  SDL_mixer as music. Playing MIDI requires a *synthesiser*. `CorsixTH/Src/sdl_audio.cpp:94`
  sets the SDL_mixer property `SDL_mixer.decoder.fluidsynth.soundfont_path`, i.e. upstream
  expects SDL_mixer's **fluidsynth** decoder plus a SoundFont.
- **Optional waveform music**: `CorsixTH/Lua/audio.lua:79` (`audio_music` / `audio_mp3` config)
  plays a folder of OGG/MP3 tracks instead. This is a *user preference*, not a substitute for
  the MIDI path, and must keep working.

### Known iOS-specific audio hazard — fix this regardless of synth choice

SDL's default iOS audio session category respects the hardware mute switch
(`SDL_HINT_AUDIO_CATEGORY` defaults to the ambient/soloambient behaviour). A game shipped
without setting this is silent for any user with the mute switch on, which reads as "audio is
broken". Set the playback category for iOS (via `SDL_HINT_AUDIO_CATEGORY=playback`, set before
`SDL_Init` of the audio subsystem, or the equivalent SDL3 hint name — check the SDL3 headers you
pinned) and verify on device with the mute switch **on**.

Also handle: audio interruption (incoming call / Siri) and route change (headphones
plugged/unplugged) without losing audio permanently. SDL3 surfaces device-removed/added events;
at minimum the game must not end up permanently silent after an interruption.

### MIDI synthesis — approach, in preference order

1. **SDL_mixer's fluidsynth decoder** with a bundled SoundFont, if `fluidsynth` can be built
   for `arm64-ios`. This is zero-code-change and matches upstream. Note that fluidsynth 2.x
   depends on **glib**, which is the risk; check whether the dependency mechanism chosen in
   Task 1 can produce both.
2. **A glib-free SoundFont synthesiser wired in behind CorsixTH's existing music API.**
   Candidates: FluidLite (fluidsynth-1.x-derived, no glib, retains a `fluid_player` MIDI
   sequencer, SF2 and optionally SF3) or TinySoundFont + TinyMidiLoader (two permissive
   header-only C files, SF2, simple to integrate). Implement it as a music source feeding
   SDL_mixer/SDL audio, keeping the Lua-facing API in `audio.lua` and the
   `SDL.audio.playMusic/pauseMusic/resumeMusic/stopMusic` surface (`CorsixTH/Lua/audio.lua:628-759`)
   unchanged, so no Lua changes are needed.
3. **iOS AudioToolbox (`MusicPlayer`/`MusicSequence`) using the system General MIDI synth** —
   no SoundFont to bundle, small code, but a separate output path from SDL audio, so volume,
   pause/resume and mixing must be wired by hand.

Pick the highest option that actually works and say why the ones above it did not. The
acceptance gate is behavioural, not architectural.

### SoundFont

If your approach needs one, bundle it. `CorsixTH/CMakeLists.txt:279-288` already has a
`FETCH_SOUNDFONT` path fetching `FluidR3.sf3` (~30 MB compressed SF3, permissively licensed) —
prefer reusing that mechanism. If your synth cannot read SF3, choose a permissively licensed
SF2 and record the licence and its size; a bundled SoundFont must be redistributable (unlike
game data, a SoundFont with a suitable licence may ship in the app bundle, but must **not** be
committed to this repository — fetch it at build time).

### Deliverables

- Working sound effects, speech and MIDI music on device, with the mute switch on.
- Bundled SoundFont (fetched at build time, not committed) if required.
- Music volume, pause/resume, track advance and the in-game jukebox
  (`CorsixTH/Lua/dialogs/`, audio settings) all functional — the jukebox exercises the music
  API surface, so use it as your test.
- `docs/port/IOS_AUDIO.md` recording the synth decision, the rejected options and why, the
  soundfont provenance/licence, and how to verify audio on device.

### Acceptance (behavioural, on the physical iPad)

State explicitly, per item, that you observed it on device:
1. Menu/UI sound effects audible.
2. In-game speech/announcements audible.
3. Original in-game music audible, playing the game's own XMI tracks (not a substitute
   waveform pack) — name the track you heard.
4. All of the above with the hardware mute switch ON.
5. Music pause/resume and next-track via the in-game controls work.
6. An interruption (start a phone timer alarm or a Siri invocation, or unplug/replug
   headphones) does not leave the game permanently silent.
7. No audio thread crash or assertion after 10 minutes of play.

If some item is not verifiable in your environment, say which and why rather than claiming it.

---

## Task 4: Shell app, bundle layout, packaging and signing pipeline

Turn Task 2's ad-hoc build into a repeatable one-command package-and-install pipeline, matching
the GeneralsX shell-app pattern (`../GeneralsX/ios/project.yml`,
`../GeneralsX/scripts/build/ios/package-ios-zh.sh` — read both before starting).

### Deliverables

- `ios/project.yml` — XcodeGen spec for a `CorsixTH` iOS app target: automatic signing,
  `DEVELOPMENT_TEAM` and `PRODUCT_BUNDLE_IDENTIFIER` overridable by environment variable
  (`CTH_TEAM_ID`, `CTH_BUNDLE_ID`) so the committed file carries no hard-coded personal team,
  `TARGETED_DEVICE_FAMILY "1,2"`, deployment target 16.0, and an `Info.plist` with:
  `UIFileSharingEnabled` + `LSSupportsOpeningDocumentsInPlace` (so users can drop game data in
  via the Files app), landscape-left/right only, `UIRequiresFullScreen`, `UIStatusBarHidden`,
  `CADisableMinimumFrameDurationOnPhone` (for 120 Hz in Task 6),
  `UIApplicationSupportsIndirectInputEvents`, and an app icon.
- App icon: generate from `CorsixTH/CorsixTH.ico` or `CorsixTH/Icon.icns` composited onto an
  opaque background (iOS icons cannot have alpha). Read the GeneralsX playbook §5 note on
  SpringBoard icon caching before debugging a missing icon.
- `scripts/build/ios/package-ios.sh` — builds/assembles the app: stage bundle resources
  (Lua tree, Bitmap, Levels, Campaigns, Graphics, font, SoundFont, `config.path.txt`), splice
  in the engine binary if using the shell pattern, sign inside-out, and support
  `--install` (install to a device via devicectl) and `--device <udid>`.
  Fail loudly on a missing required input; never silently ship an incomplete bundle.
- **Do not** ship a `config.path.txt` with a hard-coded container path. The iOS data container
  path contains a per-install UUID (`/var/mobile/Containers/Data/Application/<UUID>/`) that
  changes on every reinstall, so a static file baked into the read-only bundle would go stale
  immediately. Task 5 owns the correct mechanism; stage nothing for it here.
- Extend `docs/port/IOS_BUILD.md` with the full build → package → install sequence.

### Acceptance

- From a clean `build/` directory, the documented sequence produces a signed `CorsixTH.app`
  and installs it on the iPad in one command, with the output quoted in the report.
- `codesign -dv --verbose=4` on the packaged app shows a valid signature and the expected
  bundle id; entitlements present. Quote it.
- The bundle contains every staged resource — verify with `find` on the built `.app` and quote
  the listing (or a `wc -l` plus spot checks of the critical paths).
- No hard-coded personal team id or bundle id in committed files.

---

## Task 5: Game-data ingest from the Files app

A user with their own copy of Theme Hospital must be able to get it into the app and play,
without a Mac. Two supported shapes: (a) copying the original `HOSP`/`ThemeHospital` folder
into the app's Documents via the Files app, and (b) dropping a `.iso` of the original CD there
and pointing the game at it (`iso_fs.cpp` reads ISO9660 directly).

### Work

- Writable-state layout per constraint 9: `Documents/CorsixTH/` holds `config.txt`,
  `hotkeys.txt`, `Saves/`, `Levels/`, `Campaigns/`, `Logs/`, `Screenshots/`.

  **How the engine already handles this — understand it before changing anything.** There is
  no save-path rewriting to do; the indirection exists:
  - `CorsixTH/CorsixTH.lua:102` injects `--config-file=<config_finder.config_filename>` into
    the command line at startup, so `App:getConfigPath()` (`app.lua:456`) returns an
    **absolute** path, not the bare `"config.txt"` its fallback suggests.
  - `getDefaultSavegameDir`, `getDefaultScreenshotsDir` and `initUserDirectories`
    (`app.lua:459-500`) all derive from that absolute path by stripping the filename. So
    everything writable follows the config file's directory automatically.
  - `config_finder.find_config()` (`config_finder.lua:60-111`) picks that directory, and its
    `check_dir_exists` helper **recursively creates** it (`check_dir_exists(subpath) and
    lfs.mkdir(path)`), so nested creation on a first-run empty Documents is already handled.

  **The iOS problem and the fix.** The default on non-Windows is
  `XDG_CONFIG_HOME or $HOME/.config` + `/CorsixTH`. On iOS `$HOME` is the app's data container
  root, so that path *is* writable and saving would work — but `.config` is a hidden directory
  outside `Documents/`, therefore invisible over Files sharing, which defeats constraint 9 and
  leaves users unable to reach their own saves.

  The bundle-root `config.path.txt` mechanism (`config_finder.lua:76-89`) cannot solve this:
  the container path contains a per-install UUID that changes on every reinstall, and the
  bundle is read-only so nothing can rewrite the file at runtime.

  So make the smallest possible change: an iOS-guarded branch in `find_config()` that uses
  `$HOME/Documents/CorsixTH` — resolved from the environment at runtime, so it is
  UUID-independent. Keep desktop behaviour byte-identical. **Verify empirically** what `$HOME`
  actually is inside the running app (log it) rather than assuming; report the real value.

- **Watch the silent fallback.** If `check_dir_exists` fails, `find_config` falls back to
  `ourpath` — the read-only bundle root (`config_finder.lua:108-110`). That would leave the
  game apparently running but unable to save anything, with no error. Confirm the fallback is
  not being taken on device (log the chosen config path at startup), and consider whether an
  iOS build should fail loudly instead of falling back to an unwritable location.
- Make the game's own data-file browser usable on iOS: it must start somewhere sensible
  (the app's Documents directory, not `/`) so a touch user can reach a dropped folder or ISO in
  a few taps. Check `CorsixTH/Lua/dialogs/resizables/directory_browser.lua` and the
  first-run/"missing data" flow, and set an iOS-appropriate starting path. Prefer a
  config/Lua-level change over new C++.
- The predictable-locations sweep in `app.lua:1470-1505` is desktop-shaped; add the iOS
  container's `Documents` (and `Documents/HOSP`, `Documents/ThemeHospital`) so a
  conventionally-named drop is found with zero user interaction. Keep the change additive and
  platform-guarded so desktop behaviour is unchanged.
- Drop a short `README-iOS.txt` (or equivalent) into `Documents/CorsixTH/` on first run
  explaining where to put game data — the Files app is the only surface a phone-only user has.
  Keep it to a few lines.

### Acceptance (on device)

- With no data present, the app launches and presents its missing-data flow rather than
  crashing, and the browser opens in a directory the user can actually navigate from.
- Copy a real Theme Hospital data folder into the app's Documents via the Files app (or via
  `xcrun devicectl device copy to` — **never** with `--remove-existing-content true`), relaunch,
  and the game loads graphics, sound and the main menu. State how you supplied the data.
- Repeat with a `.iso` of the original disc if one is available; if not available, say so and
  verify the ISO code path as far as you can (e.g. that the browser offers `.iso` files).
- `config.txt` and a savegame are written under `Documents/CorsixTH/` and both are visible over
  Files sharing (verify with `xcrun devicectl device info files --domain-type appDataContainer`).
- Saves survive an app relaunch.

---

## Task 6: Display, scaling and frame rate

Make the game legible and correctly proportioned on iPad and iPhone panels.

### Use the engine's existing UI scale — do not build a new one

**CorsixTH already has a full UI scaling system.** Do not invent one, and do not reach for
renderer logical presentation to fake it:

- `ui_scale` (config, `0` = automatic) — pixel ratio for UI elements; runtime-capped to the
  largest integer that still fits "assuming an original size of 640x480"
  (`CorsixTH/Lua/config_finder.lua:130`, docs at `:322-329`).
- `cursor_scale` (`0` = follow UI scale) — `config_finder.lua:131`, applied at
  `CorsixTH/Lua/graphics.lua:260`.
- `apply_window_display_scale` (default `true`) — automatically applies the window's display
  scale to UI and cursor (`config_finder.lua:171`, `graphics.lua:72,487-514`), driven by
  `SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED`, which the event pump already handles
  (`CorsixTH/Src/sdl_core.cpp:452` → `App:onWindowDisplayScaleChanged`, `app.lua:62`).
- `debug_fractional_scaling` — allows non-integer scale factors together with
  `apply_window_display_scale` (`config_finder.lua:752-756`).
- Fonts are **re-rendered** at the UI scale rather than stretched
  (`graphics.lua:543,585,613-680` — `apply_ui_scale` font option), so text stays sharp at scale.

The consequence, and the target configuration for this port: **render at the panel's full native
resolution** (`hidpi = true`) and let the UI scale size the interface. The isometric map, sprites
and text then draw at native 4K-class sharpness while the UI panels sit at the right physical
size. This is strictly better than rendering the whole frame at half resolution.

`App.MIN_WINDOW_WIDTH/HEIGHT = 640x480` (`CorsixTH/Lua/app.lua:38`) is the floor the scale cap
is computed against, and it works out for every device we care about:

- **iPad Pro 13"** — 2752×2064 native. Display scale 2 → `ui_scale` 2; cap allows up to 4.
- **iPad (A16)** — 2360×1640 native, scale 2 → `ui_scale` 2, cap 3.
- **iPhone 15 Pro-class** — 2556×1179 native landscape. 1179/480 = 2.45, so the cap is 2, and
  display scale 3 would be clamped to 2. Verify this clamp actually happens rather than
  producing a UI that overflows the screen; that is the one edge case in the existing logic
  most likely to misbehave on iOS.

### Work

- Make iOS default to `hidpi = true` and rely on `apply_window_display_scale` for the UI size.
  Verify the automatic path end to end on device before touching any scaling code: read what
  `render_target::getWindowDisplayScale` returns on iOS, what `graphics.lua:_adjustWindowDisplayScale`
  does with it, and what `getUIScale` settles on. Report the three numbers.
- Only if the automatic result is wrong (UI overflowing on iPhone, or too small/large to use)
  adjust it — preferring a minimal, platform-guarded change to the existing cap/auto logic over
  any new mechanism. Say exactly what you changed and why the existing logic was insufficient.
- Check the very high-density case: if `ui_scale` 2 on a 13" iPad leaves the UI too small for
  comfortable touch, `ui_scale` 3 is within the cap — decide from how it actually looks and
  reaches, and make the iOS default match. Touch targets should be around 44 pt.
- Respect **safe-area insets**: the notch/Dynamic Island and the home-indicator strip must not
  cover interactive UI. SDL3 exposes safe-area information (`SDL_GetWindowSafeArea` or the
  equivalent in your pinned SDL3); inset the render area accordingly rather than letting the
  game draw under the cutouts.
- Fullscreen/orientation: the app is landscape-only and full screen. Verify no window
  decoration assumptions leak through (`th_gfx_sdl.cpp:578-625` branches on
  `SDL_WINDOW_FULLSCREEN`).
### Frame rate: 120 Hz on ProMotion

Read the loop before changing anything. The architecture is already decoupled, unlike the
GeneralsX case:

- `usertick_period_ms = 18` (`CorsixTH/Src/lua_sdl.h:43`) drives an `SDL_AddTimer`
  (`sdl_core.cpp:302`) that posts `SDL_USEREVENT_TICK` → `"timer"` → `App:onTick`
  (`app.lua:1290`) → `World:onTick` + `UI:onTick`. **All** simulation, sprite animation and
  camera scrolling advance there, at ~55.6 Hz.
- `"frame"` → `App:drawFrame` (`app.lua:1330`) only renders: `video:startFrame()`,
  `ui:draw()`, `video:endFrame()`. It advances no game state. So drawing more often cannot
  change game speed. (The one exception is `moviePlayer:refresh()` inside `drawFrame` — movies
  are off for v1, but if they are ever enabled, check the movie clock is timestamp-based.)
- `App:onTick` returns `true` unconditionally ("tick events always result in a repaint"), and
  with `limit_fps = true` (the default, `sdl_core.cpp:82`) a frame is drawn only when something
  requests one. **The effective frame-rate ceiling today is therefore the tick rate, ~55 fps** —
  not vsync, not the panel.
- `limit_fps = false` renders in a tight `while (!SDL_PollEvent(nullptr))` busy loop
  (`sdl_core.cpp:542`). That is benchmark mode. **Never ship it on a battery-powered device.**

So reaching 120 Hz means requesting frames independently of the 18 ms tick, and it only buys
anything for continuously-moving things — the camera. Character animation is fixed-frame 1997
sprite data advancing every 18 ms; drawing it twice as often shows the same image twice.

Work:

- Enable up to 120 Hz on ProMotion panels (`CADisableMinimumFrameDurationOnPhone` from Task 4
  plus whatever the SDL renderer needs). Keep the renderer's vsync present
  (`th_gfx_sdl.cpp:527` — `present_immediate ? 0 : 1`); present-on-vsync at 120 Hz is the goal,
  never a spin loop.
- Drive rendered frames at display rate while leaving the tick at 18 ms. Keep the change small
  and platform-guarded; do not restructure the loop.
- Coordinate with Task 7: the payoff is that camera pan/pinch/inertia update **per rendered
  frame** from touch deltas rather than per tick. Note that today momentum decays per tick
  (`game_ui.lua:onTick`) and `scrollMap` steps in whole pixels carrying the fraction over
  (`game_ui.lua:597-605`), which at native iPad resolution may read as slight stepping —
  sub-pixel camera offset is the fix if it visibly does.
- **Verify game speed is unaffected** despite the architecture making it structurally safe:
  measure in-game clock advance over a fixed wall-clock interval at the capped and uncapped
  rates and show the numbers. Structural safety is not evidence.
- Handle rotation between landscape-left and landscape-right cleanly (the window-resize path
  already exists, `sdl_core.cpp:424-457`).

### Acceptance (on device)

- iPad: main menu and in-game UI correctly proportioned, no letterbox artefacts, no UI under
  the home indicator. Screenshot it (`xcrun devicectl` or the device's own screenshot pulled
  back) and describe what you see. Confirm the map is rendering at native resolution (not
  upscaled) — a screenshot at full panel resolution with crisp sprite edges is the evidence.
- The chosen `ui_scale`/`cursor_scale`/display-scale values reported, and whether they came from
  the automatic path or an override.
- iPhone (if the offline iPhone can be brought online; otherwise use an iPhone simulator and
  say so): UI legible and fully on screen, nothing clipped, nothing under the notch.
- Rotating the device 180° keeps everything correct.
- Game speed unchanged between capped and uncapped frame rates — state how you measured it
  (e.g. in-game clock advance over a wall-clock interval at each setting).

---

## Task 7: Touch input

CorsixTH is mouse-driven. Give it a touch layer that feels like an iOS app, following the
GeneralsX deferred-tap architecture (read `PORTING_PLAYBOOK.md` §6 first — the state machine and
its rationale transfer directly, the gesture *mapping* does not).

### Architecture

Translate touches into synthetic SDL mouse events injected through the same path real mouse
events take, so the game and all 298 Lua UI files stay unchanged. Keep the implementation in one
place, iOS-guarded (`CORSIX_TH_IOS`), close to the event pump in `CorsixTH/Src/sdl_core.cpp`.

Requirements carried over from the GeneralsX port, each of which was a real bug there:

- **On finger-down, emit nothing.** Commit only when the gesture identifies itself. A premature
  button-down that is later "cancelled" is still a real click to the game.
- Finger up while still pending → a full click: motion + button-down + button-up **at the
  original touch point** (dense UI buttons miss otherwise).
- Movement past a dead zone (~8 pt) → drag.
- Held still (~600 ms) → long-press. **Long-press must be polled from the frame loop**, since a
  stationary finger generates no events.
- Synthetic events must carry a valid `windowID`, or coordinate scaling is silently skipped.
- Drop any event with `which == SDL_TOUCH_MOUSEID` in the engine loop so SDL's own
  touch-as-mouse emulation cannot double-deliver.

### Gesture mapping for CorsixTH specifically

- **Tap** → left click at the press point.
- **Long-press** → right click (`CorsixTH/Lua/game_ui.lua:686,742` uses right-click in game).
- **Two fingers → pan and pinch-zoom, always, in every mode.** This is the primary navigation
  gesture and the one that must feel native:
  - Pan follows the two-finger **centroid** 1:1 — the ground under the fingers stays under the
    fingers.
  - Pinch zooms, anchored between the fingers, continuous (not ratcheted).
  - **Pan and zoom must work simultaneously**, not be locked to whichever moved first. The
    GeneralsX port hit exactly this: the engine locked the gesture on first movement, so you
    could not pinch mid-drag without lifting. Require zoom to out-pace the pan before it
    engages, so a straight two-finger pan does not leak zoom, but once engaged both apply in
    the same frame.
  - A flick coasts and a touch catches it; measure release velocity over a real ~60 ms window
    from timestamped samples, not a per-frame filter (people ease off as they lift, and a
    filter turns a genuine flick into a stop). `scrolling_momentum` already exists as a config
    value (`game_ui.lua:100`) — drive it or match its feel; UIScrollView's deceleration rate
    (0.998/ms) is the reference.
- **One-finger drag on the map** → also pans, 1:1, with the same inertia. CorsixTH has no
  drag-box selection, so one-finger panning is free to exist and is the fastest way to nudge the
  view — but it is the gesture that yields when a mode needs it (below). Two-finger pan is the
  one that always works.
- **Pinch plumbing:** pinch is already implemented in Lua via the SDL pinch events. Verify it
  fires from real touches on device (SDL's iOS backend must actually synthesise
  `SDL_EVENT_PINCH_*` from touches — if it does not, generate the pinch events, or drive zoom
  directly, from the touch layer) and retune `game_ui.lua:34`'s pinch factor for fingers rather
  than a trackpad.
- **Drag on lists, dropdowns and scrollable panels** → scroll them directly. Scrollbar thumbs
  are a few points wide at these resolutions; dragging the content is the only usable gesture.
  Prefer a Lua-side change in the relevant widgets over faking wheel events if that is cleaner.
- **The load-bearing conflict: room building and object placement are drag-to-size /
  drag-to-place operations on the map**, which collides with one-finger map pan. Resolution:
  because two-finger pan/zoom is always available, **one-finger drag belongs to the active mode**
  — during room sizing, object placement or staff placement, a one-finger drag places/sizes and
  never pans; outside those modes it pans. Read how GeneralsX solved the equivalent structure-
  placement problem, then design for CorsixTH's own placement flow rather than copying it.
  Edge-scroll while carrying an object (drag toward the screen edge and the map scrolls, the
  carried thing staying under the finger) is a strong addition if the placement flow allows it.
- **Text entry** (hospital name, save names) → bring up the on-screen keyboard via
  `SDL_StartTextInput` when a text field takes focus, and hide it on blur. The Lua UI already
  has text-entry widgets and the engine already dispatches `SDL_EVENT_TEXT_INPUT`
  (`sdl_core.cpp:348`); the missing piece is showing the keyboard.

### Acceptance (on device, all of it hands-on)

State, per item, that you performed it on the iPad:
1. Every main-menu and in-game toolbar button can be hit reliably on the first tap.
2. Map pans 1:1 under a dragging finger — verified with **both** one finger and two — and a
   flick coasts and can be caught.
3. Pinch zooms smoothly, anchored between the fingers, without ratcheting; and **panning and
   zooming at the same time with two fingers works** — you can pinch mid-drag without lifting,
   and a straight two-finger pan does not drift the zoom.
4. Two-finger pan and pinch work while a room/object placement mode is active, with one finger
   still driving the placement.
5. Long-press produces the game's right-click behaviour.
6. A room can be built end-to-end by touch: pick it, size/place it, place its objects, confirm.
7. Hiring/placing staff and picking up/moving an object all work.
8. A long list (e.g. staff management, save/load list) scrolls by dragging, and a tap in it
   still selects.
9. Naming a hospital or a savegame brings up the keyboard and the text lands.
10. A two-finger gesture never leaves a stray click behind (no accidental selections/placements).

---

## Task 8: App lifecycle

An iOS app is suspended and resumed constantly, and can be killed without warning.

### Work

- Register an `SDL_AddEventWatch` (a watcher, not a poll — these can arrive after the loop
  stops) for `SDL_EVENT_WILL_ENTER_BACKGROUND`, `SDL_EVENT_DID_ENTER_BACKGROUND`,
  `SDL_EVENT_WILL_ENTER_FOREGROUND`, `SDL_EVENT_DID_ENTER_FOREGROUND`.
- While backgrounded, skip **simulation and presentation**. GPU work around suspension queues
  drawable-acquire timeouts that read as multi-second input hangs after resume (the GeneralsX
  port's exact failure). CorsixTH's loop is `SDL_WaitEvent`-driven with an `SDL_AddTimer` tick,
  so the tick handler and the render path both need gating.
- Pause audio on background, resume on foreground (mirror the existing focus-lost/gained
  handling at `sdl_core.cpp:412-423`, which already maps to Lua's `onWindowActiveEvent`).
- **Autosave on suspend**, so a memory kill during a long session is not silent data loss.
  CorsixTH has an autosave concept in Lua — reuse it rather than inventing a save path, and
  keep it fast: iOS gives a background transition very little time. State what the time budget
  turned out to be.
- Ensure a background→foreground cycle mid-game does not corrupt the tick clock (a large
  elapsed-time jump must not fast-forward the simulation).

### Acceptance (on device)

1. Home-swipe out of an in-progress game and back in, 10 times, no crash, no hang, no input
   lag on resume.
2. Audio stops on background and resumes cleanly.
3. Kill the app from the app switcher mid-game, relaunch, and the autosave is present and
   loadable.
4. After a 60-second background period, the game clock has not fast-forwarded.
5. Leave the game running in the foreground for 30 minutes without a crash or a memory kill;
   report peak memory (`xcrun devicectl device info processes` or Instruments).

---

## Task 9: Verification and release pass

Prove the port works end-to-end, and write down how to reproduce it.

### Work

- Full playthrough gate on the iPad: start a new game on level 1, build a reception and a GP's
  office, hire a receptionist and a doctor, admit and cure a patient, save, quit, relaunch,
  load, continue. Every step by touch only.
- 30-minute stability session with audio on; report peak memory and any warnings in the log.
- Re-verify Task 3's audio checklist after all later changes (input, scaling and lifecycle work
  all touch the event loop; regressions land here).
- Confirm the whole pipeline from a clean clone: document exact commands in
  `docs/port/IOS_BUILD.md` and follow them yourself from a fresh `build/` directory.
- Write `docs/port/IOS_PORT_NOTES.md`: what was done per subsystem, every non-obvious failure
  and its root cause (the GeneralsX playbook's style), known issues, and what a follow-up would
  tackle. Include which changes are candidates to offer upstream to CorsixTH and which are
  delivery-only.
- Update the repo `README.md` with a short iOS section pointing at the docs (keep it brief and
  in the existing tone; do not restructure the README).

### Acceptance

- The playthrough completed, described step by step, with the touch gestures used.
- Audio checklist from Task 3 re-verified and quoted.
- Clean-tree build reproduced from the documented commands, with output.
- Docs committed.
