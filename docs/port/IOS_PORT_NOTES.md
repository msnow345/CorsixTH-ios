# CorsixTH on iOS — known limitations

Things the port does not do, or does differently, that are understood and accepted rather
than undiagnosed. Each entry says what the cause is and what the fix would be, so nobody
has to re-derive it.

## Input

### A trackpad pinch does not zoom (relevant to upstreaming)

`GameUI:onPinchUpdate` discards SDL's pinch accumulator when `touch_input` is true, so
that a two-finger pinch on the glass is not applied twice — once by the touch layer's own
anchored `setZoom`, and again a tick later, unanchored, through
`current_momentum.z`/`World:adjustZoom`.

The gate is the **platform** (`TH.GetCompileOptions().os == "ios"`, a compile-time
constant), not the gesture. An external trackpad — a Magic Keyboard on an iPad Pro, say —
sends `SDL_EVENT_PINCH_*` but **no finger events**, so the touch layer never runs and that
pinch now has no zoom path at all. Keyboard zoom hotkeys are also unavailable there, so a
trackpad user has no way to zoom.

Accepted because this port's user does not use a trackpad with their iPad.

**The fix, if anyone needs it:** suppress the accumulator only while our own recogniser
actually has a gesture in progress, rather than for the whole platform. The `touch_catch`
and `touch_gesture_end` dispatches already bracket exactly that interval — set a flag on
the UI in the first and clear it in the second, and gate on that instead of on
`touch_input`. No device detection required.

**This should be tightened before any of the touch work is offered upstream.** Shipped
as-is it would break trackpad zoom for every iPad user who has one.

### Features with no pointer route

Found by sweeping every `addKeyHandler` binding for ones that are the sole route to a
feature. These are hotkey-only and an iPad has no keyboard:

* **Selling a picked-up item.** `UIPlaceObjects:sell` (`place_objects.lua:359`) is bound
  only to `ingame_sellPickedUpItem` (`:96`); there is no button for it. Cancelling the
  placement works instead, so this is not blocking. Adding a button needs new art in an
  already-full dialog.
* **Camera bookmarks**, `ingame_storePosition_N` / `ingame_recallPosition_N`. No pointer
  route on any platform.
* **`global_pause_movie`.** Tapping during a movie stops it outright instead
  (`ui.lua:867`), so a movie can always be dismissed; it just cannot be paused.

Debug and developer bindings (cheat window, Lua console, screenshot, log dump, reset) are
deliberately not addressed.

### The mouse cursor sprite is not drawn

Deliberate: it is a picture of a mouse pointer, and with a finger it sits wherever the
last tap landed. One consequence is that cursor-shaped affordances are invisible — the
bank manager's pie-chart cursor, for instance, conveys nothing on iOS.
