-- Lottie demo: opens a window at startup with three UILottie widgets that load
-- the same file. Closing the window destroys it (playback, frames and textures
-- go with it); modules.lottie_demo.show() opens it again.

local window = nil
local mainLottie = nil
local toggleLottie = nil
local frameLabel = nil
local markerLabel = nil
local speedButton = nil

local SPEEDS = { 0.5, 1, 2 }
local speedIndex = 2

local DAY_IDLE, DAY_TO_NIGHT = 'Day Idle', 'Day to Night'
local NIGHT_IDLE, NIGHT_TO_DAY = 'Night Idle', 'Night to Day'
local toggleState = nil -- marker that is playing on the toggle
local showEvent = nil

local function hasMarker(widget, name)
  for _, marker in ipairs(widget:getMarkers()) do
    if marker == name then
      return true
    end
  end
  return false
end

local function playMarker(name, loop)
  toggleState = name
  toggleLottie:setLoop(loop)
  toggleLottie:playSegment(name)
  markerLabel:setText('marker: ' .. name)
end

local function updateFrameLabel(widget)
  frameLabel:setText(string.format('frame %d / %d  (%.2fs / %.2fs)', widget:getCurrentFrame() + 1,
    widget:getTotalFrames(), widget:getElapsed(), widget:getDuration()))
end

local function onToggleEnd()
  -- a transition ended: stay on the idle loop of the other side
  if toggleState == DAY_TO_NIGHT then
    playMarker(NIGHT_IDLE, true)
  elseif toggleState == NIGHT_TO_DAY then
    playMarker(DAY_IDLE, true)
  end
end

local function destroyWindow()
  if window then
    window:destroy()
  end
  window = nil
  mainLottie = nil
  toggleLottie = nil
  frameLabel = nil
  markerLabel = nil
  speedButton = nil
  toggleState = nil
end

function show()
  if window then
    window:show()
    window:raise()
    window:focus()
    return
  end

  window = g_ui.displayUI('lottie_demo')
  mainLottie = window:getChildById('mainLottie')
  toggleLottie = window:getChildById('toggleLottie')
  frameLabel = window:getChildById('frameLabel')
  markerLabel = window:getChildById('markerLabel')
  speedButton = window:getChildById('speedButton')

  if not mainLottie:isLoaded() then
    frameLabel:setText('animation not loaded (see the log)')
  else
    mainLottie.onNewFrame = updateFrameLabel
    updateFrameLabel(mainLottie)
  end

  speedIndex = 2
  speedButton:setText('Speed 1x')

  toggleLottie.onLottieEnd = onToggleEnd
  if toggleLottie:isLoaded() and hasMarker(toggleLottie, DAY_IDLE) and hasMarker(toggleLottie, DAY_TO_NIGHT) then
    playMarker(DAY_IDLE, true)
  else
    toggleLottie:play()
    markerLabel:setText('this file has no day/night markers')
  end

  window:raise()
  window:focus()
end

function hide()
  destroyWindow()
end

function play()
  if mainLottie then
    mainLottie:play()
  end
end

function pause()
  if mainLottie then
    mainLottie:pause()
  end
end

function restart()
  if mainLottie then
    mainLottie:reset()
    mainLottie:play()
  end
end

function nextSpeed()
  if not mainLottie then
    return
  end
  speedIndex = speedIndex % #SPEEDS + 1
  mainLottie:setSpeed(SPEEDS[speedIndex])
  speedButton:setText('Speed ' .. SPEEDS[speedIndex] .. 'x')
end

function toggleDayNight()
  if not toggleLottie or not toggleState then
    return
  end
  if toggleState == DAY_IDLE then
    playMarker(DAY_TO_NIGHT, false)
  elseif toggleState == NIGHT_IDLE then
    playMarker(NIGHT_TO_DAY, false)
  end
  -- a click during a transition is ignored
end

function init()
  -- after the other startup modules, so the window opens on top of them
  showEvent = addEvent(function()
    showEvent = nil
    show()
  end)
end

function terminate()
  removeEvent(showEvent)
  showEvent = nil
  destroyWindow()
end
