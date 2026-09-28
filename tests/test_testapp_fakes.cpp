// The fakes the test application uses to stand in for CSP (tools/testapp):
// the parameter blocks of the fake NGX module (tests/fake_nvngx), checked
// through the bridge's real NgxHook, because the test app's --fake-ngx mode
// hands the hook exactly such blocks.
#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <string>
#include <vector>

#include "gpu_test_devices.h"
#include "ngx_hook.h"
#include "test_framework.h"
#include "fake_nvngx/fake_nvngx.h"

using namespace acdb;
using namespace acdb_test;

namespace {

using AllocateFn = int(__cdecl*)(NgxParameter**);
using DestroyFn = int(__cdecl*)(NgxParameter*);
using CreateFn = int(__cdecl*)(ID3D11DeviceContext*, unsigned int, NgxParameter*, void**);
using EvalFn = int(__cdecl*)(ID3D11DeviceContext*, const void*, const NgxParameter*, void*);

struct RecordingSink : NgxEvaluateSink {
    std::vector<NgxCreateInfo> creates;
    std::vector<NgxEvaluateInputs> evals;
    void OnCreateFeature(uint64_t, const NgxCreateInfo& info) override { creates.push_back(info); }
    void OnEvaluate(const NgxEvaluateInputs& in) override { evals.push_back(in); }
};

// The fake module with the hook installed on it; torn down even when a
// REQUIRE aborts the test.
struct FakeNgxFixture {
    HMODULE module = nullptr;
    FakeNgxState* state = nullptr;
    RecordingSink sink;
    FakeNgxFixture() {
        module = LoadLibraryW(FAKE_NVNGX_PATH);
        if (!module) return;
        auto get = reinterpret_cast<FakeNgxState* (*)()>(GetProcAddress(module, "FakeNgxGetState"));
        state = get ? get() : nullptr;
        if (state) *state = FakeNgxState{};
        NgxHook::Get().Uninstall();
        std::string err;
        NgxHook::Get().Install(&sink, &err);
    }
    ~FakeNgxFixture() {
        NgxHook::Get().Uninstall();
        if (module) FreeLibrary(module);
    }
    template <typename T>
    T Proc(const char* name) const {
        return reinterpret_cast<T>(GetProcAddress(module, name));
    }
};

}  // namespace

// The test app creates its parameter blocks through the fake module, as CSP
// gets them from NGX; the hook must read what was set through the block's
// NVSDK_NGX_Parameter interface.
TEST(FakeNgx_AllocatedParameterBlockReachesTheHook) {
    GpuTestDevices d;
    if (!CreateGpuTestDevices(&d)) {
        std::printf("  no D3D device; skipping\n");
        return;
    }
    FakeNgxFixture fx;
    REQUIRE(fx.module != nullptr && fx.state != nullptr);
    const auto allocate = fx.Proc<AllocateFn>("NVSDK_NGX_D3D11_AllocateParameters");
    const auto destroy = fx.Proc<DestroyFn>("NVSDK_NGX_D3D11_DestroyParameters");
    const auto create = fx.Proc<CreateFn>("NVSDK_NGX_D3D11_CreateFeature");
    const auto eval = fx.Proc<EvalFn>("NVSDK_NGX_D3D11_EvaluateFeature");
    REQUIRE(allocate && destroy && create && eval);

    NgxParameter* p = nullptr;
    CHECK_EQ(allocate(&p), kNgxSuccess);
    REQUIRE(p != nullptr);
    CHECK_EQ(fx.state->allocatedParams, 1L);

    p->Set(ngxkey::kWidth, 853u);
    p->Set(ngxkey::kHeight, 480u);
    p->Set(ngxkey::kOutWidth, 1280u);
    p->Set(ngxkey::kOutHeight, 720u);
    p->Set(ngxkey::kCreateFlags, static_cast<int>(kNgxDlssFlagMVLowRes));
    fx.state->nextHandle = reinterpret_cast<void*>(0x150000);
    void* handle = nullptr;
    CHECK_EQ(create(d.ctx11.Get(), kNgxFeatureSuperSampling, p, &handle), kNgxSuccess);
    REQUIRE(fx.sink.creates.size() == 1);
    CHECK_EQ(fx.sink.creates[0].width, 853u);
    CHECK_EQ(fx.sink.creates[0].outHeight, 720u);
    CHECK_EQ(fx.sink.creates[0].createFlags, kNgxDlssFlagMVLowRes);

    p->Set(ngxkey::kDepth, reinterpret_cast<ID3D11Resource*>(0xAAA0));
    p->Set(ngxkey::kMotionVectors, reinterpret_cast<ID3D11Resource*>(0xBBB0));
    p->Set(ngxkey::kJitterOffsetX, 0.25f);
    p->Set(ngxkey::kJitterOffsetY, -0.125f);
    p->Set(ngxkey::kMvScaleX, -853.0f);
    p->Set(ngxkey::kMvScaleY, -480.0f);
    p->Set(ngxkey::kSubrectWidth, 853u);
    p->Set(ngxkey::kSubrectHeight, 480u);
    p->Set(ngxkey::kReset, 1);
    CHECK_EQ(eval(d.ctx11.Get(), handle, p, nullptr), kNgxSuccess);
    CHECK_EQ(fx.state->evalCalls, 1L);
    REQUIRE(fx.sink.evals.size() == 1);
    const NgxEvaluateInputs& in = fx.sink.evals[0];
    CHECK(in.depth == reinterpret_cast<ID3D11Resource*>(0xAAA0));
    CHECK(in.mvec == reinterpret_cast<ID3D11Resource*>(0xBBB0));
    CHECK(in.jitterX == 0.25f && in.jitterY == -0.125f);
    CHECK(in.mvScaleX == -853.0f && in.mvScaleY == -480.0f);
    CHECK_EQ(in.subrectW, 853u);
    CHECK_EQ(in.subrectH, 480u);
    CHECK(in.reset);
    CHECK(in.createObserved);

    CHECK_EQ(destroy(p), kNgxSuccess);
    CHECK_EQ(fx.state->destroyedParams, 1L);
}

TEST(FakeNgx_ParameterExportsRefuseNull) {
    FakeNgxFixture fx;
    REQUIRE(fx.module != nullptr && fx.state != nullptr);
    const auto allocate = fx.Proc<AllocateFn>("NVSDK_NGX_D3D11_AllocateParameters");
    const auto destroy = fx.Proc<DestroyFn>("NVSDK_NGX_D3D11_DestroyParameters");
    REQUIRE(allocate && destroy);
    CHECK(allocate(nullptr) != kNgxSuccess);
    CHECK(destroy(nullptr) != kNgxSuccess);
    CHECK_EQ(fx.state->allocatedParams, 0L);
    CHECK_EQ(fx.state->destroyedParams, 0L);
}
