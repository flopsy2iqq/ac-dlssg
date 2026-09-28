// NgxHook::SetSink, the interface the presenter uses to attach its capture
// coordinator to the process-wide hook (installed once) and to detach it
// before the coordinator is destroyed.
#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <string>
#include <vector>

#include "fake_nvngx/fake_nvngx.h"
#include "gpu_test_devices.h"
#include "ngx_hook.h"
#include "test_framework.h"

using namespace acdb;

namespace {

struct CountingSink : NgxEvaluateSink {
    int creates = 0;
    int evals = 0;
    void OnCreateFeature(uint64_t, const NgxCreateInfo&) override { ++creates; }
    void OnEvaluate(const NgxEvaluateInputs&) override { ++evals; }
};

using CreateFn = int(__cdecl*)(void*, unsigned int, void*, void**);
using EvalFn = int(__cdecl*)(void*, const void*, const void*, void*);

}  // namespace

TEST(NgxSetSink_SwapsTheSinkAndNullDetaches) {
    acdb_test::GpuTestDevices d;
    if (!acdb_test::CreateGpuTestDevices(&d)) return;
    HMODULE fake = LoadLibraryW(FAKE_NVNGX_PATH);
    REQUIRE(fake != nullptr);
    auto state = reinterpret_cast<FakeNgxState* (*)()>(GetProcAddress(fake, "FakeNgxGetState"));
    auto create = reinterpret_cast<CreateFn>(GetProcAddress(fake, "NVSDK_NGX_D3D11_CreateFeature"));
    auto eval = reinterpret_cast<EvalFn>(GetProcAddress(fake, "NVSDK_NGX_D3D11_EvaluateFeature"));
    REQUIRE(state && create && eval);
    *state() = FakeNgxState{};
    state()->nextHandle = reinterpret_cast<void*>(0x230000);

    CountingSink first;
    CountingSink second;
    NgxHook::Get().Uninstall();
    std::string err;
    REQUIRE(NgxHook::Get().Install(&first, &err));

    FakeNgxParam cp;
    cp.SetU(ngxkey::kWidth, 1280u);
    cp.SetU(ngxkey::kHeight, 720u);
    cp.SetI(ngxkey::kCreateFlags, static_cast<int>(kNgxDlssFlagMVLowRes));
    void* h = nullptr;
    CHECK_EQ(create(d.ctx11.Get(), kNgxFeatureSuperSampling, &cp, &h), kNgxSuccess);
    FakeNgxParam ep;
    ep.SetRes(ngxkey::kDepth, reinterpret_cast<ID3D11Resource*>(0xAAA0));
    ep.SetRes(ngxkey::kMotionVectors, reinterpret_cast<ID3D11Resource*>(0xBBB0));
    CHECK_EQ(eval(d.ctx11.Get(), h, &ep, nullptr), kNgxSuccess);
    CHECK_EQ(first.creates, 1);
    CHECK_EQ(first.evals, 1);

    // The feature record survives the swap: the next evaluate reaches the new sink.
    NgxHook::Get().SetSink(&second);
    CHECK_EQ(eval(d.ctx11.Get(), h, &ep, nullptr), kNgxSuccess);
    CHECK_EQ(first.evals, 1);
    CHECK_EQ(second.evals, 1);

    // No sink: calls are still forwarded, nothing is reported.
    NgxHook::Get().SetSink(nullptr);
    CHECK_EQ(eval(d.ctx11.Get(), h, &ep, nullptr), kNgxSuccess);
    CHECK_EQ(state()->evalCalls, 3L);
    CHECK_EQ(first.evals, 1);
    CHECK_EQ(second.evals, 1);

    NgxHook::Get().Uninstall();
    FreeLibrary(fake);
}
