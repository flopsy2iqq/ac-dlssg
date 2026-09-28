#pragma once
// NgxCapture's hooking layer (spec 6.5): it detours the NGX D3D11 CreateFeature
// and EvaluateFeature entry points in every loaded module that exports them,
// filters the calls down to CSP's own DLSS super-resolution feature, reads the
// evaluate parameters, and hands them to a sink. The GPU copies of depth and
// motion vectors are the "slots" unit; this unit stops at the parameters.
#include <windows.h>

#include <cstdint>
#include <string>

#include "ngx_params.h"

namespace acdb {

// Create parameters recorded for one counted SuperSampling feature (spec 6.5).
struct NgxCreateInfo {
    uint32_t featureId = 0;
    uint32_t width = 0, height = 0, outWidth = 0, outHeight = 0;
    uint32_t createFlags = 0;
    bool hasDenoiserKeys = false;
};

// One captured evaluate. Resource pointers are read on every evaluate, because
// CSP ping-pongs its motion-vector textures (spec 6.5).
struct NgxEvaluateInputs {
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11Resource* depth = nullptr;
    ID3D11Resource* mvec = nullptr;
    float jitterX = 0, jitterY = 0, mvScaleX = 0, mvScaleY = 0;
    // DLSS.Render.Subrect.Dimensions; when absent or 0 ("not set"), the
    // evaluate block's Width/Height, else the observed create's input size.
    // 0 only when none of them is known.
    uint32_t subrectW = 0, subrectH = 0;
    uint32_t createFlags = 0;
    bool reset = false;
    uint64_t featureKey = 0;    // the returned NVSDK_NGX_Handle* value
    bool createObserved = false;  // false when the create was never seen
};

// The sink is called only for counted calls, after the original returned.
class NgxEvaluateSink {
public:
    virtual ~NgxEvaluateSink() = default;
    // Counted SuperSampling creates only.
    virtual void OnCreateFeature(uint64_t featureKey, const NgxCreateInfo& info) = 0;
    // Counted evaluates only, after the original returned Success.
    //
    // in.depth and in.mvec can be null (ngx review F7): for a handle whose
    // create was observed, the evaluate is counted even when Depth or
    // MotionVectors is missing from the parameter block or set to null. The
    // sink must treat a null resource as "no capture this frame" and must not
    // pass it on (CaptureSlots refuses null sources).
    //
    // in.ctx, in.depth and in.mvec are CSP's raw pointers, not AddRef'd: they
    // are valid only for the duration of this call, and nothing may keep them.
    virtual void OnEvaluate(const NgxEvaluateInputs& in) = 0;
};

// Skip decision for one candidate module (spec 6.5). Exposed so the rules are
// unit-tested without pathological DLLs.
enum class NgxSkip {
    None,
    HostExe,       // the process image; its NGX exports sit in the game's code
    ModelsPath,    // under \NGX\models\ or a .bin: a model store, never called
    FillerStub,    // an entry is a 0x90/0xCC sled, not code
    SharedAddress  // two entries resolve to one address: a placeholder
};
NgxSkip NgxClassifyModule(const wchar_t* path, bool isHostExe, const void* create, const void* eval,
                          const void* eval_c);
// True when the first bytes at fn are one filler byte (0x90 or 0xCC) repeated.
bool NgxIsFillerStub(const void* fn);

class NgxHook {
public:
    static NgxHook& Get();

    // Scans loaded modules and registers for later ones. Never called from
    // DllMain. Returns false and sets *error only on a fatal setup failure.
    bool Install(NgxEvaluateSink* sink, std::string* error);
    // Applies the loads and unloads the loader notification queued (unloads
    // first, never writing to an unloaded module) and rescans after a load.
    // Call from Present. Without queued work it takes no lock and calls nothing.
    void ProcessPendingRescan();
    // Restores patched bytes of modules still loaded (tests / shutdown).
    void Uninstall();
    // Swaps the sink atomically. When it returns, no call into the previous
    // sink is in progress on any other thread (it waits for them), so the
    // caller may destroy that sink: the presenter calls SetSink(nullptr) before
    // it is destroyed. Called from inside a sink call on the same thread it
    // swaps without waiting, because it would otherwise wait on its own caller.
    // Install and Uninstall swap the sink the same way.
    void SetSink(NgxEvaluateSink* sink);
    uint32_t HookedModules() const;

    // Test-only: Install calls fn after its first scan and before it registers
    // for load notifications, so a test can load a module inside that gap.
    void SetInstallGapHookForTest(void (*fn)());
    // Test-only: hold every internal lock on the calling thread, to prove the
    // loader-notification callback never waits on one (spec 6.5).
    void LockStateForTest();
    void UnlockStateForTest();

    // Internal, referenced by the per-slot detours. Not for callers.
    NgxResult DispatchCreate(int slot, ID3D11DeviceContext* ctx, uint32_t featureId,
                             NgxParameter* params, NgxHandle** outHandle) noexcept;
    NgxResult DispatchEvaluate(int slot, bool isC, ID3D11DeviceContext* ctx, const NgxHandle* handle,
                               const NgxParameter* params, void* callback) noexcept;

private:
    NgxHook() = default;
};

}  // namespace acdb
