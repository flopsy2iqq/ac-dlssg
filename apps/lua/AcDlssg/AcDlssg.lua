--[[
  AC DLSS-G: the CSP side of ac-dlssg, the bridge that brings DLSS Frame
  Generation to Assetto Corsa with CSP (design spec 6.6 and 6.9). It does two
  things:

  1. The camera writer. Once per frame, just before the main scene render,
  the app writes one record into the shared memory section
  Local\AcDlssg.Camera.v1. The bridge reads it at CSP's DLSS evaluate of the
  same frame and builds DLSS-G's camera constants from it. The record is a
  seqlock: seq is odd while the app writes and even when the record is
  stable, and frame counts the writes so the bridge can tell a record written
  during this frame from an old one. It runs from the moment AC loads
  (manifest: LAZY = NONE), whether or not the window is open.

  2. The settings window "AC DLSS-G", in ReShade mode and in standalone mode
  alike. It reads the bridge's status from Local\AcDlssg.Status.v1 and writes
  the user's requests (frame generation on or off, the camera switches, save
  as default) into Local\AcDlssg.Control.v1; both are seqlocks too, and the
  bridge applies a request at its next frame.

  The layout strings, the constants and the order of the stores in publish()
  and sendRequest() are checked against src/camera_layout.h and
  src/panel_status.h by tests/test_camera_channel.cpp and tests/test_panel.cpp.
  Change them together.

  Per frame the app allocates nothing: every FFI reference it needs is taken
  once, each frame reads and writes plain numbers, and the window's texts are
  built only when the bridge publishes a new status (about once per second).
]]

local SECTION_NAME = 'AcDlssg.Camera.v1'
local MAGIC = 0x47534C44 -- 'DLSG' little-endian
local VERSION = 1

local LAYOUT = [[
  uint32_t magic; uint32_t version; uint32_t seq; uint32_t frame;
  float pos[3]; float fwd[3]; float up[3]; float side[3];
  float fovVDeg; float clipNear; float clipFar; float originShift[3];
  float renderW; float renderH; uint32_t flags; float dt; double simTimeMs;
]]

local FLAG_JUMPED = 1
local FLAG_PAUSED = 2
local FLAG_REPLAY = 4
local FLAG_VR = 8
local FLAG_TRIPLE = 16
local FLAG_MAIN_MENU = 32
local FLAG_WRITE_FAILED = 64

-- A camera cut: the camera moved further in one frame than anything in AC
-- drives (1 m plus 150 m/s over the frame's real or simulated time, whichever
-- is longer, so fast-forwarded replays do not count), turned by more than
-- 30 degrees, or changed its vertical FOV by more than 5 degrees.
local JUMP_MIN_METERS = 1.0
local JUMP_MAX_SPEED = 150.0
local JUMP_MIN_COS = 0.866
local JUMP_MAX_FOV_DEG = 5.0

local sim = ac.getSim()
local uiState = ac.getUI()
local memoryBarrier = ac.memoryBarrier or function () end

-- References into CSP's state structs and into the section, taken once.
local simPos, simLook, simUp, simSide, simOrigin, simRenderSize
local mmf, mPos, mFwd, mUp, mSide, mOrigin
local frameCounter = 0

local function open()
  simPos, simLook, simUp, simSide = sim.cameraPosition, sim.cameraLook, sim.cameraUp, sim.cameraSide
  simOrigin, simRenderSize = sim.originShift, sim.renderSize
  -- persist = true: CSP never unmaps the view, not even if this reference
  -- were collected; mmf also stays referenced by the callbacks below.
  mmf = ac.writeMemoryMappedFile(SECTION_NAME, LAYOUT, true)
  mPos, mFwd, mUp, mSide, mOrigin = mmf.pos, mmf.fwd, mmf.up, mmf.side, mmf.originShift
  -- An existing section keeps its content: continue its counter, so the
  -- bridge sees fresh records right away after this app reloads.
  frameCounter = mmf.frame
end

local opened, openError = pcall(open)
if not opened then
  ac.error('AcDlssg: cannot open the camera section: ' .. tostring(openError))
end

-- Camera of the previous write, for the cut detection.
local havePrev = false
local prevX, prevY, prevZ, prevFx, prevFy, prevFz, prevFov = 0, 0, 0, 0, 0, 0, 0
local prevMode, prevDriveable, prevCarCamera, prevFocused, prevTrackSet = -1, -1, -1, -1, -1

local function normalize(x, y, z)
  local len = math.sqrt(x * x + y * y + z * z)
  if len > 1e-6 then return x / len, y / len, z / len end
  return x, y, z
end

-- Writes every field except magic, version, seq and frame. Runs inside pcall:
-- an error leaves the record flagged as failed instead of stopping the app.
local function fill()
  local px, py, pz = simPos.x, simPos.y, simPos.z
  local fx, fy, fz = normalize(simLook.x, simLook.y, simLook.z)
  local ux, uy, uz = normalize(simUp.x, simUp.y, simUp.z)
  local sx, sy, sz = normalize(simSide.x, simSide.y, simSide.z)
  local fov = sim.cameraFOV
  local dt = uiState.dt
  local mode, driveable, carCamera = sim.cameraMode, sim.driveableCameraMode, sim.carCameraIndex
  local focused, trackSet = sim.focusedCar, sim.trackCamerasSet

  local jumped = sim.cameraJumped or not havePrev or mode ~= prevMode or driveable ~= prevDriveable
    or carCamera ~= prevCarCamera or focused ~= prevFocused or trackSet ~= prevTrackSet
  if not jumped then
    local dx, dy, dz = px - prevX, py - prevY, pz - prevZ
    local limit = JUMP_MIN_METERS + JUMP_MAX_SPEED * math.max(dt, sim.dt)
    jumped = dx * dx + dy * dy + dz * dz > limit * limit
      or fx * prevFx + fy * prevFy + fz * prevFz < JUMP_MIN_COS
      or math.abs(fov - prevFov) > JUMP_MAX_FOV_DEG
  end
  havePrev = true
  prevX, prevY, prevZ, prevFx, prevFy, prevFz, prevFov = px, py, pz, fx, fy, fz, fov
  prevMode, prevDriveable, prevCarCamera, prevFocused, prevTrackSet = mode, driveable, carCamera, focused, trackSet

  local flags = 0
  if jumped then flags = flags + FLAG_JUMPED end
  if sim.isPaused then flags = flags + FLAG_PAUSED end
  if sim.isReplayActive then flags = flags + FLAG_REPLAY end
  if sim.isVRMode or sim.isVRConnected then flags = flags + FLAG_VR end
  if sim.isTripleMode then flags = flags + FLAG_TRIPLE end
  if sim.isInMainMenu then flags = flags + FLAG_MAIN_MENU end

  -- The 3D render size fed to the upscaler; the window size until CSP has one.
  local rw, rh = simRenderSize.x, simRenderSize.y
  if not (rw > 0 and rh > 0) then rw, rh = sim.windowWidth, sim.windowHeight end

  mPos[0], mPos[1], mPos[2] = px, py, pz
  mFwd[0], mFwd[1], mFwd[2] = fx, fy, fz
  mUp[0], mUp[1], mUp[2] = ux, uy, uz
  mSide[0], mSide[1], mSide[2] = sx, sy, sz
  -- AC's camera FOV is vertical, in degrees (the SDK's ac.GrabbedCamera calls
  -- the same camera's FOV "Original camera vertical FOV, in degrees").
  mmf.fovVDeg = fov
  mmf.clipNear = sim.cameraClipNear
  mmf.clipFar = sim.cameraClipFar
  mOrigin[0], mOrigin[1], mOrigin[2] = simOrigin.x, simOrigin.y, simOrigin.z
  mmf.renderW = rw
  mmf.renderH = rh
  mmf.flags = flags
  mmf.dt = dt
  mmf.simTimeMs = sim.time
end

local writeFailureLogged = false

-- Seqlock writer (spec 6.6). Parity is forced rather than incremented
-- blindly, so a seq left odd by an interrupted write heals with this one; the
-- mask keeps seq below 2^31 so the bit operations never go negative.
local function publish()
  local s = bit.band(bit.bor(mmf.seq, 1), 0x7FFFFFFF)
  mmf.seq = s
  memoryBarrier()
  frameCounter = (frameCounter + 1) % 4294967296
  mmf.magic = MAGIC
  mmf.version = VERSION
  mmf.frame = frameCounter
  local ok, err = pcall(fill)
  if not ok then
    mmf.flags = bit.bor(mmf.flags, FLAG_WRITE_FAILED)
    if not writeFailureLogged then
      writeFailureLogged = true
      ac.error('AcDlssg: camera write failed: ' .. tostring(err))
    end
  end
  memoryBarrier()
  mmf.seq = s + 1
end

-- The main camera as the camera API reports it, to check once in game that
-- the state struct's camera is this frame's camera when the scene is ready.
local probe = vec3()
local probeMismatchLogged = false

local function checkProbe()
  ac.getCameraPositionTo(probe)
  local d = math.abs(probe.x - simPos.x) + math.abs(probe.y - simPos.y) + math.abs(probe.z - simPos.z)
  if d > 0.01 then
    probeMismatchLogged = true
    ac.warn(string.format('AcDlssg: at scene ready ac.getSim().cameraPosition (%.3f, %.3f, %.3f) differs from'
      .. ' ac.getCameraPosition() (%.3f, %.3f, %.3f); originShift (%.3f, %.3f, %.3f)',
      simPos.x, simPos.y, simPos.z, probe.x, probe.y, probe.z, simOrigin.x, simOrigin.y, simOrigin.z))
  end
end

local sceneReadyFired = false
local fallbackWrites = 0
local sceneReadySubscription

if opened then
  -- Fires every frame once shadow maps and the reflection cubemap are
  -- updated and before the main render starts, so before CSP's DLSS pass.
  if render and render.onSceneReady then
    sceneReadySubscription = render.onSceneReady(function ()
      if not sceneReadyFired then
        sceneReadyFired = true
        ac.log('AcDlssg: writing the camera from render.onSceneReady')
      end
      publish()
      if not probeMismatchLogged then checkProbe() end
    end)
  end
end

function script.update(dt)
  -- Fallback only while render.onSceneReady has never fired.
  if not opened or sceneReadyFired then return end
  publish()
  fallbackWrites = fallbackWrites + 1
  if fallbackWrites == 120 then
    ac.warn('AcDlssg: render.onSceneReady has not fired; writing the camera from script.update')
  end
end

-- ---------------------------------------------------------------------------
-- The settings window (spec 6.9): the bridge's status and the user's requests.

local STATUS_SECTION = 'AcDlssg.Status.v1'
local STATUS_MAGIC = 0x54534C44 -- 'DLST' little-endian
local STATUS_VERSION = 1

local STATUS_LAYOUT = [[
  uint32_t magic; uint32_t version; uint32_t seq; uint32_t heartbeat;
  uint32_t ownerPid; uint32_t bridgeState; uint32_t mode; uint32_t fgOn; uint32_t fgUserOn; uint32_t fgPaused;
  uint32_t spoofLoaded; uint32_t rtx30; uint32_t vsyncNote; uint32_t driverWarning;
  uint32_t cameraFlipHandedness; uint32_t cameraNegateSide; uint32_t startWithFg;
  uint32_t controlApplied; uint32_t saveCounter; uint32_t saveOk;
  float baseFps; float presentedFps; float bridgeGpuMs; uint32_t vramUsageMib; uint32_t vramBudgetMib;
  float capturesPerSec; float cameraFreshPerSec; float taggedPerSec;
  char reason[160]; char stateReason[256]; char warning[160]; char gpuName[64]; char hotkey[32];
  char bridgeVersion[32];
]]

local CONTROL_SECTION = 'AcDlssg.Control.v1'
local CONTROL_MAGIC = 0x43534C44 -- 'DLSC' little-endian
local CONTROL_VERSION = 1

local CONTROL_LAYOUT = [[
  uint32_t magic; uint32_t version; uint32_t seq; uint32_t requestCounter;
  uint32_t fgEnabled; uint32_t cameraFlipHandedness; uint32_t cameraNegateSide; uint32_t saveAsDefault;
]]

-- bridgeState and mode of the status record.
local STATE_NOT_LOADED = 0
local STATE_PASS_THROUGH = 1
local STATE_PROXY_NO_FG = 2
local STATE_FG_AVAILABLE = 3
local MODE_RESHADE = 1
local MODE_STANDALONE = 2

-- A presenter publishes at least once per second; a status that has not
-- changed for this long while one should be running means the bridge stopped.
local HEARTBEAT_TIMEOUT = 3.0
-- How often a section that could not be opened is tried again, in seconds.
local REOPEN_INTERVAL = 2.0

local COLOR_ON = rgbm(0.16, 0.55, 0.24, 1)
local COLOR_ON_HOVER = rgbm(0.2, 0.65, 0.3, 1)
local COLOR_OFF = rgbm(0.32, 0.32, 0.32, 1)
local COLOR_OFF_HOVER = rgbm(0.4, 0.4, 0.4, 1)
local COLOR_GOOD = rgbm(0.45, 0.9, 0.5, 1)
local COLOR_WARN = rgbm(1, 0.7, 0.2, 1)
local COLOR_BAD = rgbm(1, 0.35, 0.3, 1)

local LABEL_ON = 'Frame generation: ON###fgToggle'
local LABEL_OFF = 'Frame generation: OFF###fgToggle'
local LABEL_SAVE = 'Save as default'
local LABEL_FLIP = 'Flip handedness (camera_flip_handedness)'
local LABEL_NEGATE = 'Negate the side vector (camera_negate_side)'
local TEXT_NOT_RUNNING = 'Bridge not running'
local TEXT_NOT_RUNNING_HINT = 'No status from ac-dlssg. Check that it is installed (ReShade\'s ProxyLibrary, or the'
  .. ' game folder\'s dxgi.dll in standalone mode) and enabled in ac-dlssg\\ac-dlssg.ini, then see'
  .. ' ac-dlssg\\logs\\bridge.log in the game folder.'
local TEXT_STOPPED_HINT = 'The bridge stopped publishing its status. See ac-dlssg\\logs\\bridge.log in the game folder.'
local TEXT_PAUSED = 'On, but paused: no frames were generated in the last second (Streamline pauses frame'
  .. ' generation while the game window is not focused)'
local TEXT_VSYNC = 'VSync is not available with frame generation here: presenting without VSync (the borderless'
  .. ' window stays tear-free).'

local st, ctl -- the two sections: status read-only, control for writing
local statusSeq = -1 -- seq of the last stable status copy
local lastBeat, lastBeatTime = -1, 0
local requestCounter = 0
local reopenAt = 0
local lastSaveRequest = 0
local desired = { fg = false, flip = false, negate = false }

local function newStatus()
  return {
    valid = false, heartbeat = 0, bridgeState = STATE_NOT_LOADED, mode = 0, fgOn = 0, fgUserOn = 0, fgPaused = 0,
    spoofLoaded = 0, rtx30 = 0, vsyncNote = 0, driverWarning = 0, cameraFlipHandedness = 0, cameraNegateSide = 0,
    startWithFg = 0, controlApplied = 0, saveCounter = 0, saveOk = 0, baseFps = 0, presentedFps = 0,
    bridgeGpuMs = -1, vramUsageMib = 0, vramBudgetMib = 0, capturesPerSec = 0, cameraFreshPerSec = 0,
    taggedPerSec = 0, reason = '', stateReason = '', warning = '', gpuName = '', hotkey = '', bridgeVersion = ''
  }
end

-- The last stable status, and the table the next one is copied into.
local status, spare = newStatus(), newStatus()

-- The window's texts, rebuilt when the status changes.
local texts = {
  status = '', unavailable = '', state = '', fps = '', gpuMs = '', vram = '', gpu = '', mode = '', hotkey = '',
  warning = '', perSecond = '', save = '', stopped = ''
}
local vramLevel = 0 -- 0 fine, 1 near the budget, 2 over it

local statusFailureLogged, controlFailureLogged = false, false

-- Both sections are opened when the window first draws, and tried again
-- every REOPEN_INTERVAL seconds while one is missing (no bridge).
local function openPanelSections()
  if not st then
    local ok, err = pcall(function ()
      st = ac.readMemoryMappedFile(STATUS_SECTION, STATUS_LAYOUT, true)
    end)
    if not ok then
      st = nil
      if not statusFailureLogged then
        statusFailureLogged = true
        ac.log('AcDlssg: the status section is not open: ' .. tostring(err))
      end
    end
  end
  if not ctl then
    local ok, err = pcall(function ()
      ctl = ac.writeMemoryMappedFile(CONTROL_SECTION, CONTROL_LAYOUT, true)
      -- An existing section keeps its content: continue its counter, so
      -- that a reloaded app never repeats a request the bridge applied.
      requestCounter = ctl.requestCounter
    end)
    if not ok then
      ctl = nil
      if not controlFailureLogged then
        controlFailureLogged = true
        ac.error('AcDlssg: cannot open the control section: ' .. tostring(err))
      end
    end
  end
end

-- Seqlock reader: copies the status when its seq changed and is even, and
-- keeps the copy only if seq did not change meanwhile. Strings are built
-- only here, so only when the bridge published something new.
local function readStatus()
  local s1 = st.seq
  if s1 == statusSeq then return false end
  if bit.band(s1, 1) ~= 0 then return false end
  memoryBarrier()
  local c = spare
  c.valid = true
  if st.magic ~= STATUS_MAGIC or st.version ~= STATUS_VERSION then
    c.valid = false
  else
    c.heartbeat = st.heartbeat
    c.bridgeState = st.bridgeState
    c.mode = st.mode
    c.fgOn = st.fgOn
    c.fgUserOn = st.fgUserOn
    c.fgPaused = st.fgPaused
    c.spoofLoaded = st.spoofLoaded
    c.rtx30 = st.rtx30
    c.vsyncNote = st.vsyncNote
    c.driverWarning = st.driverWarning
    c.cameraFlipHandedness = st.cameraFlipHandedness
    c.cameraNegateSide = st.cameraNegateSide
    c.startWithFg = st.startWithFg
    c.controlApplied = st.controlApplied
    c.saveCounter = st.saveCounter
    c.saveOk = st.saveOk
    c.baseFps = st.baseFps
    c.presentedFps = st.presentedFps
    c.bridgeGpuMs = st.bridgeGpuMs
    c.vramUsageMib = st.vramUsageMib
    c.vramBudgetMib = st.vramBudgetMib
    c.capturesPerSec = st.capturesPerSec
    c.cameraFreshPerSec = st.cameraFreshPerSec
    c.taggedPerSec = st.taggedPerSec
    c.reason = ffi.string(st.reason)
    c.stateReason = ffi.string(st.stateReason)
    c.warning = ffi.string(st.warning)
    c.gpuName = ffi.string(st.gpuName)
    c.hotkey = ffi.string(st.hotkey)
    c.bridgeVersion = ffi.string(st.bridgeVersion)
  end
  memoryBarrier()
  if st.seq ~= s1 then return false end
  statusSeq = s1
  status, spare = c, status
  return true
end

-- A request of this app the bridge has not applied yet: the window shows
-- what was asked for until the status confirms it.
local function pending()
  return ctl ~= nil and requestCounter ~= 0 and status.controlApplied ~= requestCounter
end

local function currentFg()
  if pending() then return desired.fg end
  return status.fgUserOn ~= 0
end

local function currentFlip()
  if pending() then return desired.flip end
  return status.cameraFlipHandedness ~= 0
end

local function currentNegate()
  if pending() then return desired.negate end
  return status.cameraNegateSide ~= 0
end

-- Seqlock writer (as publish() above): seq odd, the whole desired state and
-- the new counter, seq even. The bridge applies it at its next frame.
local function sendRequest(fg, flip, negate, save)
  local s = bit.band(bit.bor(ctl.seq, 1), 0x7FFFFFFF)
  ctl.seq = s
  memoryBarrier()
  requestCounter = requestCounter % 0x7FFFFFFF + 1
  ctl.magic = CONTROL_MAGIC
  ctl.version = CONTROL_VERSION
  ctl.fgEnabled = fg and 1 or 0
  ctl.cameraFlipHandedness = flip and 1 or 0
  ctl.cameraNegateSide = negate and 1 or 0
  ctl.saveAsDefault = save and 1 or 0
  ctl.requestCounter = requestCounter
  memoryBarrier()
  ctl.seq = s + 1
  desired.fg, desired.flip, desired.negate = fg, flip, negate
end

local function rebuildTexts()
  local s = status
  if s.fgOn ~= 0 and s.fgPaused ~= 0 then
    texts.status = TEXT_PAUSED
  elseif s.fgOn ~= 0 then
    texts.status = 'On: frame generation is running'
  else
    texts.status = 'Off: ' .. s.reason
  end
  texts.unavailable = 'Frame generation is unavailable: ' .. s.stateReason
  if s.bridgeState == STATE_PASS_THROUGH then
    texts.state = 'The bridge passes the game through: ' .. s.stateReason
  else
    texts.state = 'The bridge is loaded; ' .. s.stateReason
  end
  texts.fps = string.format('Real %.0f fps, output %.0f fps', s.baseFps, s.presentedFps)
  if s.bridgeGpuMs >= 0 then
    texts.gpuMs = string.format('Bridge GPU time %.2f ms per frame', s.bridgeGpuMs)
  else
    texts.gpuMs = 'Bridge GPU time: not measured yet'
  end
  if s.vramBudgetMib > 0 then
    texts.vram = string.format('Video memory %d / %d MiB', s.vramUsageMib, s.vramBudgetMib)
    if s.vramUsageMib >= s.vramBudgetMib then
      vramLevel = 2
    elseif s.vramUsageMib >= s.vramBudgetMib * 0.9 then
      vramLevel = 1
    else
      vramLevel = 0
    end
  else
    texts.vram = 'Video memory: unknown'
    vramLevel = 0
  end
  local spoof = ''
  if s.rtx30 ~= 0 then
    spoof = s.spoofLoaded ~= 0 and ', RTX 30 via dlssg_for_sm86' or ', RTX 30 without dlssg_for_sm86'
  elseif s.spoofLoaded ~= 0 then
    spoof = ', dlssg_for_sm86 loaded'
  end
  texts.gpu = (s.gpuName ~= '' and s.gpuName or 'GPU unknown') .. spoof
  local mode = 'mode unknown'
  if s.mode == MODE_RESHADE then
    mode = 'ReShade mode'
  elseif s.mode == MODE_STANDALONE then
    mode = 'standalone mode'
  end
  texts.mode = 'ac-dlssg ' .. s.bridgeVersion .. ', ' .. mode
  texts.hotkey = 'Hotkey: ' .. s.hotkey
  texts.warning = s.warning
  texts.perSecond = string.format('Per second: %.0f captures, %.0f fresh camera, %.0f tagged', s.capturesPerSec,
    s.cameraFreshPerSec, s.taggedPerSec)
  if lastSaveRequest ~= 0 and s.saveCounter == lastSaveRequest then
    texts.save = s.saveOk ~= 0 and 'Saved as the default in ac-dlssg.ini' or 'Saving failed; see bridge.log'
  else
    texts.save = ''
  end
  texts.stopped = s.stateReason ~= '' and (TEXT_STOPPED_HINT .. ' Last state: ' .. s.stateReason) or TEXT_STOPPED_HINT
end

local function refreshStatus(now)
  if not st or not ctl then
    if now >= reopenAt then
      reopenAt = now + REOPEN_INTERVAL
      openPanelSections()
    end
    if not st then return end
  end
  if readStatus() then
    if status.heartbeat ~= lastBeat then
      lastBeat = status.heartbeat
      lastBeatTime = now
    end
    if status.valid then rebuildTexts() end
  end
end

-- 0: running; 1: no status at all; 2: a presenter stopped publishing.
local function bridgeProblem(now)
  if not st or not status.valid then return 1 end
  if status.bridgeState >= STATE_PROXY_NO_FG and now - lastBeatTime > HEARTBEAT_TIMEOUT then return 2 end
  return 0
end

local toggleSize = vec2(0, 44)
local saveSize = vec2(0, 0)

local function debugContent()
  local controls = ctl ~= nil and status.bridgeState >= STATE_PROXY_NO_FG
  if not controls then ui.pushDisabled() end
  if ui.checkbox(LABEL_FLIP, currentFlip()) and controls then
    sendRequest(currentFg(), not currentFlip(), currentNegate(), false)
  end
  if ui.checkbox(LABEL_NEGATE, currentNegate()) and controls then
    sendRequest(currentFg(), currentFlip(), not currentNegate(), false)
  end
  if not controls then ui.popDisabled() end
  ui.text(texts.perSecond)
end

function script.windowMain(dt)
  local now = os.preciseClock()
  refreshStatus(now)
  local problem = bridgeProblem(now)
  if problem ~= 0 then
    ui.textColored(TEXT_NOT_RUNNING, COLOR_BAD)
    ui.textWrapped(problem == 1 and TEXT_NOT_RUNNING_HINT or texts.stopped)
    return
  end
  local s = status
  if s.bridgeState < STATE_PROXY_NO_FG then
    ui.textColored(texts.state, COLOR_WARN)
    ui.text(texts.gpu)
    ui.text(texts.mode)
    return
  end

  -- The big switch; the hotkey toggles the same state.
  local available = ctl ~= nil and s.bridgeState == STATE_FG_AVAILABLE
  local on = currentFg()
  toggleSize.x = ui.availableSpaceX()
  ui.pushStyleColor(ui.StyleColor.Button, on and COLOR_ON or COLOR_OFF)
  ui.pushStyleColor(ui.StyleColor.ButtonHovered, on and COLOR_ON_HOVER or COLOR_OFF_HOVER)
  ui.pushStyleColor(ui.StyleColor.ButtonActive, on and COLOR_ON_HOVER or COLOR_OFF_HOVER)
  local clicked = ui.button(on and LABEL_ON or LABEL_OFF, toggleSize,
    available and ui.ButtonFlags.None or ui.ButtonFlags.Disabled)
  ui.popStyleColor(3)
  if clicked and available then sendRequest(not on, currentFlip(), currentNegate(), false) end
  if not available then ui.textWrapped(texts.unavailable) end

  -- Wrapped: an off reason or the pause note is longer than the window is wide.
  ui.pushStyleColor(ui.StyleColor.Text, (s.fgOn ~= 0 and s.fgPaused == 0) and COLOR_GOOD or COLOR_WARN)
  ui.textWrapped(texts.status)
  ui.popStyleColor(1)
  ui.text(texts.fps)
  ui.text(texts.gpuMs)
  if vramLevel == 0 then
    ui.text(texts.vram)
  else
    ui.textColored(texts.vram, vramLevel == 2 and COLOR_BAD or COLOR_WARN)
  end
  ui.text(texts.gpu)
  ui.text(texts.mode)
  ui.text(texts.hotkey)
  if s.vsyncNote ~= 0 then ui.textWrapped(TEXT_VSYNC) end
  if s.driverWarning ~= 0 then
    ui.pushStyleColor(ui.StyleColor.Text, COLOR_WARN)
    ui.textWrapped(texts.warning)
    ui.popStyleColor(1)
  end

  ui.offsetCursorY(4)
  if ui.button(LABEL_SAVE, saveSize, ctl ~= nil and ui.ButtonFlags.None or ui.ButtonFlags.Disabled) and ctl ~= nil then
    sendRequest(currentFg(), currentFlip(), currentNegate(), true)
    lastSaveRequest = requestCounter
  end
  if texts.save ~= '' then
    ui.sameLine()
    ui.textDisabled(texts.save)
  end
  ui.offsetCursorY(4)
  ui.treeNode('Debug', ui.TreeNodeFlags.Framed, debugContent)
end
