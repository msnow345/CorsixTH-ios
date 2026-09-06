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

// SDL TimerCallback
Uint32 timer_frame_callback(void*, SDL_TimerID, Uint32 interval) {
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
  if (want_display_rate_frames.load(std::memory_order_relaxed)) {
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

int l_quit(lua_State*) {
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
constexpr std::string_view dispatch_touch_drag_query("touch_drag_query");
constexpr std::string_view dispatch_touch_rotate("touch_rotate");

// CorsixTH-iOS @feature 2026-09-07 translate raw touches into game input.
//
// SDL's own touch-as-mouse emulation is switched off in l_init, so every mouse
// event the Lua UI sees on iOS is produced here. The recogniser is deliberately
// ignorant of the game: it decides only *what kind* of gesture happened, and
// asks Lua (touch_drag_query) what a one-finger drag should mean at the point it
// started. Everything it emits goes through App:dispatch, which is the exact
// entry point real SDL mouse events use, so the 298 Lua UI files are unchanged.
//
// The division is absolute and has no modes in it: one finger interacts, two
// fingers navigate. One finger never moves the camera, anywhere, so there is
// never a question of whether a drag was meant to pan or to do the thing under
// it; and two-finger pan and zoom work in every mode, including in the middle
// of a placement, so the one finger is always free to be spoken for.
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
  drag_none,    // one finger past the dead zone with nothing to drag: a no-op
  drag_mouse,   // one finger past the dead zone, held left button (room sizing)
  drag_carry,   // one finger carrying a placement: motion only, no button held
  carry_armed,  // a second finger has landed on a carry, intent not yet known
  drag_wheel,   // one finger past the dead zone over a scrollable list
  longpress,    // long press fired (right click sent), swallow until lift
  two_pending,  // two fingers down, waiting for movement to start the gesture
  two_active    // two-finger pan and pinch, both live
};

//! What Lua says a one-finger drag starting at a given point means. One finger
//! never moves the camera: that is two fingers, in every mode, always. So a
//! drag with nothing under it is not a pan, it is nothing at all.
enum class drag_mode {
  none = 0,    // nothing here to drag; swallow the gesture
  button = 1,  // held left-button drag: room sizing, sliders, window dragging
  wheel = 2,   // scroll the list under the finger
  carry = 3    // position something on the map; lifting drops it
};

constexpr Uint64 long_press_ms = 600;
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

bool emit_click(lua_State* L, int button, float x, float y) {
  // Motion first: CorsixTH highlights buttons, sets the cursor entity and
  // arms tooltips from hover, and a real mouse always moves before it clicks.
  bool repaint = emit_motion(L, x, y);
  repaint = emit_button(L, true, button, x, y) || repaint;
  repaint = emit_button(L, false, button, x, y) || repaint;
  return repaint;
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
  const float px = e.tfinger.x;
  const float py = e.tfinger.y;
  const SDL_FingerID id = e.tfinger.fingerID;
  bool repaint = false;

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
          set_phase(phase::pending, "first finger down");
          reset_samples();
          // A finger landing catches a coasting map, as it does in any iOS
          // scroll view.
          repaint = dispatch(L, dispatch_touch_catch, {}) || repaint;
          // Hover before anything else; no button is committed yet.
          repaint = emit_motion(L, px, py) || repaint;
          break;
        case phase::pending:
          s.f2 = id;
          s.f2x = px;
          s.f2y = py;
          s.f2_down_x = px;
          s.f2_down_y = py;
          if (query_drag_mode(L, s.down_x, s.down_y) ==
              static_cast<int>(drag_mode::carry)) {
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
        case phase::drag_none:
          // The finger was already dragging deliberately, with nothing under
          // it. A second finger landing can only mean "move the view", so
          // start immediately rather than making the user re-cross a dead zone
          // and throwing away the opening travel of the pan.
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
          const int mode = query_drag_mode(L, s.down_x, s.down_y);
          if (mode == static_cast<int>(drag_mode::button)) {
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
          } else if (mode == static_cast<int>(drag_mode::carry)) {
            // No button is pressed while carrying. That is the whole point: a
            // second finger, or a cancelled touch, can end the gesture without
            // an outstanding press that the game would read as "place it here".
            s.last_x = px;
            s.last_y = py;
            set_phase(phase::drag_carry, "drag while placing");
            repaint = emit_motion(L, px, py) || repaint;
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
          if (e.type == SDL_EVENT_FINGER_UP) {
            repaint = emit_click(L, 1, s.down_x, s.down_y) || repaint;
          }
          break;
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
  if (s.ph != phase::pending) {
    return false;
  }
  if (SDL_GetTicks() - s.down_ticks < long_press_ms) {
    return false;
  }
  if (s.long_press_suppressed) {
    return false;
  }
  if (query_drag_mode(L, s.down_x, s.down_y) ==
      static_cast<int>(drag_mode::carry)) {
    // Right click undoes a placement, and holding still is exactly what someone
    // lining an object up does. Take the long press out of every placement
    // mode rather than have it throw the placement away mid-aim. The phase is
    // deliberately left alone, so this finger can still tap or start a carry.
    s.long_press_suppressed = true;
    log_event("long press suppressed while placing");
    return false;
  }
  // No left button was ever sent, so this is a pure right click.
  const bool repaint = emit_click(L, 3, s.down_x, s.down_y);
  set_phase(phase::longpress, "held 600 ms");
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

}  // namespace touch
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
        // "the game went permanently silent". Nudge the device whenever we come
        // back to the front or the device list changes; track pause state is
        // deliberately untouched.
        case SDL_EVENT_DID_ENTER_FOREGROUND:
        case SDL_EVENT_AUDIO_DEVICE_ADDED:
        case SDL_EVENT_AUDIO_DEVICE_REMOVED:
          th::sound::resume_audio_device();
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
          lua_pop(L, 2);
        }
        infinite_loop_counter = 0;
      } while (fps.limit_fps == false && !SDL_PollEvent(nullptr));
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
  SDL_RemoveTimer(display_frame_timer);
#endif
  SDL_RemoveTimer(timer);
}

int luaopen_sdl(lua_State* L) {
  fps.init();
  luaT_register(L, "sdl", sdllib);
  load_extra(L, "audio", luaopen_sdl_audio);
  load_extra(L, "wm", luaopen_sdl_wm);

  return 1;
}
