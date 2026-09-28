#pragma once
// The camera record the CSP Lua app (apps/lua/AcDlssg/AcDlssg.lua) writes into
// the shared section Local\AcDlssg.Camera.v1 once per frame (spec 6.6).
//
// The struct mirrors the Lua ffi layout string field by field. LuaJIT lays
// that string out with natural C alignment, like MSVC does here, so the two
// agree byte for byte; the static_asserts below pin every offset, and
// tests/test_camera_channel.cpp parses the Lua file and compares its layout
// string with this struct, so the two cannot drift apart.
#include <cstddef>
#include <cstdint>

namespace acdb {

constexpr uint32_t kCameraMagic = 0x47534C44;  // 'DLSG' little-endian
constexpr uint32_t kCameraVersion = 1;

enum CameraFlags : uint32_t {
    kCamJumped = 1,        // camera cut or teleport: no history for this frame
    kCamPaused = 2,        // AC is paused
    kCamReplay = 4,        // a replay is playing
    kCamVR = 8,            // AC was started in a VR mode
    kCamTriple = 16,       // AC was started in triple-screen mode
    kCamMainMenu = 32,     // AC's in-game menu is open
    kCamWriteFailed = 64,  // the Lua writer hit an error; the record is not valid
};

struct CameraLayout {
    uint32_t magic;    // kCameraMagic once the app has completed a write
    uint32_t version;  // kCameraVersion
    uint32_t seq;      // seqlock: odd while the app writes, even when stable
    uint32_t frame;    // +1 per write; the freshness counter (spec 6.6)
    float pos[3];      // camera position, world space, metres
    float fwd[3];      // unit forward vector
    float up[3];       // unit up vector
    float side[3];     // unit side vector as AC reports it
    float fovVDeg;     // vertical field of view, degrees
    float clipNear;    // near clip plane, metres
    float clipFar;     // far clip plane, metres
    float originShift[3];  // CSP's graphics-space origin offset, metres (zeros when off)
    float renderW;     // 3D render size fed to the upscaler, pixels
    float renderH;
    uint32_t flags;    // CameraFlags
    float dt;          // real time since the previous frame, seconds
    double simTimeMs;  // AC time in milliseconds
};

static_assert(sizeof(CameraLayout) == 112);
static_assert(alignof(CameraLayout) == 8);
static_assert(offsetof(CameraLayout, magic) == 0);
static_assert(offsetof(CameraLayout, version) == 4);
static_assert(offsetof(CameraLayout, seq) == 8);
static_assert(offsetof(CameraLayout, frame) == 12);
static_assert(offsetof(CameraLayout, pos) == 16);
static_assert(offsetof(CameraLayout, fwd) == 28);
static_assert(offsetof(CameraLayout, up) == 40);
static_assert(offsetof(CameraLayout, side) == 52);
static_assert(offsetof(CameraLayout, fovVDeg) == 64);
static_assert(offsetof(CameraLayout, clipNear) == 68);
static_assert(offsetof(CameraLayout, clipFar) == 72);
static_assert(offsetof(CameraLayout, originShift) == 76);
static_assert(offsetof(CameraLayout, renderW) == 88);
static_assert(offsetof(CameraLayout, renderH) == 92);
static_assert(offsetof(CameraLayout, flags) == 96);
static_assert(offsetof(CameraLayout, dt) == 100);
static_assert(offsetof(CameraLayout, simTimeMs) == 104);

}  // namespace acdb
