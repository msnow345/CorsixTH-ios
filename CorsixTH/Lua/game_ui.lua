--[[ Copyright (c) 2010 Manuel "Roujin" Wolf

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
SOFTWARE. --]]

corsixth.require("ui")
corsixth.require("announcer")

--! Variant of UI for running games
class "GameUI" (UI)

---@type GameUI
local GameUI = _G["GameUI"]

local TH = require("TH")

local Announcer = _G["Announcer"]

-- Factor to multiply pinch_zoom scale by, bigger number results in
-- faster zoom changes
local pinch_zoom_sensitivity = 1

-- The maximum distance to shake the screen from the origin during an
-- earthquake with full intensity.
local shake_screen_max_movement = 50 --pixels

-- Speed of scrolling when using keys. Pixels / tick (18ms)
-- This scroll speed is further adjusted by the configured scroll_speed
local key_scroll_speed = 10

-- CorsixTH-iOS @feature 2026-09-07 true on a build whose only pointer is a
-- finger. File-local, because the UI is persisted into savegames and where the
-- game is being played is not a property of the save.
local touch_input = TH.GetCompileOptions().os == "ios"

-- ---------------------------------------------------------------------------
-- CorsixTH-iOS @feature 2026-09-07 THE ONE-FINGER PAN SWITCH.
--
-- Does a one-finger drag across open map move the camera?
--
--   true  -- one finger pans outside placement modes, two fingers pan in every
--            mode. Placement modes still take the one finger for themselves,
--            so this never applies while something is being carried or sized.
--   false -- one finger never moves the camera anywhere. A drag on open map is
--            inert: it emits nothing while it moves and nothing when it lifts.
--            Two fingers become the only way to move the view.
--
-- Both models are fully implemented and this line chooses between them; there
-- is deliberately no settings-screen option, because this is a question about
-- which one feels right rather than a preference to be configured. Flipping it
-- is the whole change: GameUI:onTouchDragQuery below is the only reader, and
-- the recogniser in sdl_core.cpp already has a phase for each answer
-- (drag_pan / drag_none).
local touch_one_finger_pan = true
-- ---------------------------------------------------------------------------

-- Deceleration of a released touch flick, per millisecond. This is
-- UIScrollView's normal rate, which is what makes a coast read as native rather
-- than merely damped. It deliberately does not use scrolling_momentum: that is
-- 0.8 per 18 ms tick, about 0.988 per millisecond, which spends a flick in a
-- tenth of a second.
local touch_glide_decay_per_ms = 0.998
-- Screen pixels per millisecond. The arming speed sits marginally above the
-- stopping speed, so arming a coast always buys at least one real step and
-- there is no threshold cliff between "drifted to a stop" and "flicked".
local touch_min_flick_speed = 0.06
local touch_min_glide_speed = 0.05

-- Exponent applied to the pinch ratio before it reaches setZoom. 1 is pure
-- direct manipulation: the zoom changes exactly as much as the fingers
-- separated, so the ground between them stays between them. Raise it for a
-- longer throw, lower it for a shorter one.
--
-- 1 was measured, not guessed. Until this build a pinch zoomed twice -- once
-- here and once again through SDL's own UIPinchGestureRecognizer feeding
-- current_momentum.z -- and the user liked how that felt, so removing the
-- duplicate must not shorten the throw. Simulating both paths at 120 fps with
-- this device's 2421 px render width: the second path contributed nothing at
-- all below about 300 ms of pinch, because its accumulator never reached the
-- 0.2 gate in GameUI:onFrame, and at most 4.1% extra zoom for the fastest
-- pinches (2x in 250 ms: 2.048 combined against 2.000 direct). The exponent
-- that would reproduce the old combined throw is therefore between 1.000 and
-- 1.052, mean 1.015 -- a difference of about 2% of final zoom on a 2x pinch,
-- which is well below what anyone can perceive. 1 is both correct and
-- indistinguishable, so the throw is kept and only the drift is lost.
local touch_pinch_zoom_gain = 1.0

--! Game UI constructor.
--!param app (Application) Application object.
--!param local_hospital Hospital to display
--!param map_editor (bool) Whether the map is editable.
function GameUI:GameUI(app, local_hospital, map_editor)
  self:UI(app, false)
  self.app = app

  self.hospital = local_hospital
  self.tutorial = { chapter = 0, phase = 0 }
  if map_editor then
    self.map_editor = UIMapEditor(self)
    self:addWindow(self.map_editor)
  else
    self.adviser = UIAdviser(self)
    self.bottom_panel = UIBottomPanel(self)
    self.bottom_panel:addWindow(self.adviser)
    self:addWindow(self.bottom_panel)
  end

  -- UI widgets
  self.menu_bar = UIMenuBar(self, self.map_editor)
  self:addWindow(self.menu_bar)
  self.subtitles = Subtitles(self)
  self:addWindow(self.subtitles)

  self.zoom_factor = 1
  local efz = self.zoom_factor * TheApp.gfx:getWindowDisplayScale()
  local scr_w, scr_h = app.video:getRenderSize()

  -- With the zoom the game screen is the pixel size of the screen divided by
  -- the effective zoom level (zoom_factor * display scale)
  local wwz, hwz = scr_w / efz, scr_h / efz
  self.visible_diamond = self:makeVisibleDiamond(wwz, hwz)
  if self.visible_diamond.w <= 0 or self.visible_diamond.h <= 0 then
    -- For a standard 128x128 map, 3276x2457 at 1x effective zoom would be too
    -- large and reveal the edges of the map. At 2x effective zoom this doubles
    -- and so on. Currently we error out, but it should be possible to instead
    -- automatically adjust the effective zoom factor until the map fits.
    if not self.map_editor then
      error("Window size too large for the map. " ..
          "Adjust your window size in Settings or the scale of your display in your operating system.")
    end
  end
  -- move the map so the top,left corner is at the camera tile
  local cx, cy = app.map.th:getCameraTile(local_hospital:getPlayerIndex())
  self.screen_offset_x, self.screen_offset_y = app.map:WorldToScreen(cx, cy)

  -- then adjust half a screen left/up to center the camera
  self:scrollMap(-wwz / 2, 16 - hwz / 2)
  self.limit_to_visible_diamond = not self.map_editor
  self.transparent_walls = false
  self.do_world_hit_test = true

  self.momentum = app.config.scrolling_momentum
  self.current_momentum = {x = 0.0, y = 0.0, z = 0.0}
  -- Sub-unit camera movement left over from the previous rendered frame.
  self.scroll_residual_x = 0.0
  self.scroll_residual_y = 0.0

  self.recallpositions = {}

  self.speed_up_key_pressed = false
  self.last_hovered_entity = nil

  -- The currently specified intensity value for earthquakes. To abstract
  -- the effect from the implementation this value is a number between 0
  -- and 1.
  self.shake_screen_intensity = 0

  self.announcer = Announcer(app)
  self.app:setCaptureMouse()
end

function GameUI:setupGlobalKeyHandlers()
  UI.setupGlobalKeyHandlers(self)

  -- Set the scrolling keys.
  self.scroll_keys = {
     [tostring(self.app.hotkeys["ingame_scroll_up"])] = {x = 0, y = -key_scroll_speed},
     [tostring(self.app.hotkeys["ingame_scroll_down"])] = {x = 0, y = key_scroll_speed},
     [tostring(self.app.hotkeys["ingame_scroll_left"])] = {x = -key_scroll_speed, y = 0},
     [tostring(self.app.hotkeys["ingame_scroll_right"])] = {x = key_scroll_speed, y = 0},
  }

  -- This is the long version of the shift speed key.
  -- i.e. if the "ingame_scroll_shift" key is "ctrl", then it will give us
  --  "left ctrl" and "right ctrl" for reference against the rawchar in
  --  "onKeyDown()" and "onKeyUp()"
  self.shift_scroll_key_long = {}
  self.shift_scroll_speed_pressed = false
  local temp_table = {}
  local shift_scroll_key_index = 1
  if type(self.app.hotkeys["ingame_scroll_shift"]) == "string" then
    temp_table = {self.app.hotkeys["ingame_scroll_shift"]}
  elseif type(self.app.hotkeys["ingame_scroll_shift"]) == "table" then
    temp_table = shallow_clone(self.app.hotkeys["ingame_scroll_shift"])
  end
  -- Go through the "ingame_scroll_shift" key table and see if it has any modifier names.
  for _, v in pairs (temp_table) do
    -- If it does then add long name version of them into the long key table.
    if v == "ctrl" then
      self.shift_scroll_key_long[shift_scroll_key_index] = "left ctrl"
      shift_scroll_key_index = shift_scroll_key_index + 1
      self.shift_scroll_key_long[shift_scroll_key_index] = "right ctrl"
      shift_scroll_key_index = shift_scroll_key_index + 1
    elseif v == "alt" then
      self.shift_scroll_key_long[shift_scroll_key_index] = "left alt"
      shift_scroll_key_index = shift_scroll_key_index + 1
      self.shift_scroll_key_long[shift_scroll_key_index] = "right alt"
      shift_scroll_key_index = shift_scroll_key_index + 1
    elseif v == "shift" then
      self.shift_scroll_key_long[shift_scroll_key_index] = "left shift"
      shift_scroll_key_index = shift_scroll_key_index + 1
      self.shift_scroll_key_long[shift_scroll_key_index] = "right shift"
      shift_scroll_key_index = shift_scroll_key_index + 1
    end
  end

  self:addKeyHandler("global_window_close", self, self.setEditRoom, false)
  self:addKeyHandler("ingame_showmenubar", self, self.showMenuBar)
  self:addKeyHandler("ingame_gamespeed_speedup", self, self.keySpeedUp)
  self:addKeyHandler("ingame_setTransparent", self, self.keyTransparent)
  self:addKeyHandler("ingame_toggleTransparent", self, self.toggleTransparent)
  self:addKeyHandler("ingame_toggleAdvisor", self, self.toggleAdviser)
  self:addKeyHandler("ingame_poopLog", self.app.world, self.app.world.dumpGameLog)
  self:addKeyHandler("ingame_poopStrings", self.app, self.app.dumpStrings)
  self:addKeyHandler("ingame_toggleAnnouncements", self, self.togglePlayAnnouncements)
  self:addKeyHandler("ingame_toggleSounds", self, self.togglePlaySounds)
  self:addKeyHandler("ingame_toggleMusic", self, self.togglePlayMusic)

  -- scroll to map position
  for i = 0, 9 do
    -- set camera view
    self:addKeyHandler(string.format("ingame_storePosition_%d", i), self, self.setMapRecallPosition, i)
    -- recall camera view
    self:addKeyHandler(string.format("ingame_recallPosition_%d", i), self, self.recallMapPosition, i)
  end

  if self.app.config.debug and self.app.world.map.level_number ~= "MAP EDITOR" then
    self:addKeyHandler("ingame_showCheatWindow", self, self.showCheatsWindow)
  end
end

function GameUI:makeVisibleDiamond(scr_w, scr_h)
  local map_w = self.app.map.width
  local map_h = self.app.map.height
  assert(map_w == map_h, "UI limiter requires square map")

  -- The visible diamond is the region which the top-left corner of the screen
  -- is limited to, and ensures that the map always covers all of the screen.
  -- Its vertices are at (x + w, y), (x - w, y), (x, y + h), (x, y - h).
  return {
    x = - scr_w / 2,
    y = 16 * map_h - scr_h / 2,
    w = 32 * map_h - scr_h - scr_w / 2,
    h = 16 * map_h - scr_h / 2 - scr_w / 4,
  }
end

--! Calculate the minimum valid zoom value
--!
--! Zooming out too much would cause negative width/height to be returned from
--! makeVisibleDiamond. This function calculates the minimum zoom_factor that
--! would be allowed.
function GameUI:calculateMinimumZoom()
  local scr_w, scr_h = TheApp.video:getRenderSize()
  local map_h = self.app.map.height

  -- Minimum width:  0 = 32 * map_h - (scr_h/factor) - (scr_w/factor) / 2,
  -- Minimum height: 0 = 16 * map_h - (scr_h/factor) / 2 - (scr_w/factor) / 4
  -- Both rearrange to:
  local factor = (scr_w + 2 * scr_h) / (64 * map_h)

  -- Due to precision issues a tolerance is needed otherwise setZoom might fail
  factor = factor + 0.001

  return factor
end

--! Set the zoom level, keeping one screen point fixed under the new zoom.
--!param factor (number) The new zoom factor.
--!param follow_cursor (boolean) Anchor on the cursor rather than the screen
-- centre. Ignored when an explicit anchor is given.
--!param anchor_x (number, optional) Screen x to hold fixed.
--!param anchor_y (number, optional) Screen y to hold fixed. Both must be given
-- for the anchor to be used; a pinch passes the point between the fingers here.
--!return (boolean) Whether the zoom was applied.
function GameUI:setZoom(factor, follow_cursor, anchor_x, anchor_y)
  if factor <= 0 then
    return false
  end
  if not factor or math.abs(factor - 1) < 0.001 then
    factor = 1
  end

  local ezf = factor * TheApp.gfx:getWindowDisplayScale()

  local scr_w, scr_h = TheApp.video:getRenderSize()
  local new_diamond = self:makeVisibleDiamond(scr_w / ezf, scr_h / ezf)
  if new_diamond.w < 0 or new_diamond.h < 0 then
    return false
  end

  self.visible_diamond = new_diamond
  local refx, refy
  if anchor_x and anchor_y then
    refx, refy = anchor_x, anchor_y
  elseif follow_cursor then
    refx, refy = self.cursor_x, self.cursor_y
  else
    refx, refy = scr_w / 2, scr_h / 2
  end
  local cx, cy = self:ScreenToWorld(refx, refy)
  self.zoom_factor = factor

  cx, cy = self.app.map:WorldToScreen(cx, cy)
  cx = cx - self.screen_offset_x - refx / ezf
  cy = cy - self.screen_offset_y - refy / ezf
  self:scrollMap(cx, cy)
  return true
end

function GameUI:draw(canvas)
  local app = self.app
  local scr_w, scr_h = canvas:getRenderSize()
  if self.map_editor or not self.in_visible_diamond then
    canvas:fillBlack()
  end
  local zoom = self:getEffectiveZoom()
  local dx = self.screen_offset_x +
      math.floor((0.5 - math.random()) * self.shake_screen_intensity * shake_screen_max_movement * 2)
  local dy = self.screen_offset_y +
      math.floor((0.5 - math.random()) * self.shake_screen_intensity * shake_screen_max_movement * 2)
  if canvas:scale(zoom) then
    app.map:draw(canvas, dx, dy, math.ceil(scr_w / zoom), math.ceil(scr_h / zoom), 0, 0)
    canvas:scale(1)
  else
    self:setZoom(1, false)
    app.map:draw(canvas, dx, dy, scr_w, scr_h, 0, 0)
  end
  Window.draw(self, canvas, 0, 0) -- NB: not calling UI.draw on purpose
  self:drawTooltip(canvas)
  -- CorsixTH-iOS @feature 2026-09-07 no pointer, so nothing to draw a pointer
  -- for. See UI:draw.
  if self.simulated_cursor and not touch_input then
    self.simulated_cursor.draw(canvas, self.cursor_x, self.cursor_y)
  end
end

function GameUI:onChangeResolution()
  -- Calculate and enforce minimum zoom
  local minimum_zoom = self:calculateMinimumZoom()
  if self.zoom_factor < minimum_zoom then
    self:setZoom(minimum_zoom, false)
  end
  -- Recalculate scrolling bounds
  local scr_w, scr_h = TheApp.video:getRenderSize()
  local zoom = self:getEffectiveZoom()
  self.visible_diamond = self:makeVisibleDiamond(scr_w / zoom, scr_h / zoom)
  self:scrollMap(0, 0)

  UI.onChangeResolution(self)
end

--! Update UI state after the UI has been depersisted
--! When an UI object is depersisted, its state will reflect how the UI was at
-- the moment of persistence, which may be different to the keyboard / mouse
-- state at the moment of depersistence.
--!param ui (UI) The previously existing UI object, from which values should be
-- taken.
function GameUI:resync(ui)
  if self.drag_mouse_move then
    -- Check that a window is actually being dragged. If none is found, then
    -- remove the drag handler.
    local something_being_dragged = false
    for _, window in ipairs(self.windows) do
      if window.dragging then
        something_being_dragged = true
        break
      end
    end
    if not something_being_dragged then
      self.drag_mouse_move = nil
    end
  end
  self.tick_scroll_amount = ui.tick_scroll_amount
  self.down_count = ui.down_count
  if ui.limit_to_visible_diamond ~= nil then
    self.limit_to_visible_diamond = ui.limit_to_visible_diamond
  end

  self.key_remaps = ui.key_remaps
  self.key_to_button_remaps = ui.key_to_button_remaps
end

function GameUI:updateKeyScroll()
  local dx, dy = 0, 0
  for key, scr in pairs(self.scroll_keys) do
    if self.buttons_down[key] then
      dx = dx + scr.x
      dy = dy + scr.y
    end
  end
  --If there is any movement on the x or y axis...
  if dx ~= 0 or dy ~= 0 then
    --Get the length of the scrolling vector.
    local mag = (dx^2 + dy^2) ^ 0.5
    --Then normalize the scrolling vector, after which multiply it by the scroll speed variable.
    dx = (dx / mag) * key_scroll_speed
    dy = (dy / mag) * key_scroll_speed
    -- Set the scroll amount to be used.
    self.tick_scroll_amount = {x = dx, y = dy}
    return true
  else
    self.tick_scroll_amount = false
    return false
  end
end

function GameUI:keySpeedUp()
  self.speed_up_key_pressed = true
  self.app.world:speedUp()
end

function GameUI:keyTransparent()
  self:setWallsTransparent(true)
end

function GameUI:toggleTransparent()
  self.toggled_transparency = not self.toggled_transparency
  self:setWallsTransparent(self.toggled_transparency)
end

function GameUI:onKeyDown(rawchar, modifiers, is_repeat)
  if UI.onKeyDown(self, rawchar, modifiers, is_repeat) then
    -- Key has been handled already
    return true
  end
  local key = rawchar:lower()
  -- If key is shift speed key...
  for _, v in pairs(self.shift_scroll_key_long) do
    if v == key then
      self.shift_scroll_speed_pressed = true
    end
  end
  if self.scroll_keys[key] then
    self:updateKeyScroll()
    return
  end
end

function GameUI:onKeyUp(rawchar)
  if UI.onKeyUp(self, rawchar) then
    return true
  end

  local key = rawchar:lower()
  for _, v in pairs(self.shift_scroll_key_long) do
    if v == key then
      self.shift_scroll_speed_pressed = false
    end
  end
  if self.scroll_keys[key] then
    self:updateKeyScroll()
    return
  end

  -- Guess that the "Speed Up" key was released because the
  -- code parameter can't provide UTF-8 key codes:
  self.speed_up_key_pressed = false
  if self.app.world:isCurrentSpeed("Speed Up") then
    self.app.world:previousSpeed()
  end

  if not self.toggled_transparency then self:setWallsTransparent(false) end
end

function GameUI:makeDebugFax()
  local message = {
    {text = "debug fax"}, -- no translation needed imo
    choices = {{text = "close debug fax", choice = "close"}},
  }
  -- Don't use "strike" type here, as these open a different window and must have an owner
  local types = {"emergency", "epidemy", "personality", "information", "disease", "report"}
  self.bottom_panel:queueMessage(types[math.random(1, #types)], message)
end

function GameUI:ScreenToWorld(x, y)
  local zoom = self:getEffectiveZoom()
  return self.app.map:ScreenToWorld(self.screen_offset_x + x / zoom, self.screen_offset_y + y / zoom)
end

function GameUI:WorldToScreen(x, y)
  local zoom = self:getEffectiveZoom()
  x, y = self.app.map:WorldToScreen(x, y)
  x = x - self.screen_offset_x
  y = y - self.screen_offset_y
  return x * zoom, y * zoom
end

function GameUI:getScreenOffset()
  return self.screen_offset_x, self.screen_offset_y
end

--! Change if the World should be tested for entities under the cursor
--!param mode (boolean or room) true to enable hit test (normal), false
--! to disable, room to enable only for non-door objects in given room
function GameUI:setWorldHitTest(mode)
  self.do_world_hit_test = mode
end

function GameUI:onCursorWorldPositionChange()
  local zoom = self:getEffectiveZoom()
  local x = math.floor(self.screen_offset_x + self.cursor_x / zoom)
  local y = math.floor(self.screen_offset_y + self.cursor_y / zoom)
  local entity = nil
  local overwindow = self:hitTest(self.cursor_x, self.cursor_y)
  if self.do_world_hit_test and not overwindow then
    entity = self.app.map.th:hitTestObjects(x, y)
    if self.do_world_hit_test ~= true then
      -- limit to non-door objects in room
      local room = self.do_world_hit_test
      entity = entity and class.is(entity, Object) and
          entity:getRoom() == room and entity ~= room.door and entity
    end
  end
  if entity ~= self.cursor_entity then
    -- Stop displaying hoverable moods for the old entity
    if self.cursor_entity then
      self.cursor_entity:setMood(nil)
    end

    -- Make the entity easily accessible when debugging, and ignore "deselecting" an entity.
    if entity then
      self.debug_cursor_entity = entity
    end

    local epidemic = self.hospital.epidemic
    local infected_cursor = TheApp.gfx:loadMainCursor("epidemic")
    local epidemic_cursor = TheApp.gfx:loadMainCursor("epidemic_hover")

    self.cursor_entity = entity
    if self.cursor ~= self.edit_room_cursor and self.cursor ~= self.waiting_cursor then
      local cursor = self.default_cursor
      if self.app.world.user_actions_allowed then
        --- If the patient is infected show the infected cursor
        if epidemic and epidemic.coverup_selected and
          entity and entity.infected and not epidemic.timer.closed then
          cursor = infected_cursor
          -- In vaccination mode display epidemic hover cursor for all entities
        elseif epidemic and epidemic.vaccination_mode_active then
          cursor = epidemic_cursor
          -- Otherwise just show the normal cursor and hover if appropriate
        else
          cursor = entity and entity.hover_cursor or
          (self.down_count ~= 0 and self.down_cursor or self.default_cursor)
        end
      end
      self:setCursor(cursor)
    end
    if self.bottom_panel then
      self.bottom_panel:setDynamicInfo(nil)
    end
  end

  -- Queueing icons over patients
  local wx, wy = self:ScreenToWorld(self.cursor_x, self.cursor_y)
  wx = math.floor(wx)
  wy = math.floor(wy)
  local room
  if not overwindow and wx > 0 and wy > 0 and wx < self.app.map.width and wy < self.app.map.height then
    room = self.app.world:getRoom(wx, wy)
  end
  -- Find the room associated with the current entity (Usually only applies to doors)
  if entity and not room then
    if entity.room then
      room = entity.room
    elseif entity.object_type and entity.object_type.id == "swing_door_left" then
      -- Special case to catch the non-dominant side of a double-door
      room = entity.master.room
    end
  end
  if room ~= self.cursor_room then
    -- Unset queue mood for patients queueing the old room
    if self.cursor_room then
      local queue = self.cursor_room.door.queue
      if queue then
        for _, humanoid in ipairs(queue) do
          humanoid:setMood("queue", "deactivate")
        end
      end
    end
    -- Set queue mood for patients queueing the new room
    if room then
      local queue = room.door.queue
      if queue and #queue > 0 then
        TheApp.ui:playSound("HLIGHTP2.wav")
      end
      if queue then
        for _, humanoid in ipairs(queue) do
          humanoid:setMood("queue", "activate")
        end
      end
    end
    self.cursor_room = room
  end

  -- Any hoverable mood should be displayed on the new entity
  if class.is(entity, Humanoid) then
    for _, value in pairs(entity.active_moods) do
      if value.on_hover then
        if not self.mood_info then
          if self.last_hovered_entity ~= entity then
            TheApp.ui:playSound("HLIGHTP2.wav")
            self.last_hovered_entity = entity
          end
          if entity.hover_moods then
            entity:setMoodInfo(value)
          end
        end
        break
      end
    end
  else
    self.last_hovered_entity = nil
  end

  -- Dynamic Info
  if entity and self.bottom_panel then
    self.bottom_panel:setDynamicInfo(entity:getDynamicInfo())
  end

  return Window.onCursorWorldPositionChange(self, self.cursor_x, self.cursor_y)
end

local UpdateCursorPosition = TH.cursor.setPosition

local highlight_x, highlight_y

--! Called when focus changes on game window.
--!param gain (number) 1 for in-focus, 0 for out-of-focus
function GameUI:onWindowActive(gain)
  if gain == 0 then
    self.tick_scroll_amount_mouse = false
  end
  UI.onWindowActive(self, gain)
end

-- TODO: try to remove duplication with UI:onMouseMove
function GameUI:onMouseMove(x, y, dx, dy)
  if self.mouse_released then
    return false
  end

  local repaint = UpdateCursorPosition(self.app.video, x, y)
  if self.app.moviePlayer.playing then
    return false
  end

  self.cursor_x = x
  self.cursor_y = y
  if self:onCursorWorldPositionChange() or self.simulated_cursor then
    repaint = true
  end

  if self:_isMouseScrollButtonDown() then
    local zoom = self:getEffectiveZoom()
    self.current_momentum.x = self.current_momentum.x - dx/zoom
    self.current_momentum.y = self.current_momentum.y - dy/zoom

    local momentum_x_int = math.round(self.current_momentum.x)
    local momentum_y_int = math.round(self.current_momentum.y)

    -- Stop zooming when the middle mouse button is pressed
    self.current_momentum.z = 0
    self:scrollMap(momentum_x_int, momentum_y_int)

    self.current_momentum.x = self.current_momentum.x - momentum_x_int
    self.current_momentum.y = self.current_momentum.y - momentum_y_int

    repaint = true
  end

  if self.drag_mouse_move then
    self.drag_mouse_move(x, y)
    return true
  end

  local scroll_region_size
  if touch_input then
    -- CorsixTH-iOS @feature 2026-09-07 a one-pixel band cannot be hit with a
    -- finger. This band is only ever live during a placement (see
    -- _edgeScrollAllowed), where it is the only way to reach past the edge.
    scroll_region_size = 24 * TheApp.gfx:getUIScale()
  elseif self.app.config.fullscreen then
    -- As the mouse is locked within the window, a 1px region feels a lot
    -- larger than it actually is.
    scroll_region_size = 1
  else
    -- In windowed mode, a reasonable size is needed, though not too large.
    scroll_region_size = 8
  end
  local scr_w, scr_h = TheApp.video:getRenderSize()
  if not self.app.config.prevent_edge_scrolling and self:_edgeScrollAllowed() and
      (x < scroll_region_size or y < scroll_region_size or
       x >= scr_w - scroll_region_size or
       y >= scr_h - scroll_region_size) then
    local scroll_dx = 0
    local scroll_dy = 0
    local scroll_power = 7
    if x < scroll_region_size then
      scroll_dx = -scroll_power
    elseif x >= scr_w - scroll_region_size then
      scroll_dx = scroll_power
    end
    if y < scroll_region_size then
      scroll_dy = -scroll_power
    elseif y >= scr_h - scroll_region_size then
      scroll_dy = scroll_power
    end

    if not self.tick_scroll_amount_mouse then
      self.tick_scroll_amount_mouse = {x = scroll_dx, y = scroll_dy}
    else
      self.tick_scroll_amount_mouse.x = scroll_dx
      self.tick_scroll_amount_mouse.y = scroll_dy
    end
  else
    self.tick_scroll_amount_mouse = false
  end

  if Window.onMouseMove(self, x, y, dx, dy) then
    repaint = true
  end

  self:updateTooltip()

  local map = self.app.map
  local wx, wy = self:ScreenToWorld(x, y)
  wx = math.floor(wx)
  wy = math.floor(wy)
  if highlight_x then
    --map.th:setCell(highlight_x, highlight_y, 4, 0)
    highlight_x = nil
  end
  local map_width, map_height = map.th:size()
  if 1 <= wx and wx <= map_width and 1 <= wy and wy <= map_height then
    if map.th:getCellFlags(wx, wy).passable then
      --map.th:setCell(wx, wy, 4, 24 + 8 * 256)
      highlight_x = wx
      highlight_y = wy
    end
  end

  return repaint
end

--! Should the pointer near a screen edge scroll the map?
--! CorsixTH-iOS @feature 2026-09-07: with touch this is an active hazard
--! everywhere except a placement. A drag that ends near an edge would pan
--! directly and edge-scroll as well, compounding into a lurch, and there is no
--! hover afterwards to move the pointer back out of the band and stop it. It is
--! still wanted while sizing or placing something, where the one finger is
--! already busy and cannot pan as well.
--!return (boolean) Whether edge scrolling may engage.
function GameUI:_edgeScrollAllowed()
  if not touch_input then
    return true
  end
  return self:_activePlacement() ~= nil
end

function GameUI:onMouseUp(code, x, y)
  if touch_input then
    -- The pointer stops dead where the finger lifted, so an edge scroll armed
    -- by the last motion of a drag would never see it leave the band.
    self.tick_scroll_amount_mouse = false
  end
  if self.app.moviePlayer.playing then
    return UI.onMouseUp(self, code, x, y)
  end

  -- Controlling debug patients movement with a cursor
  local button = self.button_codes[code]
  if button == "right" and not self.map_editor and highlight_x then
    local window = self:getWindow(UIPatient)
    local patient = (window and window.patient.is_debug and window.patient) or self.hospital:getDebugPatient()
    if patient then
      patient:walkTo(highlight_x, highlight_y)
      patient:queueAction(IdleAction())
    end
  end

  if self.edit_room then
    if class.is(self.edit_room, Room) then
      if button == "right" and self.cursor == self.waiting_cursor then
        -- Still waiting for people to leave the room, abort editing it.
        self:setEditRoom(false)
      end
    else -- No room chosen yet, but about to edit one.
      if button == "left" then -- Take the clicked one.
        local room = self.app.world:getRoom(self:ScreenToWorld(x, y))
        if room then
          if not room.crashed then
            self:setCursor(self.waiting_cursor)
            self.edit_room = room
            room:tryToEdit()
          else
            if self.app.config.remove_destroyed_rooms then
              local room_cost = room:calculateRemovalCost()
              self:setEditRoom(false)
              -- show confirmation dialog for removing the room
              self:addWindow(UIConfirmDialog(self, false, _S.confirmation.remove_destroyed_room:format(room_cost),
              --[[persistable:remove_destroyed_room_confirm_dialog]]function()
                local world = room.world
                UIEditRoom:removeRoom(false, room, world)
                world:resetSideObjects()
                world.rooms[room.id] = nil
                self.hospital:spendMoney(room_cost, _S.transactions.remove_room)
                end
              ))
            end
          end
        end
      else -- right click, we don't want to edit a room after all.
        self:setEditRoom(false)
      end
    end
  end

  -- During vaccination mode you can only interact with infected patients
  local epidemic = self.hospital.epidemic
  if epidemic and epidemic.vaccination_mode_active then
    if button == "left" then
      if self.cursor_entity then
        -- Allow click behaviour for infected patients
        if self.cursor_entity.infected then
          self.cursor_entity:onClick(self,button)
        end
      end
    elseif button == "right" then
      --Right click turns vaccination mode off
      local watch = TheApp.ui:getWindow(UIWatch)
      watch:toggleVaccinationMode()
    end
  end

  return UI.onMouseUp(self, code, x, y)
end

--! Process SDL_EVENT_PINCH_BEGIN.
--!
--!return (boolean) event processed indicator
function GameUI:onPinchBegin()
  self.current_momentum.z = 0
end

--! Process SDL_EVENT_PINCH_UPDATE.
--!
--!return (boolean) event processed indicator
--!param scale (number) The scale change since the last SDL_EVENT_PINCH_UPDATE.
--!                     Scale < 1 is "zoom out". Scale > 1 is "zoom in"
function GameUI:onPinchUpdate(scale)
  if touch_input then
    -- CorsixTH-iOS @bugfix 2026-09-07 SDL's iOS backend runs a
    -- UIPinchGestureRecognizer with cancelsTouchesInView = NO, so a pinch
    -- arrives here *as well as* through the finger events the touch layer is
    -- already tracking. Left alone, every pinch zoomed twice: once directly and
    -- anchored between the fingers, and again through this accumulator, applied
    -- a tick later, unanchored, and still drifting for several ticks after the
    -- fingers lift. Only the direct path may zoom.
    --
    -- KNOWN LIMITATION, and a blocker for upstreaming this file as-is: the gate
    -- is the platform, not the gesture, so it also silences a genuine trackpad
    -- pinch on an iPad -- which sends no finger events and so has no other zoom
    -- path left. Accepted deliberately because this user does not use a
    -- trackpad. The fix is to suppress the accumulator only while our own touch
    -- recogniser has a gesture in progress: touch_catch and touch_gesture_end
    -- already bracket exactly that interval. See docs/port/IOS_PORT_NOTES.md.
    self.current_momentum.z = 0
    return true
  end
  self.current_momentum.z = self.current_momentum.z + (scale - 1) * pinch_zoom_sensitivity
  return true
end

--! Process SDL_EVENT_PINCH_END.
--!
--!return (boolean) event processed indicator
function GameUI:onPinchEnd()
end

--! A finger landed on the glass: catch whatever the camera was still doing,
--! the way touching a coasting iOS scroll view stops it.
--! CorsixTH-iOS @feature 2026-09-07 catch a coasting camera.
--!return (boolean) event processed indicator
function GameUI:onTouchCatch()
  self.current_momentum.x = 0.0
  self.current_momentum.y = 0.0
  self.current_momentum.z = 0.0
  self.touch_glide = nil
  self:_stopTouchEdgeScroll()
  return false
end

--! CorsixTH-iOS @bugfix 2026-09-07 the gesture ended, however it ended.
--! Reported for cancellations too -- an incoming call, a Control Centre swipe,
--! palm rejection -- which is the case _stopTouchEdgeScroll's other callers
--! miss: a carry that is cancelled rather than lifted emits no click and no
--! fling, so nothing else would ever run. A cancelled carry must place nothing,
--! but it must not leave the camera running either.
--!return (boolean) event processed indicator
function GameUI:onTouchGestureEnd()
  self.touch_gesture_active = false
  self:_stopTouchEdgeScroll()
  return false
end

--! Disarm edge scrolling.
--! CorsixTH-iOS @bugfix 2026-09-07 edge scrolling is armed by a mouse move into
--! the band and disarmed by one out of it. A finger leaving the glass produces
--! neither, so an edge scroll armed while carrying something to the edge stayed
--! armed after the fingers lifted and scrolled the map for ever. onMouseUp
--! covers the gestures that end in a click; these are the ones that do not --
--! every two-finger gesture, and every drag that was not a press.
function GameUI:_stopTouchEdgeScroll()
  if touch_input then
    self.tick_scroll_amount_mouse = false
  end
end

--! Direct-manipulation camera for touch.
--! CorsixTH-iOS @feature 2026-09-07 pan and pinch, applied together.
--!
--! Pan and zoom arrive in the same message and are applied in the same frame,
--! which is what lets a pinch start mid-drag without lifting a finger. The zoom
--! is applied straight to the zoom factor rather than accumulated into
--! current_momentum.z: that accumulator is applied a tick later and shaped by
--! World:adjustZoom's zoom_speed factor and gaussian modifier, which exist to
--! smooth discrete mouse-wheel clicks. A pinch is already a continuous ratio
--! describing exactly the zoom the fingers asked for.
--!
--!param dx,dy (number) Movement of the finger, or of the two-finger centroid,
-- in screen pixels since the previous message.
--!param ratio (number) Change in finger separation, 1 when not pinching.
--!param ax,ay (number) The screen point to hold still while zooming, which is
-- the point between the fingers.
--!return (boolean) event processed indicator
function GameUI:onTouchCamera(dx, dy, ratio, ax, ay)
  self.touch_gesture_active = true
  if ratio ~= 1 then
    -- Applied as an exponent rather than a multiplier so the response stays
    -- multiplicative: pinching in and back out returns to the same zoom
    -- instead of drifting.
    self:setZoom(self.zoom_factor * ratio ^ touch_pinch_zoom_gain, false, ax, ay)
  end
  if dx ~= 0 or dy ~= 0 then
    -- The camera moves opposite the finger: the ground stays under the finger.
    local zoom = self:getEffectiveZoom()
    self:_scrollMapFractional(-dx / zoom, -dy / zoom)
  end
  return true
end

--! The fingers left the glass. Everything up to this point was direct
--! manipulation; momentum exists only for the release.
--! CorsixTH-iOS @feature 2026-09-07 release flick.
--!param vx,vy (number) Release velocity in screen pixels per millisecond,
-- measured by the platform layer over a real time window rather than filtered
-- per frame, because people ease off as they lift.
--!return (boolean) event processed indicator
function GameUI:onTouchFling(vx, vy)
  self.touch_gesture_active = false
  self:_stopTouchEdgeScroll()
  if (vx * vx + vy * vy) ^ 0.5 < touch_min_flick_speed then
    self.touch_glide = nil
    return false
  end
  self.touch_glide = {x = -vx, y = -vy}
  return true
end

--! CorsixTH-iOS @feature 2026-09-07 advance a released touch flick.
--! Velocity is in screen pixels per
--! millisecond and the decay is applied per millisecond, so the coast is
--! identical at any frame rate.
--!param dt (number) Milliseconds since the previous rendered frame.
--!return (boolean) Whether the camera is still coasting.
function GameUI:_advanceTouchGlide(dt)
  local glide = self.touch_glide
  if not glide then
    return false
  end
  local zoom = self:getEffectiveZoom()
  local step_x, step_y = glide.x * dt / zoom, glide.y * dt / zoom
  local before_x, before_y = self.screen_offset_x, self.screen_offset_y
  self:_scrollMapFractional(step_x, step_y)

  local decay = touch_glide_decay_per_ms ^ dt
  glide.x, glide.y = glide.x * decay, glide.y * decay

  -- Judge "pinned against the edge of the map" against the distance actually
  -- asked for. A fixed threshold cannot tell a camera jammed against the map
  -- edge from a frame so short that it asked the camera to move almost nothing.
  local asked = (step_x * step_x + step_y * step_y) ^ 0.5
  local moved_x, moved_y = self.screen_offset_x - before_x, self.screen_offset_y - before_y
  local pinned = asked > 1 and (moved_x * moved_x + moved_y * moved_y) ^ 0.5 < asked * 0.25
  if pinned or (glide.x * glide.x + glide.y * glide.y) ^ 0.5 < touch_min_glide_speed then
    self.touch_glide = nil
    return false
  end
  return true
end

function GameUI:onWindowDisplayScaleChanged(scale)
  local gfx = self.app.gfx
  local old_ds = gfx:getWindowDisplayScale()
  UI.onWindowDisplayScaleChanged(self, scale)
  local new_ds = gfx:getWindowDisplayScale()

  if old_ds ~= new_ds then
    self:setZoom(self.zoom_factor, false)
  end
end

--! Process SDL_EVENT_MOUSE_WHEEL
--!
--!param x (number) the amount scrolled horizontally (+x = right, -x = left)
--!param y (number) the amount scrolled vertically (+y = up/away, -y = down/towards)
--!param touch (boolean) whether the mouse wheel is from a touch gesture
--!param flipped (boolean) whether the axis are flipped such as MacOS natural scrolling
function GameUI:onMouseWheel(x, y, touch, flipped)
  local inside_window = false
  if self.windows then
    for _, window in ipairs(self.windows) do
      local wx, wy = window:getRealXY()
      if window:hitTest(self.cursor_x - wx, self.cursor_y - wy) then
        inside_window = true
        break
      end
    end
  end
  if not inside_window then
    -- Consider making touch scrolling pan instead of zoom, you can pinch to zoom
    -- on a touch pad.

    -- Apply momentum to the zoom
    if math.abs(self.current_momentum.z) < 12 then
      self.current_momentum.z = self.current_momentum.z + y
    end
  end
  return UI.onMouseWheel(self, x, y)
end

--! Announcements to the player.
--!param msgs (array of string) Messages to select from.
--!param priority (optional valid AnnouncementPriority selection) Priority of announcement
--!param chance_to_play (optional float in range (0, 1]) Fraction of times that the
--    call actually says something.
--!return (boolean) Whether a message was given to the user.
function GameUI:playRandomAnnouncement(msgs, priority, chance_to_play, played_callback, played_callback_delay)
  local max_rnd = #msgs
  if chance_to_play and chance_to_play > 0 and chance_to_play < 1 then
    -- Scale by the fraction.
    max_rnd = math.floor(max_rnd / chance_to_play)
  end

  local index = (max_rnd == 1) and 1 or math.random(1, max_rnd)
  if index <= #msgs then
    self:playAnnouncement(msgs[index], priority, played_callback, played_callback_delay)
    return true
  end
  return false
end

function GameUI:playAnnouncement(name, priority, played_callback, played_callback_delay)
  self.announcer:playAnnouncement(name, priority, played_callback, played_callback_delay)
end

--! CorsixTH-iOS @feature 2026-09-07 is something being positioned on the map?
--! One rule for every placement flow: sizing a room, placing its door and
--! windows, dropping an object, siting a member of staff. While any of them is
--! live the one finger drives it and never moves the camera, whatever
--! touch_one_finger_pan says; two fingers move the view instead, which is what
--! they do everywhere else too.
--!return (Window, boolean) The window doing the placing and whether it wants a
--! held button rather than a carry, or nil.
function GameUI:_activePlacement()
  local place_objects = self:getWindow(UIPlaceObjects)
  if place_objects then
    -- Sizing the walls of a room is a press, drag and release on the map: the
    -- rectangle is anchored where the press landed, so the button has to be
    -- held for the whole gesture.
    local phase = place_objects.phase
    if phase == "walls" then
      return place_objects, true
    end
    -- Siting the door and the windows are map placements too, just click-sized
    -- ones, so the finger carries rather than presses.
    if phase == "door" or phase == "windows" then
      return place_objects, false
    end
    -- Otherwise nothing is in hand unless the dialog says so. place_objects is
    -- false while it is being used to *choose* objects for a room being built,
    -- and once the last object has been placed; treating those as a placement
    -- would take the one finger away with nothing to give it to, which with
    -- one-finger pan enabled reads as the map having stopped working.
    if place_objects.place_objects then
      return place_objects, false
    end
    return nil, false
  end
  local place_staff = self:getWindow(UIPlaceStaff)
  if place_staff then
    return place_staff, false
  end
  return nil, false
end

--! CorsixTH-iOS @feature 2026-09-07 decide what a one-finger drag means.
--! Anything over a dialog is the
--! dialog's, as elsewhere in the UI. On the map it belongs to whatever is being
--! placed, if anything is; that test comes first, and is why a stray finger can
--! never shift the map out from under a room being sized. With nothing being
--! placed there is nothing for the finger to be busy with, so it pans -- or
--! does nothing, if touch_one_finger_pan is off.
--!param x,y (number) Where the finger first landed, in screen coordinates.
--!return (integer) One of the UI.TOUCH_DRAG_* values.
function GameUI:onTouchDragQuery(x, y)
  local mode = UI.onTouchDragQuery(self, x, y)
  if mode ~= UI.TOUCH_DRAG_NONE then
    return mode
  end
  if self.drag_mouse_move then
    return UI.TOUCH_DRAG_BUTTON
  end
  local placement, wants_button = self:_activePlacement()
  if placement then
    if wants_button then
      return UI.TOUCH_DRAG_BUTTON
    end
    -- A picked-up person has no cancel button anywhere on screen; the only way
    -- to put them back is the right click UIPlaceStaff:onMouseUp handles. Say
    -- so, so the long press that produces it is not suppressed.
    if class.is(placement, UIPlaceStaff) then
      return UI.TOUCH_DRAG_CARRY_CANCELLABLE
    end
    return UI.TOUCH_DRAG_CARRY
  end
  return touch_one_finger_pan and UI.TOUCH_DRAG_CAMERA or UI.TOUCH_DRAG_NONE
end

--! CorsixTH-iOS @feature 2026-09-07 second-finger tap while carrying: rotate.
--! CorsixTH's
--! orientations are discrete -- up to four -- so one tap is one step round
--! them, which is the same shape as the ingame_rotateobject hotkey this stands
--! in for. Where a placement has no orientation, staff being the case in point,
--! this deliberately does nothing at all rather than falling through to a click.
--!return (boolean) event processed indicator
function GameUI:onTouchRotate()
  local placement = self:_activePlacement()
  if placement and placement.tryNextOrientation then
    placement:tryNextOrientation()
    return true
  end
  return false
end

--! Where should a long press deliver its right click?
--!
--! CorsixTH-iOS @bugfix 2026-09-07: at the point the finger pressed, patients
--! and staff have walked away by the time the hold completes, so the click
--! lands on bare floor. That is why picking someone up by long press kept
--! failing, and why neither a shorter hold nor a double tap would fix it: both
--! still aim at a coordinate the target has left.
--!
--! Aim at the entity instead. `cursor_entity` is the one the motion emitted at
--! finger-down resolved -- the same lookup the game already uses for highlights
--! and tooltips -- and no motion is emitted again while the press is merely
--! being held, so it is still the thing that was pressed. Its drawn position is
--! its tile plus the sub-tile offset it has walked into, which is what makes
--! this track a walking target rather than snap between tiles.
--!return (number, number) Screen position to click, or nil to use the press
-- point.
function GameUI:onTouchLongPressAnchor()
  local entity = self.cursor_entity
  if not entity or not entity.tile_x or not entity.th then
    return nil
  end
  local x, y = self:WorldToScreen(entity.tile_x, entity.tile_y)
  local ok, offset_x, offset_y = pcall(entity.th.getPosition, entity.th)
  if ok and offset_x and offset_y then
    local zoom = self:getEffectiveZoom()
    x = x + offset_x * zoom
    y = y + offset_y * zoom
  end
  return x, y
end

--! CorsixTH-iOS @feature 2026-09-07 the staff member a double tap would pick
--! up, if any. Staff are the only thing the engine can pick up: Staff:setPickup
--! exists only on Staff, and Patient:onClick handles no button but "left".
--!return (Staff) The entity, or nil.
function GameUI:_pickableEntity()
  if not self.app.world.user_actions_allowed then
    return nil
  end
  if self:_activePlacement() then
    return nil
  end
  local entity = self.cursor_entity
  if entity and entity.setPickup and not entity.pickup and not entity.fired then
    return entity
  end
  return nil
end

--! CorsixTH-iOS @feature 2026-09-07 should this tap wait for a second one?
--! Only where a double tap would actually do something, which is a member of
--! staff and nothing else. Every other tap in the game -- buttons, rooms,
--! patients, bare floor -- answers no and is delivered the instant the finger
--! lifts.
--!return (boolean) Whether to hold the tap back.
function GameUI:onTouchDeferTap()
  -- Remembered rather than resolved again on the second tap: the whole point is
  -- to catch someone who is walking, and by the second tap they have moved off
  -- the point the first one landed on.
  self.touch_deferred_entity = self:_pickableEntity()
  return self.touch_deferred_entity ~= nil
end

--! CorsixTH-iOS @feature 2026-09-07 two quick taps on a member of staff pick
--! them up. The 400 ms long press this replaces was losing races against people
--! who walk; a double tap is quicker, and because setPickup takes the entity
--! rather than a screen point, it cannot miss a moving target at all.
--!
--! Picking up establishes a mode that outlives the finger: setPickup queues a
--! PickupAction which opens UIPlaceStaff, and from there the existing placement
--! rules apply unchanged -- one finger carries, two fingers pan and zoom, edge
--! scrolling engages. So the sequence is double tap, lift, then drag and
--! release, with a two-finger pan anywhere in between.
--!return (boolean) event processed indicator
function GameUI:onTouchDoubleTap()
  local entity = self.touch_deferred_entity
  self.touch_deferred_entity = nil
  if not entity or entity.pickup or entity.fired then
    return false
  end
  if not self.app.world.user_actions_allowed then
    return false
  end
  -- Close their dialog if it happens to be the one on screen, which is what
  -- UIStaff's own pick-up button does.
  local dialog = self:getWindow(UIStaff)
  if dialog and dialog.staff ~= entity then
    dialog = nil
  end
  entity:setPickup(self, dialog)
  return true
end

--! Check whether the configured mouse drag button is being held down (true) or not (false).
-- fixme: right mouse scrolling currently breaks other mouse operations (see issue 2469).
function GameUI:_isMouseScrollButtonDown()
  local mouse_scroll_button_down
  if self.app.config.right_mouse_scrolling then
    mouse_scroll_button_down = self.buttons_down.mouse_right
  else
    mouse_scroll_button_down = self.buttons_down.mouse_middle
  end
  return mouse_scroll_button_down
end

--! Scroll the map by a possibly fractional amount, carrying the sub-unit
--! remainder over to the next call.
--! GameUI:scrollMap rounds the camera to a whole map-screen unit, so feeding it
--! the small deltas produced at a high frame rate would quantise most of the
--! movement away. Accumulating the remainder keeps the scroll speed correct at
--! any frame rate and removes the visible stepping at native resolution.
--!param dx (number) Horizontal amount to scroll by.
--!param dy (number) Vertical amount to scroll by.
function GameUI:_scrollMapFractional(dx, dy)
  dx = dx + (self.scroll_residual_x or 0)
  dy = dy + (self.scroll_residual_y or 0)
  local old_x, old_y = self.screen_offset_x, self.screen_offset_y
  self:scrollMap(dx, dy)
  -- Clamped, because against the edge of the visible diamond the requested and
  -- the applied movement can differ by an arbitrary amount.
  local rx = dx - (self.screen_offset_x - old_x)
  local ry = dy - (self.screen_offset_y - old_y)
  self.scroll_residual_x = rx < -1 and -1 or (rx > 1 and 1 or rx)
  self.scroll_residual_y = ry < -1 and -1 or (ry > 1 and 1 or ry)
end

--! Advance the camera. Called once per rendered frame, which may be far more
--! often than the simulation tick, so every rate below is expressed per
--! classic tick and scaled by the frame's elapsed time. At exactly one frame
--! per tick this reduces to the behaviour it replaced.
--!param dt (number) Milliseconds since the previous rendered frame.
--!return (boolean) Whether the camera is still moving.
function GameUI:onFrame(dt)
  local ticks = dt / App.TICK_PERIOD_MS
  local moving = false
  local momentum = self.current_momentum
  if not self:_isMouseScrollButtonDown() then
    local decay = self.momentum ^ ticks
    if math.abs(momentum.x) < 0.2 and math.abs(momentum.y) < 0.2 then
      -- Stop scrolling
      momentum.x = 0.0
      momentum.y = 0.0
    else
      momentum.x = momentum.x * decay
      momentum.y = momentum.y * decay
      self:_scrollMapFractional(momentum.x * ticks, momentum.y * ticks)
      moving = true
    end
    if math.abs(momentum.z) > 0.2 then
      self.app.world:adjustZoom(momentum.z * ticks)
      moving = true
    end
    momentum.z = momentum.z * decay
  end
  if self.tick_scroll_amount or self.tick_scroll_amount_mouse then
    -- The scroll amount per tick gradually increases as the duration of the
    -- scroll increases due to this multiplier.
    local mult = self.tick_scroll_mult
    mult = mult + 0.02 * ticks
    if mult > 2 then
      mult = 2
    end
    self.tick_scroll_mult = mult

    -- Combine the mouse scroll and keyboard scroll
    local dx, dy = 0, 0
    if self.tick_scroll_amount_mouse then
      dx, dy = self.tick_scroll_amount_mouse.x, self.tick_scroll_amount_mouse.y
      -- If the middle mouse button is down, then the world is being dragged,
      -- and so the scroll direction due to the cursor being at the map edge
      -- should be opposite to normal to make it feel more natural.
      if self:_isMouseScrollButtonDown() then
        dx, dy = -dx, -dy
      end
    end
    if self.tick_scroll_amount then
      dx = dx + self.tick_scroll_amount.x
      dy = dy + self.tick_scroll_amount.y
    end

    -- Adjust scroll speed based on config value:
    -- there is a separate config value for whether or not shift is held.
    -- the speed is multiplied by 0.5 for consistency between the old and
    -- new configuration. In the past scroll_speed applied only to shift
    -- and defaulted to 2, where 1 was regular scroll speed. By
    -- By multiplying by 0.5, we allow for setting slower than normal
    -- scroll speeds, and ensure there is no behaviour change for players
    -- who do not modify their config file. Later the tick speed was
    -- doubled so now we multiply by 0.25.
    if self.shift_scroll_speed_pressed then
      mult = mult * self.app.config.shift_scroll_speed * 0.25
    else
      mult = mult * self.app.config.scroll_speed * 0.25
    end

    self:_scrollMapFractional(dx * mult * ticks, dy * mult * ticks)
    moving = true
  else
    self.tick_scroll_mult = 1
  end
  -- CorsixTH-iOS @feature 2026-09-07 touch camera, per rendered frame.
  if self:_advanceTouchGlide(dt) then
    moving = true
  end
  if self.touch_gesture_active then
    -- Keep asking for frames while fingers are on the glass, so the camera is
    -- redrawn at the panel rate rather than only when a touch event happens to
    -- land.
    moving = true
  end
  return moving
end

function GameUI:onTick()
  local repaint = UI.onTick(self)
  if self:onCursorWorldPositionChange() then
    repaint = true
  end

  self.announcer:onTick()

  return repaint
end

local abs, sqrt_5, floor = math.abs, math.sqrt(1 / 5), math.floor

function GameUI:scrollMapTo(x, y)
  local zoom = 2 * self:getEffectiveZoom()
  local scr_w, scr_h = TheApp.video:getRenderSize()
  return self:scrollMap(x - self.screen_offset_x - scr_w / zoom,
                        y - self.screen_offset_y - scr_h / zoom)
end

function GameUI.limitPointToDiamond(dx, dy, visible_diamond, do_limit)
  -- If point outside visible diamond, then move point to the nearest position
  -- on the edge of the diamond (NB: relies on diamond.w == 2 * diamond.h).
  local rx = dx - visible_diamond.x
  local ry = dy - visible_diamond.y
  if abs(rx) + abs(ry) * 2 > visible_diamond.w then
    if do_limit then
      -- Determine the quadrant which the point lies in and accordingly set:
      --  (vx, vy) : a unit vector perpendicular to the diamond edge in the quadrant
      --  (p1x, p1y), (p2x, p2y) : the two diamond vertices in the quadrant
      --  d : distance from the point to the line defined by the diamond edge (not the line segment itself)
      local vx, vy, d
      local p1x, p1y, p2x, p2y = 0, 0, 0, 0
      if rx >= 0 and ry >= 0 then
        p1x, p2y =  visible_diamond.w,  visible_diamond.h
        vx, vy = sqrt_5, 2 * sqrt_5
        d = (rx * vx + ry * vy) - (p1x * vx)
      elseif rx >= 0 and ry < 0 then
        p2x, p1y =  visible_diamond.w, -visible_diamond.h
        vx, vy = sqrt_5, -2 * sqrt_5
        d = (rx * vx + ry * vy) - (p2x * vx)
      elseif rx < 0 and ry >= 0 then
        p2x, p1y = -visible_diamond.w,  visible_diamond.h
        vx, vy = -sqrt_5, 2 * sqrt_5
        d = (rx * vx + ry * vy) - (p2x * vx)
      else--if rx < 0 and ry < 0 then
        p1x, p2y = -visible_diamond.w, -visible_diamond.h
        vx, vy = -sqrt_5, -2 * sqrt_5
        d = (rx * vx + ry * vy) - (p1x * vx)
      end
      -- In the unit vector parallel to the diamond edge, resolve the two vertices and
      -- the point, and either move the point to the edge or to one of the two vertices.
      -- NB: vx, vy, p1x, p1y, p2x, p2y are set such that p1 < p2.
      local p1 = vx * p1y - vy * p1x
      local p2 = vx * p2y - vy * p2x
      local pd = vx * ry - vy * rx
      if pd < p1 then
        dx, dy = p1x + visible_diamond.x, p1y + visible_diamond.y
      elseif pd > p2 then
        dx, dy = p2x + visible_diamond.x, p2y + visible_diamond.y
      else--if p1 <= pd and pd <= p2 then
        dx, dy = dx - d * vx, dy - d * vy
      end
      return math.floor(dx), math.floor(dy), true
    else
      return dx, dy, false
    end
  end
  return dx, dy, true
end

function GameUI:scrollMap(dx, dy)
  dx = dx + self.screen_offset_x
  dy = dy + self.screen_offset_y

  dx, dy, self.in_visible_diamond = self.limitPointToDiamond(dx, dy,
    self.visible_diamond, self.limit_to_visible_diamond)

  self.screen_offset_x = floor(dx + 0.5)
  self.screen_offset_y = floor(dy + 0.5)
end

--! Start shaking the screen, e.g. an earthquake effect (unless disabled in config)
--!param intensity (number) The magnitude of the effect, between 0 for no
-- movement to 1 for significant shaking.
function GameUI:beginShakeScreen(intensity)
  if self.app.config.enable_screen_shake then
    self.shake_screen_intensity = intensity
  else
    self.shake_screen_intensity = 0
  end
end

--! Stop the screen from shaking after beginShakeScreen is called.
function GameUI:endShakeScreen()
  self.shake_screen_intensity = 0
end

function GameUI:limitCamera(mode)
  self.limit_to_visible_diamond = mode
  self:scrollMap(0, 0)
end

--! Applies the current setting for wall transparency to the map
function GameUI:applyTransparency()
  self.app.map.th:setWallDrawFlags(self.transparent_walls and 4 or 0)
end

--! Sets wall transparency to the specified parameter
--!param mode (boolean) whether to enable or disable wall transparency
function GameUI:setWallsTransparent(mode)
  if mode ~= self.transparent_walls then
    self.transparent_walls = mode
    self:applyTransparency()
  end
end

function UI:toggleAdviser()
  self.app.config.adviser_disabled = not self.app.config.adviser_disabled
  self.app:saveConfig()
end

function UI:togglePlaySounds()
  self.app.config.play_sounds = not self.app.config.play_sounds
  self.app.audio:playSoundEffects(self.app.config.play_sounds)
  self.app:saveConfig()
end

function UI:togglePlayAnnouncements()
  self.app.config.play_announcements = not self.app.config.play_announcements
  self.app:saveConfig()
end

function UI:togglePlayMusic()
  if not self.app.audio.background_music then
    self.app.config.play_music = true
    self.app.audio:playRandomBackgroundTrack() -- play
  else
    self.app.config.play_music = false
    self.app.audio:stopBackgroundTrack() -- stop
  end
 -- self.app.config.play_music = not self.app.config.play_music
  self.app:saveConfig()
end

local tutorial_phases
local function make_tutorial_phases()
tutorial_phases = {
  {
    -- 1) build reception
    { text = _A.tutorial.build_reception,              -- 1
      begin_callback = function() TheApp.ui:getWindow(UIBottomPanel):startButtonBlinking(3) end,
      end_callback = function() TheApp.ui:getWindow(UIBottomPanel):stopButtonBlinking() end, },
    { text = _A.tutorial.order_one_reception,          -- 2
      begin_callback = function() TheApp.ui:getWindow(UIFurnishCorridor):startButtonBlinking(3) end,
      end_callback = function() TheApp.ui:getWindow(UIFurnishCorridor):stopButtonBlinking(3) end, },
    { text = _A.tutorial.accept_purchase,              -- 3
      begin_callback = function() TheApp.ui:getWindow(UIFurnishCorridor):startButtonBlinking(2) end,
      end_callback = function() TheApp.ui:getWindow(UIFurnishCorridor):stopButtonBlinking(2) end, },
    _A.tutorial.rotate_and_place_reception,            -- 4
    _A.tutorial.reception_invalid_position,            -- 5
                                                       -- 6: object other than reception selected. currently no text for this phase.
  },

  {
    -- 2) hire receptionist
    { text = _A.tutorial.hire_receptionist,            -- 1
      begin_callback = function() TheApp.ui:getWindow(UIBottomPanel):startButtonBlinking(5) end,
      end_callback = function() TheApp.ui:getWindow(UIBottomPanel):stopButtonBlinking() end, },
    { text = _A.tutorial.select_receptionists,         -- 2
      begin_callback = function() TheApp.ui:getWindow(UIHireStaff):startButtonBlinking(4) end,
      end_callback = function() TheApp.ui:getWindow(UIHireStaff):stopButtonBlinking() end, },
    { text = _A.tutorial.next_receptionist,            -- 3
      begin_callback = function() TheApp.ui:getWindow(UIHireStaff):startButtonBlinking(8) end,
      end_callback = function() TheApp.ui:getWindow(UIHireStaff):stopButtonBlinking() end, },
    { text = _A.tutorial.prev_receptionist,            -- 4
      begin_callback = function() TheApp.ui:getWindow(UIHireStaff):startButtonBlinking(5) end,
      end_callback = function() TheApp.ui:getWindow(UIHireStaff):stopButtonBlinking() end, },
    { text = _A.tutorial.choose_receptionist,          -- 5
      begin_callback = function() TheApp.ui:getWindow(UIHireStaff):startButtonBlinking(6) end,
      end_callback = function() TheApp.ui:getWindow(UIHireStaff):stopButtonBlinking() end, },
    _A.tutorial.place_receptionist,                    -- 6
    _A.tutorial.receptionist_invalid_position,         -- 7
  },

  {
    -- 3) build GP's office
    -- 3.1) room window
    { text = _A.tutorial.build_gps_office,             -- 1
      begin_callback = function() TheApp.ui:getWindow(UIBottomPanel):startButtonBlinking(2) end,
      end_callback = function() TheApp.ui:getWindow(UIBottomPanel):stopButtonBlinking() end, },
    { text = _A.tutorial.select_diagnosis_rooms,       -- 2
      begin_callback = function() TheApp.ui:getWindow(UIBuildRoom):startButtonBlinking(1) end,
      end_callback = function() TheApp.ui:getWindow(UIBuildRoom):stopButtonBlinking() end, },
    { text = _A.tutorial.click_gps_office,             -- 3
      begin_callback = function() TheApp.ui:getWindow(UIBuildRoom):startButtonBlinking(5) end,
      end_callback = function() TheApp.ui:getWindow(UIBuildRoom):stopButtonBlinking() end, },

    -- 3.2) blueprint
    -- [11][58] was maybe planned to be used in this place, but is not needed.
    _A.tutorial.click_and_drag_to_build,               -- 4
    _A.tutorial.room_in_invalid_position,              -- 5
    _A.tutorial.room_too_small,                        -- 6
    _A.tutorial.room_too_small_and_invalid,            -- 7
    { text = _A.tutorial.room_big_enough,              -- 8
      begin_callback = function() TheApp.ui:getWindow(UIEditRoom):startButtonBlinking(4) end,
      end_callback = function() TheApp.ui:getWindow(UIEditRoom):stopButtonBlinking() end, },

    -- 3.3) door and windows
    _A.tutorial.place_door,                            -- 9
    _A.tutorial.door_in_invalid_position,              -- 10
    { text = _A.tutorial.place_windows,                -- 11
      begin_callback = function() TheApp.ui:getWindow(UIEditRoom):startButtonBlinking(4) end,
      end_callback = function() TheApp.ui:getWindow(UIEditRoom):stopButtonBlinking() end, },
    { text = _A.tutorial.window_in_invalid_position,   -- 12
      begin_callback = function() TheApp.ui:getWindow(UIEditRoom):startButtonBlinking(4) end,
      end_callback = function() TheApp.ui:getWindow(UIEditRoom):stopButtonBlinking() end, },

    -- 3.4) objects
    _A.tutorial.place_objects,                         -- 13
    _A.tutorial.object_in_invalid_position,            -- 14
    { text = _A.tutorial.confirm_room,                 -- 15
      begin_callback = function() TheApp.ui:getWindow(UIEditRoom):startButtonBlinking(4) end,
      end_callback = function() TheApp.ui:getWindow(UIEditRoom):stopButtonBlinking() end, },
    { text = _A.tutorial.information_window,           -- 16
      begin_callback = function() TheApp.ui:getWindow(UIInformation):startButtonBlinking(1) end,
      end_callback = function() TheApp.ui:getWindow(UIInformation):stopButtonBlinking() end, },
  },

  {
    -- 4) hire doctor
    { text = _A.tutorial.hire_doctor,                  -- 1
      begin_callback = function() TheApp.ui:getWindow(UIBottomPanel):startButtonBlinking(5) end,
      end_callback = function() TheApp.ui:getWindow(UIBottomPanel):stopButtonBlinking() end, },
    { text = _A.tutorial.select_doctors,               -- 2
      begin_callback = function() TheApp.ui:getWindow(UIHireStaff):startButtonBlinking(1) end,
      end_callback = function() TheApp.ui:getWindow(UIHireStaff):stopButtonBlinking() end, },
    { text = _A.tutorial.choose_doctor,                -- 3
      begin_callback = function() TheApp.ui:getWindow(UIHireStaff):startButtonBlinking(6) end,
      end_callback = function() TheApp.ui:getWindow(UIHireStaff):stopButtonBlinking() end, },
    _A.tutorial.place_doctor,                          -- 4
    _A.tutorial.doctor_in_invalid_position,            -- 5
  },
  {
    -- 5) end of tutorial
    { begin_callback = function()
        -- The demo uses a single string for the post-tutorial info while
        -- the real game uses three.
        local texts = TheApp.using_demo_files and {
          {_S.introduction_texts["level15"]},
        } or {
          {_S.introduction_texts["level15"]},
          {_S.introduction_texts["level16"]},
          {_S.introduction_texts["level17"]},
        }
        if TheApp.world.map.level_number ~= 1 then
          table.insert(texts, _S.introduction_texts["level1"])
        end
        TheApp.ui:addWindow(UIInformation(TheApp.ui, texts))
        TheApp.ui:addWindow(UIWatch(TheApp.ui, "initial_opening"))
      end,
    },
  },
}
end
tutorial_phases = setmetatable({}, {__index = function(_, k)
  make_tutorial_phases()
  return tutorial_phases[k]
end})

-- Called to trigger step to another part of the tutorial.
-- chapter:    Individual parts of the tutorial. Step will only happen if it's the current chapter.
-- phase_from: Phase we need to be in for this step to happen. Multiple phases can be given here in an array.
-- phase_to:   Phase we want to step to or "next" to go to next chapter or "end" to end tutorial.
-- returns true if we changed phase, false if we didn't
function GameUI:tutorialStep(chapter, phase_from, phase_to, ...)
  if self.tutorial.chapter ~= chapter then
    return false
  end
  if type(phase_from) == "table" then
    local contains_current = false
    for _, phase in ipairs(phase_from) do
      if phase == self.tutorial.phase then
        contains_current = true
        break
      end
    end
    if not contains_current then return false end
  else
    if self.tutorial.phase ~= phase_from then return false end
  end

  local old_phase = tutorial_phases[self.tutorial.chapter][self.tutorial.phase]
  if old_phase and old_phase.end_callback and type(old_phase.end_callback) == "function" then
    old_phase.end_callback(...)
  end

  if phase_to == "end" then
    self.tutorial.chapter = 0
    self.tutorial.phase = 0
    return true
  elseif phase_to == "next" then
    self.tutorial.chapter = self.tutorial.chapter + 1
    self.tutorial.phase = 1
  else
    self.tutorial.phase = phase_to
  end

  if TheApp.config.debug then print("Tutorial: Now in " .. self.tutorial.chapter .. ", " .. self.tutorial.phase) end
  local new_phase = tutorial_phases[self.tutorial.chapter][self.tutorial.phase]
  local str, callback
  if (type(new_phase) == "table" and type(new_phase.text) == "table") or not new_phase.text then
    str = new_phase.text
    callback = new_phase.begin_callback
  else
    str = new_phase
  end
  if str and str.text then
    self.adviser:say(str, true, true)
  else
    self.adviser.stay_up = nil
  end
  if callback then
    callback(...)
  end
  return true
end

function GameUI:startTutorial(chapter)
  chapter = chapter or 1
  self.tutorial.chapter = chapter
  self.tutorial.phase = 0

  self:tutorialStep(chapter, 0, 1)
end

--! Converts centre of screen coordinates to world tile positions and stores the values for later recall
-- param index (integer) Position in recallpositions table
function GameUI:setMapRecallPosition(index)
  local scr_w, scr_h = TheApp.video:getRenderSize()
  local cx, cy = self:ScreenToWorld(scr_w / 2, scr_h / 2)
  self.recallpositions[index] = {x = cx, y = cy, z = self.zoom_factor}
end

--! Retrieves stored recall position and attempts to scroll to that position - will be limited to the bounds of the camera when zoomed out
-- param index (integer) Position in recallpositions table
function GameUI:recallMapPosition(index)
  if self.recallpositions[index] ~= nil then
    local scr_w, scr_h = TheApp.video:getRenderSize()
    local sx, sy = self.app.map:WorldToScreen(self.recallpositions[index].x,  self.recallpositions[index].y)
    local dx, dy = self.app.map:ScreenToWorld(scr_w / 2, scr_h / 2)
    self:setZoom(self.recallpositions[index].z, false)
    self:scrollMapTo(sx + dx, sy + dy)
  end
end

function GameUI:setEditRoom(enabled)
  -- TODO: Make the room the cursor is over flash
  if enabled then
    self:setCursor(self.edit_room_cursor)
    self.edit_room = true
  else
    -- If the actual editing hasn't started yet but is on its way,
    -- activate the room again.
    if class.is(self.edit_room, Room) and self.cursor == self.waiting_cursor then
      self.app.world:markRoomAsBuilt(self.edit_room)
    else
      -- If we are currently editing a room it may happen that we need to abort it.
      -- Also remove any dialog where the user is buying items.
      local item_window = self.app.ui:getWindow(UIFurnishCorridor)
      if item_window and item_window.edit_dialog then
        item_window:close()
      end
      local edit_window = self.app.ui:getWindow(UIEditRoom)
      if edit_window then
        edit_window:verifyOrAbortRoom()
      end
    end
    self:setCursor(self.default_cursor)
    self.edit_room = false
  end
end

function GameUI:afterLoad(old, new)
  if old < 16 then
    self.zoom_factor = 1
  end
  if old < 23 then
    self.do_world_hit_test = not self:getWindow(UIPlaceObjects)
  end
  if old < 34 then
    self.adviser.queued_messages = {}
    self.adviser.phase = 0
    self.adviser.timer = nil
    self.adviser.frame = 1
    self.adviser.number_frames = 4
  end
  if old < 75 then
    self.current_momentum = { x = 0, y = 0 }
    self.momentum = self.app.config.scrolling_momentum
  end
  if old < 78 then
    self.current_momentum = { x = 0, y = 0, z = 0}
  end
  if old < 115 then
    self.shake_screen_intensity = 0
  end
  if old < 129 then
    self.recallpositions = {}
  end
  if old < 130 then
    self.ticks_since_last_announcement = nil -- cleanup
    self.announcer = Announcer(self.app)
  end
  if old < 240 then
    self.subtitles = Subtitles(self)
    self:addWindow(self.subtitles)
  end
  if old < 264 then
    self.multigesturemove = nil
  end

  self.announcer.playing = false

  self.app:setCaptureMouse()
  return UI.afterLoad(self, old, new)
end

function GameUI:showBriefing()
  local level = self.app.world.map.level_number
  local text = {_S.information.custom_game}
  if type(level) == "number" then
    text = {_S.introduction_texts[TheApp.using_demo_files and "demo" or "level" .. level]}
  elseif self.app.world.map.level_intro then
    text = {self.app.world.map.level_intro}
  end
  self:addWindow(UIInformation(self, text))
end

--! Offers a confirmation window to quit the game and return to main menu
-- NB: overrides UI.quit, do NOT call it from here
--!param mapeditor (boolean) If the user is quitting the map editor
function GameUI:quit(mapeditor)
  local msg = mapeditor and _S.confirmation.quit_mapeditor or _S.confirmation.quit
  self:addWindow(UIConfirmDialog(self, false, msg, --[[persistable:gameui_confirm_quit]] function()
    self.app:loadMainMenu()
    -- Release the mouse regardless of setting
    self.app.video:setCaptureMouse(false)
  end))
end

function GameUI:showCheatsWindow()
  self:addWindow(UICheats(self))
end

function GameUI:showMenuBar()
  self.menu_bar:appear()
end

function GameUI:restartMapEditor()
  self:addWindow(UIConfirmDialog(self, false, _S.confirmation.restart_mapeditor,
    --[[persistable:app_hotkey_confirm_mapeditor_restart]] function() self.app:mapEdit() end))
end

function GameUI:getEffectiveZoom()
  return self.zoom_factor * TheApp.gfx:getWindowDisplayScale()
end
