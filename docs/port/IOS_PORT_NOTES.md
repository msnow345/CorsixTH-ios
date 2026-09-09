# CorsixTH on iOS — port notes

What the iOS/iPadOS port does, subsystem by subsystem; the failures that were not obvious and
what actually caused them; what is still wrong; and which of the changes are worth offering
upstream.

Companion documents:

* `docs/port/IOS_BUILD.md` — how to build, sign, install and seed the app.
* `docs/port/IOS_AUDIO.md` — the audio design, the SoundFont, and how to verify sound.
* `README.md` — the touch controls, for people who just want to play.

Reference hardware: an iPad Pro 11-inch (M5) running iOS 26, built with Xcode 26.6 / iOS
SDK 26.5. Everything measured below is from that device unless it says otherwise. Signing and
device identifiers live in the git-ignored `scripts/build/ios/ios-signing.env`; nothing personal
is committed.

---

## 1. What was done, per subsystem

### 1.1 Dependencies and toolchain

vcpkg in manifest mode, cross-built to `arm64-ios` and `arm64-ios-simulator`, installed under
`build/ios-deps/{device,simulator}` — out of the source tree, nothing committed. Two overlay
triplets in `CMake/ios/triplets/` are the only additions to stock vcpkg; no port is patched.
Three CMake presets (`ios-device`, `ios-simulator`, `ios-device-xcode`) configure the tree
against them.

The full dependency set builds, fluidsynth included, so SDL_mixer's fluidsynth decoder — the
whole of the MIDI story — was available from the start. Lua is 5.5.0 from the repo's own pinned
registry baseline, vanilla, never LuaJIT; `luafilesystem` and `lpeg` are statically linked and
registered from C through the tree's existing `CORSIX_TH_LINK_LUA_MODULES` path, which is
already gated on `VCPKG_TARGET_TRIPLET` and therefore needed no iOS special case.

Two manifest entries are platform-qualified to `!ios`: `lua[tools]` (vcpkg declares it
unsupported on iOS, and an app has no use for the `lua`/`luac` executables) and
`fluidsynth[sndfile]` (see §2.4). Both are no-ops off iOS.

### 1.2 App bundle, build and signing

**There is no shell app and no inside-out re-signing.** Everything CorsixTH links on iOS is
static — `otool -L` on the shipped binary lists nothing but `/usr/lib` and system frameworks —
so the CMake app target *is* the shipping bundle. That is the single decision the rest of the
packaging rests on.

`CORSIX_TH_DATADIR` becomes `CorsixTH.app` on iOS, which means the tree's own `install()` rules
populate a flat iOS bundle with no bespoke copy logic: `cmake --install` is the staging step.
`CMake/ios/Info.plist.in` is the single source of truth for bundle metadata (landscape-only,
`UIDeviceFamily [1,2]`, `MinimumOSVersion 16.0`, an empty `UILaunchScreen` dict so the app gets
the native drawable rather than a scaled compatibility frame, file sharing on, icon keys,
`CADisableMinimumFrameDurationOnPhone`).

`scripts/build/ios/package-ios.sh` is the whole pipeline: configure → `xcodebuild` (which signs
the bare app and mints or refreshes the provisioning profile) → `cmake --install` → generate and
stage the icon → a fail-loudly manifest check → `codesign --force` with the entitlements
extracted from the Xcode product → `devicectl` install / copy / launch. No team id, identity or
bundle id is committed; the script reads them from the environment or from a git-ignored env
file.

Writable state lives in `Documents/CorsixTH/` inside the app container — `config.txt`,
`hotkeys.txt`, `Saves`, `Levels`, `Campaigns`, `Logs`, `Screenshots` — created by the engine
itself, and visible in the Files app. Nothing writable is ever inside the bundle.

### 1.3 Audio

SDL_mixer's fluidsynth decoder, with **GeneralUser GS v2.0.3** (SF2, 30 MB) fetched at configure
time by hash and installed into the bundle root. The original XMI tracks go through CorsixTH's
own `xmi2mid` conversion and are synthesised by fluidsynth; effects and speech come from
`SOUND-0.DAT` through ordinary SDL_mixer chunks and never touch the synth.

`SDL_HINT_AUDIO_CATEGORY` is set to `"playback"` before the audio device is opened, so the
silent switch and Focus modes do not silence the game. The app reads the category *back* from
`AVAudioSession` after the device is open (through the Objective-C runtime, so `th_sound.cpp`
stays plain C++) and logs it, so "the hint took" is an observation rather than an assumption.

Two diagnostics exist because you cannot listen to a device from a console: a decoder/duration
line per loaded track, and an opt-in final-output level meter
(`CORSIXTH_AUDIO_LEVEL_METER=1`) that prints peak and RMS once a second and labels each second
`SOUND` or `silence`. That one word separates "the mixer stopped" from "the audio session is
wrong", which are different bugs.

Device pause/resume around backgrounding and interruptions acts on the mixer's audio device and
never on track pause state, so music resumes where it stopped and anything the player paused
stays paused.

**Re-verified on the finished tree**, after all the input, scaling and lifecycle work that
touches the event loop, in a 100-second console session on the device with the level meter on:

```
MIDI soundfont: ./GeneralUser-GS.sf2
iOS audio session category: AVAudioSessionCategoryPlayback
audio device format: 48000 Hz, 2 channels
Music loaded: decoder=FLUIDSYNTH duration=148.1s soundfont=./GeneralUser-GS.sf2
Music loaded: decoder=FLUIDSYNTH duration=225.5s soundfont=./GeneralUser-GS.sf2
audio level: peak=0.4384 rms=0.05685 (SOUND)
... 96 consecutive one-second reports, every one of them SOUND ...
```

96 non-silent seconds, the right decoder, the right SoundFont, the playback category still in
effect, no `No preset found` warning and no Lua error anywhere in the run. That is the music
path only — nobody was playing, so no effect or speech was mixed — and nobody heard it.

### 1.4 Display, scaling and frame rate

The existing UI-scale path is used unchanged — no new scaling mechanism. On the reference iPad
it settles on UI scale 3 from the automatic path with the shipped defaults, and the map is drawn
into the full native drawable.

Rendered frames were previously capped by the 18 ms simulation tick (measured: 54.0 fps on a
120 Hz panel). Camera advancement moved out of `GameUI:onTick` into a new `UI:onFrame(dt)`
called from `App:drawFrame`, with every rate scaled by `dt / App.TICK_PERIOD_MS` so that at one
frame per tick the arithmetic is the one it replaces. An iOS-only timer then requests repaints
at the display rate while — and only while — the frame handler reports something still
animating. Measured after: **120.0 fps**, with the in-game date byte-identical at every matched
tick count and the game clock within 0.28 % of the old build (which is the engine's own
tick-dispatch jitter, not the change).

The app renders full-bleed: SDL is asked for a fullscreen iOS window,
`SDL_HINT_IOS_HIDE_HOME_INDICATOR` is set, and only the *horizontal* safe-area inset is honoured
(§2.7). `SDL_EVENT_WINDOW_SAFE_AREA_CHANGED` recomputes the viewport.

### 1.5 Touch input

A gesture recogniser in `CorsixTH/Src/sdl_core.cpp` (`namespace touch`, guarded by
`CORSIX_TH_IOS`) translates raw finger events into the mouse events the game already
understands. SDL's own touch-as-mouse emulation is switched off, and any mouse event still
carrying `SDL_TOUCH_MOUSEID` is dropped, so the two paths can never both deliver.

The recogniser is deliberately ignorant of the game: it decides only *what kind* of gesture
happened, and asks Lua (`touch_drag_query`) what a one-finger drag means at the point it
started. All the game-side knowledge sits in `GameUI:_activePlacement` and
`UI:onTouchDragQuery`, not spread across dialogs. Nothing is dispatched on finger-down except a
motion, because a press that is later cancelled is still a real click to the game, and because
CorsixTH's highlights, tooltips and `cursor_entity` are hover-driven.

The settled model:

| | Outside a placement | Inside a placement or room sizing |
| --- | --- | --- |
| **One finger** | tap · long press → right click · drag pans 1:1 with inertia | carries or sizes · never moves the camera · edge scroll live · second-finger tap rotates |
| **Two fingers** | pan by centroid + pinch zoom, together, with inertia | *identical* — available in every mode |

The placement test runs *before* the pan is considered, so it is structurally impossible for a
stray finger to shift the map out from under a room being sized.

Whether one finger pans at all is one line: `touch_one_finger_pan` in `CorsixTH/Lua/game_ui.lua`.
Both models are fully implemented (`drag_pan` / `drag_none` in the recogniser); the flag chooses.
It is deliberately not a settings-screen option.

Tuning constants, all in `sdl_core.cpp` beside each other: 400 ms long press, 8 pt drag dead
zone, 22 pt pinch activation with a 0.5 pinch-vs-pan ratio, 20 pt per synthetic wheel tick,
300 ms / 32 pt double-tap window, 60 ms release-velocity window. Thresholds are in window points
and converted to render pixels once per gesture, so they stay physical at native resolution.

### 1.6 App lifecycle

`SDL.quit()` is refused on iOS (Apple's guidance is that an app must not offer a control that
quits it) unless `_RESTART` is set, which only `App:reset()` does. The main menu's **Exit** item
and the first-run directory browser's **Exit** button are not built on iOS; the in-game **QUIT**
item is untouched and still returns to the main menu — it never quit the process in the first
place (§2.14) — and now writes an autosave on the way.

Backgrounding hangs off one `SDL_AddEventWatch`, because SDL never queues the six application
lifecycle events (§2.15). An atomic flag set at will-enter-background and cleared at
will-enter-foreground stops the tick timer, stops the display-rate repaint requests, and makes
the main loop skip the frame block entirely — events still arrive and are dispatched, but
nothing advances the world and nothing touches the GPU. Ticks already in flight are counted and
discarded, so the hospital is not fast-forwarded: 113.8 s off screen advanced the in-game clock
by one day, the same as ~3 s of play.

On the way out the app saves config, hotkeys and the game's own autosave. Measured cost:
**33–37 ms** for a mid-campaign hospital producing a 551 KB save, against iOS's ~5 s budget.
Audio is paused and resumed. An in-flight gesture is cancelled cleanly (§2.22).

---

## 2. Failures that were not obvious, and their root causes

### 2.1 Every dependency was stamped with the SDK's minimum OS, and the build was green

vcpkg's community `arm64-ios` triplets leave `CMAKE_OSX_DEPLOYMENT_TARGET` unset, so every
static library came out `LC_BUILD_VERSION minos 26.5` — the SDK version — silently raising the
app's real minimum OS. `vcpkg install` exited 0 throughout. The overlay triplets set the
deployment target explicitly, and `fetch-deps.sh` asserts `platform` and `minos` on every
installed `.a` afterwards. The triplets are deliberately named differently from the built-in
ones so that forgetting `--overlay-triplets` fails loudly instead of quietly producing
SDK-minimum binaries.

### 2.2 fluidsynth would not configure, because CMake turns helper executables into `.app`s

`CMAKE_MACOSX_BUNDLE` defaults to `ON` when `CMAKE_SYSTEM_NAME` is `iOS`, so any port that
`install()`s an executable with only a `RUNTIME DESTINATION` dies with *"install TARGETS given
no BUNDLE DESTINATION for MACOSX_BUNDLE executable target"*. fluidsynth's CLI is such a target.
`-DCMAKE_MACOSX_BUNDLE=OFF` in the triplet builds it unpatched.

### 2.3 `lua_pushliteral(L, )` — a syntax error produced by an empty CMake variable

`CORSIX_TH_ARCH` comes from `CMAKE_SYSTEM_PROCESSOR`, which CMake leaves **empty** when
cross-compiling to iOS unless you set it. `config.h` therefore contained
`/* #undef CORSIX_TH_ARCH */` and `th_lua.cpp` failed to compile. Fixed with
`CMAKE_SYSTEM_PROCESSOR: arm64` in the preset. Note it is baked into
`CMakeFiles/<ver>/CMakeSystem.cmake` on first configure: re-running the preset over an existing
cache will not change it, you need a fresh binary directory.

### 2.4 An autotools port in the graph cannot cross-compile, and it cost us SF3

`fluidsynth[sndfile]` pulls `libsndfile[external-libs]` → `mp3lame`. `vcpkg-make` passes
`--host` and `--build` as the same triple, so `configure` decides it is not cross-compiling and
tries to run an iOS binary: *"cannot run C compiled programs"*. Dropping the feature costs SF3
(Vorbis-compressed SoundFont) support and render-to-file. **The consequence is a hard constraint
on the shipped SoundFont: it must be SF2.** Plain SF2 playback is unaffected.

### 2.5 `$HOME` exists on iOS, and writing to it fails anyway

First launch on device died before video init with
`app.lua: unable to generate a unique filename` and then `signal 11`. `config_finder.lua` falls
back to `$HOME/.config`, and `$HOME` *is* set on iOS — it is the data container root — but the
sandbox refuses to create new entries at that root:

```
PROBE mkdir <container>/.config -> -1 errno=1 (Operation not permitted)
```

so `config_path` degraded to `ourpath`, which is `"./"`, and the process's working directory is
the read-only signed bundle. `App:writeToFileOrTmp`'s `os.tmpname()` fallback then failed too,
because `tmpnam` hands back `/var/tmp/…`, outside the sandbox. Fixed by pointing
`XDG_CONFIG_HOME` at the app's Documents directory before Lua starts — which reuses the existing
lookup rather than adding an iOS search path, and lands writable state where it is visible in
the Files app. `$HOME/.config` would have been a hidden directory invisible to file sharing even
if it had been creatable.

### 2.6 The signing values that are not the values you think they are

* **The team id and the certificate identity's parenthetical are different values.** Passing the
  certificate's parenthetical as `DEVELOPMENT_TEAM` fails with *"No Account for Team"*. Both are
  needed, and they are separate environment variables.
* **Never shorten the identity to `"Apple Development"`.** Once a machine holds certificates from
  more than one team the prefix matches several and `codesign` fails. `package-ios.sh` counts
  `security find-identity` matches and aborts on zero or more than one.
* **Staging invalidates the signature.** `cmake --install` adds ~360 files to a bundle Xcode has
  already signed, so a `codesign --force` pass afterwards is not optional. The entitlements for
  it are extracted from the Xcode product rather than hand-written.
* A free Apple team mints 7-day provisioning profiles; a paid one 365-day. An incremental
  `xcodebuild` is what refreshes them, which is why `--resign-only` still runs it.

### 2.7 A 98-pixel letterbox caused by a status bar that should not have been there

The first safe-area implementation insetted the render viewport by the whole safe area and cost
5.9 % of the panel. UIKit was reporting a 32 pt top inset — and that inset existed only because
SDL had created a *windowed* iOS window: SDL's UIKit view controller answers
`-prefersStatusBarHidden` from the window's fullscreen flag, so the clock/battery strip was on
screen despite `UIStatusBarHidden` in the `Info.plist`, which a view controller can override.

Two fixes, and a policy. Ask SDL for a fullscreen window on iOS (top inset 32 pt → 0 pt, which
is UIKit itself confirming the status bar is gone), set the hide-home-indicator hint, and honour
**only the horizontal** safe-area inset: in landscape the one obstruction that actually eats
pixels is a display cutout, while the vertical insets describe overlays Apple expects content to
run underneath. `render_target::update` also stops resizing or un-fullscreening the window on
iOS, so the settings dialog cannot shrink the screen out from under the game.

The lesson worth keeping: *measure, but explain the measurement before acting on it.*

### 2.8 The MIDI SoundFont path was a dangling pointer, and it looked like working music

`sdl_audio.cpp`'s `l_init` stored the `const char*` that `luaL_optlstring` returned — a pointer
into a Lua string that nothing kept alive past the call. Once collected, the path handed to
fluidsynth was freed memory, fluidsynth failed to load a bank, and it produced a synth with no
presets **while still reporting itself as the `FLUIDSYNTH` decoder with a correct track
duration**. That is exactly why the symptom is "music plays but is silent" rather than an error.
The console gives it away as a flood of `fluidsynth: warning: No preset found on channel N`.
Owning the string took the warnings from 15 in one run to 0 on the same tracks. **Not an
iOS-specific defect; iOS just made it reproducible.**

### 2.9 The default audio category is silenced by the mute switch

`SDL_HINT_AUDIO_CATEGORY` defaults to `"ambient"` → `AVAudioSessionCategoryAmbient`, which the
hardware silent switch mutes and which ducks under any other playing app. A game wants
`"playback"`. The hint is read when the audio device is opened, so it must be set before
`th::sound::init()` runs — in `main()`, before Lua starts.

### 2.10 The frame rate was capped by the simulation tick, and the fix quantised the camera away

`App:onTick` returns `true` unconditionally and, with `limit_fps`, the loop draws only when
something asks for a repaint — so the frame ceiling was the tick rate: 54.0 fps on a 120 Hz
panel. Two non-obvious things fell out of fixing it:

* **`GameUI:scrollMap` rounds to a whole map-screen unit and discards the remainder.** At 120 Hz
  the per-frame delta is often below 0.5, so the camera would have moved *slower*, not merely
  steppily. `_scrollMapFractional` carries the sub-unit remainder to the next frame (clamped,
  because at the edge of the visible diamond requested and applied movement can differ
  arbitrarily). With it, the camera lands within one map unit of the same place at 54 and at
  120 fps.
* **Requesting a repaint once per display frame period aliases against vsync**: 8 ms on a 120 Hz
  panel measured 112 fps. Requesting every *half* period gives 120.0 fps, because vsync inside
  `SDL_RenderPresent` — not the timer — then sets the pace. The timer remains a hard upper
  bound and `limit_fps` is untouched, so this is never the busy-loop path.

### 2.11 The very first two-finger touch crashed the Lua handler

`App:onPinchBegin` forwarded `SDL_EVENT_PINCH_BEGIN` to `GameUI:onPinchUpdate`, which
immediately evaluates `(scale - 1)`. The begin event carries no scale, so every first pinch
raised *"attempt to perform arithmetic on a nil value (local 'scale')"*, and
`GameUI:onPinchBegin` — which exists to reset the zoom momentum — was never called at all. This
is upstream code and not platform specific: a trackpad pinch on any desktop build takes the same
path.

### 2.12 Every pinch zoomed twice

SDL's iOS backend runs a `UIPinchGestureRecognizer` with `cancelsTouchesInView = NO`, so a pinch
arrives as `SDL_EVENT_PINCH_UPDATE` **as well as** through the finger events the touch layer is
already tracking. Left alone, every pinch zoomed once directly and anchored between the fingers,
and again a tick later through `current_momentum.z`, unanchored, still drifting for several
ticks after the fingers lifted. Only the direct path may zoom.

Removing the duplicate was measured rather than assumed, because the user liked the feel of the
buggy build: simulating both paths at 120 fps showed the accumulator contributed **nothing at
all** for any pinch slower than ~300 ms (it never reaches the `> 0.2` gate in `onFrame`) and at
most 4.1 % extra zoom for the fastest ones. The equivalent gain is between 1.000 and 1.052, mean
1.015 — below perception on a 2× pinch. So the throw is kept and only the drift is lost.
`touch_pinch_zoom_gain` exists if anyone disagrees.

**This fix is gated on the platform, not on the gesture, and that is a known defect — see §3.1.**

### 2.13 The long press kept missing the person it was aimed at

`Staff:onClick(ui, "right")` calls `setPickup`, so right-click *was* the correct mapping — the
press was aimed wrong. Patients and staff walk, and by the time a hold completes they have left
the coordinate the finger pressed, so the click landed on bare floor. Neither a shorter hold nor
a double tap fixes that; both still aim at a coordinate. `touch_longpress_anchor` asks the game
where the pressed thing is *now*, using `cursor_entity` (the entity the finger-down motion
already resolved) plus its sub-tile offset, so the click tracks a walking target.

For staff there is now also a double tap, which cannot miss at all because `setPickup` takes the
entity rather than a screen point. The entity is captured on the first tap and reused, since by
the second tap they have walked off the first tap's point. Only a tap on a pickable staff member
is ever deferred — in one logged session 38 of 38 taps were delivered immediately — so nothing
else in the game pays any latency for it.

Applying the anchor to things that do *not* walk turned out to break two of them; see §2.24.

### 2.14 The Quit that froze the app was not either of the Quit menu items

`GameUI:quit` overrides `UI:quit` and has never called `SDL.quit`; it puts up a confirmation and
returns to the main menu. Upstream even says so in a comment. What really called
`App:exit()` → `SDL.quit()` → `SDL_EVENT_QUIT` → `main()` returns — leaving a frozen window,
because on iOS the process is not supposed to end — was **`UIMainMenu:buttonExit`** and the
first-run directory browser's **Exit** button. A C backstop in `l_quit` now refuses the event on
iOS regardless of any future caller, mod or hotkey.

### 2.15 SDL never queues the application lifecycle events

`SDL_SendAppEvent` has an explicit special case: for `TERMINATING`, `LOW_MEMORY`,
`WILL/DID_ENTER_BACKGROUND` and `WILL/DID_ENTER_FOREGROUND` it calls the event watchers and
returns **without touching the queue**, because they must be handled inside the
`UIApplicationDelegate` call stack. A `case SDL_EVENT_DID_ENTER_FOREGROUND:` in the main loop's
switch therefore could never fire — one had been added in good faith and was dead. Everything
lifecycle-related must hang off `SDL_AddEventWatch`, which is called on whichever thread pushed
the event, so it filters by type and asserts `SDL_IsMainThread()` before touching Lua.

### 2.16 An app that was never active does not resign active

The suspend-time autosave originally ran at `WILL_ENTER_BACKGROUND`, which is
`applicationWillResignActive`. With the iPad's screen asleep, an app launched by `devicectl` gets
`DID_ENTER_BACKGROUND` and nothing else — no resign-active, no did-become-active — and the save
was silently skipped. It now runs at whichever of the two arrives first and is idempotent within
one background episode.

### 2.17 The tap had no pressed flash

CorsixTH draws a button's pressed sprite only while it is held, from `active_button`. The
synthetic tap dispatched down and up back to back inside a single pass of the event drain, so
the pressed state existed for zero rendered frames and **no button in the game ever flashed** — a
regression against SDL's own emulation, which delivers down and up across separate frames. The
down still goes out the instant the finger lifts; only the up waits, for exactly one presented
frame.

### 2.18 The hover that never went away, and the fix that was worse

A finger never sends the motion that moves *away*, so the hover applied by a tap's leading
motion stayed applied and every tapped button was left looking hovered. Generic buttons have no
hover sprite at all — the sticky state is per-dialog and named differently everywhere
(`hover_index`, `list_hover_index`, `active_hover`, `hover_id`, `hover_plot`) — so reaching into
fields was not viable, and each dialog's own `onMouseMove` is driven with a point outside it
instead.

The first version broadcast that phantom pointer to **every** open window, and a made-up
coordinate lands inside somebody's hover band sooner or later: `UIFurnishCorridor`'s band
contains its own centre, so a screen-centre sweep moved its list selection, played its hover
sound and swapped its preview on every unrelated tap elsewhere on screen. The dialogs that
escaped did so by the luck of their geometry.

The fix drives only the window under the release point, with a **negative** coordinate that is
outside every window's local bounds and so fails every hover band without this code needing to
know where any of them are. Scoping is sufficient, not merely safer: the tap's own leading
motion already ran every window's `onMouseMove` at the real press point, so the only window that
can be left hovered is the one the finger was on. Windows that use hover to *reveal* something
(the menu bar, the bottom panel) opt out via `ui.touch_clearing_hover`, because what they are
showing was deliberately opened by a tap.

### 2.19 The menu bar was invisible to the touch router

`UIMenuBar` draws with fonts and sprite lists and adds no panels at all, so the inherited
`Window:hitTest` — which knows only about panels and child windows — reported false for every
point on it. Consequences: its item highlight was never released on lift, and a drag across an
open menu panned the map behind it. It now has a real `hitTest` covering the bar strip and any
open menu.

Reaching the bar at all needed *nothing*: a tap in the top strip runs `onMouseMove` first (the
touch layer emits a motion before every click), which is what sets `visible` and calls
`appear()`, and `disappear()` is only ever reached from `onMouseMove` — which touch stops
producing once the finger lifts. So the bar stays up indefinitely, waiting for the tap that
opens a menu. A pin flag was written for this, found to be dead (`visible` was already true by
the time `onMouseDown` ran), and deleted; only the comment explaining why nothing is needed
survives.

Holding a menu item was a separate bug with a separate cause: at 400 ms the long press fired a
right click that `UIMenuBar` ignores (`button ~= "left"` → early return) and then swallowed the
gesture until lift, so **press-to-preview worked and release-to-act did not**. A `preview` drag
mode — windows opt in with `touch_hold_previews` — suppresses the long press there instead,
reusing the mechanism already built for placements.

### 2.20 The bottom panel's hover-reveal, and the regression fixing it caused

The right-hand end of the bottom panel replaces the dynamic info bar with buttons *on hover*,
and a finger has no hover: the tap's motion revealed them and the tap's click landed on
whichever one happened to appear under the finger, before the user had seen what appeared. The
first tap in that region is now spent revealing.

The first version gated the swallow on `UIBottomPanel:hitTest`, which is the **whole** panel, so
every permanently-visible control — the bank button, the centre toolbar — needed two taps. The
region is now derived from the leftmost of the `additional_panels` themselves rather than
written down, so it cannot drift away from them. Both halves were confirmed in one instrumented
session on device.

### 2.21 Edge scrolling latched on and never stopped

`tick_scroll_amount_mouse` is armed by a pointer move *into* the band and disarmed by one *out*
of it. A finger leaving the glass produces neither. Carrying an object to the edge and then
repositioning the view with two fingers left it armed with nothing able to disarm it, and the
per-frame camera then scrolled indefinitely. Fixed at the single point where a gesture goes
idle: `touch_gesture_end` is dispatched from there for **every** exit — cancelled carry,
cancelled press, inert drag, two-finger gesture that never started — so no future phase can
forget it. Edge scrolling is also now off entirely except while a placement is live, where the
finger is occupied and there is no alternative, and its band is widened from 1 px (unhittable
with a finger) to 24 UI-scale units.

### 2.22 Backgrounding mid-gesture would have resumed holding the mouse button

The deferred mouse-up of §2.17 is flushed in the frame path, which is exactly what backgrounding
shuts down. Home-swiping during the flash of a tap would have resumed the game holding the left
button, and the next finger anywhere on the map would have dragged from wherever the tap was.
iOS does send `SDL_EVENT_FINGER_CANCELED` for the touches it takes away, but it is a *queued*
event so it arrives after the suspension — and it never arrives at all for the deferred release,
which is no longer a touch. `touch::cancel_for_suspend` flushes the held tap, flushes the
deferred release, ends a held-button drag, resets the phase to idle (which makes the late
`FINGER_CANCELED` events harmless no-ops) and reports the gesture end. Deliberately **not** done:
no fling on suspend, and no drop of a carried object — that would place a building because the
phone rang.

### 2.23 Two upstream crashes found by running on a device

* **A Lua stack leak on the frame error path.** `lua_pop(L, 2)` after the frame dispatch sat
  inside the success branch, so every failing frame left the handler and its result on the
  stack. Repeated frame errors overflow the Lua stack and panic the process — on iOS, a crash.
* **`App:fixConfig`'s `player_name` fallback.** It falls back to `os.getenv("USER")` or
  `os.getenv("USERNAME")` with no final default, so where neither is set the next `value:match()`
  indexes nil and the process dies with `signal 11`. Reproduced by simulating both env vars
  absent; the physical iPad happens to set `USER`, an iOS simulator does not.

### 2.24 The long-press anchor made bins and fire extinguishers impossible to pick up

The fix in §2.13 was applied to every long press, not only to the walking targets it was written
for, and an animation's origin is a point the sprite is drawn *around* rather than one it is
obliged to cover. Decompressing `VSTART/VFRA/VLIST/VELE-1.ANI` and walking the frame the way
`animation_manager::hit_test` does settles it per object: the litter bin's idle animation (1752)
draws two elements, at (-1,-17) 23×17 and (1,0) 18×15, and the origin falls in the **gap between
them**; the fire extinguisher's frames do not reach the origin at all. So for those two the
re-aimed right click could never land on the object — `emit_click`'s leading motion cleared
`cursor_entity`, `Object:onClick` was never reached, and the hold did nothing whatsoever. The
bench, plant, radiator, drinks machine and reception desk all cover their origin, which is why
only two objects in the game were affected and it read as intermittent.

`onTouchLongPressAnchor` now re-aims only for `Humanoid`s. Nothing that stands still needs it:
the press point is the point `cursor_entity` was resolved from, so it is already known to hit,
and moving off it can only lose.

---

## 3. Known issues

### 3.1 A trackpad pinch does not zoom — and this blocks upstreaming the touch work

`GameUI:onPinchUpdate` discards SDL's pinch accumulator when `touch_input` is true (§2.12). The
gate is the **platform** (`TH.GetCompileOptions().os == "ios"`, a compile-time constant), not the
gesture. An external trackpad — a Magic Keyboard on an iPad Pro — sends `SDL_EVENT_PINCH_*` but
**no finger events**, so the touch layer never runs and that pinch now has no zoom path at all.
Keyboard zoom hotkeys are also unavailable there, so a trackpad user has no way to zoom.

Accepted because this port's user does not use a trackpad with their iPad.

**The fix:** suppress the accumulator only while our own recogniser actually has a gesture in
progress, rather than for the whole platform. The `touch_catch` and `touch_gesture_end`
dispatches already bracket exactly that interval — set a flag on the UI in the first and clear it
in the second, and gate on that instead of on `touch_input`. No device detection required.
**This must be tightened before any of the touch work is offered upstream**; shipped as-is it
would break trackpad zoom for every iPad user who has one.

### 3.2 Features with no pointer route

Found by sweeping every `addKeyHandler` binding for ones that are the sole route to a feature.
These are hotkey-only and an iPad has no keyboard:

* **Selling a picked-up item.** `UIPlaceObjects:sell` (`place_objects.lua:359`) is bound only to
  `ingame_sellPickedUpItem` (`:96`); there is no button for it. Cancelling the placement works
  instead, so this is not blocking. Adding a button needs new art in an already-full dialog.
* **Camera bookmarks**, `ingame_storePosition_N` / `ingame_recallPosition_N`. No pointer route on
  any platform.
* **`global_pause_movie`.** Tapping during a movie stops it outright instead (`ui.lua:867`), so a
  movie can always be dismissed; it just cannot be paused.

Debug and developer bindings (cheat window, Lua console, screenshot, log dump, reset) are
deliberately not addressed.

### 3.3 The mouse cursor sprite is not drawn

Deliberate: it is a picture of a mouse pointer, and with a finger it sits wherever the last tap
landed. One consequence is that cursor-shaped affordances are invisible — the bank manager's
pie-chart cursor, for instance, conveys nothing on iOS.

### 3.4 Settings the platform overrides

`render_target::update` ignores `params.fullscreen`, `params.maximized` and the requested size on
iOS, so the in-game Settings dialog's resolution and fullscreen controls are **inert** rather
than hidden. A visible inconsistency, not a malfunction.

### 3.5 Memory

Reported from the jetsam ledger (`phys_footprint` / peak) at every lifecycle transition. On a
mid-campaign hospital: steady foreground **380–394 MB**, **peak 630 MB**, reached during startup
and level load rather than during play. The peak did not grow across 11 background/foreground
cycles, so there is no per-cycle leak. Comfortable on an M5 iPad Pro; on a 3–4 GB device it would
be closer to the jetsam limit. The 30 MB SoundFont is resident in that figure.

### 3.6 Smaller ones

* **Savegames are not interchangeable with a desktop build linked against a different Lua.**
  CorsixTH's persistence writes Lua bytecode, which is not portable across Lua versions. This
  tree's pinned vcpkg baseline resolves Lua to 5.5.0 on *every* platform, so this is not an iOS
  divergence — but a distro build against 5.4 will not read these saves. iOS-to-iOS is fine.
* **Movies are off** (`WITH_MOVIES=OFF`, no ffmpeg in the iOS dependency set).
* **SF3 SoundFonts are unsupported** (§2.4). SF2 only.
* **`CADisableMinimumFrameDurationOnPhone` is untested** — no iPhone hardware was available. It
  has no effect on iPad.
* **The `App:reset()` restart path** (`SDL_Quit()` then re-init in the same process, reachable
  from the folder-settings data-directory flow) is allowed through the `l_quit` backstop but has
  never been exercised on iOS.
* **No simulator packaging path.** `package-ios.sh` targets the device preset only.
* **No desktop build was ever re-verified on this host.** Every change is inside `if(IOS)` /
  `#ifdef CORSIX_TH_IOS` / `if not TheApp.ios` except the deliberately platform-neutral fixes in
  §4, but no desktop preset was configured to prove it.
* **`clang-format` is not installed on this host**, so added C++ was hand-matched to the
  surrounding style (2-space indent, 80 columns).
* **A harmless pre-existing link warning:** `ld: warning: ignoring duplicate libraries: …liblua.a`
  — `${LUA_LIBRARIES}` lists it twice. Not iOS-specific.

### 3.7 Not verified on device at all

Stated plainly because the reports these notes draw on were written from console output, log
files and the device's own filesystem — nobody driving this port could see the iPad's screen or
touch its glass.

* A full playthrough by touch (new game → reception → GP's office → hire → cure → save → quit →
  relaunch → load → continue) has never been performed end to end in one session.
* A 30-minute uninterrupted foreground stability run. The longest observed is ~7 minutes,
  crash-free with a flat peak; longer sessions ended because the iPad auto-locked or the
  Wi-Fi `devicectl` tunnel dropped, never because the app failed.
* An audio interruption (Siri, a timer, headphones) recovering on its own.
* Silent mode / Focus with the game running.
* An icon launch from the Home Screen, which is the one launch path subject to SpringBoard's
  watchdog — a `devicectl` launch bypasses it.
* Backgrounding *during* a gesture (§2.22).
* Text entry: naming a savegame should raise the on-screen keyboard, since `Window` already calls
  `TheApp:startTextInput()` on textbox focus and `SDL_StartTextInput` is what raises it, but
  nobody has typed a save name.
* `drag_wheel` list scrolling, `UIQueue`'s drag-to-reorder, `UIPolicy`'s sliders and the
  `do_scroll` view circles — all reach the code through the same dead-zone commit as everything
  else, but no logged session exercised them.

---

## 4. Candidates to offer upstream

Platform-neutral, and each one fixes or extends behaviour that is wrong or missing on desktop
too. Listed strongest first.

| Change | File | Why it is upstream's problem, not iOS's |
| --- | --- | --- |
| `fix(ui): dispatch pinch begin to the pinch begin handler` | `Lua/app.lua` | `App:onPinchBegin` forwarded to `onPinchUpdate`, which evaluates `(scale - 1)` on an event that carries no scale. A trackpad pinch on any desktop build raises the same Lua error, and `GameUI:onPinchBegin` never ran at all. One-line fix (§2.11). |
| SoundFont path lifetime | `Src/sdl_audio.cpp` | A `const char*` into a collected Lua string was handed to fluidsynth per track load. Use-after-free, and the symptom is silent MIDI with a healthy-looking decoder and duration (§2.8). |
| Unconditional `lua_pop(L, 2)` on the frame dispatch error path | `Src/sdl_core.cpp` | Two Lua stack slots leaked per failing frame; repeated frame errors overflow the stack and panic. The timer dispatch above it already pops unconditionally (§2.23). |
| `fix(config): guard player_name fallback against nil USER/USERNAME` | `Lua/app.lua` | `os.getenv("USER") or os.getenv("USERNAME")` with no final `""`, then `value:match()` on nil. Crashes on any platform where neither variable is set (§2.23). |
| `UIMenuBar:hitTest` | `Lua/dialogs/menu.lua` | The bar draws no panels, so the inherited `Window:hitTest` reported false for every point on it and the bar was invisible to anything asking which window is under a pointer (§2.19). A pointer-driven build never noticed, but the predicate is simply wrong. |
| `feat(ui): let setZoom anchor on an explicit screen point` | `Lua/game_ui.lua` | Purely additive: `setZoom` could anchor only on the cursor or the screen centre. The cursor-or-centre choice is unchanged when the new arguments are absent. Useful to anyone adding a zoom that anchors somewhere else. |
| `refactor(ui): advance the camera per rendered frame instead of per tick` | `Lua/app.lua`, `ui.lua`, `game_ui.lua` | Camera motion could never be smoother than the fixed 18 ms tick, whatever the display. Rates are still written per classic tick and scaled by `dt / TICK_PERIOD_MS`, so at one frame per tick it is bit-for-bit the behaviour it replaces; no simulation moved. The bigger of these, and the one that would need the most upstream discussion. `_scrollMapFractional` comes with it (§2.10). |
| `refactor(audio): call MIX_GetAudioDuration once in the music load log` | `Src/sdl_audio.cpp` | Trivial tidy-up, only worth carrying if the logging around it goes too. |

**Delivery-only, and not upstream material as written:** the whole touch layer and its Lua
handlers, the lifecycle event watch, forcing a fullscreen window and the safe-area policy, the
`XDG_CONFIG_HOME` and audio-category setup in `main()`, the display-rate frame timer, the iOS
`Info.plist`, the overlay triplets and presets, and everything under `scripts/build/ios/`. The
touch layer in particular is not offerable until §3.1 is fixed.

---

## 5. What a follow-up would tackle

Roughly in the order that would pay off.

1. **Game-data ingest from the Files app.** This was planned and cut. Today the user drops a
   `HOSP` folder into `Documents/CorsixTH/` by hand (over `devicectl`, or through the Files app,
   which already works because file sharing is on) and the engine finds it with no UI at all.
   An in-app "import your Theme Hospital data" flow — accepting a zip or a folder, unpacking it
   into that directory — is the largest remaining gap for anyone who is not the developer.
2. **Fix the pinch gate (§3.1)** so the touch work can be offered upstream at all.
3. **Split the upstreamable fixes (§4) onto a clean branch off `master`** and open them as
   separate pull requests. They are independent of the iOS work and most are one-liners.
4. **A sell button for a picked-up item (§3.2)**, or a gesture for it.
5. **Hide or disable the inert resolution/fullscreen controls on iOS (§3.4).**
6. **An iPhone pass.** The UI-scale cap edge case (display scale 3, cap 2) is verified only on a
   simulator, and `CADisableMinimumFrameDurationOnPhone` has never run on ProMotion iPhone
   hardware.
7. **Memory on a smaller device (§3.5).** A 630 MB peak wants checking against a 3–4 GB iPad, and
   the 30 MB SoundFont is the obvious thing to trim if it is a problem.
8. **Simulator packaging and some CI**, even just a configure-and-build of both iOS triplets plus
   a desktop preset, so the platform-neutral changes stop being unproven off iOS.
