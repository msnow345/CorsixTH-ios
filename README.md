<picture>![image](https://github.com/CorsixTH/CorsixTH/assets/20030128/923883d1-cd2b-48a9-8506-6ee03e2745dc)</picture>

### Latest Release <a href="https://github.com/CorsixTh/CorsixTH/releases/latest"><img src="https://img.shields.io/github/v/release/CorsixTH/CorsixTH?style=for-the-badge&color=green" align="top"></a>
[![Linux and Tests](https://github.com/CorsixTH/CorsixTH/actions/workflows/Linux.yml/badge.svg?branch=master)](https://github.com/CorsixTH/CorsixTH/actions/workflows/Linux.yml) [![Windows](https://github.com/CorsixTH/CorsixTH/actions/workflows/Windows.yml/badge.svg?branch=master&event=push)](https://github.com/CorsixTH/CorsixTH/actions/workflows/Windows.yml) [![AppVeyor Build Status](https://ci.appveyor.com/api/projects/status/github/CorsixTH/CorsixTH?branch=master&svg=true&passingText=Windows%20-%20OK&failingText=Windows%20-%20Failing)](https://ci.appveyor.com/project/TheCycoONE/corsixth)
##### [Matrix Space](https://matrix.to/#/#CorsixTH:matrix.org) | [Matrix Chat](https://matrix.to/#/#corsixth-general:matrix.org) | [Discord](https://discord.gg/Mxeztvh) | [Report Issue](https://github.com/CorsixTH/CorsixTH/issues/new) | [Reddit](https://www.reddit.com/r/corsixth) | [Twitter/X](https://twitter.com/CorsixTH) | [Facebook](https://facebook.com/CorsixTH)
----

A reimplementation of the 1997 Bullfrog business sim Theme Hospital. As well as faithfully recreating the original, CorsixTH adds support for modern operating systems (Windows, macOS, Linux and BSD), high resolutions and much more.

<picture>![image](https://github.com/CorsixTH/CorsixTH/assets/20030128/71a42d5f-d486-4309-ba85-77e114880bcb)</picture>


## Getting Started ##

You will need the following:

- Grab the latest installer for your system:
   - Windows and macOS builds, and an AppImage for Linux can be downloaded directly from [releases](https://github.com/CorsixTH/CorsixTH/releases).
   - Linux and BSD repositories use either corsixth or corsix-th names [packaged versions](https://repology.org/metapackage/corsixth).
   - A Flatpak for Linux users is available on [Flathub](https://flathub.org/apps/details/com.corsixth.corsixth).
   - A Snap for Linux users is available on [Snapcraft](https://snapcraft.io/corsixth) [(support page)](https://github.com/snapcrafters/corsixth).
   - An unofficial Anylinux AppImage is available from [Pkgforge](https://github.com/pkgforge-dev/CorsixTH-AppImage-Enhanced).
- We use graphics, sound and other data from the original game so one of the following is required:
   - Original game CD from eBay etc. or your dusty bookshelf :smile:
   - A download from [GOG.com](https://www.gog.com/game/theme_hospital) or [EA](https://www.ea.com/games/theme/theme-hospital)

 Head over to our [getting started](https://github.com/CorsixTH/CorsixTH/wiki/Getting-Started) page for more detail.

### What's Working? ###
Most features of the game are available -- and we're at a state where you can complete the full campaign without issue.
##### Original Features #####
- Single player campaign
- All diseases, objects, rooms are available
- All events (emergencies, earthquakes, epidemics, VIP visits)
- Management windows (managing staff, patients, policies etc.)
- Music/Jukebox
- Gameplay videos
- Cheats (naughty!)
  
##### New Features #####
- Custom levels and campaigns
- Full HD and 4K support
- Zooming
- UI scaling
- Subtitles
- More than 20 different languages
- Make your own maps and levels with built-in map editor
- Unlimited save files
- Play your own music!
- Option to build rooms while paused
- Option to remove destroyed rooms for a fee
- Improved game logic
- Full control over all hotkeys
- Machine menu
- Adviser messages history 

### What's missing/needs improvement? ###
There are some areas of the game still missing, and while we work to get them integrated any additional help from the community is always appreciated!
- Multiplayer/LAN
- AI Hospitals (and the components associated with it)
- Rats (but rat holes are present) and the bonus rat level
- Vomit waves
- Win level video/letter
- The original graphics do not have a complete set for Pregnancy, Alien DNA, and female Fractured Bones patients -- these may cause anomalies if you enable regular spawning in settings
- Some objects in the game may glitch with walls

## iOS and iPadOS ##

There is an unofficial iOS/iPadOS port in this tree. It is not on the App Store -- you build it
yourself and install it with your own Apple developer account. `docs/port/IOS_BUILD.md` has the
build, signing and install commands, `docs/port/IOS_PORT_NOTES.md` records what was changed and
what is still missing, and `docs/port/IOS_AUDIO.md` covers sound. You supply your own copy of the
original game data as usual: launch the app once, then put your `HOSP` folder into
`On My iPad -> CorsixTH -> CorsixTH` in the Files app, where your saves, config and logs live too.

### Touch controls ###

Everything the game does with a mouse is reachable with a finger, but several of the gestures are
not guessable, so they are all listed here. In short: **one finger interacts, two fingers
navigate** -- and two fingers pan and zoom in every mode, including while you are carrying or
sizing something.

| Gesture | What it does |
| --- | --- |
| Tap | Left click. The click lands where your finger went *down*, not where it lifted, so small buttons do not slip. |
| Tap the very top edge of the screen | Reveals the menu bar (File, Options, ...), and it stays up until you choose something. This is the only way to it: there is no Escape key. |
| Tap the right-hand end of the bottom panel | The first tap reveals the buttons that normally appear on hover in place of the information bar; the second tap presses one. Controls that are permanently on screen -- the bank, the middle toolbar -- still act on the first tap. |
| Long press, about 0.4 s | Right click, aimed at whatever was under your finger even if it has walked off since. Right click is how you pick an object up, and how you put a carried person back down. |
| Long press an object, then keep the finger down and move | The press picks the object up and the same finger carries it straight on, from where your finger is. Release to place it. |
| Double tap a member of staff | Picks them up. Then lift; drag one finger and release to put them down. They stay in hand in between, so you can two-finger pan somewhere else first. |
| Second-finger tap while carrying an object | Rotates it one step, standing in for the rotate hotkey. Staff have no orientation, so it does nothing for them. |
| Long press while carrying a person | Puts them back down. It is the only way -- that dialog has no cancel button. |
| Drag one finger on the map | Pans the view 1:1. A flick coasts to a stop; touch the screen again to catch it. |
| Drag one finger while building or placing | Sizes the room, or carries whatever is in hand. The map deliberately never moves under your finger while something is being placed -- use two fingers for that. Drag to the edge of the screen and the map scrolls, so you can place things beyond the current view. |
| Drag one finger inside a list | Scrolls the list. On the scrollbar itself, it drags the scrollbar. |
| Drag one finger on a dialog | Acts as a held mouse button: sliders, reordering a queue, dragging a window about. |
| Drag two fingers | Pans by the point between them. Available in every mode, including mid-placement. |
| Pinch two fingers | Zooms, anchored between your fingers. Pinch and pan apply together, so you can start pinching in the middle of a drag without lifting. |

If you would rather one finger never moved the view, set `touch_one_finger_pan = false` near the
top of `CorsixTH/Lua/game_ui.lua` and rebuild. A one-finger drag on open map then does nothing at
all, leaving two fingers as the only way to move the camera; nothing else changes.

A few things have no touch route, because they were only ever bound to keys and an iPad has no
keyboard:

- **Selling an item you have already picked up** (`ingame_sellPickedUpItem`) has no button in the
  placement dialog. Cancel the placement instead.
- **Camera bookmarks** (store and recall position) have no pointer route on any platform.
- **Pausing an in-game video** (`global_pause_movie`) is keyboard-only; a tap stops the video
  outright instead. Moot in practice, as videos are disabled in the iOS build.

The mouse cursor sprite is not drawn either, since there is no pointer to draw -- so hints carried
by the cursor's shape, such as the bank manager's pie chart, do not show up.

## Developers
### Coders and non-coders we want you!

We are always looking for help with improving CorsixTH. The code base is made up of Lua and C++. Most of the game logic is written in Lua, we love Lua and its approachable and easy to pick up nature, so hit fork and get started! But don't worry if you don't code as we can always use your help in other areas and if you have ideas for the project please contact us or open a new issue! We could also use help updating the documentation in the wiki and keeping the issue list up to date.\
You can also [click here](https://github.com/CorsixTH/CorsixTH/issues?q=is%3Aissue+is%3Aopen+label%3A%22Good+First+Issue%22) to find issues that would suit a first-time contributor to take on!

###### Features & Bugfixes ######
We still have features to add and bugs to fix, check out the issue tracker [here](https://github.com/CorsixTH/CorsixTH/issues). Want to talk about adding a feature? post on our Google group or [contact us](#Contact).

###### Translation ######
CorsixTH has translations for more than 20 different languages, some of which need updating. Read our [wiki](https://github.com/CorsixTH/CorsixTH/wiki/Localization) for more information.

## More

Our [wiki](https://github.com/CorsixTH/CorsixTH/wiki) is a good place to start, if you can't find what you are looking for feel free to contact us using one of the methods below.

## Contact

- Follow us on [Reddit](https://www.reddit.com/r/corsixth), Twitter/X ([**@CorsixTH**](https://twitter.com/CorsixTH)), and on [Facebook](https://facebook.com/CorsixTH)
- <details>
  <summary>Hit us up on Matrix! (Discord bridged) [click to expand]</summary>
  
  - **CorsixTH Space** (includes all rooms below, if your client supports it) [#CorsixTH:matrix.org](https://matrix.to/#/#CorsixTH:matrix.org)
  - **General Chat** [#corsixth-general:matrix.org](https://matrix.to/#/#corsixth-general:matrix.org)
  - **Announcements** [#corsixth-announcements:matrix.org](https://matrix.to/#/#corsixth-announcements:matrix.org)
  - **Technical Discussion** (DevOps) [#corsixth-technical:matrix.org](https://matrix.to/#/#corsixth-technical:matrix.org)
  - **Help!** [#corsixth-help:matrix.org](https://matrix.to/#/#corsixth-help:matrix.org)
  - **Community Content** [#corsixth-usercontent:matrix.org](https://matrix.to/#/#corsixth-usercontent:matrix.org)
  
</details>

- Join the server on [Discord](https://discord.gg/Mxeztvh)
- Subscribe to our [Google Developer group](https://groups.google.com/g/corsix-th-dev)
