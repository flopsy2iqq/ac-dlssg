#pragma once
// FrameConstants (spec 6.7): builds Streamline's per-frame sl::Constants from
// the CSP camera snapshots of bridge frames N and N-1 and the parameters of
// CSP's DLSS evaluate. Pure math, no D3D, unit-tested.
//
// Conventions (sl_consts.h and sl_matrix_helpers.h of Streamline 2.14.1):
//  - Matrices are row-major and used with row vectors: p' = p * M. So
//    clipToPrevClip = clipToCameraView * viewToViewPrev * viewToClipPrev.
//  - Camera view space is D3D's: x to the screen's right, y up, z forward
//    (into the screen). cameraViewToClip is a standard, non-reversed D3D
//    perspective (depth 0 at near, 1 at far) without jitter.
//  - The camera-to-world matrix has the rows right, up, fwd, pos, like
//    Streamline's recalculateCameraMatrices; viewToViewPrev is computed in
//    double and camera-centred, like its calcCameraToPrevCamera.
//
// AC's camera basis. CSP builds side vectors as cross(look, up), and the
// camera's side points to the screen's right: AC's world is right-handed
// (y up), so right = fwd x up and (right, up, fwd) has determinant -1. NVIDIA
// passes the same determinant to Streamline in its own DLSS-G samples
// (Streamline_Sample and RTXPT set cameraRight = normalize(cross(dir, up))).
// Only sl_matrix_helpers.h's recalculateCameraMatrices, marked "DO NOT USE
// THIS IN ANYTHING PROPER", rebuilds up = fwd x right (determinant +1). The
// view is built from the orthonormalized side as is.
// The evidence, and the one script that points the other way, are recorded
// in frame_constants.cpp; the in-game check of spec 6.7 settles it.
#include <sl_consts.h>

#include <cstdint>
#include <string>

#include "camera_layout.h"
#include "ngx_params.h"

namespace acdb {

// NVSDK_NGX_DLSS_Feature_Flags bits of DLSS.Feature.Create.Flags. CSP passes
// 2 (MVLowRes only). The values are defined once, with the rest of our NGX
// declaration, in ngx_params.h; these are the names the math uses.
namespace dlss_create_flags {
constexpr uint32_t kIsHDR = kNgxDlssFlagIsHDR;
constexpr uint32_t kMVLowRes = kNgxDlssFlagMVLowRes;
constexpr uint32_t kMVJittered = kNgxDlssFlagMVJittered;
constexpr uint32_t kDepthInverted = kNgxDlssFlagDepthInverted;
}  // namespace dlss_create_flags
static_assert(dlss_create_flags::kIsHDR == 1u && dlss_create_flags::kMVLowRes == 2u &&
                  dlss_create_flags::kMVJittered == 4u && dlss_create_flags::kDepthInverted == 8u,
              "NVSDK_NGX_DLSS_Feature_Flags bits (nvsdk_ngx_defs.h)");

// What the NGX evaluate hook captured for the frame.
struct CaptureParams {
    float jitterX = 0, jitterY = 0;      // Jitter.Offset.X/Y, render pixels
    float mvScaleX = 0, mvScaleY = 0;    // MV.Scale.X/Y (CSP: -(render size))
    uint32_t renderW = 0, renderH = 0;   // render size the MV scale refers to
    uint32_t createFlags = 0;            // DLSS.Feature.Create.Flags
    bool ngxReset = false;               // the evaluate's Reset
};

// Debug switches for the in-game A/B check of spec 6.7. Both default to the
// convention above. They are independent: one changes what the matrices
// assume, the other only the vector Streamline's scene-change detector sees.
struct FrameConstantsOptions {
    // The single flag of spec 6.7: flips the handedness of the camera basis
    // the matrix code uses, taking -side as the screen's right. This mirrors
    // the view horizontally, so clipToPrevClip changes for sideways motion
    // and yaw, and cameraRight flips with it.
    bool flipHandedness = false;
    // Negates cameraRight, the side vector Streamline receives, and nothing
    // else: the matrices stay as they are. This flips the determinant of
    // (cameraRight, cameraUp, cameraFwd) to +1 (up = fwd x right, as in
    // sl_matrix_helpers.h), in case DLSS-G wants it. NVIDIA's samples send
    // determinant -1, and scene-change detection only checks that the three
    // vectors are orthonormal, which holds either way.
    bool negateSide = false;
};

struct ConstantsInput {
    const CameraLayout* cur = nullptr;   // frame N, required
    const CameraLayout* prev = nullptr;  // frame N-1; null: no previous camera
    CaptureParams capture;
    bool prevFrameHadInputs = false;     // frame N-1 was presented with DLSS-G inputs
    FrameConstantsOptions options;
};

// Fills every field of *out that Streamline 2.14.1 requires, and sets every
// sl::Boolean explicitly:
//  - the four matrices (see above); a null prev uses cur as the previous
//    camera, so clipToPrevClip and prevClipToClip are the identity;
//  - cameraPos (pos as given; originShift is not applied), cameraRight/Up/Fwd
//    re-orthonormalized with Gram-Schmidt that keeps fwd's direction and the
//    signs of up and side (cameraRight is the screen's right unless
//    negateSide is set); cameraNear/Far = clipNear/clipFar;
//    cameraFOV = fovVDeg in radians (vertical); cameraAspectRatio =
//    renderW / renderH of cur;
//  - jitterOffset = the NGX jitter in pixels; mvecScale = MV.Scale / render
//    size ({-1,-1} for CSP); cameraPinholeOffset = {0,0};
//  - depthInverted and motionVectorsJittered from the create flags;
//    cameraMotionIncluded eTrue; motionVectors3D, orthographicProjection and
//    motionVectorsDilated eFalse;
//  - reset eTrue when ngxReset, when cur has kCamJumped, when
//    !prevFrameHadInputs, or when prev is null.
// clipToLensClip (optional) and motionVectorsInvalidValue (needed only
// without camera motion in the vectors) stay at Streamline's "not provided"
// defaults, as does minRelativeLinearDepthObjectSeparation.
//
// Returns false, leaves *out untouched and says why for invalid input: a null
// cur or out; a non-finite value; a render size <= 0; clipNear <= 0 or
// clipNear >= clipFar; fovVDeg outside (0, 180); a zero or non-finite MV
// scale; a degenerate basis (a zero vector, up parallel to fwd, or side in
// the fwd/up plane). A non-null prev is checked the same way. Finite but
// extreme input whose result overflows a float, or lands on
// sl::INVALID_FLOAT (Streamline's "not provided"), is refused too.
bool BuildFrameConstants(const ConstantsInput& in, sl::Constants* out, std::string* why);

// One log line with the camera fields, the flags and the four matrices, for
// the presenter to log the camera once (validation of spec 6.7).
std::string FormatConstantsForLog(const sl::Constants& c);

}  // namespace acdb
