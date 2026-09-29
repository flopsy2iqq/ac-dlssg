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
  the user's requests (frame generation on or off, 2X/3X/4X, the camera
  switches, save as default) into Local\AcDlssg.Control.v1; both are
  seqlocks too, and the bridge applies a request at its next frame. The
  fields the multiplier and the notes added sit at the end of both layouts,
  so the ones before keep their offsets.

  The layout strings, the constants and the order of the stores in publish()
  and sendRequest() are checked against src/camera_layout.h and
  src/panel_status.h by tests/test_camera_channel.cpp and tests/test_panel.cpp.
  Change them together.

  Per frame the app allocates nothing: every FFI reference it needs is taken
  once, each frame reads and writes plain numbers, and the window's texts are
  built only when the bridge publishes a new status (about once per second).
  The window is custom-drawn (a dark card layout with a red accent, spec 6.9)
  and its animations are numbers eased with the frame time; its colours and
  vectors are made once and rewritten in place.
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
  uint32_t fgMultRequested; uint32_t fgMultUsed; uint32_t fgMultMax;
  char fgMultNote[160]; char vramNote[160]; char restartNote[160];
  char autoFixNote[384];
]]

local CONTROL_SECTION = 'AcDlssg.Control.v1'
local CONTROL_MAGIC = 0x43534C44 -- 'DLSC' little-endian
local CONTROL_VERSION = 1

local CONTROL_LAYOUT = [[
  uint32_t magic; uint32_t version; uint32_t seq; uint32_t requestCounter;
  uint32_t fgEnabled; uint32_t cameraFlipHandedness; uint32_t cameraNegateSide; uint32_t saveAsDefault;
  uint32_t desiredMultiplier;
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

-- The state colours: running, tight or paused, off or an error, and the
-- restart note.
local COLOR_GOOD = rgbm(0.2, 0.78, 0.35, 1)
local COLOR_WARN = rgbm(1, 0.69, 0.13, 1)
local COLOR_BAD = rgbm(1, 0.27, 0.23, 1)
local COLOR_RESTART = rgbm(0.35, 0.66, 1, 1)

local LABEL_SAVE = 'Save as default'
local LABEL_FLIP = 'Flip handedness (camera_flip_handedness)'
local LABEL_NEGATE = 'Negate the side vector (camera_negate_side)'
local TEXT_NOT_RUNNING = 'Bridge not running'
local TEXT_NOT_RUNNING_HINT = 'No status from ac-dlssg. Check that it is installed (ReShade\'s ProxyLibrary, or the'
  .. ' game folder\'s dxgi.dll in standalone mode) and enabled in ac-dlssg\\ac-dlssg.ini, then see'
  .. ' ac-dlssg\\logs\\bridge.log in the game folder.'
local TEXT_STOPPED_HINT = 'The bridge stopped publishing its status. See ac-dlssg\\logs\\bridge.log in the game folder.'
-- A status record with our magic but another version: the bridge and this
-- window come from different releases, and the record is not read.
local TEXT_VERSIONS_DIFFER = 'Bridge and window versions differ'
local TEXT_VERSIONS_HINT = 'The bridge publishes status version %d and this window reads version %d. Run install.bat'
  .. ' of one release again, so that ac-dlssg and this app (apps\\lua\\AcDlssg) come from the same release.'
local TEXT_PAUSED = 'On, but paused: no frames were generated in the last second (Streamline pauses frame'
  .. ' generation while the game window is not focused)'
local TEXT_VSYNC = 'VSync is not available with frame generation here: presenting without VSync (the borderless'
  .. ' window stays tear-free).'
local TEXT_RESTART = 'Restart the game to apply'
local TEXT_MULT_UNSUPPORTED = 'not supported by this GPU/driver'
-- The 2X/3X/4X segments: IDs that never change, so the selection can move
-- without ImGui seeing new widgets; the text drawn on them is MULT_TEXTS.
local MULT_LABELS = { [2] = '2X###fgMult2', [3] = '3X###fgMult3', [4] = '4X###fgMult4' }
local MULT_TEXTS = { [2] = '2X', [3] = '3X', [4] = '4X' }

local st, ctl -- the two sections: status read-only, control for writing
local statusSeq = -1 -- seq of the last stable status copy
local lastBeat, lastBeatTime = -1, 0
local requestCounter = 0
local reopenAt = 0
local lastSaveRequest = 0
-- mult: the multiplier asked for (2..4), or 0 to keep the bridge's.
local desired = { fg = false, flip = false, negate = false, mult = 0 }

local function newStatus()
  return {
    valid = false, heartbeat = 0, bridgeState = STATE_NOT_LOADED, mode = 0, fgOn = 0, fgUserOn = 0, fgPaused = 0,
    spoofLoaded = 0, rtx30 = 0, vsyncNote = 0, driverWarning = 0, cameraFlipHandedness = 0, cameraNegateSide = 0,
    startWithFg = 0, controlApplied = 0, saveCounter = 0, saveOk = 0, baseFps = 0, presentedFps = 0,
    bridgeGpuMs = -1, vramUsageMib = 0, vramBudgetMib = 0, capturesPerSec = 0, cameraFreshPerSec = 0,
    taggedPerSec = 0, reason = '', stateReason = '', warning = '', gpuName = '', hotkey = '', bridgeVersion = '',
    fgMultRequested = 0, fgMultUsed = 0, fgMultMax = 0, fgMultNote = '', vramNote = '', restartNote = '',
    autoFixNote = '',
    -- Not a field: the version of a record with our magic that this window
    -- cannot read, else 0.
    otherVersion = 0
  }
end

-- The last stable status, and the table the next one is copied into.
local status, spare = newStatus(), newStatus()

-- The window's texts, rebuilt when the status changes.
local texts = {
  status = '', unavailable = '', state = '', version = '', fgHint = '', gpuMs = '', vramValue = '', gpu = '',
  mode = '', hotkey = '', warning = '', perSecond = '', save = '', stopped = '', mult = '', multNote = '',
  vramNote = '', restartNote = '', autoFixNote = '', versions = ''
}
local vramLevel = 0 -- 0 fine, 1 near the budget, 2 over it
local vramFill = 0 -- used / budget, 0 while unknown

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
    c.otherVersion = st.magic == STATUS_MAGIC and st.version or 0
  else
    c.otherVersion = 0
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
    c.fgMultRequested = st.fgMultRequested
    c.fgMultUsed = st.fgMultUsed
    c.fgMultMax = st.fgMultMax
    c.fgMultNote = ffi.string(st.fgMultNote)
    c.vramNote = ffi.string(st.vramNote)
    c.restartNote = ffi.string(st.restartNote)
    c.autoFixNote = ffi.string(st.autoFixNote)
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

-- The multiplier asked for; 0 while the bridge publishes none (an older one).
local function currentMult()
  if pending() and desired.mult ~= 0 then return desired.mult end
  return status.fgMultRequested
end

-- Seqlock writer (as publish() above): seq odd, the whole desired state and
-- the new counter, seq even. The bridge applies it at its next frame. mult
-- is 2..4, or 0 to keep the bridge's multiplier.
local function sendRequest(fg, flip, negate, save, mult)
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
  ctl.desiredMultiplier = mult
  ctl.requestCounter = requestCounter
  memoryBarrier()
  ctl.seq = s + 1
  desired.fg, desired.flip, desired.negate, desired.mult = fg, flip, negate, mult
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
  texts.version = s.bridgeVersion ~= '' and ('v' .. s.bridgeVersion) or ''
  texts.fgHint = s.hotkey ~= '' and ('Hotkey ' .. s.hotkey) or ''
  if s.bridgeGpuMs >= 0 then
    texts.gpuMs = string.format('%.2f ms per frame', s.bridgeGpuMs)
  else
    texts.gpuMs = 'not measured yet'
  end
  if s.vramBudgetMib > 0 then
    texts.vramValue = string.format('%d / %d MiB', s.vramUsageMib, s.vramBudgetMib)
    vramFill = s.vramUsageMib / s.vramBudgetMib
    if s.vramUsageMib >= s.vramBudgetMib then
      vramLevel = 2
    elseif s.vramUsageMib >= s.vramBudgetMib * 0.9 then
      vramLevel = 1
    else
      vramLevel = 0
    end
  else
    texts.vramValue = 'unknown'
    vramFill = 0
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
  texts.hotkey = s.hotkey
  texts.warning = s.warning
  texts.perSecond = string.format('%.0f captures, %.0f fresh camera, %.0f tagged', s.capturesPerSec,
    s.cameraFreshPerSec, s.taggedPerSec)
  if s.fgMultUsed ~= 0 and s.fgMultUsed ~= s.fgMultRequested then
    texts.mult = string.format('Multiplier (using %dX)', s.fgMultUsed)
  else
    texts.mult = 'Multiplier'
  end
  texts.multNote = s.fgMultNote
  -- While the guard keeps DLSS-G off, the status line already says why.
  texts.vramNote = s.vramNote ~= s.reason and s.vramNote or ''
  texts.restartNote = s.restartNote
  texts.autoFixNote = s.autoFixNote
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
    if status.valid then
      rebuildTexts()
    elseif status.otherVersion ~= 0 then
      texts.versions = string.format(TEXT_VERSIONS_HINT, status.otherVersion, STATUS_VERSION)
    end
  end
end

-- 0: running; 1: no status at all; 2: a presenter stopped publishing; 3: a
-- status of another version (bridge and window of different releases).
local function bridgeProblem(now)
  if not st then return 1 end
  if not status.valid then return status.otherVersion ~= 0 and 3 or 1 end
  if status.bridgeState >= STATE_PROXY_NO_FG and now - lastBeatTime > HEARTBEAT_TIMEOUT then return 2 end
  return 0
end

-- ---------------------------------------------------------------------------
-- Drawing (spec 6.9, the window's layout). Top to bottom: the header with
-- the status pill, the notes, the frame generation switch, the 2X/3X/4X
-- segments, the fps card, video memory, "Save as default" and "Details".
--
-- Everything is drawn with the ui.draw* primitives at positions taken from
-- the cursor (window space, as the drawing functions expect), and every block
-- ends with an item of its size so that the window's layout and scrolling
-- know it. Per frame nothing is allocated: the colours and vectors below are
-- made once and rewritten in place, the animations are plain numbers eased
-- with the frame time, the numbers' strings are cached, and a note is
-- measured only when its text or the window's width changes.
--
-- The constants and the animation state are grouped in tables: LuaJIT allows
-- 200 locals per function, the main chunk included, and the camera writer and
-- the channels above already use more than half of them.

local FONT = {
  text = 'Segoe UI:@System',
  semi = 'Segoe UI:@System;Weight=SemiBold',
  bold = 'Segoe UI:@System;Weight=Bold',
  digits = 'Bahnschrift:@System;Weight=SemiBold',
}
local LOGO_IMAGE = 'logo.png' -- relative to the app folder

local WORDS = {
  title = 'AC DLSS-G', fg = 'Frame generation', fpsReal = 'real fps', fpsOutput = 'output fps',
  vram = 'Video memory', details = 'Details', passThrough = 'Passing the game through',
  waiting = 'Waiting for the game', gpu = 'GPU', bridge = 'Bridge', gpuTime = 'GPU time', hotkey = 'Hotkey',
  perSecond = 'Per second',
}
local PILL = {
  running = { [2] = 'Running 2X', [3] = 'Running 3X', [4] = 'Running 4X' }, runningAny = 'Running',
  paused = 'Paused', off = 'Off', unavailable = 'Unavailable', waiting = 'Waiting', notRunning = 'Not running',
  versions = 'Versions differ',
}
local ID = {
  toggle = '###fgToggle', save = '###fgSave', details = '###fgDetails', detailsChild = 'fgDetailsContent',
}

-- The palette: near-black panels, racing red, white and two greys.
local C = {
  panel = rgbm(0.043, 0.043, 0.051, 0.94), -- #0B0B0D
  track = rgbm(0.086, 0.086, 0.106, 1),    -- #16161B
  line = rgbm(0.15, 0.15, 0.17, 1),        -- #26262C
  button = rgbm(0.075, 0.075, 0.09, 0.96),
  buttonHover = rgbm(0.12, 0.1, 0.105, 0.96),
  hover = rgbm(1, 1, 1, 0.06),
  text = rgbm(1, 1, 1, 1),
  muted = rgbm(0.61, 0.61, 0.65, 1),       -- #9C9CA6
  faint = rgbm(0.37, 0.37, 0.41, 1),       -- #5E5E68
  red = rgbm(0.882, 0.024, 0, 1),          -- #E10600
  redDeep = rgbm(0.478, 0.016, 0, 1),      -- #7A0400
  switchOff = rgbm(0.17, 0.17, 0.2, 1),
  knobOff = rgbm(0.8, 0.8, 0.84, 1),
  shadow = rgbm(0, 0, 0, 0.35),
  barReal = rgbm(0.86, 0.86, 0.9, 1),
  barNormal = rgbm(0.72, 0.73, 0.78, 1),
}
-- Scratch colours, rewritten for each shape that needs a computed colour.
local c1, c2 = rgbm(0, 0, 0, 0), rgbm(0, 0, 0, 0)
-- The pill's colour, eased towards the state's.
local pillColor = rgbm(0.61, 0.61, 0.65, 1)

-- Scratch positions, the item size and the details' size.
local P1, P2, P3 = vec2(0, 0), vec2(0, 0), vec2(0, 0)
local SIZE, CHILD_SIZE = vec2(0, 0), vec2(0, 0)

local K = {
  alignStart = ui.Alignment.Start, alignCenter = ui.Alignment.Center, alignEnd = ui.Alignment.End,
  corners = ui.CornerFlags.All, buttonNone = ui.ButtonFlags.None, buttonDisabled = ui.ButtonFlags.Disabled,
  -- The details' child: the wheel goes to the window (the SDK: "forwarded to
  -- the parent unless NoScrollbar is also set", so no NoScrollbar here).
  childFlags = bit.bor(ui.WindowFlags.NoScrollWithMouse, ui.WindowFlags.NoBackground),
}

-- Layout, in pixels; gap is added between blocks on top of ImGui's spacing.
local L = {
  gap = 8, headerH = 34, pillW = 108, pillH = 22, switchH = 60, segH = 34, fpsH = 88, saveH = 34,
  detailsRowH = 26, detailValueX = 84, cardPad = 10,
  savedHold = 2.2, savedFade = 0.8, -- seconds "Saved" shows, then fades
}

-- The frame time for the easing, and the content column of this frame.
local frameDt = 0
local lx, lw = 0, 0

-- Animation state: every value eases towards its target each frame.
local anim = {
  switchT = -1, switchHover = 0, switchPress = 0,
  segPos = -1, segHover = { [2] = 0, [3] = 0, [4] = 0 },
  fpsReal = -1, fpsOut = 0, fpsShare = 1,
  vramShown = 0, vramLevel = 0,
  saveHover = 0, savePress = 0, savedAt = -100, lastSaveText = '',
  detailsOpen = false, detailsT = 0, detailsH = 0, detailsHover = 0, detailsControls = false,
  detailsFailureLogged = false,
}

-- Exponential easing towards a target at a rate per second: the same curve
-- at any frame rate.
local function ease(value, target, rate)
  local v = value + (target - value) * (1 - math.exp(-rate * frameDt))
  if math.abs(target - v) < 0.001 then return target end
  return v
end

local function mix(a, b, t)
  return a + (b - a) * t
end

-- out = a to b at t, its alpha scaled by alpha.
local function lerpColor(out, a, b, t, alpha)
  out.r = a.r + (b.r - a.r) * t
  out.g = a.g + (b.g - a.g) * t
  out.b = a.b + (b.b - a.b) * t
  out.mult = (a.mult + (b.mult - a.mult) * t) * alpha
  return out
end

-- out = a with its alpha scaled by alpha.
local function faded(out, a, alpha)
  out.r, out.g, out.b, out.mult = a.r, a.g, a.b, a.mult * alpha
  return out
end

local function fillRect(x1, y1, x2, y2, color, rounding)
  P1.x, P1.y, P2.x, P2.y = x1, y1, x2, y2
  ui.drawRectFilled(P1, P2, color, rounding, K.corners)
end

local function strokeRect(x1, y1, x2, y2, color, rounding)
  P1.x, P1.y, P2.x, P2.y = x1, y1, x2, y2
  ui.drawRect(P1, P2, color, rounding, K.corners, 1)
end

local function fillCircle(x, y, radius, color)
  P1.x, P1.y = x, y
  ui.drawCircleFilled(P1, radius, color, 24)
end

local function line(x1, y1, x2, y2, color, thickness)
  P1.x, P1.y, P2.x, P2.y = x1, y1, x2, y2
  ui.drawLine(P1, P2, color, thickness)
end

-- A TTF text in a box, vertically centred.
local function label(text, font, size, x1, y1, x2, y2, align, color)
  P1.x, P1.y, P2.x, P2.y = x1, y1, x2, y2
  ui.pushDWriteFont(font)
  ui.dwriteDrawTextClipped(text, size, P1, P2, align, K.alignCenter, false, color)
  ui.popDWriteFont()
end

-- Closes a block drawn from (x, y): one item of its size at its place, and
-- the cursor below it.
local function finishBlock(x, y, h)
  ui.setCursorX(x)
  ui.setCursorY(y)
  SIZE.x, SIZE.y = lw, h
  ui.dummy(SIZE)
  ui.offsetCursorY(L.gap)
end

-- A wrapped line of the window's font in one colour, across the column.
local function wrappedText(text, color)
  ui.setCursorX(lx + 2)
  ui.pushStyleColor(ui.StyleColor.Text, color)
  ui.textWrapped(text, lx + lw - 2)
  ui.popStyleColor(1)
  ui.offsetCursorY(L.gap)
end

local function sectionLabel(text)
  local x, y = lx, ui.getCursorY()
  label(text, FONT.text, 12, x + 2, y, x + lw, y + 18, K.alignStart, C.muted)
  SIZE.x, SIZE.y = lw, 18
  ui.dummy(SIZE)
end

-- Integer strings for the eased numbers, each made once.
local NUMBER_TEXT = {}
local function numberText(value)
  local n = math.floor(value + 0.5)
  if not (n >= 0) then n = 0 elseif n > 99999 then n = 99999 end
  local t = NUMBER_TEXT[n]
  if not t then
    t = tostring(n)
    NUMBER_TEXT[n] = t
  end
  return t
end

-- Header: the logo, the title and the bridge's version, and the status pill
-- whose dot pulses (a ping while frame generation runs, a slow breath else).
local function pillState(problem)
  if problem == 3 then return PILL.versions, COLOR_BAD, false end
  if problem ~= 0 then return PILL.notRunning, COLOR_BAD, false end
  local s = status
  if s.bridgeState == STATE_NOT_LOADED then return PILL.waiting, COLOR_WARN, false end
  if s.bridgeState < STATE_FG_AVAILABLE then return PILL.unavailable, COLOR_BAD, false end
  if s.fgOn ~= 0 and s.fgPaused ~= 0 then return PILL.paused, COLOR_WARN, false end
  if s.fgOn ~= 0 then
    local m = s.fgMultUsed ~= 0 and s.fgMultUsed or s.fgMultRequested
    return PILL.running[m] or PILL.runningAny, texts.vramNote ~= '' and COLOR_WARN or COLOR_GOOD, true
  end
  return PILL.off, COLOR_BAD, false
end

local function drawHeader(problem, now)
  local x, y = lx, ui.getCursorY()
  P1.x, P1.y, P2.x, P2.y = x, y + 3, x + 28, y + 31
  ui.drawImage(LOGO_IMAGE, P1, P2)
  local textRight = x + lw - L.pillW - 6
  label(WORDS.title, FONT.bold, 16, x + 36, y, textRight, y + 20, K.alignStart, C.text)
  label(texts.version, FONT.text, 11, x + 36, y + 19, textRight, y + 33, K.alignStart, C.muted)

  local text, color, running = pillState(problem)
  pillColor.r = ease(pillColor.r, color.r, 8)
  pillColor.g = ease(pillColor.g, color.g, 8)
  pillColor.b = ease(pillColor.b, color.b, 8)
  local px2 = x + lw
  local px1 = px2 - L.pillW
  local py1 = y + (L.headerH - L.pillH) / 2
  local py2 = py1 + L.pillH
  fillRect(px1, py1, px2, py2, faded(c1, pillColor, 0.14), L.pillH / 2)
  strokeRect(px1, py1, px2, py2, faded(c1, pillColor, 0.4), L.pillH / 2)
  local dx, dy = px1 + 12, py1 + L.pillH / 2
  if running then
    local phase = (now * 0.7) % 1
    fillCircle(dx, dy, 3.5 + 5 * phase, faded(c1, pillColor, 0.45 * (1 - phase)))
    fillCircle(dx, dy, 3.5, pillColor)
  else
    fillCircle(dx, dy, 3.5, faded(c1, pillColor, 0.7 + 0.3 * math.sin(now * 2.4)))
  end
  label(text, FONT.semi, 12, px1 + 21, py1, px2 - 6, py2, K.alignStart, lerpColor(c1, pillColor, C.text, 0.35, 1))

  -- A hairline under the header, red where it starts.
  local ly = y + L.headerH + 4
  fillRect(x, ly, x + lw, ly + 1, C.line, 0)
  fillRect(x, ly, x + 28, ly + 1, C.red, 0)
  finishBlock(x, y, L.headerH + 5)
end

-- A note as a small card that fades in: an accent bar, an optional title and
-- the text wrapped inside. A card holds its fade and the measured text height.
local function newCard()
  return { alpha = 0, text = '', wrapW = -1, textH = 0 }
end

local cards = {
  autoFix = newCard(), restart = newCard(), vsync = newCard(), driver = newCard(), state = newCard(),
  problem = newCard(),
}

local function noteCard(card, title, text, accent)
  local x, y = lx, ui.getCursorY()
  local textX = x + L.cardPad + 5
  local wrapX = x + lw - L.cardPad
  local wrapW = wrapX - textX
  if card.text ~= text or card.wrapW ~= wrapW then
    card.text, card.wrapW = text, wrapW
    card.textH = ui.measureText(text, wrapW).y
  end
  card.alpha = ease(card.alpha, 1, 7)
  local a = card.alpha
  local titleH = title and 20 or 0
  local h = L.cardPad * 2 + titleH + card.textH
  fillRect(x, y, x + lw, y + h, faded(c1, C.panel, a), 8)
  fillRect(x, y, x + lw, y + h, faded(c1, accent, 0.08 * a), 8)
  fillRect(x + 5, y + 7, x + 8, y + h - 7, faded(c1, accent, a), 1.5)
  if title then
    label(title, FONT.semi, 13, textX, y + L.cardPad - 2, wrapX, y + L.cardPad + titleH - 2, K.alignStart,
      faded(c1, accent, a))
  end
  ui.setCursorX(textX)
  ui.setCursorY(y + L.cardPad + titleH)
  ui.pushStyleColor(ui.StyleColor.Text, lerpColor(c1, accent, C.text, title and 0.85 or 0.55, a))
  ui.textWrapped(text, wrapX)
  ui.popStyleColor(1)
  finishBlock(x, y, h)
end

-- The big switch: the whole card is the button, the knob slides and the
-- track fades to red, the card's edge warms on hover.
local function fgSwitch(available)
  local on = currentFg()
  local x, y = lx, ui.getCursorY()
  SIZE.x, SIZE.y = lw, L.switchH
  local clicked = ui.invisibleButton(ID.toggle, SIZE, available and K.buttonNone or K.buttonDisabled)
  local hovered = available and ui.itemHovered()
  local pressed = available and ui.itemActive()
  if hovered then ui.setMouseCursor(ui.MouseCursor.Hand) end
  -- The hotkey toggles the same state.
  if clicked and available then
    sendRequest(not on, currentFlip(), currentNegate(), false, currentMult())
    on = not on
  end
  local a = anim
  if a.switchT < 0 then a.switchT = on and 1 or 0 end
  a.switchT = ease(a.switchT, on and 1 or 0, 12)
  a.switchHover = ease(a.switchHover, hovered and 1 or 0, 12)
  a.switchPress = ease(a.switchPress, pressed and 1 or 0, 24)
  local t = a.switchT
  local alpha = available and 1 or 0.45

  fillRect(x, y, x + lw, y + L.switchH, C.panel, 10)
  strokeRect(x, y, x + lw, y + L.switchH, lerpColor(c1, C.line, C.redDeep, a.switchHover, 1), 10)
  local textRight = x + lw - 84
  label(WORDS.fg, FONT.semi, 15, x + 16, y + 10, textRight, y + 32, K.alignStart, faded(c1, C.text, alpha))
  label(texts.fgHint, FONT.text, 12, x + 16, y + 32, textRight, y + 50, K.alignStart, faded(c1, C.muted, alpha))

  local tw, th = 50, 28
  local tx2 = x + lw - 16
  local tx1 = tx2 - tw
  local ty1 = y + (L.switchH - th) / 2
  lerpColor(c1, C.switchOff, C.red, t, alpha)
  local lift = 0.1 * a.switchHover
  c1.r, c1.g, c1.b = mix(c1.r, 1, lift), mix(c1.g, 1, lift), mix(c1.b, 1, lift)
  fillRect(tx1, ty1, tx2, ty1 + th, c1, th / 2)
  local r = th / 2 - 3 - 1.5 * a.switchPress
  local kx = mix(tx1 + th / 2, tx2 - th / 2, t)
  local ky = ty1 + th / 2
  fillCircle(kx, ky + 1, r + 1, faded(c1, C.shadow, alpha))
  fillCircle(kx, ky, r, lerpColor(c1, C.knobOff, C.text, t, alpha))
  finishBlock(x, y, L.switchH)
end

-- The 2X/3X/4X segments (spec 6.9): the red highlight slides to the one
-- asked for; one above the maximum Streamline reported (fgMultMax, 0 while
-- unknown) is dimmed and says why on hover. The bridge applies a click at
-- its next frame.
local function multiplierButtons(available)
  local selected = currentMult()
  local max = status.fgMultMax
  local a = anim
  sectionLabel(texts.mult)
  local x, y = lx, ui.getCursorY()
  local pad = 3
  local segH = L.segH
  local segW = (lw - pad * 2) / 3
  fillRect(x, y, x + lw, y + segH, C.track, 9)
  strokeRect(x, y, x + lw, y + segH, C.line, 9)
  local target = (selected >= 2 and selected <= 4) and selected - 2 or -1
  if target >= 0 then
    if a.segPos < 0 then a.segPos = target end
    a.segPos = ease(a.segPos, target, 14)
    local hx = x + pad + a.segPos * segW
    fillRect(hx, y + pad, hx + segW, y + segH - pad, faded(c1, C.red, available and 1 or 0.45), 7)
  end
  for m = 2, 4 do
    local supported = max < 2 or m <= max
    local usable = available and supported
    local chosen = m == selected
    local sx = x + pad + (m - 2) * segW
    ui.setCursorX(sx)
    ui.setCursorY(y + pad)
    SIZE.x, SIZE.y = segW, segH - pad * 2
    local clicked = ui.invisibleButton(MULT_LABELS[m], SIZE, usable and K.buttonNone or K.buttonDisabled)
    local hovered = ui.itemHovered(ui.HoveredFlags.AllowWhenDisabled)
    if not supported and hovered then
      ui.setTooltip(TEXT_MULT_UNSUPPORTED)
    end
    if usable and hovered and not chosen then ui.setMouseCursor(ui.MouseCursor.Hand) end
    local hover = ease(a.segHover[m], (usable and hovered and not chosen) and 1 or 0, 14)
    a.segHover[m] = hover
    if hover > 0 then
      fillRect(sx, y + pad, sx + segW, y + segH - pad, faded(c1, C.hover, hover), 7)
    end
    -- The text turns white as the highlight arrives.
    local near = target >= 0 and math.max(0, 1 - math.abs(a.segPos - (m - 2))) or 0
    label(MULT_TEXTS[m], FONT.bold, 14, sx, y, sx + segW, y + segH, K.alignCenter,
      lerpColor(c1, usable and C.muted or C.faint, C.text, near, 1))
    if clicked and usable and not chosen then
      sendRequest(currentFg(), currentFlip(), currentNegate(), false, m)
    end
  end
  finishBlock(x, y, segH)
  if texts.multNote ~= '' then wrappedText(texts.multNote, COLOR_WARN) end
end

-- Real and output fps: numbers that ease to each new value, an arrow whose
-- chevrons run while frames are generated, and a bar of the real and the
-- generated share of the output.
local function fpsCard(running, now)
  local s = status
  local a = anim
  local real, out = s.baseFps, s.presentedFps
  if not (real >= 0) then real = 0 end
  if not (out >= 0) then out = 0 end
  if a.fpsReal < 0 then a.fpsReal, a.fpsOut = real, out end
  a.fpsReal = ease(a.fpsReal, real, 6)
  a.fpsOut = ease(a.fpsOut, out, 6)
  a.fpsShare = ease(a.fpsShare, out > real and real / out or 1, 6)

  local x, y = lx, ui.getCursorY()
  local h = L.fpsH
  fillRect(x, y, x + lw, y + h, C.panel, 10)
  strokeRect(x, y, x + lw, y + h, C.line, 10)
  local mid = x + lw / 2
  label(numberText(a.fpsReal), FONT.digits, 30, x + 12, y + 8, mid - 24, y + 46, K.alignEnd, C.text)
  label(numberText(a.fpsOut), FONT.digits, 30, mid + 24, y + 8, x + lw - 12, y + 46, K.alignStart,
    running and C.red or C.text)
  label(WORDS.fpsReal, FONT.text, 11, x + 12, y + 45, mid - 24, y + 59, K.alignEnd, C.muted)
  label(WORDS.fpsOutput, FONT.text, 11, mid + 24, y + 45, x + lw - 12, y + 59, K.alignStart, C.muted)

  -- Three chevrons between the numbers; a wave runs through them.
  local ay = y + 28
  local phase = (now * 1.4) % 1
  for i = 0, 2 do
    local cx = mid - 9 + i * 7
    local glow = 0
    if running then glow = math.max(0, math.cos((phase - i / 3) * math.pi * 2)) end
    local col = lerpColor(c1, C.faint, C.red, glow, 1)
    line(cx - 2.5, ay - 5, cx + 2.5, ay, col, 2)
    line(cx + 2.5, ay, cx - 2.5, ay + 5, col, 2)
  end

  local bx1, bx2 = x + 14, x + lw - 14
  local by = y + h - 17
  fillRect(bx1, by, bx2, by + 5, C.track, 2.5)
  local split = bx1 + (bx2 - bx1) * math.min(math.max(a.fpsShare, 0), 1)
  fillRect(bx1, by, split, by + 5, C.barReal, 2.5)
  if split < bx2 - 3 then fillRect(split + 2, by, bx2, by + 5, C.red, 2.5) end
  finishBlock(x, y, h)
end

-- Video memory: used / budget as a bar that eases to each value, grey while
-- it fits, amber from 90% of the budget and red above it.
local function vramBlock()
  local x, y = lx, ui.getCursorY()
  local a = anim
  a.vramShown = ease(a.vramShown, vramFill, 5)
  a.vramLevel = ease(a.vramLevel, vramLevel, 6)
  if a.vramLevel <= 1 then
    lerpColor(c2, C.barNormal, COLOR_WARN, a.vramLevel, 1)
  else
    lerpColor(c2, COLOR_WARN, COLOR_BAD, a.vramLevel - 1, 1)
  end
  label(WORDS.vram, FONT.text, 12, x + 2, y, x + lw, y + 18, K.alignStart, C.muted)
  label(texts.vramValue, FONT.semi, 12, x, y, x + lw - 2, y + 18, K.alignEnd, vramLevel > 0 and c2 or C.text)
  local by = y + 22
  fillRect(x, by, x + lw, by + 6, C.track, 3)
  if a.vramShown > 0.002 then
    fillRect(x, by, x + math.max(lw * math.min(a.vramShown, 1), 6), by + 6, c2, 3)
  end
  finishBlock(x, y, 28)
end

-- "Save as default": warms on hover, sinks while pressed, and says "Saved"
-- for a moment in place of its label. A failed save stays under it.
local function saveButton(now)
  local usable = ctl ~= nil
  local a = anim
  local x, y = lx, ui.getCursorY()
  local h = L.saveH
  SIZE.x, SIZE.y = lw, h
  local clicked = ui.invisibleButton(ID.save, SIZE, usable and K.buttonNone or K.buttonDisabled)
  local hovered = usable and ui.itemHovered()
  local pressed = usable and ui.itemActive()
  if hovered then ui.setMouseCursor(ui.MouseCursor.Hand) end
  if clicked and usable then
    sendRequest(currentFg(), currentFlip(), currentNegate(), true, currentMult())
    lastSaveRequest = requestCounter
    -- Only this save's result counts from now on: the one before is
    -- cleared, and "Saved" shows once the bridge has confirmed this one.
    texts.save = ''
    a.lastSaveText = ''
    a.savedAt = -100
  end
  if texts.save ~= a.lastSaveText then
    a.lastSaveText = texts.save
    if texts.save ~= '' and status.saveOk ~= 0 then a.savedAt = now end
  end
  local since = now - a.savedAt
  local flash = since < L.savedHold and 1 or math.max(0, 1 - (since - L.savedHold) / L.savedFade)
  -- "Saved" stands in for the label only while the text is a success.
  if texts.save == '' or status.saveOk == 0 then flash = 0 end
  a.saveHover = ease(a.saveHover, hovered and 1 or 0, 14)
  a.savePress = ease(a.savePress, pressed and 1 or 0, 24)
  local alpha = usable and 1 or 0.45
  local inset = 1.5 * a.savePress
  fillRect(x + inset, y + inset, x + lw - inset, y + h - inset,
    lerpColor(c1, C.button, C.buttonHover, a.saveHover, alpha), 8)
  strokeRect(x + inset, y + inset, x + lw - inset, y + h - inset, lerpColor(c1, C.line, C.red, a.saveHover, alpha), 8)
  if flash > 0 then
    label(texts.save, FONT.semi, 13, x, y, x + lw, y + h, K.alignCenter, faded(c1, COLOR_GOOD, flash * alpha))
  end
  if flash < 1 then
    label(LABEL_SAVE, FONT.semi, 13, x, y, x + lw, y + h, K.alignCenter, faded(c1, C.text, (1 - flash) * alpha))
  end
  finishBlock(x, y, h)
  if texts.save ~= '' and status.saveOk == 0 then wrappedText(texts.save, COLOR_BAD) end
end

-- "Details": a header row whose chevron turns, and a child window whose
-- height eases between 0 and its content's, measured when it last drew.
local function detailRow(name, value)
  ui.pushStyleColor(ui.StyleColor.Text, C.muted)
  ui.text(name)
  ui.popStyleColor(1)
  ui.sameLine(L.detailValueX)
  ui.textWrapped(value)
end

-- The camera switches for checking the camera constants in game.
local function debugSwitches()
  local controls = anim.detailsControls
  if not controls then ui.pushDisabled() end
  ui.pushStyleColor(ui.StyleColor.CheckMark, C.red)
  ui.pushStyleColor(ui.StyleColor.FrameBg, C.track)
  ui.pushStyleColor(ui.StyleColor.FrameBgHovered, C.buttonHover)
  ui.pushStyleColor(ui.StyleColor.FrameBgActive, C.buttonHover)
  if ui.checkbox(LABEL_FLIP, currentFlip()) and controls then
    sendRequest(currentFg(), not currentFlip(), currentNegate(), false, currentMult())
  end
  if ui.checkbox(LABEL_NEGATE, currentNegate()) and controls then
    sendRequest(currentFg(), currentFlip(), not currentNegate(), false, currentMult())
  end
  ui.popStyleColor(4)
  if not controls then ui.popDisabled() end
end

local function detailsContent()
  detailRow(WORDS.gpu, texts.gpu)
  detailRow(WORDS.bridge, texts.mode)
  detailRow(WORDS.gpuTime, texts.gpuMs)
  detailRow(WORDS.hotkey, texts.hotkey)
  detailRow(WORDS.perSecond, texts.perSecond)
  ui.offsetCursorY(4)
  debugSwitches()
  anim.detailsH = ui.getCursorY()
end

local function detailsSection(controls)
  local a = anim
  a.detailsControls = controls
  local x, y = lx, ui.getCursorY()
  local rowH = L.detailsRowH
  SIZE.x, SIZE.y = lw, rowH
  if ui.invisibleButton(ID.details, SIZE, K.buttonNone) then a.detailsOpen = not a.detailsOpen end
  local hovered = ui.itemHovered()
  if hovered then ui.setMouseCursor(ui.MouseCursor.Hand) end
  a.detailsHover = ease(a.detailsHover, hovered and 1 or 0, 14)
  a.detailsT = ease(a.detailsT, a.detailsOpen and 1 or 0, 10)
  local col = lerpColor(c1, C.muted, C.text, a.detailsHover, 1)
  -- A triangle pointing right, turned by up to 90 degrees to point down: the
  -- tip, then the lower and the upper corner, in the order ImGui draws its own
  -- arrows (clockwise on screen, which its anti-aliased fill expects).
  local cx, cy = x + 8, y + rowH / 2
  local turn = a.detailsT * math.pi / 2
  local ca, sa = math.cos(turn), math.sin(turn)
  P1.x, P1.y = cx + 4 * ca, cy + 4 * sa
  P2.x, P2.y = cx - 3 * ca - 4 * sa, cy - 3 * sa + 4 * ca
  P3.x, P3.y = cx - 3 * ca + 4 * sa, cy - 3 * sa - 4 * ca
  ui.drawTriangleFilled(P1, P2, P3, col)
  label(WORDS.details, FONT.semi, 13, x + 20, y, x + lw, y + rowH, K.alignStart, col)
  if a.detailsT > 0 then
    local h = (a.detailsH > 0 and a.detailsH or 120) * a.detailsT
    if h >= 1 then
      ui.setCursorX(x + 4)
      CHILD_SIZE.x, CHILD_SIZE.y = lw - 4, h
      -- While it opens, the child is lower than its content and would show a
      -- scrollbar: a zero-wide one, for the child only (ImGui sizes and draws
      -- it in beginChild). pcall: a failing row cannot leave the child open.
      ui.pushStyleVar(ui.StyleVar.ScrollbarSize, 0)
      local visible = ui.beginChild(ID.detailsChild, CHILD_SIZE, false, K.childFlags)
      ui.popStyleVar(1)
      if visible then
        local ok, err = pcall(detailsContent)
        if not ok and not a.detailsFailureLogged then
          a.detailsFailureLogged = true
          ac.error('AcDlssg: drawing the details failed: ' .. tostring(err))
        end
      end
      ui.endChild()
    end
  end
end

function script.windowMain(dt)
  local now = os.preciseClock()
  local frame = ui.deltaTime()
  frameDt = (frame > 0 and frame < 0.1) and frame or 0.016
  refreshStatus(now)
  local problem = bridgeProblem(now)
  lx, lw = ui.getCursorX(), ui.availableSpaceX()
  drawHeader(problem, now)
  if problem == 3 then
    noteCard(cards.problem, TEXT_VERSIONS_DIFFER, texts.versions, COLOR_BAD)
    return
  end
  if problem ~= 0 then
    noteCard(cards.problem, TEXT_NOT_RUNNING, problem == 1 and TEXT_NOT_RUNNING_HINT or texts.stopped, COLOR_BAD)
    return
  end
  local s = status
  if s.bridgeState < STATE_PROXY_NO_FG then
    noteCard(cards.state, s.bridgeState == STATE_PASS_THROUGH and WORDS.passThrough or WORDS.waiting, texts.state,
      COLOR_WARN)
    detailsSection(false)
    return
  end

  -- A setting the bridge fixed by itself so that frame generation can run
  -- (a number as fg_vram_headroom_mib switched to auto), first of all.
  if texts.autoFixNote ~= '' then
    noteCard(cards.autoFix, nil, texts.autoFixNote, COLOR_GOOD)
  else
    cards.autoFix.alpha = 0
  end

  -- A change that applies only at the next start ("Save as default").
  if texts.restartNote ~= '' then
    noteCard(cards.restart, TEXT_RESTART, texts.restartNote, COLOR_RESTART)
  else
    cards.restart.alpha = 0
  end

  -- The big switch; under it, why frame generation is off or paused.
  local available = ctl ~= nil and s.bridgeState == STATE_FG_AVAILABLE
  local running = s.fgOn ~= 0 and s.fgPaused == 0
  fgSwitch(available)
  if not available then
    wrappedText(texts.unavailable, COLOR_BAD)
  elseif not running then
    wrappedText(texts.status, s.fgUserOn ~= 0 and COLOR_WARN or C.muted)
  end
  -- An older bridge publishes no multiplier (0): no segments.
  if s.fgMultRequested ~= 0 then multiplierButtons(available) end

  fpsCard(running, now)
  vramBlock()
  -- The video memory guard's note: tight (on anyway), or not enough (off).
  if texts.vramNote ~= '' then wrappedText(texts.vramNote, COLOR_WARN) end
  if s.vsyncNote ~= 0 then noteCard(cards.vsync, nil, TEXT_VSYNC, C.muted) else cards.vsync.alpha = 0 end
  if s.driverWarning ~= 0 then noteCard(cards.driver, nil, texts.warning, COLOR_WARN) else cards.driver.alpha = 0 end

  saveButton(now)
  detailsSection(ctl ~= nil)
end
