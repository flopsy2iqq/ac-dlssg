#pragma once
// The camera section written by the CSP Lua app (spec 6.6). Mirrors the Lua
// ffi layout string exactly:
//   uint32_t magic; uint32_t version; uint32_t seq; uint32_t frame;
//   float pos[3]; float fwd[3]; float up[3]; float side[3];
//   float fovVDeg; float clipNear; float clipFar; float originShift[3];
//   float renderW; float renderH; uint32_t flags; float dt; double simTimeMs;
#include <cstddef>
#include <cstdint>

namespace acdb {

constexpr uint32_t kCameraMagic = 0x47534C44;  // 'DLSG' little-endian
constexpr uint32_t kCameraVersion = 1;

enum CameraFlags : uint32_t {
    kCamJumped = 1,
    kCamPaused = 2,
    kCamReplay = 4,
    kCamVR = 8,
    kCamTriple = 16,
    kCamMainMenu = 32,
    kCamWriteFailed = 64,
};

struct CameraLayout {
    uint32_t magic;
    uint32_t version;
    uint32_t seq;
    uint32_t frame;
    float pos[3];
    float fwd[3];
    float up[3];
    float side[3];
    float fovVDeg;
    float clipNear;
    float clipFar;
    float originShift[3];
    float renderW;
    float renderH;
    uint32_t flags;
    float dt;
    double simTimeMs;
};

static_assert(sizeof(CameraLayout) == 112, "CameraLayout must match the Lua layout");
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
