# iOS audio

Everything the desktop build plays, the iOS build plays: sound effects and speech from
`SOUND-0.DAT`, and the original XMI music tracks synthesised by fluidsynth. Nothing about
the audio architecture is iOS-specific except the audio session and the choice of
soundfont.

## Synth decision: SDL_mixer's fluidsynth decoder (option 1)

`CorsixTH/Src/sdl_audio.cpp` hands MIDI data to SDL_mixer with the
`SDL_mixer.decoder.fluidsynth.soundfont_path` property set; SDL_mixer's `FLUIDSYNTH`
decoder synthesises it. That is upstream's design and it needs no code change on iOS.

The usual worry — fluidsynth 2.x depends on glib — did not bite: vcpkg builds
`fluidsynth 2.5.5` static for `arm64-ios` (with its glib dependency) and SDL3_mixer is
built with its `fluidsynth` feature on. Confirmed present in the linked binary:

```
$ strings build/ios-deps/device/arm64-ios-min16/lib/libSDL3_mixer.a | grep -i fluidsynth | sort -u
_MIX_Decoder_FLUIDSYNTH
FLUIDSYNTH
SDL_mixer.decoder.fluidsynth.props
SDL_mixer.decoder.fluidsynth.soundfont_iostream
SDL_mixer.decoder.fluidsynth.soundfont_path
```

and confirmed at runtime on the device, which logs the decoder that actually claimed each
track:

```
Music loaded: decoder=FLUIDSYNTH duration=225.5s soundfont=./GeneralUser-GS.sf2
```

**FluidLite, TinySoundFont and AudioToolbox were therefore never needed and were not
tried.** Option 1 worked, so nothing below it was evaluated.

The missing piece was never the synth: it was the instrument bank. Before this task
fluidsynth ran with no soundfont loaded and filled the console with
`fluidsynth: warning: No preset found on channel N [bank=0 prog=P]` while rendering
silence.

## The soundfont

| | |
| --- | --- |
| Name | GeneralUser GS v2.0.3 (`GeneralUser-GS.sf2`) |
| Author | S. Christian Collins |
| Format | SF2 — RIFF/`sfbk`, uncompressed 16-bit PCM in `sdta/smpl`, zero Ogg pages |
| Size | 32,319,396 bytes (30.8 MiB) |
| SHA-256 | `9575028c7a1f589f5770fccc8cff2734566af40cd26ed836944e9a5152688cfe` |
| Source | `https://raw.githubusercontent.com/mrbumpy409/GeneralUser-GS/684543d5e5efaef08d02be50dcda8d552478fa60/GeneralUser-GS.sf2` (the author's own repository, pinned to an immutable commit) |
| Licence | GeneralUser GS License v2.0 — see `documentation/LICENSE.txt` in the same repository |

Licence text, the operative sentences:

> You may use GeneralUser GS without restriction for your own music creation, private or
> commercial. This SoundFont bank is provided to the community free of charge. Please feel
> free to use it in your software projects, and to modify the SoundFont bank or its
> packaging to suit your needs.

Redistribution inside a software product is explicitly permitted. The licence also asks
that websites *featuring* GeneralUser GS not deep-link the author's own download files;
we fetch from the author's GitHub repository at build time, which is not that, but if that
ever becomes a concern the fetch URL is a single line in `CorsixTH/CMakeLists.txt`.

### Why not FluidR3

Two independent reasons.

1. **SF3 cannot be decoded here.** `FETCH_SOUNDFONT` upstream fetches `FluidR3.sf3`, whose
   samples are Ogg-Vorbis compressed. Our fluidsynth is built without `libsndfile`, so it
   cannot decompress them. `MuseScore_General.sf3` (39.9 MB, a real file) fails for the
   same reason.
2. **The SF2 edition is too big and not reliably fetchable.** `FluidR3_GM.sf2` is ~148 MB,
   against a 33 MB app bundle that a free developer account reinstalls weekly.
   `raw.githubusercontent.com/Jacalz/fluid-soundfont/master/original-files/FluidR3_GM.sf2`
   returns HTTP 200 and 134 bytes — a Git LFS pointer, not the font. Likewise
   `schristiancollins.com/soundfonts/GeneralUser_GS_1.471.zip` returns 200 and 3,872
   bytes, a landing page.

GeneralUser GS is ~1/5 of FluidR3's size and covers every program the Theme Hospital
tracks use — a host-side render of all three demo tracks produces **no** "No preset found"
warnings at all (see below). It takes the bundle from 33 MB to 63 MB.

## How the soundfont path reaches fluidsynth on iOS

```
CorsixTH/CMakeLists.txt   FETCH_SOUNDFONT + if(IOS) -> FetchContent GeneralUser-GS.sf2
        |                 install(FILES ${SOUNDFONT_FILE} DESTINATION ${CORSIX_TH_DATADIR})
        v                 CORSIX_TH_DATADIR is "CorsixTH.app", so it lands in the bundle root
CorsixTH.app/GeneralUser-GS.sf2
        |
        v                 App:getFullPath() is debug.getinfo(1,"S").source:sub(2,-12) = "./"
CorsixTH/Lua/app.lua      App:findSoundFont() tries data_dir .. "GeneralUser-GS.sf2"
        |                 lfs.attributes("./GeneralUser-GS.sf2") succeeds because the
        |                 process working directory on iOS IS the signed bundle
        v
CorsixTH/Lua/audio.lua    SDL.audio.init(self.app:findSoundFont())
        |
        v
CorsixTH/Src/sdl_audio.cpp  l_init stores the path; createMusicAudio sets
                            "SDL_mixer.decoder.fluidsynth.soundfont_path" on every load
```

Observed on device:

```
MIDI soundfont: ./GeneralUser-GS.sf2
Music loaded: decoder=FLUIDSYNTH duration=225.5s soundfont=./GeneralUser-GS.sf2
```

The relative path is not a shortcut — it is the same mechanism `CorsixTHUnicode.ttf`,
`Bitmap/`, `Levels/` and the whole Lua tree already use on iOS.

> **`sound_font` used to dangle.** `l_init` stored the `const char*` that
> `luaL_optlstring` returned, i.e. a pointer into a Lua string that nothing kept alive
> after the call returned. Once the string was collected, fluidsynth was handed freed
> memory, failed to load a bank, and rendered silence — while still reporting itself as
> the `FLUIDSYNTH` decoder with a correct track duration. It is now a `std::string`. This
> is not an iOS-specific bug; iOS just made it reproducible.

## Audio session and the hardware mute switch

The hint name was read out of the SDL3 headers actually pinned here
(`build/ios-deps/device/arm64-ios-min16/include/SDL3/SDL_hints.h`, SDL **3.4.10**), not
from memory:

```c
/**
 * A variable controlling the audio category on iOS and macOS.
 *
 * - "ambient": Use the AVAudioSessionCategoryAmbient audio category, will be
 *   muted by the phone mute switch (default)
 * - "playback": Use the AVAudioSessionCategoryPlayback category.
 *
 * This hint should be set before an audio device is opened.
 */
#define SDL_HINT_AUDIO_CATEGORY "SDL_AUDIO_CATEGORY"
```

`CorsixTH/SrcUnshared/main.cpp` calls `SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback")`
from `set_ios_audio_session()`, before Lua starts and therefore long before
`th::sound::init()` opens the device.

Rather than trust that, the app asks AVAudioSession what category is in effect once the
device is open and logs it (`th::sound::log_ios_audio_session_category()`, via the
Objective-C runtime so the translation unit stays plain C++). On device:

```
iOS audio session category: AVAudioSessionCategoryPlayback
```

`AVAudioSessionCategoryPlayback` is by definition not silenced by the Ring/Silent switch;
`Ambient` and `SoloAmbient` are.

### Interruptions and route changes

SDL3 already installs an `SDLInterruptionListener` for
`AVAudioSessionInterruptionNotification` (visible in `libSDL3.a`'s symbol table), and
because the mixer opens `SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK`, SDL migrates the logical
device when the default output changes (headphones in/out).

On top of that, `CorsixTH/Src/sdl_core.cpp` calls `th::sound::resume_audio_device()` on
`SDL_EVENT_DID_ENTER_FOREGROUND`, `SDL_EVENT_AUDIO_DEVICE_ADDED` and
`SDL_EVENT_AUDIO_DEVICE_REMOVED`. It resumes only the *device*, and only if
`SDL_AudioDevicePaused()` says it is suspended — track pause state is untouched, so music
the player deliberately paused stays paused.

## Verifying audio on device

### Console instrumentation

Launch with the level meter enabled:

```sh
xcrun devicectl device process launch --console --terminate-existing \
  --device "$DEVICE" -e '{"CORSIXTH_AUDIO_LEVEL_METER":"1"}' com.example.corsixth
```

`CORSIXTH_AUDIO_LEVEL_METER` is read with `SDL_GetHintBoolean`, so an environment variable
or an `SDL_SetHint` both work. It installs a `MIX_SetPostMixCallback` on the final mix and
prints, once per second of output:

```
audio device format: 48000 Hz, 2 channels
audio level: peak=0.4346 rms=0.05473 (SOUND)
```

`silence` instead of `SOUND` means the mixer is producing nothing — the meter sits after
all mixing, so it sees effects, speech and music together. It is off by default and costs
nothing when off.

Unconditional lines worth watching, all of them on stdout:

* `MIDI soundfont: <path>` or `MIDI soundfont: (none found)`
* `iOS audio session category: <category>`
* `Music loaded: decoder=<name> duration=<s> soundfont=<path>`
* `Music failed to load: <SDL error>`

### Identifying which track is playing, from the log alone

Render the demo's XMI files on the host through the game's own `xmi2mid.cpp` and the same
soundfont, then compare durations. Each track has a distinct length:

| XMI | Title (`MIDIDEM.TXT`) | Duration |
| --- | --- | --- |
| `ATLANTIS.XMI` | Atlantis | 208.5 s |
| `STEADY.XMI` | Steady Pulse | 215.3 s |
| `NIGHTSH.XMI` | Night Shift | 225.5 s |

The device's `Music loaded: … duration=…` lines match these to a tenth of a second, so the
log names the track.

### Host-side reference render

```sh
# build a host tool around the repo's own XMI converter
clang++ -std=c++17 -I<stub dir with empty config.h> -ICorsixTH/Src \
    main.cpp CorsixTH/Src/xmi2mid.cpp -o xmi2mid
./xmi2mid ~/CorsixTH-testdata/demo/HOSP/SOUND/MIDI/NIGHTSH.XMI NIGHTSH.mid
fluidsynth -ni -g 0.6 -r 44100 -F NIGHTSH.wav GeneralUser-GS.sf2 NIGHTSH.mid
```

The result is real music, not a click track: 225.5 s, peak -2.7 dBFS, RMS -22.0 dBFS,
sustained per-second RMS across the whole track, and **no** `No preset found` warnings for
any of the three tracks.

### Things that will bite you

* **A locked iPad refuses `devicectl process launch`** (`FBSOpenApplicationErrorDomain
  error 7 … Locked`), and if it locks while the app is running the app is suspended and
  then terminated — which looks like a crash in the console but is not. Disable auto-lock
  before a long soak.
* **The bundle is 63 MB** with the soundfont, up from 33 MB. That is the reinstall cost.
* **`FETCH_SOUNDFONT` downloads 30.8 MB at configure time.** It is cached in
  `build/<preset>/_deps/generalusergs-src/`, and it is never committed.
