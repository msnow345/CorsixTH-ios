/*
Copyright (c) 2009 Peter "Corsix" Cawley

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "sdl_core.h"

#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#endif

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string_view>

#ifdef CORSIX_TH_IOS
// CorsixTH-iOS @feature 2026-09-08 phys_footprint is the number iOS's jetsam
// ledger judges a memory kill against, so it is the one worth reporting.
#include <mach/mach.h>
#endif

#include "lua.hpp"
#include "lua_sdl.h"
#include "th_gfx.h"
#include "th_lua.h"
#include "th_sound.h"

namespace {

int l_init(lua_State* L) {
#ifdef CORSIX_TH_IOS
  // CorsixTH-iOS @feature 2026-09-07 the touch recogniser below synthesises
  // every mouse event the game sees, so SDL's own touch-as-mouse emulation must
  // be off or both paths deliver. It also presses on finger-down, which is the
  // premature click the deferred-tap state machine exists to avoid.
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
  Uint32 flags = 0;
  int i;
  int argc = lua_gettop(L);
  for (i = 1; i <= argc; ++i) {
    const char* s = luaL_checkstring(L, i);
    if (std::strcmp(s, "video") == 0)
      flags |= SDL_INIT_VIDEO;
    else if (std::strcmp(s, "audio") == 0)
      flags |= SDL_INIT_AUDIO;
    else
      luaL_argerror(L, i, "Expected SDL part name");
  }
  if (!SDL_Init(flags)) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    lua_pushboolean(L, 0);
    return 1;
  }
  if (!MIX_Init()) {
    std::fprintf(stderr, "MIX_Init failed: %s\n", SDL_GetError());
    lua_pushboolean(L, 0);
    return 1;
  }

  lua_pushboolean(L, 1);
  return 1;
}

#ifdef CORSIX_TH_IOS
// CorsixTH-iOS @feature 2026-09-08 set from the moment iOS tells us we are
// leaving the screen until it tells us we are back. While it is set the game is
// neither simulated nor presented. Read from SDL's timer thread as well as the
// main loop, hence atomic.
std::atomic<bool> app_backgrounded{false};
#endif

// SDL TimerCallback
Uint32 timer_frame_callback(void*, SDL_TimerID, Uint32 interval) {
#ifdef CORSIX_TH_IOS
  // CorsixTH-iOS @bugfix 2026-09-08 do not queue simulation ticks that will not
  // be run. They would sit in the event queue for the whole suspension and then
  // fast-forward the hospital by however long the player was away.
  if (app_backgrounded.load(std::memory_order_relaxed)) {
    return interval;
  }
#endif
  SDL_Event e;
  e.type = SDL_USEREVENT_TICK;
  SDL_PushEvent(&e);
  return interval;
}

#ifdef CORSIX_TH_IOS
// CorsixTH-iOS @feature 2026-09-06 pace *rendered* frames at the panel refresh
// rate while the camera is moving. Simulation is untouched: it stays on the
// 18 ms usertick above. This timer only posts a repaint request, and only while
// the Lua frame handler reports that something is still animating, so an idle
// game still repaints at the tick rate. Presentation remains on vsync, so this
// is not the busy loop that limit_fps == false produces.
std::atomic<bool> want_display_rate_frames{false};

Uint32 display_frame_callback(void*, SDL_TimerID, Uint32 interval) {
  if (want_display_rate_frames.load(std::memory_order_relaxed) &&
      !app_backgrounded.load(std::memory_order_relaxed)) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_USEREVENT_FRAME;
    SDL_PushEvent(&e);
  }
  return interval;
}

//! Interval at which to ask for a repaint, in whole milliseconds: half the
//! frame period of the display showing the window.
//! Half, so that the pace is set by the vsync inside SDL_RenderPresent rather
//! than by this timer's millisecond granularity. Asking exactly once per frame
//! period aliases against vsync and loses frames: measured 112 fps on a 120 Hz
//! panel at 8 ms, and a solid 120 fps at 4 ms. It is still a hard upper bound,
//! so this can never become the busy loop that limit_fps == false produces.
Uint32 display_frame_request_period_ms(SDL_Window* window) {
  float hz = 0.0f;
  const SDL_DisplayMode* mode =
      SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
  if (mode != nullptr) {
    hz = mode->refresh_rate;
  }
  if (hz <= 0.0f) {
    hz = 60.0f;
  }
  Uint32 period = static_cast<Uint32>(500.0f / hz);
  return period < 1 ? 1 : period;
}
#endif

class fps_ctrl {
 public:
  bool limit_fps{true};
  bool track_fps{true};

  size_t q_front{0};
  size_t q_back{0};
  int frame_count{0};
  std::array<Uint32, 4096> frame_time{};

  void init() {
    limit_fps = true;
    track_fps = true;
    q_front = 0;
    q_back = 0;
    frame_count = 0;
  }

  void count_frame() {
    Uint32 now = SDL_GetTicks();
    frame_time[q_front] = now;
    q_front = (q_front + 1) % frame_time.size();
    if (q_front == q_back) {
      q_back = (q_back + 1) % frame_time.size();
    } else {
      ++frame_count;
    }

    if (now < 1000) {
      now = 0;
    } else {
      now -= 1000;
    }

    while (frame_time[q_back] < now) {
      --frame_count;
      q_back = (q_back + 1) % frame_time.size();
    }
  }
};

fps_ctrl fps;
constexpr uint32_t infinite_loop_limit{100};
uint32_t infinite_loop_counter{0};

void l_infinite_loop_hook(lua_State* L, lua_Debug*) {
  infinite_loop_counter++;
  if (infinite_loop_counter >= infinite_loop_limit) {
    luaL_error(L, "Suspected infinite loop");
  }
}

void l_push_modifiers_table(lua_State* L, Uint16 mod) {
  lua_newtable(L);
  if ((mod & SDL_KMOD_SHIFT) != 0) {
    luaT_pushtablebool(L, "shift", true);
  }
  if ((mod & SDL_KMOD_ALT) != 0) {
    luaT_pushtablebool(L, "alt", true);
  }
  if ((mod & SDL_KMOD_CTRL) != 0) {
    luaT_pushtablebool(L, "ctrl", true);
  }
  if ((mod & SDL_KMOD_GUI) != 0) {
    luaT_pushtablebool(L, "gui", true);
  }
  if ((mod & SDL_KMOD_NUM) != 0) {
    luaT_pushtablebool(L, "numlockactive", true);
  }
}

int l_get_key_modifiers(lua_State* L) {
  l_push_modifiers_table(L, SDL_GetModState());
  return 1;
}

int l_quit([[maybe_unused]] lua_State* L) {
#ifdef CORSIX_TH_IOS
  // CorsixTH-iOS @bugfix 2026-09-08 an iOS app must never terminate itself.
  // SDL_EVENT_QUIT ends mainloop, and main() then either returns -- which iOS
  // records as a crash -- or leaves the app on screen with a dead render loop,
  // a frozen last frame the player has to force close from the app switcher.
  // Apple's guidance is explicit that an app must not offer a control that
  // quits it. The one legitimate use of this path is App:reset, which restarts
  // the Lua state inside the same process after a data-directory change; that
  // sets _RESTART in the registry first, so allow it and refuse everything
  // else. This is the backstop; the Lua side no longer offers the control.
  lua_getfield(L, LUA_REGISTRYINDEX, "_RESTART");
  const bool restarting = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  if (!restarting) {
    std::printf("SDL.quit() ignored: an iOS app does not quit itself.\n");
    std::fflush(stdout);
    return 0;
  }
#endif
  SDL_Event e;
  e.type = SDL_EVENT_QUIT;
  SDL_PushEvent(&e);
  return 0;
}

/// Lua CFunction error handler for dispatch calls
/**
 * Calls TheApp:errorHandler with the dispatch type and stacktrace of the
 * error.
 */
int l_error_handler(lua_State* L) {
  lua_getglobal(L, "debug");
  lua_getfield(L, -1, "traceback");
  int traceArgs = 1;
  if (lua_type(L, 1) == LUA_TSTRING) {
    traceArgs = 2;
    lua_pushvalue(L, 1);
  }
  lua_pushinteger(L, 2);  // skip this level of the traceback
  int err = lua_pcall(L, traceArgs, 1, 0);
  if (err != LUA_OK) {
    return err;
  }

  lua_getglobal(L, "TheApp");
  lua_getfield(L, -1, "errorHandler");
  lua_pushvalue(L, -2);  // TheApp
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_pushvalue(L, -5);  // The traceback result

  err = lua_pcall(L, 3, 0, 0);
  lua_pushinteger(L, err);
  return 1;
}

/// Add dispatch call to the stack
/**
 * The resulting lua stack becomes:
 * L(-4) - error handler function
 * L(-3) - dispatch function
 * L(-2) - TheApp global (first argument)
 * L(-1) - dispatch type string
 *
 * Further arguments can be added before calling with lua_pcall
 */
void push_app_dispatch(lua_State* L, std::string_view dispatch_event) {
  lua_pushlstring(L, dispatch_event.data(), dispatch_event.size());
  lua_pushcclosure(L, &l_error_handler, 1);
  lua_getglobal(L, "TheApp");
  lua_getfield(L, -1, "dispatch");
  lua_pushvalue(L, -2);
  lua_pushlstring(L, dispatch_event.data(), dispatch_event.size());

  lua_remove(L, -4);  // TheApp global
}

int l_track_fps(lua_State* L) {
  fps.track_fps = lua_isnone(L, 1) ? true : (lua_toboolean(L, 1) != 0);
  return 0;
}

int l_limit_fps(lua_State* L) {
  fps.limit_fps = lua_isnone(L, 1) ? true : (lua_toboolean(L, 1) != 0);
  return 0;
}

int l_get_fps(lua_State* L) {
  if (fps.track_fps) {
    lua_pushinteger(L, fps.frame_count);
  } else {
    lua_pushnil(L);
  }
  return 1;
}

int l_get_ticks(lua_State* L) {
  lua_pushinteger(L, SDL_GetTicks());
  return 1;
}

int l_start_text_input(lua_State* L) {
  render_target* rt = luaT_testuserdata<render_target>(L, 1);
  if (!SDL_StartTextInput(rt->get_window())) {
    std::fprintf(stderr, "start_text_input error: %s", SDL_GetError());
  }
  return 0;
}

int l_stop_text_input(lua_State* L) {
  render_target* rt = luaT_testuserdata<render_target>(L, 1);
  if (!SDL_StopTextInput(rt->get_window())) {
    std::fprintf(stderr, "stop_text_input error: %s", SDL_GetError());
  }
  return 0;
}

constexpr std::array<luaL_Reg, 10> sdllib{
    {{"init", l_init},
     {"quit", l_quit},
     {"getTicks", l_get_ticks},
     {"getKeyModifiers", l_get_key_modifiers},
     {"startTextInput", l_start_text_input},
     {"stopTextInput", l_stop_text_input},
     {"getFPS", l_get_fps},
     {"trackFPS", l_track_fps},
     {"limitFPS", l_limit_fps},
     {nullptr, nullptr}}};

void load_extra(lua_State* L, const char* name, lua_CFunction fn) {
  luaT_pushcfunction(L, fn);
  lua_call(L, 0, 1);
  lua_setfield(L, -2, name);
}

}  // namespace

constexpr std::string_view dispatch_keydown("keydown");
constexpr std::string_view dispatch_keyup("keyup");
constexpr std::string_view dispatch_textinput("textinput");
constexpr std::string_view dispatch_textediting("textediting");
constexpr std::string_view dispatch_buttondown("buttondown");
constexpr std::string_view dispatch_buttonup("buttonup");
constexpr std::string_view dispatch_mousewheel("mousewheel");
constexpr std::string_view dispatch_motion("motion");
constexpr std::string_view dispatch_pinch_begin("pinch_begin");
constexpr std::string_view dispatch_pinch_update("pinch_update");
constexpr std::string_view dispatch_pinch_end("pinch_end");
constexpr std::string_view dispatch_active("active");
constexpr std::string_view dispatch_music_over("music_over");
constexpr std::string_view dispatch_movie_over("movie_over");
constexpr std::string_view dispatch_sound_over("sound_over");
constexpr std::string_view dispatch_timer("timer");
constexpr std::string_view dispatch_callback("callback");
constexpr std::string_view dispatch_window_resized("window_resized");
constexpr std::string_view dispatch_window_pixel_size_changed(
    "window_pixel_size_changed");
constexpr std::string_view dispatch_window_display_scale_changed(
    "window_display_scale_changed");
constexpr std::string_view dispatch_window_maximized("window_maximized");
constexpr std::string_view dispatch_window_restored("window_restored");
constexpr std::string_view dispatch_frame("frame");

#ifdef CORSIX_TH_IOS
constexpr std::string_view dispatch_touch_camera("touch_camera");
constexpr std::string_view dispatch_touch_fling("touch_fling");
constexpr std::string_view dispatch_touch_catch("touch_catch");
constexpr std::string_view dispatch_touch_gesture_end("touch_gesture_end");
constexpr std::string_view dispatch_touch_double_tap("touch_double_tap");
constexpr std::string_view dispatch_touch_defer_tap("touch_defer_tap");
constexpr std::string_view dispatch_touch_hover_end("touch_hover_end");
constexpr std::string_view dispatch_touch_drag_query("touch_drag_query");
constexpr std::string_view dispatch_touch_rotate("touch_rotate");
constexpr std::string_view dispatch_touch_longpress_anchor(
    "touch_longpress_anchor");
constexpr std::string_view dispatch_app_suspend("app_suspend");
constexpr std::string_view dispatch_app_resume("app_resume");

// CorsixTH-iOS @feature 2026-09-07 translate raw touches into game input.
//
// SDL's own touch-as-mouse emulation is switched off in l_init, so every mouse
// event the Lua UI sees on iOS is produced here. The recogniser is deliberately
// ignorant of the game: it decides only *what kind* of gesture happened, and
// asks Lua (touch_drag_query) what a one-finger drag should mean at the point it
// started. Everything it emits goes through App:dispatch, which is the exact
// entry point real SDL mouse events use, so the 298 Lua UI files are unchanged.
//
// Two fingers navigate in every mode, always: pan by centroid and pinch, at the
// same time, with inertia. That is what frees the one finger to belong to
// whatever is going on where it landed -- and inside a placement it does,
// carrying or sizing and never moving the camera, so a stray finger cannot
// shift the map out from under a room being sized. Outside a placement there is
// nothing for it to be busy with, so it pans; see touch_one_finger_pan in
// game_ui.lua, which is the single switch controlling that.
//
// Two rules carried over from the GeneralsX port, each of which was a real bug
// there:
//   * a finger landing emits NOTHING but a motion. A button-down that is later
//     "cancelled" is still a real click to the game.
//   * the motion is emitted before the button-down of a tap, because CorsixTH's
//     UI is hover-driven (button highlights, tooltips, cursor entity).
//
// Coordinates: the main loop has already run SDL_ConvertEventToRenderCoordinates
// over the finger event, so tfinger.x/y arrive in *render* pixels, the same space
// the Lua UI works in. Synthetic events are therefore dispatched directly rather
// than pushed back into the SDL queue, which would convert them a second time.
namespace {
namespace touch {

enum class phase {
  idle,         // no fingers tracked
  pending,      // one finger down, gesture unidentified, no button emitted
  drag_pan,     // one finger past the dead zone, panning the camera
  drag_none,    // one finger past the dead zone with nothing to drag: a no-op
  drag_mouse,   // one finger past the dead zone, held left button (room sizing)
  drag_carry,   // one finger carrying a placement: motion only, no button held
  carry_armed,  // a second finger has landed on a carry, intent not yet known
  drag_wheel,   // one finger past the dead zone over a scrollable list
  longpress,    // long press fired (right click sent), swallow until lift
  two_pending,  // two fingers down, waiting for movement to start the gesture
  two_active    // two-finger pan and pinch, both live
};

//! What Lua says a one-finger drag starting at a given point means. Whether a
//! drag on open map comes back as `camera` or as `none` is the game's decision,
//! not this file's: see the touch_one_finger_pan switch in game_ui.lua.
enum class drag_mode {
  none = 0,    // nothing here to drag; swallow the gesture
  button = 1,  // held left-button drag: room sizing, sliders, window dragging
  wheel = 2,   // scroll the list under the finger
  carry = 3,   // position something on the map; lifting drops it
  camera = 4,  // pan the map 1:1, with a flick at the end
  //! A carry whose only way out is a right click. Handled exactly as `carry`,
  //! except that the long press is not suppressed: for a picked-up member of
  //! staff there is no cancel button anywhere on screen, so taking the long
  //! press away would leave someone holding a person they cannot put down.
  carry_cancellable = 5,
  //! A control that previews while held and acts on release -- a menu item.
  //! Drags like `button`, but is never turned into a right click: holding one
  //! is how you look at it before committing, so the hold must stay a hold and
  //! the lift must still activate it.
  preview = 6
};

//! Both carry modes drag the same way.
bool is_carry(int mode) {
  return mode == static_cast<int>(drag_mode::carry) ||
         mode == static_cast<int>(drag_mode::carry_cancellable);
}

//! Modes that drag as a held left button.
bool is_button_drag(int mode) {
  return mode == static_cast<int>(drag_mode::button) ||
         mode == static_cast<int>(drag_mode::preview);
}

//! 600 ms is an RTS figure, chosen where the thing under the finger is not
//! going anywhere. In CorsixTH the long press is how a member of staff is
//! picked up, and they walk, so the hold is the whole cost of catching one.
//! This is close to UILongPressGestureRecognizer's own 500 ms default.
constexpr Uint64 long_press_ms = 400;
//! Movement, in window points, before a press becomes a drag.
constexpr float dead_zone_pt = 8.0f;
//! Change in finger separation, in window points, before zoom engages at all.
//! Two fingers dragging in parallel never stay exactly parallel, so without an
//! activation distance that incidental divergence leaks into every pan.
constexpr float zoom_activate_pt = 22.0f;
//! A pinch must out-pace the pan to count, or a long enough pan eventually
//! accumulates finger drift into the activation distance and zooms unasked.
constexpr float zoom_vs_pan_ratio = 0.5f;
//! Ratio noise below this is not zoom.
constexpr double pinch_deadband = 0.004;
//! Drag distance, in window points, per synthetic wheel tick on a list.
constexpr float wheel_step_pt = 20.0f;
//! How long a second tap has to arrive to count as a double tap. Long enough
//! not to demand a sharp double, short enough that the deferred first tap is
//! not perceived as lag on the few things that defer at all.
constexpr Uint64 double_tap_window_ms = 300;
//! How far apart, in window points, two taps may be and still be a double. A
//! second tap further away than this is a second tap somewhere else.
constexpr float double_tap_max_move_pt = 32.0f;
//! Release velocity is measured over a real time window from timestamped
//! samples rather than a per-frame filter: people ease off as they lift, and a
//! filter turns a genuine flick into a stop.
constexpr int velocity_sample_count = 16;
constexpr double velocity_window_ms = 60.0;
//! Render pixels per millisecond. A hard flick on a 120 Hz panel is ~4.
constexpr double max_fling_speed = 8.0;

struct sample {
  double ms;
  float x;
  float y;
};

//! A button press whose release is waiting for a frame to be presented.
//! CorsixTH draws a button's pressed sprite only while it is held, from
//! active_button. A synthetic tap that dispatched down and up back to back set
//! and cleared that within a single pass of the event loop, so the pressed
//! state existed for zero rendered frames and no button in the game ever
//! flashed -- a regression against SDL's own touch-mouse emulation, which
//! delivered its down and up across separate frames. The down still goes out
//! the instant the finger lifts; only the up waits, for one frame, which is
//! 8 ms on this panel.
struct pending_release {
  bool active{false};
  int button{0};
  float x{0.0f};
  float y{0.0f};
};

//! A tap that has been held back to see whether a second one follows.
//! Only taps the game says are worth deferring are ever held, so nothing else
//! in the game pays any latency for this: see query_defer_tap.
struct deferred_tap {
  bool active{false};
  float x{0.0f};
  float y{0.0f};
  Uint64 ms{0};
};

struct state {
  phase ph{phase::idle};
  SDL_FingerID f1{0};
  SDL_FingerID f2{0};
  float f1x{0.0f}, f1y{0.0f};
  float f2x{0.0f}, f2y{0.0f};
  float down_x{0.0f}, down_y{0.0f};
  //! Reference point the next delta is measured from: the finger while one
  //! finger is down, the centroid while two are.
  float last_x{0.0f}, last_y{0.0f};
  Uint64 down_ticks{0};
  float two_start_x{0.0f}, two_start_y{0.0f}, two_start_dist{0.0f};
  float pinch_dist{0.0f};
  bool zoom_active{false};
  float zoom_ref_x{0.0f}, zoom_ref_y{0.0f};
  float wheel_accum{0.0f};
  //! Where the second finger of a carry landed, and whether it has moved. A
  //! second finger that lands and lifts without travelling is a rotate; one
  //! that travels is the start of a two-finger camera gesture.
  float f2_down_x{0.0f}, f2_down_y{0.0f};
  //! Set once the long press has been ruled out for this finger, so the check
  //! is not repeated on every pass of the main loop while it is held.
  bool long_press_suppressed{false};
  //! Set while gesture logging is on (CORSIXTH_TOUCH_LOG in the environment).
  int log_gestures{-1};
  //! Render pixels per window point, so thresholds stay physical.
  float render_scale{1.0f};
  std::array<sample, velocity_sample_count> samples{};
  int sample_next{0};
  int samples_held{0};
  float travel_x{0.0f}, travel_y{0.0f};
};

state s;
deferred_tap held_tap;
pending_release held_release;

double now_ms() {
  return static_cast<double>(SDL_GetTicksNS()) / 1'000'000.0;
}

float render_scale(SDL_Renderer* renderer) {
  float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
  if (!SDL_RenderCoordinatesFromWindow(renderer, 0.0f, 0.0f, &x0, &y0) ||
      !SDL_RenderCoordinatesFromWindow(renderer, 0.0f, 100.0f, &x1, &y1)) {
    return 1.0f;
  }
  const float scale = (y1 - y0) / 100.0f;
  return scale > 0.01f ? scale : 1.0f;
}

float dead_zone() { return dead_zone_pt * s.render_scale; }

float centroid_x() { return (s.f1x + s.f2x) * 0.5f; }
float centroid_y() { return (s.f1y + s.f2y) * 0.5f; }

float finger_distance() {
  const float dx = s.f1x - s.f2x;
  const float dy = s.f1y - s.f2y;
  return SDL_sqrtf(dx * dx + dy * dy);
}

//! Phases in which a second finger is being tracked.
bool two_finger_phase() {
  return s.ph == phase::two_pending || s.ph == phase::two_active ||
         s.ph == phase::carry_armed;
}

const char* phase_name(phase p) {
  switch (p) {
    case phase::idle: return "idle";
    case phase::pending: return "pending";
    case phase::drag_pan: return "drag_pan";
    case phase::drag_none: return "drag_none";
    case phase::drag_mouse: return "drag_mouse";
    case phase::drag_carry: return "drag_carry";
    case phase::carry_armed: return "carry_armed";
    case phase::drag_wheel: return "drag_wheel";
    case phase::longpress: return "longpress";
    case phase::two_pending: return "two_pending";
    case phase::two_active: return "two_active";
  }
  return "?";
}

//! Gesture-state logging, off unless CORSIXTH_TOUCH_LOG is set. Touch is
//! judged by feel on a device nobody can attach a debugger to, so being able to
//! read back the exact sequence of states a gesture went through is the only
//! way to tell "that felt wrong" from "that was recognised wrong".
void log_phase(phase from, phase to, const char* why) {
  if (s.log_gestures < 0) {
    const char* env = SDL_getenv("CORSIXTH_TOUCH_LOG");
    s.log_gestures = (env != nullptr && env[0] != '0') ? 1 : 0;
  }
  if (s.log_gestures == 1 && from != to) {
    std::printf("[touch] %s -> %s (%s)\n", phase_name(from), phase_name(to),
                why);
    std::fflush(stdout);
  }
}

void set_phase(phase to, const char* why) {
  log_phase(s.ph, to, why);
  s.ph = to;
}

void log_event(const char* what) {
  if (s.log_gestures == 1) {
    std::printf("[touch] %s (phase %s)\n", what, phase_name(s.ph));
    std::fflush(stdout);
  }
}

void reset_samples() {
  s.sample_next = 0;
  s.samples_held = 0;
  s.travel_x = 0.0f;
  s.travel_y = 0.0f;
}

void add_sample(float dx, float dy) {
  s.travel_x += dx;
  s.travel_y += dy;
  s.samples[s.sample_next] = {now_ms(), s.travel_x, s.travel_y};
  s.sample_next = (s.sample_next + 1) % velocity_sample_count;
  if (s.samples_held < velocity_sample_count) {
    ++s.samples_held;
  }
}

//! Average velocity over the newest samples spanning up to the window, in
//! render pixels per millisecond.
void sampled_velocity(double* vx, double* vy) {
  *vx = 0.0;
  *vy = 0.0;
  if (s.samples_held < 2) {
    return;
  }
  const double now = now_ms();
  const int newest =
      (s.sample_next - 1 + velocity_sample_count) % velocity_sample_count;
  int oldest = newest;
  for (int i = 1; i < s.samples_held; ++i) {
    const int idx = (s.sample_next - 1 - i + 2 * velocity_sample_count) %
                    velocity_sample_count;
    if (now - s.samples[idx].ms > velocity_window_ms) {
      break;
    }
    oldest = idx;
  }
  const double span = s.samples[newest].ms - s.samples[oldest].ms;
  if (span <= 0.0) {
    return;
  }
  double x = (s.samples[newest].x - s.samples[oldest].x) / span;
  double y = (s.samples[newest].y - s.samples[oldest].y) / span;
  const double speed = SDL_sqrt(x * x + y * y);
  if (speed > max_fling_speed) {
    x *= max_fling_speed / speed;
    y *= max_fling_speed / speed;
  }
  *vx = x;
  *vy = y;
}

//! Call TheApp:dispatch(name, ...) exactly as the main event switch does, and
//! report whether the handler asked for a repaint.
bool dispatch(lua_State* L, std::string_view name,
              std::initializer_list<double> args) {
  push_app_dispatch(L, name);
  for (double arg : args) {
    lua_pushnumber(L, arg);
  }
  const int nargs = 1 + static_cast<int>(args.size());
  bool repaint = false;
  if (lua_pcall(L, nargs + 1, 1, -3 - nargs) != LUA_OK) {
    std::fprintf(stderr, "Error in %.*s: %s\n", static_cast<int>(name.size()),
                 name.data(), lua_tostring(L, -1));
  } else {
    repaint = lua_toboolean(L, -1) != 0;
  }
  lua_pop(L, 2);
  return repaint;
}

bool emit_motion(lua_State* L, float x, float y) {
  return dispatch(L, dispatch_motion, {x, y, 0.0, 0.0});
}

bool emit_button(lua_State* L, bool down, int button, float x, float y) {
  return dispatch(L, down ? dispatch_buttondown : dispatch_buttonup,
                  {static_cast<double>(button), x, y});
}

//! Deliver a button release that was waiting for its frame. Called after the
//! frame has been presented, and defensively before anything else is dispatched,
//! so a release can never be dropped or arrive out of order behind a later
//! event. A dropped release would leave the game holding a mouse button down,
//! which is a great deal worse than a missing highlight.
bool flush_pending_release(lua_State* L) {
  if (!held_release.active) {
    return false;
  }
  held_release.active = false;
  const float x = held_release.x;
  const float y = held_release.y;
  bool repaint = emit_button(L, false, held_release.button, x, y);
  // A finger does not move away afterwards the way a mouse does, so the hover
  // the tap's leading motion applied would otherwise stay applied for ever --
  // every button tapped left looking hovered. Touch hover is transient: it
  // lasts for the touch and is released with it. The release point goes with
  // it, because only the window under it can have been left hovered.
  repaint = dispatch(L, dispatch_touch_hover_end, {x, y}) || repaint;
  return repaint;
}

bool emit_click(lua_State* L, int button, float x, float y) {
  // Any release still outstanding belongs to an earlier click and must land
  // before this one starts.
  bool repaint = flush_pending_release(L);
  // Motion first: CorsixTH highlights buttons, sets the cursor entity and
  // arms tooltips from hover, and a real mouse always moves before it clicks.
  repaint = emit_motion(L, x, y) || repaint;
  repaint = emit_button(L, true, button, x, y) || repaint;
  held_release.active = true;
  held_release.button = button;
  held_release.x = x;
  held_release.y = y;
  // Always ask for a frame: the whole point is that the press gets drawn.
  return true;
}

bool emit_wheel(lua_State* L, float x, float y, double wheel_y) {
  bool repaint = emit_motion(L, x, y);
  push_app_dispatch(L, dispatch_mousewheel);
  lua_pushnumber(L, 0.0);
  lua_pushnumber(L, wheel_y);
  lua_pushboolean(L, 1);
  lua_pushboolean(L, 0);
  const int nargs = 5;
  if (lua_pcall(L, nargs + 1, 1, -3 - nargs) != LUA_OK) {
    std::fprintf(stderr, "Error in mousewheel: %s\n", lua_tostring(L, -1));
  } else {
    repaint = (lua_toboolean(L, -1) != 0) || repaint;
  }
  lua_pop(L, 2);
  return repaint;
}

//! Ask Lua where a long press that began under an entity should land.
//! The click cannot go to the point the finger pressed: patients and staff walk,
//! and by the time the timer fires that point is bare floor -- which is exactly
//! why a shorter hold, or a double tap, would not fix this on its own. Lua
//! answers with where the thing that was under the finger is *now*.
//!return (bool) Whether an anchor was supplied; x and y are left alone if not.
bool query_long_press_anchor(lua_State* L, float* x, float* y) {
  push_app_dispatch(L, dispatch_touch_longpress_anchor);
  const int nargs = 1;
  if (lua_pcall(L, nargs + 1, 2, -3 - nargs) != LUA_OK) {
    std::fprintf(stderr, "Error in touch_longpress_anchor: %s\n",
                 lua_tostring(L, -1));
    lua_pop(L, 2);
    return false;
  }
  bool anchored = false;
  if (lua_isnumber(L, -2) && lua_isnumber(L, -1)) {
    *x = static_cast<float>(lua_tonumber(L, -2));
    *y = static_cast<float>(lua_tonumber(L, -1));
    anchored = true;
  }
  lua_pop(L, 3);
  return anchored;
}

//! Ask Lua what a one-finger drag starting here should do. The recogniser owns
//! gesture identity; the game owns what a gesture means, and only the game
//! knows whether a room is being sized or an object carried.
int query_drag_mode(lua_State* L, float x, float y) {
  push_app_dispatch(L, dispatch_touch_drag_query);
  lua_pushnumber(L, x);
  lua_pushnumber(L, y);
  const int nargs = 3;
  int mode = 0;
  if (lua_pcall(L, nargs + 1, 1, -3 - nargs) != LUA_OK) {
    std::fprintf(stderr, "Error in touch_drag_query: %s\n",
                 lua_tostring(L, -1));
  } else if (lua_isnumber(L, -1)) {
    mode = static_cast<int>(lua_tointeger(L, -1));
  }
  lua_pop(L, 2);
  return mode;
}

//! Would a double tap here do something? Asked only when a tap has already
//! completed, and answered by the game, which is the only thing that knows what
//! is under the finger. A false answer -- every button, every room, every
//! patient, empty floor -- means the tap is delivered immediately, so the
//! deferral below is confined to the handful of things a double tap acts on.
bool query_defer_tap(lua_State* L, float x, float y) {
  push_app_dispatch(L, dispatch_touch_defer_tap);
  lua_pushnumber(L, x);
  lua_pushnumber(L, y);
  const int nargs = 3;
  bool defer = false;
  if (lua_pcall(L, nargs + 1, 1, -3 - nargs) != LUA_OK) {
    std::fprintf(stderr, "Error in touch_defer_tap: %s\n", lua_tostring(L, -1));
  } else {
    defer = lua_toboolean(L, -1) != 0;
  }
  lua_pop(L, 2);
  return defer;
}

//! Deliver a tap that was held back, as the ordinary click it always was.
bool flush_held_tap(lua_State* L) {
  if (!held_tap.active) {
    return false;
  }
  held_tap.active = false;
  log_event("held tap released as a single tap");
  return emit_click(L, 1, held_tap.x, held_tap.y);
}

//! Is this press close enough, and soon enough, to be the second of a pair?
bool continues_held_tap(float x, float y) {
  if (!held_tap.active) {
    return false;
  }
  if (SDL_GetTicks() - held_tap.ms >= double_tap_window_ms) {
    return false;
  }
  const float dx = x - held_tap.x;
  const float dy = y - held_tap.y;
  return SDL_sqrtf(dx * dx + dy * dy) <= double_tap_max_move_pt * s.render_scale;
}

void begin_two_pending() {
  s.two_start_x = centroid_x();
  s.two_start_y = centroid_y();
  s.two_start_dist = finger_distance();
  s.last_x = s.two_start_x;
  s.last_y = s.two_start_y;
  s.pinch_dist = s.two_start_dist;
  set_phase(phase::two_pending, "second finger");
}

//! Start the live two-finger gesture. The pinch reference is re-taken here so
//! the spread that happened while the gesture was still being recognised does
//! not land as one lurch of zoom on the first frame.
void begin_two_active() {
  s.last_x = centroid_x();
  s.last_y = centroid_y();
  s.pinch_dist = finger_distance();
  s.zoom_active = false;
  s.zoom_ref_x = s.last_x;
  s.zoom_ref_y = s.last_y;
  reset_samples();
  set_phase(phase::two_active, "two fingers moved");
}

//! One-finger pan. The delta is measured from the previous reported position,
//! so the ground under the finger stays under the finger.
bool pan_to(lua_State* L, float x, float y) {
  const float dx = x - s.last_x;
  const float dy = y - s.last_y;
  s.last_x = x;
  s.last_y = y;
  add_sample(dx, dy);
  if (dx == 0.0f && dy == 0.0f) {
    return false;
  }
  return dispatch(L, dispatch_touch_camera, {dx, dy, 1.0, x, y});
}

//! Pan and zoom in the same message, so they are applied in the same frame and
//! neither is locked out by the other. Zoom stays dormant until the fingers
//! deliberately change separation faster than they are travelling, which is
//! what stops a straight two-finger pan drifting the zoom.
bool two_finger_update(lua_State* L) {
  const float cx = centroid_x();
  const float cy = centroid_y();
  const float dx = cx - s.last_x;
  const float dy = cy - s.last_y;
  s.last_x = cx;
  s.last_y = cy;
  add_sample(dx, dy);

  double ratio = 1.0;
  const float dist = finger_distance();
  if (!s.zoom_active) {
    const float activate = zoom_activate_pt * s.render_scale;
    const float separation = SDL_fabsf(dist - s.pinch_dist);
    const float pan_dx = cx - s.zoom_ref_x;
    const float pan_dy = cy - s.zoom_ref_y;
    const float pan_travel = SDL_sqrtf(pan_dx * pan_dx + pan_dy * pan_dy);
    if (separation >= activate && separation >= pan_travel * zoom_vs_pan_ratio) {
      s.zoom_active = true;
      s.pinch_dist = dist;
      s.zoom_ref_x = cx;
      s.zoom_ref_y = cy;
    } else if (pan_travel >= activate) {
      // Still panning: re-take the reference so incidental drift cannot
      // accumulate across a long drag and trip the threshold on its own.
      s.pinch_dist = dist;
      s.zoom_ref_x = cx;
      s.zoom_ref_y = cy;
    }
  } else if (s.pinch_dist > 1.0f && dist > 1.0f) {
    const double candidate = static_cast<double>(dist) / s.pinch_dist;
    if (SDL_fabs(candidate - 1.0) >= pinch_deadband) {
      ratio = candidate;
      s.pinch_dist = dist;
    }
  }

  if (dx == 0.0f && dy == 0.0f && ratio == 1.0) {
    return false;
  }
  return dispatch(L, dispatch_touch_camera, {dx, dy, ratio, cx, cy});
}

bool release_fling(lua_State* L) {
  double vx = 0.0;
  double vy = 0.0;
  sampled_velocity(&vx, &vy);
  return dispatch(L, dispatch_touch_fling, {vx, vy});
}

bool handle(lua_State* L, render_target* target, const SDL_Event& e) {
  // Nothing may get between a press and its release.
  bool repaint_release = flush_pending_release(L);
  const float px = e.tfinger.x;
  const float py = e.tfinger.y;
  const SDL_FingerID id = e.tfinger.fingerID;
  bool repaint = repaint_release;

  switch (e.type) {
    case SDL_EVENT_FINGER_DOWN:
      switch (s.ph) {
        case phase::idle:
          s.render_scale = render_scale(target->get_renderer());
          s.f1 = id;
          s.f1x = px;
          s.f1y = py;
          s.down_x = s.last_x = px;
          s.down_y = s.last_y = py;
          s.down_ticks = SDL_GetTicks();
          s.long_press_suppressed = false;
          if (!continues_held_tap(px, py)) {
            // Too late, or too far away, to be the second of a pair: whatever
            // this press turns out to be, the earlier tap was a single one.
            repaint = flush_held_tap(L) || repaint;
          }
          set_phase(phase::pending, "first finger down");
          reset_samples();
          // A finger landing catches a coasting map, as it does in any iOS
          // scroll view.
          repaint = dispatch(L, dispatch_touch_catch, {}) || repaint;
          // Hover before anything else; no button is committed yet.
          repaint = emit_motion(L, px, py) || repaint;
          break;
        case phase::pending:
          repaint = flush_held_tap(L) || repaint;
          s.f2 = id;
          s.f2x = px;
          s.f2y = py;
          s.f2_down_x = px;
          s.f2_down_y = py;
          if (is_carry(query_drag_mode(L, s.down_x, s.down_y))) {
            // Something is being placed. A second finger here is a rotate until
            // it moves, exactly as it is once the carry is under way.
            set_phase(phase::carry_armed, "second finger during placement");
          } else {
            begin_two_pending();
          }
          break;
        case phase::drag_carry:
          // Nothing has been committed for this finger, so arming it costs
          // nothing to undo: a tap rotates, a travel becomes a camera gesture.
          s.f2 = id;
          s.f2x = px;
          s.f2y = py;
          s.f2_down_x = px;
          s.f2_down_y = py;
          set_phase(phase::carry_armed, "second finger on carry");
          break;
        case phase::drag_pan:
        case phase::drag_none:
          // The finger was already dragging deliberately. A second one landing
          // can only mean "move the view", so start immediately rather than
          // making the user re-cross a dead zone, which would throw away the
          // opening travel of every two-finger gesture begun this way.
          s.f2 = id;
          s.f2x = px;
          s.f2y = py;
          begin_two_active();
          break;
        case phase::drag_mouse:
          s.f2 = id;
          s.f2x = px;
          s.f2y = py;
          repaint = emit_button(L, false, 1, s.last_x, s.last_y) || repaint;
          begin_two_pending();
          break;
        case phase::drag_wheel:
          s.f2 = id;
          s.f2x = px;
          s.f2y = py;
          begin_two_pending();
          break;
        default:
          break;
      }
      break;

    case SDL_EVENT_FINGER_MOTION:
      if (s.ph == phase::idle) {
        break;
      }
      if (id == s.f1) {
        s.f1x = px;
        s.f1y = py;
      } else if (two_finger_phase() && id == s.f2) {
        s.f2x = px;
        s.f2y = py;
      } else {
        break;
      }

      switch (s.ph) {
        case phase::pending: {
          if (id != s.f1) {
            break;
          }
          const float dx = px - s.down_x;
          const float dy = py - s.down_y;
          if (SDL_sqrtf(dx * dx + dy * dy) < dead_zone()) {
            break;
          }
          // Committing to anything other than a tap settles the question:
          // release the held tap first so it cannot arrive after this gesture.
          repaint = flush_held_tap(L) || repaint;
          const int mode = query_drag_mode(L, s.down_x, s.down_y);
          if (is_button_drag(mode)) {
            // Anchor the press at the original touch point: room sizing starts
            // its rectangle where the finger first landed.
            repaint = emit_motion(L, s.down_x, s.down_y) || repaint;
            repaint = emit_button(L, true, 1, s.down_x, s.down_y) || repaint;
            repaint = emit_motion(L, px, py) || repaint;
            s.last_x = px;
            s.last_y = py;
            set_phase(phase::drag_mouse, "drag over a dialog or room sizing");
          } else if (mode == static_cast<int>(drag_mode::wheel)) {
            s.wheel_accum = 0.0f;
            s.last_x = px;
            s.last_y = py;
            set_phase(phase::drag_wheel, "drag over a list");
          } else if (is_carry(mode)) {
            // No button is pressed while carrying. That is the whole point: a
            // second finger, or a cancelled touch, can end the gesture without
            // an outstanding press that the game would read as "place it here".
            s.last_x = px;
            s.last_y = py;
            set_phase(phase::drag_carry, "drag while placing");
            repaint = emit_motion(L, px, py) || repaint;
          } else if (mode == static_cast<int>(drag_mode::camera)) {
            // Pan from the original touch point rather than from here, so the
            // ground under the finger stays under the finger from the first
            // pixel and the dead-zone travel is not thrown away.
            set_phase(phase::drag_pan, "drag on the map");
            reset_samples();
            s.last_x = s.down_x;
            s.last_y = s.down_y;
            repaint = pan_to(L, px, py) || repaint;
          } else {
            // Nothing here to drag. CorsixTH has no drag-box selection, so this
            // is deliberately inert: it emits nothing now and nothing on
            // release, rather than leaving a stray click or a selection behind.
            s.last_x = px;
            s.last_y = py;
            set_phase(phase::drag_none, "drag with nothing under it");
          }
          break;
        }
        case phase::drag_pan:
          if (id == s.f1) {
            repaint = pan_to(L, px, py) || repaint;
          }
          break;
        case phase::drag_none:
          break;
        case phase::drag_mouse:
          if (id == s.f1) {
            s.last_x = px;
            s.last_y = py;
            repaint = emit_motion(L, px, py) || repaint;
          }
          break;
        case phase::drag_carry:
          if (id == s.f1) {
            s.last_x = px;
            s.last_y = py;
            repaint = emit_motion(L, px, py) || repaint;
          }
          break;
        case phase::carry_armed: {
          if (id == s.f2) {
            // Only the second finger decides. Judging this on the centroid
            // would let the unavoidable wobble of the carrying finger turn a
            // rotate tap into a camera pan.
            const float fdx = px - s.f2_down_x;
            const float fdy = py - s.f2_down_y;
            if (SDL_sqrtf(fdx * fdx + fdy * fdy) >= dead_zone()) {
              begin_two_active();
            }
            break;
          }
          // The carrying finger keeps carrying while the second finger's
          // intent is undecided.
          s.last_x = px;
          s.last_y = py;
          repaint = emit_motion(L, px, py) || repaint;
          break;
        }
        case phase::drag_wheel: {
          if (id != s.f1) {
            break;
          }
          s.wheel_accum += py - s.last_y;
          s.last_x = px;
          s.last_y = py;
          const float step = wheel_step_pt * s.render_scale;
          // Dragging the content down reveals what is above it, which is a
          // wheel scroll away from the viewer.
          while (s.wheel_accum >= step) {
            s.wheel_accum -= step;
            repaint = emit_wheel(L, px, py, 1.0) || repaint;
          }
          while (s.wheel_accum <= -step) {
            s.wheel_accum += step;
            repaint = emit_wheel(L, px, py, -1.0) || repaint;
          }
          break;
        }
        case phase::two_pending: {
          const float pan_dx = centroid_x() - s.two_start_x;
          const float pan_dy = centroid_y() - s.two_start_y;
          const float pan_travel =
              SDL_sqrtf(pan_dx * pan_dx + pan_dy * pan_dy);
          const float pinch_travel =
              SDL_fabsf(finger_distance() - s.two_start_dist);
          // Either kind of movement starts the gesture, and neither is chosen
          // over the other, so there is no wrong choice to be stuck with.
          if (pan_travel >= dead_zone() || pinch_travel >= dead_zone()) {
            begin_two_active();
          }
          break;
        }
        case phase::two_active:
          repaint = two_finger_update(L) || repaint;
          break;
        default:
          break;
      }
      break;

    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
      if (s.ph == phase::idle) {
        break;
      }
      if (id != s.f1 && !(two_finger_phase() && id == s.f2)) {
        break;
      }
      if (s.ph == phase::carry_armed && id == s.f2) {
        // The second finger landed and left without travelling: that is the
        // rotate tap. The carry is untouched, so the gesture simply continues.
        if (e.type == SDL_EVENT_FINGER_UP) {
          log_event("rotate tap");
          repaint = dispatch(L, dispatch_touch_rotate, {}) || repaint;
        }
        s.f2 = 0;
        // Put the pointer back under the carrying finger so the ghost is drawn
        // where the finger is, whatever the rotate changed about its shape.
        repaint = emit_motion(L, s.last_x, s.last_y) || repaint;
        set_phase(phase::drag_carry, "rotate tap released");
        break;
      }
      switch (s.ph) {
        case phase::pending:
          // A cancelled touch (a call, the notification shade, palm rejection)
          // must not become a committed click.
          if (e.type != SDL_EVENT_FINGER_UP) {
            repaint = flush_held_tap(L) || repaint;
            break;
          }
          if (continues_held_tap(s.down_x, s.down_y)) {
            // Second of a pair. The first was never delivered, so there is no
            // click to undo and nothing flashes on screen in between.
            held_tap.active = false;
            log_event("double tap");
            repaint = dispatch(L, dispatch_touch_double_tap, {}) || repaint;
            break;
          }
          if (query_defer_tap(L, s.down_x, s.down_y)) {
            // Hold it back just long enough to see whether a second follows.
            held_tap.active = true;
            held_tap.x = s.down_x;
            held_tap.y = s.down_y;
            held_tap.ms = SDL_GetTicks();
            log_event("tap held, waiting for a possible double");
            break;
          }
          // Everything else -- every button, every room, every patient, bare
          // floor -- clicks immediately, exactly as before.
          repaint = emit_click(L, 1, s.down_x, s.down_y) || repaint;
          break;
        case phase::drag_pan:
        case phase::two_active:
          repaint = release_fling(L) || repaint;
          break;
        case phase::drag_none:
          break;
        case phase::drag_mouse:
          repaint = emit_button(L, false, 1, s.last_x, s.last_y) || repaint;
          break;
        case phase::drag_carry:
        case phase::carry_armed:
          // Lifting the carrying finger drops what it was carrying, at the
          // point it was lifted from. Never on a cancelled touch: that would
          // place a building because a phone call arrived.
          if (e.type == SDL_EVENT_FINGER_UP) {
            repaint = emit_click(L, 1, s.last_x, s.last_y) || repaint;
          }
          break;
        default:
          break;
      }
      set_phase(phase::idle, e.type == SDL_EVENT_FINGER_UP ? "finger up"
                                                           : "touch cancelled");
      s.f1 = 0;
      s.f2 = 0;
      // Every route out of a gesture reports the end, including the ones that
      // emit nothing else: a cancelled carry, a cancelled press, an inert drag,
      // a two-finger gesture that never started. Some game state is armed by a
      // pointer entering a region and disarmed by it leaving -- edge scrolling
      // is -- and a finger leaving the glass is neither, so without this a
      // touch cancelled by an incoming call or a Control Centre swipe can leave
      // the camera running until something else happens to touch the screen.
      // Said once, here, rather than per phase, so no future phase can forget.
      repaint = dispatch(L, dispatch_touch_gesture_end, {}) || repaint;
      break;

    default:
      break;
  }
  return repaint;
}

//! Polled from the main loop rather than driven by events: a perfectly
//! stationary finger produces no SDL events, so an event-driven long press
//! would never fire.
bool poll_long_press(lua_State* L) {
  bool repaint_expired = false;
  if (held_tap.active && s.ph == phase::idle &&
      SDL_GetTicks() - held_tap.ms >= double_tap_window_ms) {
    repaint_expired = flush_held_tap(L);
  }
  if (s.ph != phase::pending) {
    return repaint_expired;
  }
  if (SDL_GetTicks() - s.down_ticks < long_press_ms) {
    return repaint_expired;
  }
  if (s.long_press_suppressed) {
    return repaint_expired;
  }
  bool repaint_held = flush_held_tap(L) || repaint_expired;
  const int held_mode = query_drag_mode(L, s.down_x, s.down_y);
  if (held_mode == static_cast<int>(drag_mode::carry)) {
    // Right click undoes a placement, and holding still is exactly what someone
    // lining an object up does. Take the long press out of every placement
    // mode rather than have it throw the placement away mid-aim. The phase is
    // deliberately left alone, so this finger can still tap or start a carry.
    //
    // Matched exactly, so a `carry_cancellable` -- a person in hand, with no
    // cancel button anywhere on screen -- keeps the long press that puts them
    // back down.
    s.long_press_suppressed = true;
    log_event("long press suppressed while placing");
    return repaint_held;
  }
  if (held_mode == static_cast<int>(drag_mode::preview)) {
    // A menu item. Holding one is how you see what you are about to activate,
    // so the hold must not turn into a right click the item ignores and then
    // swallow the lift that was going to choose it. The phase is left in
    // `pending`, so the highlight the press already applied stays up for as
    // long as the finger does, sliding to another item still works, and the
    // release still activates whatever is under it.
    s.long_press_suppressed = true;
    log_event("long press suppressed: held control previews instead");
    return repaint_held;
  }
  // Follow whatever was under the finger, if it has walked off since.
  float ax = s.down_x;
  float ay = s.down_y;
  const bool anchored = query_long_press_anchor(L, &ax, &ay);
  // No left button was ever sent, so this is a pure right click.
  bool repaint = emit_click(L, 3, ax, ay) || repaint_held;
  // Release it now rather than after the frame. Picking an object up happens on
  // the button UP -- Object:onClick is reached from UI:onMouseUp -- so whether
  // anything was picked up is simply not answerable until the release has
  // landed. Nothing is lost by not waiting: the deferred release exists so a
  // left click's pressed sprite gets a frame to be drawn in, and a right click
  // arms no such sprite.
  repaint = flush_pending_release(L) || repaint;

  // Did that pick something up? Then the finger is still down and the gesture
  // is not over: it continues as a carry, so the object follows immediately
  // rather than making the user lift and touch down again to move what they
  // have just picked up.
  //
  // Matched exactly against `carry`, not is_carry(), and deliberately so. A
  // `carry_cancellable` is a person, and picking a person up is expected to
  // outlive the gesture -- they are carried across a two-finger pan and put
  // down somewhere else entirely -- so that one still ends here and is resumed
  // by a later drag. Objects do not walk away and are placed where they were
  // picked up from, give or take, so for them one continuous gesture is right.
  if (query_drag_mode(L, s.f1x, s.f1y) ==
      static_cast<int>(drag_mode::carry)) {
    // From where the finger is NOW. Anything else and the object jumps by
    // however far the finger drifted during the hold -- which is bounded by
    // the drag dead zone, but visible, and it would land on the wrong tile.
    s.last_x = s.f1x;
    s.last_y = s.f1y;
    repaint = emit_motion(L, s.f1x, s.f1y) || repaint;
    set_phase(phase::drag_carry, "long press picked something up: carry on");
    return repaint;
  }

  set_phase(phase::longpress,
            anchored ? "held: right click on the entity it started on"
                     : "held: right click where it started");
  return repaint;
}

//! Drop the mouse events SDL synthesises from touches. The hint below should
//! already prevent them; this is the belt to that pair of braces, so the two
//! paths can never both deliver.
bool is_emulated_mouse(const SDL_Event& e) {
  switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION:
      return e.motion.which == SDL_TOUCH_MOUSEID;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      return e.button.which == SDL_TOUCH_MOUSEID;
    case SDL_EVENT_MOUSE_WHEEL:
      return e.wheel.which == SDL_TOUCH_MOUSEID;
    default:
      return false;
  }
}

//! Wind up whatever gesture is in flight, because the app is about to leave
//! the screen.
/*!
    Backgrounding mid-gesture is the one way the deferred-release machinery can
    lose a button. `emit_click` holds the mouse-up back for exactly one
    presented frame so the pressed sprite is actually drawn, and the flush lives
    in the frame path -- which is precisely what suspension shuts down. Without
    this, home-swiping during the flash of a tap resumes the game with the left
    button still down, and the next finger anywhere on the map drags a selection
    from wherever the tap was.

    iOS does also send SDL_EVENT_FINGER_CANCELED for the touches it takes away,
    but it is queued, so it arrives after the suspension rather than before it,
    and it never arrives at all for the deferred release, which is not a touch
    any more. So do it here, from the app-lifecycle watch, while we still run.
    The phase is left idle, which makes the cancel events that follow no-ops.
*/
void cancel_for_suspend(lua_State* L) {
  const bool anything = held_release.active || held_tap.active ||
                        s.ph != phase::idle;
  if (!anything) {
    return;
  }
  log_event("app suspending: winding up the gesture");
  // A tap held back to see whether a second one follows will never get its
  // answer now, so deliver it as the single tap it turned out to be. This runs
  // before the release flush because emit_click arms a fresh deferred release.
  flush_held_tap(L);
  // Then the deferred mouse-up: the game must never be left holding a button.
  flush_pending_release(L);
  if (s.ph == phase::drag_mouse) {
    // The only phase that holds a real button down for the length of the drag.
    emit_button(L, false, 1, s.last_x, s.last_y);
  }
  s.zoom_active = false;
  set_phase(phase::idle, "app suspending");
  s.f1 = 0;
  s.f2 = 0;
  reset_samples();
  // Same reasoning as the finger-up path: some game state is armed by the
  // pointer entering a region and disarmed by it leaving, and a suspension is
  // neither, so say the gesture ended.
  dispatch(L, dispatch_touch_gesture_end, {});
  dispatch(L, dispatch_touch_hover_end, {s.last_x, s.last_y});
}

}  // namespace touch
}  // namespace
#endif

#ifdef CORSIX_TH_IOS
// CorsixTH-iOS @feature 2026-09-08 iOS application lifecycle.
//
// The six application events are never queued. SDL_SendAppEvent hands
// SDL_EVENT_WILL_ENTER_BACKGROUND and its siblings straight to the event
// watchers and returns without touching the event queue, exactly because they
// have to be handled inside the UIApplicationDelegate call stack -- which is
// also the only window in which iOS still lets the app do work such as writing
// a save. A `case` for one of them in the main loop's switch can therefore
// never run, so everything here hangs off SDL_AddEventWatch.
namespace {
namespace lifecycle {

//! The state to dispatch into, valid for the length of one mainloop call.
lua_State* watch_lua = nullptr;

//! Re-entrancy guard. SDL only pumps the UIKit run loop from SDL_PumpEvents, so
//! in practice the watch fires from SDL_WaitEvent/SDL_PollEvent with a balanced
//! Lua stack and nothing part-dispatched; a nested notification would not be
//! survivable, so refuse one rather than corrupt the stack.
bool dispatching = false;

//! Current and peak physical footprint of the process, in MB.
void log_memory(const char* when) {
  task_vm_info_data_t info{};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO,
                reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
    return;
  }
  constexpr double mb = 1024.0 * 1024.0;
  if (count >= TASK_VM_INFO_REV1_COUNT) {
    std::printf("[lifecycle] %s: footprint %.1f MB (peak %.1f MB)\n", when,
                static_cast<double>(info.phys_footprint) / mb,
                static_cast<double>(info.ledger_phys_footprint_peak) / mb);
  } else {
    std::printf("[lifecycle] %s: footprint %.1f MB\n", when,
                static_cast<double>(info.phys_footprint) / mb);
  }
  std::fflush(stdout);
}

//! Everything that has to happen before the app leaves the screen, in the order
//! it has to happen in. Runs inside the delegate callback, so this is real time
//! against iOS's transition budget: it is timed and logged for that reason.
void on_suspend() {
  if (watch_lua == nullptr || dispatching) {
    return;
  }
  dispatching = true;
  const Uint64 started = SDL_GetTicks();
  // Wind up any gesture first, so no button is left held across the gap.
  touch::cancel_for_suspend(watch_lua);
  // Then let Lua write the config, the hotkeys and an autosave.
  touch::dispatch(watch_lua, dispatch_app_suspend, {});
  std::printf("[lifecycle] suspend handler took %u ms\n",
              static_cast<unsigned>(SDL_GetTicks() - started));
  std::fflush(stdout);
  dispatching = false;
}

void on_resume() {
  if (watch_lua == nullptr || dispatching) {
    return;
  }
  dispatching = true;
  touch::dispatch(watch_lua, dispatch_app_resume, {});
  dispatching = false;
}

//! Drop anything the transition left in the queue that would advance the game.
//! Simulation ticks stop being pushed the moment app_backgrounded is set, but a
//! handful can already be in flight when it is, and every one of them is a
//! whole 18 ms of hospital that the player was not there for. Counted rather
//! than SDL_FlushEvent'd so the log says how much time was actually saved.
void discard_stale_frames(const char* when) {
  std::array<SDL_Event, 64> drop{};
  int ticks = 0;
  int frames = 0;
  int n;
  while ((n = SDL_PeepEvents(drop.data(), static_cast<int>(drop.size()),
                             SDL_GETEVENT, SDL_USEREVENT_TICK,
                             SDL_USEREVENT_TICK)) > 0) {
    ticks += n;
  }
  while ((n = SDL_PeepEvents(drop.data(), static_cast<int>(drop.size()),
                             SDL_GETEVENT, SDL_USEREVENT_FRAME,
                             SDL_USEREVENT_FRAME)) > 0) {
    frames += n;
  }
  if (ticks != 0 || frames != 0) {
    std::printf(
        "[lifecycle] %s: discarded %d queued ticks (%d ms of simulation) and "
        "%d repaint requests\n",
        when, ticks, ticks * usertick_period_ms, frames);
    std::fflush(stdout);
  }
}

//! SDL_GetTicks at the last background transition, for the resume log.
Uint64 backgrounded_at = 0;

//! Whether the save has already been written for the background we are in.
/*!
    WILL_ENTER_BACKGROUND is applicationWillResignActive, and an app that was
    never active does not resign: measured on the iPad with the screen asleep,
    an app launched into the background gets DID_ENTER_BACKGROUND and nothing
    else at all. Doing the save only on WILL would silently skip it there, so
    DID is a fallback rather than merely a belt to its braces.
*/
bool saved_for_this_background = false;

//! Stop the world and write it down. Idempotent within one background episode.
void go_to_background(const char* why) {
  want_display_rate_frames.store(false, std::memory_order_relaxed);
  if (saved_for_this_background) {
    return;
  }
  saved_for_this_background = true;
  th::sound::pause_audio_device();
  backgrounded_at = SDL_GetTicks();
  on_suspend();
  log_memory(why);
}

bool SDLCALL watch(void*, SDL_Event* e) {
  switch (e->type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_BACKGROUND:
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
    case SDL_EVENT_DID_ENTER_FOREGROUND:
    case SDL_EVENT_LOW_MEMORY:
    case SDL_EVENT_TERMINATING:
      break;
    default:
      // The watch is called for every event on whichever thread pushed it --
      // the simulation tick arrives here on SDL's timer thread -- so get out
      // before touching anything that is not thread safe.
      return true;
  }
  if (!SDL_IsMainThread()) {
    return true;
  }

  switch (e->type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
      // applicationWillResignActive, and the preferred moment for all of this:
      // the app is still active, so nothing here is spent against the system's
      // background-transition budget. Stop presenting here rather than at
      // DID_ENTER_BACKGROUND too -- drawing into a drawable the compositor is
      // about to take away is what queues the acquire timeouts that surface as
      // a multi-second input hang on the way back in.
      app_backgrounded.store(true, std::memory_order_relaxed);
      go_to_background("entering background");
      break;

    case SDL_EVENT_DID_ENTER_BACKGROUND:
      // From here the app can lose the CPU at any moment, so nothing may still
      // be running -- and if there was no resign-active, this is the only
      // notice we get, so the save happens here instead.
      app_backgrounded.store(true, std::memory_order_relaxed);
      go_to_background("entered background");
      discard_stale_frames("did enter background");
      break;

    case SDL_EVENT_WILL_ENTER_FOREGROUND:
      // Order matters: drop the stale ticks before letting the timer queue new
      // ones, or a tick pushed in between survives the flush.
      discard_stale_frames("will enter foreground");
      saved_for_this_background = false;
      app_backgrounded.store(false, std::memory_order_relaxed);
      break;

    case SDL_EVENT_DID_ENTER_FOREGROUND:
      app_backgrounded.store(false, std::memory_order_relaxed);
      saved_for_this_background = false;
      if (backgrounded_at != 0) {
        std::printf("[lifecycle] back after %.1f s off screen\n",
                    static_cast<double>(SDL_GetTicks() - backgrounded_at) /
                        1000.0);
        std::fflush(stdout);
        backgrounded_at = 0;
      }
      // Un-suspend the audio device. This also covers the case Task 3 added it
      // for: an AVAudioSession interruption that ends while we are away.
      th::sound::resume_audio_device();
      on_resume();
      log_memory("returned to foreground");
      break;

    case SDL_EVENT_LOW_MEMORY:
      log_memory("low memory warning");
      if (watch_lua != nullptr && !dispatching) {
        lua_gc(watch_lua, LUA_GCCOLLECT, 0);
        log_memory("after full collection");
      }
      break;

    case SDL_EVENT_TERMINATING:
      // iOS is killing us. If we are still in front this is the only warning
      // there will be; if we are not, the background save already ran.
      log_memory("terminating");
      app_backgrounded.store(true, std::memory_order_relaxed);
      go_to_background("terminating");
      break;

    default:
      break;
  }
  return true;
}

void install(lua_State* L) {
  watch_lua = L;
  if (!SDL_AddEventWatch(watch, nullptr)) {
    std::fprintf(stderr, "SDL_AddEventWatch failed: %s\n", SDL_GetError());
    watch_lua = nullptr;
    return;
  }
  log_memory("main loop start");
}

void uninstall() {
  SDL_RemoveEventWatch(watch, nullptr);
  watch_lua = nullptr;
}

}  // namespace lifecycle
}  // namespace
#endif

void mainloop(lua_State* L) {
  SDL_TimerID timer =
      SDL_AddTimer(usertick_period_ms, timer_frame_callback, nullptr);
  SDL_Event e;

#ifndef TRACY_ENABLE
  lua_Hook hookFn = lua_gethook(L);
  if (!hookFn) {
    lua_sethook(L, l_infinite_loop_hook, LUA_MASKCOUNT, 10'000'000);
  } else {
    std::printf(
        "Warning: Infinite loop detection disabled due to existing Lua hook\n");
  }
#endif

  std::string_view last_dispatch;
  bool wait_error = false;

  lua_getglobal(L, "TheApp");
  lua_getfield(L, -1, "video");
  render_target* target = static_cast<render_target*>(lua_touserdata(L, -1));

#ifdef CORSIX_TH_IOS
  const Uint32 display_frame_period =
      display_frame_request_period_ms(target->get_window());
  std::printf("Display frame pacing: repaint requested every %u ms\n",
              display_frame_period);
  SDL_TimerID display_frame_timer =
      SDL_AddTimer(display_frame_period, display_frame_callback, nullptr);
  lifecycle::install(L);
#endif

  while ((wait_error = SDL_WaitEvent(&e))) {
    bool do_frame = false;
    bool do_timer = false;

    do {
      SDL_ConvertEventToRenderCoordinates(target->get_renderer(), &e);

#ifdef CORSIX_TH_IOS
      if (touch::is_emulated_mouse(e)) {
        continue;
      }
#endif

      int nargs;
      switch (e.type) {
        case SDL_EVENT_QUIT:
          goto leave_loop;
        case SDL_EVENT_KEY_DOWN:
          last_dispatch = dispatch_keydown;
          push_app_dispatch(L, last_dispatch);
          lua_pushstring(L, SDL_GetKeyName(e.key.key));
          l_push_modifiers_table(L, e.key.mod);
          lua_pushboolean(L, e.key.repeat != 0);
          nargs = 4;
          break;
        case SDL_EVENT_KEY_UP:
          last_dispatch = dispatch_keyup;
          push_app_dispatch(L, last_dispatch);
          lua_pushstring(L, SDL_GetKeyName(e.key.key));
          l_push_modifiers_table(L, e.key.mod);
          nargs = 3;
          break;
        case SDL_EVENT_TEXT_INPUT:
          last_dispatch = dispatch_textinput;
          push_app_dispatch(L, last_dispatch);
          lua_pushstring(L, e.text.text);
          nargs = 2;
          break;
        case SDL_EVENT_TEXT_EDITING:
          last_dispatch = dispatch_textediting;
          push_app_dispatch(L, dispatch_textediting);
          lua_pushstring(L, e.edit.text);
          lua_pushinteger(L, e.edit.start);
          lua_pushinteger(L, e.edit.length);
          nargs = 4;
          break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
          last_dispatch = dispatch_buttondown;
          push_app_dispatch(L, last_dispatch);
          lua_pushinteger(L, e.button.button);
          lua_pushnumber(L, e.button.x);
          lua_pushnumber(L, e.button.y);
          nargs = 4;
          break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
          last_dispatch = dispatch_buttonup;
          push_app_dispatch(L, dispatch_buttonup);
          lua_pushinteger(L, e.button.button);
          lua_pushnumber(L, e.button.x);
          lua_pushnumber(L, e.button.y);
          nargs = 4;
          break;
        case SDL_EVENT_MOUSE_WHEEL:
          last_dispatch = dispatch_mousewheel;
          push_app_dispatch(L, last_dispatch);
          lua_pushnumber(L, e.wheel.x);
          lua_pushnumber(L, e.wheel.y);
          lua_pushboolean(L, e.wheel.which == SDL_TOUCH_MOUSEID);
          lua_pushboolean(L, e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED);
          nargs = 5;
          break;
        case SDL_EVENT_MOUSE_MOTION:
          last_dispatch = dispatch_motion;
          push_app_dispatch(L, last_dispatch);
          lua_pushnumber(L, e.motion.x);
          lua_pushnumber(L, e.motion.y);
          lua_pushnumber(L, e.motion.xrel);
          lua_pushnumber(L, e.motion.yrel);
          nargs = 5;
          break;
        case SDL_EVENT_PINCH_BEGIN:
          last_dispatch = dispatch_pinch_begin;
          push_app_dispatch(L, last_dispatch);
          nargs = 1;
          break;
        case SDL_EVENT_PINCH_UPDATE:
          last_dispatch = dispatch_pinch_update;
          push_app_dispatch(L, last_dispatch);
          lua_pushnumber(L, e.pinch.scale);
          nargs = 2;
          break;
        case SDL_EVENT_PINCH_END:
          last_dispatch = dispatch_pinch_end;
          push_app_dispatch(L, last_dispatch);
          nargs = 1;
          break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
          last_dispatch = dispatch_active;
          push_app_dispatch(L, last_dispatch);
          lua_pushinteger(L, 1);
          nargs = 2;
          break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
          last_dispatch = dispatch_active;
          push_app_dispatch(L, last_dispatch);
          lua_pushinteger(L, 0);
          nargs = 2;
          break;
        case SDL_EVENT_WINDOW_RESIZED:
          last_dispatch = dispatch_window_resized;
          push_app_dispatch(L, last_dispatch);
          lua_pushinteger(L, e.window.data1);
          lua_pushinteger(L, e.window.data2);
          {
            SDL_WindowFlags flags = SDL_GetWindowFlags(target->get_window());
            uint32_t window_state = 0;
            if (flags & SDL_WINDOW_FULLSCREEN) {
              window_state = 1;
            } else if (flags & SDL_WINDOW_MAXIMIZED) {
              window_state = 2;
            } else if (flags & SDL_WINDOW_MINIMIZED) {
              window_state = 3;
            }
            lua_pushinteger(L, window_state);
          }
          nargs = 4;
          break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
          target->on_pixel_size_change();

          last_dispatch = dispatch_window_pixel_size_changed;
          push_app_dispatch(L, last_dispatch);
          lua_pushinteger(L, e.window.data1);
          lua_pushinteger(L, e.window.data2);
          nargs = 3;
          break;
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
          last_dispatch = dispatch_window_display_scale_changed;
          push_app_dispatch(L, last_dispatch);
          lua_pushnumber(L, target->get_display_scale());
          nargs = 2;
          break;
        case SDL_EVENT_WINDOW_MAXIMIZED:
          last_dispatch = dispatch_window_maximized;
          push_app_dispatch(L, last_dispatch);
          nargs = 1;
          break;
        case SDL_EVENT_WINDOW_RESTORED:
          last_dispatch = dispatch_window_restored;
          push_app_dispatch(L, last_dispatch);
          nargs = 1;
          break;
        case SDL_USEREVENT_MUSIC_OVER:
          last_dispatch = dispatch_music_over;
          push_app_dispatch(L, last_dispatch);
          nargs = 1;
          break;
        case SDL_USEREVENT_MUSIC_LOADED:
          last_dispatch = dispatch_callback;
          lua_pushlstring(L, last_dispatch.data(), last_dispatch.size());
          lua_pushcclosure(L, &l_error_handler, 1);
          lua_pushcfunction(L, &l_load_music_async_callback);
          lua_pushlightuserdata(L, e.user.data1);
          if (lua_pcall(L, 1, 0, -3) != LUA_OK) {
            SDL_RemoveTimer(timer);
          }
          lua_pop(L, 1);  // Remove l_error_handler
          nargs = 0;
          break;
        case SDL_USEREVENT_TICK:
          do_timer = true;
          nargs = 0;
          break;
#ifdef CORSIX_TH_IOS
        case SDL_USEREVENT_FRAME:
          do_frame = true;
          nargs = 0;
          break;
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
          // CorsixTH-iOS @feature 2026-09-07 the recogniser dispatches whatever
          // the gesture turned out to mean itself, so nothing is left for the
          // generic path below to send.
          do_frame = touch::handle(L, target, e) || do_frame;
          nargs = 0;
          break;
        case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED:
          // CorsixTH-iOS @feature 2026-09-06 a display cutout insets the
          // render viewport, so a safe-area change is a render size change.
          target->on_pixel_size_change();

          last_dispatch = dispatch_window_pixel_size_changed;
          push_app_dispatch(L, last_dispatch);
          {
            render_size size = target->get_size();
            lua_pushinteger(L, size.width);
            lua_pushinteger(L, size.height);
          }
          nargs = 3;
          break;
#endif
        case SDL_USEREVENT_MOVIE_OVER:
          last_dispatch = dispatch_movie_over;
          push_app_dispatch(L, last_dispatch);
          nargs = 1;
          break;
        case SDL_USEREVENT_SOUND_OVER:
          last_dispatch = dispatch_sound_over;
          push_app_dispatch(L, last_dispatch);
          lua_pushinteger(L, *(static_cast<int*>(e.user.data1)));
          nargs = 2;
          break;
#ifdef CORSIX_TH_IOS
        // CorsixTH-iOS @bugfix 2026-09-06 an AVAudioSession interruption (call,
        // Siri, alarm) or an output route change (headphones in/out) can leave
        // the audio device suspended once the interruption ends, which reads as
        // "the game went permanently silent". Nudge the device whenever the
        // device list changes; track pause state is deliberately untouched.
        // CorsixTH-iOS @bugfix 2026-09-08 SDL_EVENT_DID_ENTER_FOREGROUND used to
        // be listed here too, and could never have fired: SDL never queues the
        // application events. It is handled in the lifecycle event watch now.
        // Not while backgrounded: resuming the device we just paused would
        // leave audio running behind the app switcher.
        case SDL_EVENT_AUDIO_DEVICE_ADDED:
        case SDL_EVENT_AUDIO_DEVICE_REMOVED:
          if (!app_backgrounded.load(std::memory_order_relaxed)) {
            th::sound::resume_audio_device();
          }
          nargs = 0;
          break;
#endif
        default:
          nargs = 0;
          break;
      }
      if (nargs != 0) {
        int res = lua_pcall(L, nargs + 1, 1, -3 - nargs);
        if (res != LUA_OK) {
          std::fprintf(stderr, "Error in %.*s: %s\n",
                       static_cast<int>(last_dispatch.size()),
                       last_dispatch.data(), lua_tostring(L, -1));
        }
        do_frame = do_frame || (lua_toboolean(L, -1) != 0);
        lua_pop(L, 2);
      }
    } while (SDL_PollEvent(&e));
#ifdef CORSIX_TH_IOS
    // CorsixTH-iOS @feature 2026-09-08 while iOS has the app off screen, neither
    // simulate nor present. Events still arrive and are still dispatched -- the
    // window and focus changes of the transition itself, and the touch cancels
    // iOS sends for the fingers it took away -- but nothing advances the world
    // and nothing touches the GPU. Rendering around a suspension queues
    // drawable-acquire timeouts that surface as a multi-second input hang after
    // the resume, and running the tick would fast-forward the hospital by
    // however long the player was away.
    if (app_backgrounded.load(std::memory_order_relaxed)) {
      lua_gc(L, LUA_GCSTEP, 2);
      infinite_loop_counter = 0;
      continue;
    }
    // CorsixTH-iOS @feature 2026-09-07 a motionless finger emits no events, so
    // the long-press timer has to be polled. The 18 ms simulation tick
    // guarantees this runs even when nothing else is happening.
    do_frame = touch::poll_long_press(L) || do_frame;
#endif
    if (do_timer) {
      last_dispatch = dispatch_timer;
      push_app_dispatch(L, last_dispatch);
      int res = lua_pcall(L, 2, 1, -4);
      if (res != LUA_OK) {
        std::fprintf(stderr, "Error in timer callback: %s\n",
                     lua_tostring(L, -1));
      }
      do_frame = do_frame || (lua_toboolean(L, -1) != 0);
      lua_pop(L, 2);
    }
    if (do_frame || !fps.limit_fps) {
      last_dispatch = dispatch_frame;
      do {
        if (fps.track_fps) {
          fps.count_frame();
        }
        push_app_dispatch(L, last_dispatch);
        int res = lua_pcall(L, 2, 1, -4);
        if (res != LUA_OK) {
          std::fprintf(stderr, "Error in frame callback: %s\n",
                       lua_tostring(L, -1));
#ifdef CORSIX_TH_IOS
          want_display_rate_frames.store(false, std::memory_order_relaxed);
#endif
        } else {
          const bool still_animating = lua_toboolean(L, -1) != 0;
#ifdef CORSIX_TH_IOS
          want_display_rate_frames.store(still_animating,
                                         std::memory_order_relaxed);
#endif
          do_frame = do_frame || still_animating;
        }
        // CorsixTH-iOS @bugfix 2026-09-08 unconditionally, including on the
        // error path: leaving the handler and its result on the stack every
        // failing frame overflows the Lua stack and panics the process, which
        // on iOS is a crash rather than an exit.
        lua_pop(L, 2);
        infinite_loop_counter = 0;
      } while (fps.limit_fps == false && !SDL_PollEvent(nullptr));
#ifdef CORSIX_TH_IOS
      // CorsixTH-iOS @bugfix 2026-09-07 a synthetic tap holds its button down
      // until a frame has been presented, so the pressed sprite is actually
      // drawn. This is that frame; release it now.
      if (touch::flush_pending_release(L)) {
        SDL_Event repaint_request;
        SDL_zero(repaint_request);
        repaint_request.type = SDL_USEREVENT_FRAME;
        SDL_PushEvent(&repaint_request);
      }
#endif
    }

    // No events pending - a good time to do a bit of garbage collection
    lua_gc(L, LUA_GCSTEP, 2);
    infinite_loop_counter = 0;
  }

  if (wait_error) {
    std::fprintf(stderr, "%s\n", SDL_GetError());
  }

leave_loop:
#ifdef CORSIX_TH_IOS
  // Before the timers, so a lifecycle event arriving during teardown cannot
  // find a lua_State that main.cpp is about to close.
  lifecycle::uninstall();
  SDL_RemoveTimer(display_frame_timer);
#endif
  SDL_RemoveTimer(timer);
}

int luaopen_sdl(lua_State* L) {
  fps.init();
  luaT_register(L, "sdl", sdllib);
#ifdef CORSIX_TH_IOS
  // CorsixTH-iOS @feature 2026-09-08 let Lua ask whether it is running on iOS,
  // so the UI can leave out controls the platform cannot honour. App:App
  // copies this to TheApp.ios, which is what the dialogs read.
  lua_pushboolean(L, 1);
  lua_setfield(L, -2, "ios");
#endif
  load_extra(L, "audio", luaopen_sdl_audio);
  load_extra(L, "wm", luaopen_sdl_wm);

  return 1;
}
