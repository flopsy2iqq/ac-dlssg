#pragma once
// Our own declaration of the parts of the NVIDIA NGX D3D11 client interface the
// bridge needs. There are no NVIDIA headers in this repository (spec 6.5): every
// value below is copied from NVIDIA's public DLSS SDK headers (github.com/NVIDIA
// /DLSS, commit 374959484, "DLSS 310.9.1 SDK") -- nvsdk_ngx.h, nvsdk_ngx_defs.h,
// nvsdk_ngx_params.h and nvsdk_ngx_defs_dlssd.h -- and confirmed against the
// modules on this machine with rabin2/capstone. Only the D3D11 entry points and
// the NVSDK_NGX_Parameter accessor interface are declared.
#include <cstdint>

// Opaque COM types; only pointers cross this boundary.
struct ID3D11DeviceContext;
struct ID3D11Resource;
struct ID3D12Resource;

namespace acdb {

// NVSDK_NGX_Result (nvsdk_ngx_defs.h). Success is 0x1; every failure has the top
// bits 0xBAD..., so "not Success" is "not set" for our reads (spec 6.5).
using NgxResult = int;
constexpr NgxResult kNgxSuccess = 0x1;

// NVSDK_NGX_Feature (nvsdk_ngx_defs.h). Only the two the bridge distinguishes.
constexpr uint32_t kNgxFeatureSuperSampling = 1;
constexpr uint32_t kNgxFeatureRayReconstruction = 13;

// NVSDK_NGX_DLSS_Feature_Flags (nvsdk_ngx_defs.h): CSP passes MVLowRes (2).
constexpr uint32_t kNgxDlssFlagIsHDR = 1u << 0;
constexpr uint32_t kNgxDlssFlagMVLowRes = 1u << 1;
constexpr uint32_t kNgxDlssFlagMVJittered = 1u << 2;
constexpr uint32_t kNgxDlssFlagDepthInverted = 1u << 3;

// Parameter key strings (nvsdk_ngx_defs.h). Exact spellings NGX matches on.
namespace ngxkey {
constexpr const char* kWidth = "Width";
constexpr const char* kHeight = "Height";
constexpr const char* kOutWidth = "OutWidth";
constexpr const char* kOutHeight = "OutHeight";
constexpr const char* kDepth = "Depth";
constexpr const char* kMotionVectors = "MotionVectors";
constexpr const char* kJitterOffsetX = "Jitter.Offset.X";
constexpr const char* kJitterOffsetY = "Jitter.Offset.Y";
constexpr const char* kMvScaleX = "MV.Scale.X";
constexpr const char* kMvScaleY = "MV.Scale.Y";
constexpr const char* kReset = "Reset";
constexpr const char* kCreateFlags = "DLSS.Feature.Create.Flags";
constexpr const char* kSubrectWidth = "DLSS.Render.Subrect.Dimensions.Width";
constexpr const char* kSubrectHeight = "DLSS.Render.Subrect.Dimensions.Height";
}  // namespace ngxkey

// Resource keys that only a denoiser / ray-reconstruction feature sets. If any is
// present the create is DLSS-D, not the super-resolution feature the bridge
// mirrors (spec 6.5; adapted from dlss5-bridge's kDenoiserKeys, MIT). Both the
// bare and DLSSD.-prefixed spellings are probed: a key this runtime does not know
// answers a non-Success code, so a wrong name is only ever a false negative.
inline const char* const kNgxDenoiserKeys[] = {
    "NormalRoughness",       "DLSSD.NormalRoughness",
    "DiffuseAlbedo",         "DLSS.Input.DiffuseAlbedo",
    "SpecularAlbedo",        "DLSS.Input.SpecularAlbedo",
    "SpecularHitDistance",   "DLSSD.SpecularHitDistance",
    "SpecularMotionVectors", "DLSSD.SpecularMotionVectors",
    "GBuffer.Normals",       "GBuffer.Roughness",
    "DLSS.Denoise.Mode",
};

// NVSDK_NGX_Parameter (nvsdk_ngx_params.h). MSVC emits same-name virtual
// overloads in reverse declaration order, so listing them in NVIDIA's exact
// order reproduces NVIDIA's vtable layout and no slot has to be hard-coded
// (the same technique dlss5-bridge documents). We only ever call Get; Set and
// Reset exist so the vtable slots line up.
struct NgxParameter {
    virtual void Set(const char* name, unsigned long long value) = 0;
    virtual void Set(const char* name, float value) = 0;
    virtual void Set(const char* name, double value) = 0;
    virtual void Set(const char* name, unsigned int value) = 0;
    virtual void Set(const char* name, int value) = 0;
    virtual void Set(const char* name, ID3D11Resource* value) = 0;
    virtual void Set(const char* name, ID3D12Resource* value) = 0;
    virtual void Set(const char* name, void* value) = 0;

    virtual NgxResult Get(const char* name, unsigned long long* out) const = 0;
    virtual NgxResult Get(const char* name, float* out) const = 0;
    virtual NgxResult Get(const char* name, double* out) const = 0;
    virtual NgxResult Get(const char* name, unsigned int* out) const = 0;
    virtual NgxResult Get(const char* name, int* out) const = 0;
    virtual NgxResult Get(const char* name, ID3D11Resource** out) const = 0;
    virtual NgxResult Get(const char* name, ID3D12Resource** out) const = 0;
    virtual NgxResult Get(const char* name, void** out) const = 0;

    virtual void Reset() = 0;
};

// Opaque NGX feature handle. NGX only ever hands back its address; we key our
// per-feature records on that address (spec 6.5).
struct NgxHandle;

// NVSDK_NGX_D3D11 entry-point signatures (nvsdk_ngx.h). NVSDK_CONV is the
// platform default calling convention on x64. The actual CreateFeature export
// takes a non-const parameter block; EvaluateFeature and EvaluateFeature_C take
// a const one and a progress callback (the _C variant uses the C callback type;
// both have identical binary layout for our purposes).
using PfnNgxCreateFeature = NgxResult(__cdecl*)(ID3D11DeviceContext* ctx, uint32_t featureId,
                                                NgxParameter* params, NgxHandle** outHandle);
using PfnNgxEvaluateFeature = NgxResult(__cdecl*)(ID3D11DeviceContext* ctx, const NgxHandle* handle,
                                                  const NgxParameter* params, void* callback);

}  // namespace acdb
