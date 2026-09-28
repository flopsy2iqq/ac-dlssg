--[[
  AC DLSS-G Camera: publishes the main camera to ac-dlssg.dll, the bridge that
  brings DLSS Frame Generation to Assetto Corsa with CSP (design spec 6.6).

  Once per frame, just before the main scene render, the app writes one record
  into the shared memory section Local\AcDlssg.Camera.v1. The bridge reads it
  at CSP's DLSS evaluate of the same frame and builds DLSS-G's camera constants
  from it. The record is a seqlock: seq is odd while the app writes and even
  when the record is stable, and frame counts the writes so the bridge can
  tell a record written during this frame from an old one.

  The layout string, the constants and the order of the stores in publish()
  are checked against src/camera_layout.h by tests/test_camera_channel.cpp.
  Change them together.

  Per frame the app allocates nothing: every FFI reference it needs is taken
  once at load, and each frame reads and writes plain numbers.
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

function script.windowMain(dt)
end
