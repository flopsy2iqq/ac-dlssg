#include "fake_nvngx.h"

#include <windows.h>

namespace {
FakeNgxState g_state;
thread_local int t_depth = 0;
}  // namespace

extern "C" FakeNgxState* FakeNgxGetState() { return &g_state; }

#ifdef FAKE_NVNGX_VARIANT_B
// Variant B (fake_nvngx_reuse_b) has different code from variant A, so the
// bytes it has at A's entry addresses differ from A's prologues.
extern "C" __declspec(dllexport) __declspec(noinline) int FakeNgxVariantPadding(int x) {
    volatile int acc = x;
    for (int i = 0; i < 64; ++i) acc = acc * 31 + i;
    return acc;
}
#endif

extern "C" int __cdecl NVSDK_NGX_D3D11_CreateFeature(void*, unsigned int featureId, void*, void** outHandle) {
#ifdef FAKE_NVNGX_VARIANT_B
    volatile unsigned int salt = featureId * 2654435761u;  // different code, same behaviour
    (void)salt;
#else
    (void)featureId;
#endif
    InterlockedIncrement(&g_state.createCalls);
    if (outHandle) *outHandle = g_state.nextHandle;
    return g_state.createResult;
}

extern "C" int __cdecl NVSDK_NGX_D3D11_EvaluateFeature(void* ctx, const void* handle,
                                                       const void* params, void* callback) {
    InterlockedIncrement(&g_state.evalCalls);
    ++t_depth;
    if (t_depth > g_state.maxReentryObserved) g_state.maxReentryObserved = t_depth;
    // Reenter through the exported _C entry once, so the detour is entered nested
    // (spec 6.5 nesting rule). The call goes through the hook again.
    if (t_depth == 1 && g_state.nestReentryDepth > 0)
        NVSDK_NGX_D3D11_EvaluateFeature_C(ctx, handle, params, callback);
    --t_depth;
    return g_state.evalResult;
}

extern "C" int __cdecl NVSDK_NGX_D3D11_EvaluateFeature_C(void*, const void*, const void*, void*) {
    InterlockedIncrement(&g_state.evalCCalls);
    ++t_depth;
    if (t_depth > g_state.maxReentryObserved) g_state.maxReentryObserved = t_depth;
    --t_depth;
    return g_state.evalResult;
}

namespace {
constexpr int kNgxFailInvalidParameter = static_cast<int>(0xBAD00005);  // NVSDK_NGX_Result_FAIL_InvalidParameter
}  // namespace

extern "C" int __cdecl NVSDK_NGX_D3D11_AllocateParameters(acdb::NgxParameter** outParams) {
    if (!outParams) return kNgxFailInvalidParameter;
    *outParams = new FakeNgxParam();
    InterlockedIncrement(&g_state.allocatedParams);
    return acdb::kNgxSuccess;
}

extern "C" int __cdecl NVSDK_NGX_D3D11_DestroyParameters(acdb::NgxParameter* params) {
    if (!params) return kNgxFailInvalidParameter;
    delete static_cast<FakeNgxParam*>(params);
    InterlockedIncrement(&g_state.destroyedParams);
    return acdb::kNgxSuccess;
}
