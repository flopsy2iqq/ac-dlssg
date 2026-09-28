// NgxHook: module scan, inline detours, feature filter, parameter reading, the
// loader-notification rescan and unload paths, and the skip rules (spec 6.5).
// The fake NGX DLL (tests/fake_nvngx) stands in for _nvngx.dll; the read-only
// case at the end runs the length decoder over the real driver export prologues.
#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <string>
#include <vector>

#include "gpu_test_devices.h"
#include "inline_hook.h"
#include "ngx_hook.h"
#include "test_framework.h"
#include "fake_nvngx/fake_nvngx.h"

using namespace acdb;
using namespace acdb_test;
using Microsoft::WRL::ComPtr;

namespace {

using CreateFn = int(__cdecl*)(void*, unsigned int, void*, void**);
using EvalFn = int(__cdecl*)(void*, const void*, const void*, void*);

struct RecSink : NgxEvaluateSink {
    std::vector<std::pair<uint64_t, NgxCreateInfo>> creates;
    std::vector<NgxEvaluateInputs> evals;
    void OnCreateFeature(uint64_t key, const NgxCreateInfo& info) override {
        creates.push_back({key, info});
    }
    void OnEvaluate(const NgxEvaluateInputs& in) override { evals.push_back(in); }
};

HMODULE LoadFake() { return LoadLibraryW(FAKE_NVNGX_PATH); }
FakeNgxState* State(HMODULE m) {
    auto fn = reinterpret_cast<FakeNgxState* (*)()>(GetProcAddress(m, "FakeNgxGetState"));
    return fn ? fn() : nullptr;
}
CreateFn Create(HMODULE m) {
    return reinterpret_cast<CreateFn>(GetProcAddress(m, "NVSDK_NGX_D3D11_CreateFeature"));
}
EvalFn Eval(HMODULE m) {
    return reinterpret_cast<EvalFn>(GetProcAddress(m, "NVSDK_NGX_D3D11_EvaluateFeature"));
}

// Create/evaluate parameter builders matching what CSP sends (spec 4).
FakeNgxParam SuperSamplingCreate() {
    FakeNgxParam p;
    p.SetU(ngxkey::kWidth, 1920u);
    p.SetU(ngxkey::kHeight, 1080u);
    p.SetU(ngxkey::kOutWidth, 1920u);
    p.SetU(ngxkey::kOutHeight, 1080u);
    p.SetI(ngxkey::kCreateFlags, static_cast<int>(kNgxDlssFlagMVLowRes));
    return p;
}
FakeNgxParam CspEvaluate() {
    FakeNgxParam p;
    p.SetRes(ngxkey::kDepth, reinterpret_cast<ID3D11Resource*>(0xAAA0));
    p.SetRes(ngxkey::kMotionVectors, reinterpret_cast<ID3D11Resource*>(0xBBB0));
    p.SetF(ngxkey::kJitterOffsetX, 0.125f);
    p.SetF(ngxkey::kJitterOffsetY, -0.2778f);
    p.SetF(ngxkey::kMvScaleX, -1920.0f);
    p.SetF(ngxkey::kMvScaleY, -1080.0f);
    p.SetU(ngxkey::kSubrectWidth, 1600u);
    p.SetU(ngxkey::kSubrectHeight, 900u);
    p.SetI(ngxkey::kReset, 0);
    return p;
}

// Installs the hook for one test and tears it down (Uninstall + FreeLibrary),
// even if the test aborts on a REQUIRE.
struct HookFixture {
    HMODULE fake = nullptr;
    RecSink sink;
    explicit HookFixture(HMODULE m, bool callbackRescan = true) : fake(m) {
        NgxHook::Get().Uninstall();  // defensive: no state from a previous test
        NgxHook::Get().SetCallbackRescanEnabled(callbackRescan);
        std::string err;
        NgxHook::Get().Install(&sink, &err);
    }
    ~HookFixture() {
        NgxHook::Get().Uninstall();
        NgxHook::Get().SetCallbackRescanEnabled(true);
        if (fake) FreeLibrary(fake);
    }
};

bool ImmediateDevices(GpuTestDevices* d) { return CreateGpuTestDevices(d); }

}  // namespace

TEST(NgxHook_HooksForwardsAndReads) {
    GpuTestDevices d;
    if (!ImmediateDevices(&d)) {
        std::printf("  no D3D device; skipping\n");
        return;
    }
    HMODULE fake = LoadFake();
    REQUIRE(fake != nullptr);
    FakeNgxState* s = State(fake);
    REQUIRE(s != nullptr);
    *s = FakeNgxState{};
    s->nextHandle = reinterpret_cast<void*>(0x120000);
    // Pointers taken BEFORE Install must still reach the detour.
    CreateFn create = Create(fake);
    EvalFn eval = Eval(fake);
    REQUIRE(create && eval);

    HookFixture fx(fake);
    CHECK(NgxHook::Get().HookedModules() >= 1);

    FakeNgxParam cp = SuperSamplingCreate();
    void* h = nullptr;
    const int rc = create(d.ctx11.Get(), kNgxFeatureSuperSampling, &cp, &h);
    CHECK_EQ(rc, kNgxSuccess);
    CHECK_EQ(s->createCalls, 1L);           // original forwarded first
    CHECK(h == reinterpret_cast<void*>(0x120000));
    REQUIRE(fx.sink.creates.size() == 1);
    CHECK_EQ(fx.sink.creates[0].first, static_cast<uint64_t>(0x120000));
    CHECK_EQ(fx.sink.creates[0].second.width, 1920u);
    CHECK_EQ(fx.sink.creates[0].second.outWidth, 1920u);
    CHECK_EQ(fx.sink.creates[0].second.createFlags, kNgxDlssFlagMVLowRes);
    CHECK(!fx.sink.creates[0].second.hasDenoiserKeys);

    FakeNgxParam ep = CspEvaluate();
    const int re = eval(d.ctx11.Get(), h, &ep, nullptr);
    CHECK_EQ(re, kNgxSuccess);
    CHECK_EQ(s->evalCalls, 1L);
    REQUIRE(fx.sink.evals.size() == 1);
    const NgxEvaluateInputs& in = fx.sink.evals[0];
    CHECK(in.depth == reinterpret_cast<ID3D11Resource*>(0xAAA0));
    CHECK(in.mvec == reinterpret_cast<ID3D11Resource*>(0xBBB0));
    CHECK(in.jitterX == 0.125f);
    CHECK(in.mvScaleX == -1920.0f);
    CHECK_EQ(in.subrectW, 1600u);
    CHECK_EQ(in.subrectH, 900u);
    CHECK_EQ(in.createFlags, kNgxDlssFlagMVLowRes);
    CHECK(in.createObserved);
    CHECK_EQ(in.featureKey, static_cast<uint64_t>(0x120000));

    // The original's result is returned unchanged; a failed original is not
    // counted (our decision, spec 6.5).
    s->evalResult = static_cast<int>(0xBAD00005);
    fx.sink.evals.clear();
    const int re2 = eval(d.ctx11.Get(), h, &ep, nullptr);
    CHECK_EQ(static_cast<unsigned>(re2), 0xBAD00005u);
    CHECK(fx.sink.evals.empty());
}

TEST(NgxHook_FeatureFilterAndReuse) {
    GpuTestDevices d;
    if (!ImmediateDevices(&d)) return;
    HMODULE fake = LoadFake();
    REQUIRE(fake != nullptr);
    FakeNgxState* s = State(fake);
    *s = FakeNgxState{};
    HookFixture fx(fake);
    CreateFn create = Create(fake);
    EvalFn eval = Eval(fake);

    // A SuperSampling create is counted.
    s->nextHandle = reinterpret_cast<void*>(0xA0000);
    FakeNgxParam ss = SuperSamplingCreate();
    void* ha = nullptr;
    create(d.ctx11.Get(), kNgxFeatureSuperSampling, &ss, &ha);
    CHECK_EQ(fx.sink.creates.size(), size_t{1});

    // A denoiser-keyed create (feature id 1 but with a GBuffer key) is not.
    s->nextHandle = reinterpret_cast<void*>(0xB0000);
    FakeNgxParam den = SuperSamplingCreate();
    den.SetRes("GBuffer.Normals", reinterpret_cast<ID3D11Resource*>(0xC0DE));
    void* hb = nullptr;
    create(d.ctx11.Get(), kNgxFeatureSuperSampling, &den, &hb);
    CHECK_EQ(fx.sink.creates.size(), size_t{1});  // still one

    // A RayReconstruction create (id 13) is not counted, and evaluates on it are
    // forwarded untouched.
    s->nextHandle = reinterpret_cast<void*>(0xC0000);
    FakeNgxParam rr = SuperSamplingCreate();
    void* hc = nullptr;
    create(d.ctx11.Get(), kNgxFeatureRayReconstruction, &rr, &hc);
    CHECK_EQ(fx.sink.creates.size(), size_t{1});
    FakeNgxParam ep = CspEvaluate();
    fx.sink.evals.clear();
    eval(d.ctx11.Get(), hc, &ep, nullptr);
    CHECK(fx.sink.evals.empty());  // not our feature

    // Evaluate on the SuperSampling handle is counted.
    eval(d.ctx11.Get(), ha, &ep, nullptr);
    CHECK_EQ(fx.sink.evals.size(), size_t{1});

    // Handle reuse: the same address created again as RayReconstruction replaces
    // the record; evaluates on it are no longer counted.
    s->nextHandle = reinterpret_cast<void*>(0xA0000);
    FakeNgxParam rr2 = SuperSamplingCreate();
    void* reused = nullptr;
    create(d.ctx11.Get(), kNgxFeatureRayReconstruction, &rr2, &reused);
    CHECK(reused == reinterpret_cast<void*>(0xA0000));
    fx.sink.evals.clear();
    eval(d.ctx11.Get(), ha, &ep, nullptr);
    CHECK(fx.sink.evals.empty());
}

TEST(NgxHook_UnobservedCreateAndSubrectFallback) {
    GpuTestDevices d;
    if (!ImmediateDevices(&d)) return;
    HMODULE fake = LoadFake();
    REQUIRE(fake != nullptr);
    FakeNgxState* s = State(fake);
    *s = FakeNgxState{};
    HookFixture fx(fake);
    EvalFn eval = Eval(fake);
    void* handle = reinterpret_cast<void*>(0xDEAD00);  // never created through the hook

    // Full block (Depth, MV, CreateFlags) present: counted, createObserved false,
    // flags from the evaluate block, and subrect falls back to Width/Height.
    FakeNgxParam full;
    full.SetRes(ngxkey::kDepth, reinterpret_cast<ID3D11Resource*>(0x11));
    full.SetRes(ngxkey::kMotionVectors, reinterpret_cast<ID3D11Resource*>(0x22));
    full.SetI(ngxkey::kCreateFlags, static_cast<int>(kNgxDlssFlagMVLowRes));
    full.SetU(ngxkey::kWidth, 1280u);
    full.SetU(ngxkey::kHeight, 720u);  // no subrect keys
    eval(d.ctx11.Get(), handle, &full, nullptr);
    REQUIRE(fx.sink.evals.size() == 1);
    CHECK(!fx.sink.evals[0].createObserved);
    CHECK_EQ(fx.sink.evals[0].createFlags, kNgxDlssFlagMVLowRes);
    CHECK_EQ(fx.sink.evals[0].subrectW, 1280u);
    CHECK_EQ(fx.sink.evals[0].subrectH, 720u);

    // Missing CreateFlags: not counted.
    fx.sink.evals.clear();
    FakeNgxParam noFlags;
    noFlags.SetRes(ngxkey::kDepth, reinterpret_cast<ID3D11Resource*>(0x11));
    noFlags.SetRes(ngxkey::kMotionVectors, reinterpret_cast<ID3D11Resource*>(0x22));
    eval(d.ctx11.Get(), handle, &noFlags, nullptr);
    CHECK(fx.sink.evals.empty());

    // Missing Depth: not counted.
    FakeNgxParam noDepth;
    noDepth.SetRes(ngxkey::kMotionVectors, reinterpret_cast<ID3D11Resource*>(0x22));
    noDepth.SetI(ngxkey::kCreateFlags, 2);
    eval(d.ctx11.Get(), handle, &noDepth, nullptr);
    CHECK(fx.sink.evals.empty());
}

TEST(NgxHook_NestingCountsOnce) {
    GpuTestDevices d;
    if (!ImmediateDevices(&d)) return;
    HMODULE fake = LoadFake();
    REQUIRE(fake != nullptr);
    FakeNgxState* s = State(fake);
    *s = FakeNgxState{};
    s->nestReentryDepth = 1;  // EvaluateFeature reenters EvaluateFeature_C once
    HookFixture fx(fake);
    CreateFn create = Create(fake);
    EvalFn eval = Eval(fake);

    s->nextHandle = reinterpret_cast<void*>(0x500000);
    FakeNgxParam ss = SuperSamplingCreate();
    void* h = nullptr;
    create(d.ctx11.Get(), kNgxFeatureSuperSampling, &ss, &h);

    FakeNgxParam ep = CspEvaluate();
    fx.sink.evals.clear();
    eval(d.ctx11.Get(), h, &ep, nullptr);
    CHECK_EQ(s->evalCalls, 1L);
    CHECK_EQ(s->evalCCalls, 1L);        // the reentry happened
    CHECK_EQ(s->maxReentryObserved, 2L);
    CHECK_EQ(fx.sink.evals.size(), size_t{1});  // but counted once
}

TEST(NgxHook_DeferredContextNotCaptured) {
    GpuTestDevices d;
    if (!ImmediateDevices(&d)) return;
    ComPtr<ID3D11DeviceContext> deferred;
    if (FAILED(d.device11->CreateDeferredContext(0, &deferred))) {
        std::printf("  no deferred context; skipping\n");
        return;
    }
    HMODULE fake = LoadFake();
    REQUIRE(fake != nullptr);
    FakeNgxState* s = State(fake);
    *s = FakeNgxState{};
    s->nextHandle = reinterpret_cast<void*>(0x900000);
    HookFixture fx(fake);
    CreateFn create = Create(fake);
    EvalFn eval = Eval(fake);

    FakeNgxParam ss = SuperSamplingCreate();
    void* h = nullptr;
    create(d.ctx11.Get(), kNgxFeatureSuperSampling, &ss, &h);
    FakeNgxParam ep = CspEvaluate();

    // Immediate context: captured.
    eval(d.ctx11.Get(), h, &ep, nullptr);
    CHECK_EQ(fx.sink.evals.size(), size_t{1});
    // Deferred context: forwarded but not captured.
    eval(deferred.Get(), h, &ep, nullptr);
    CHECK_EQ(s->evalCalls, 2L);
    CHECK_EQ(fx.sink.evals.size(), size_t{1});
}

TEST(NgxHook_LateLoadThenRescanThenUnload) {
    HMODULE preloaded = LoadFake();  // hold one ref so classify/scan is exercised on unload only for our second ref
    FreeLibrary(preloaded);          // ensure not resident from a prior test
    NgxHook::Get().Uninstall();
    NgxHook::Get().SetCallbackRescanEnabled(false);  // force the deferred path
    RecSink sink;
    std::string err;
    REQUIRE(NgxHook::Get().Install(&sink, &err));

    HMODULE fake = LoadFake();  // loaded AFTER Install
    REQUIRE(fake != nullptr);
    // The load notification only set the pending flag; nothing hooked yet.
    CHECK_EQ(NgxHook::Get().HookedModules(), 0u);
    NgxHook::Get().ProcessPendingRescan();
    CHECK(NgxHook::Get().HookedModules() >= 1);

    FreeLibrary(fake);  // its unload notification drops the layer, no memory write
    NgxHook::Get().ProcessPendingRescan();
    CHECK_EQ(NgxHook::Get().HookedModules(), 0u);

    NgxHook::Get().Uninstall();
    NgxHook::Get().SetCallbackRescanEnabled(true);
}

TEST(NgxHook_UninstallRestores) {
    GpuTestDevices d;
    if (!ImmediateDevices(&d)) return;
    HMODULE fake = LoadFake();
    REQUIRE(fake != nullptr);
    FakeNgxState* s = State(fake);
    *s = FakeNgxState{};
    s->nextHandle = reinterpret_cast<void*>(0x700000);
    RecSink sink;
    std::string err;
    NgxHook::Get().Uninstall();
    REQUIRE(NgxHook::Get().Install(&sink, &err));
    CreateFn create = Create(fake);
    FakeNgxParam ss = SuperSamplingCreate();
    void* h = nullptr;
    create(d.ctx11.Get(), kNgxFeatureSuperSampling, &ss, &h);
    CHECK_EQ(sink.creates.size(), size_t{1});

    NgxHook::Get().Uninstall();
    CHECK_EQ(NgxHook::Get().HookedModules(), 0u);
    // The export runs its original again and the sink is silent.
    sink.creates.clear();
    const long before = s->createCalls;
    create(d.ctx11.Get(), kNgxFeatureSuperSampling, &ss, &h);
    CHECK_EQ(s->createCalls, before + 1);
    CHECK(sink.creates.empty());
    FreeLibrary(fake);
}

TEST(NgxHook_SkipsAliasedModule) {
    HMODULE alias = LoadLibraryW(FAKE_NVNGX_ALIAS_PATH);
    REQUIRE(alias != nullptr);
    RecSink sink;
    std::string err;
    NgxHook::Get().Uninstall();
    REQUIRE(NgxHook::Get().Install(&sink, &err));
    // CreateFeature and EvaluateFeature resolve to one address: skipped.
    CHECK_EQ(NgxHook::Get().HookedModules(), 0u);
    NgxHook::Get().Uninstall();
    FreeLibrary(alias);
}

TEST(NgxClassify_SkipRules) {
    CHECK(NgxClassifyModule(L"C:\\game\\acs.exe", true, reinterpret_cast<void*>(0x1000),
                            reinterpret_cast<void*>(0x1100), nullptr) == NgxSkip::HostExe);
    CHECK(NgxClassifyModule(L"C:\\ProgramData\\NVIDIA\\NGX\\models\\dlssg\\x.dll", false,
                            reinterpret_cast<void*>(0x1000), reinterpret_cast<void*>(0x1100),
                            nullptr) == NgxSkip::ModelsPath);
    CHECK(NgxClassifyModule(L"C:\\x\\y.bin", false, reinterpret_cast<void*>(0x1000),
                            reinterpret_cast<void*>(0x1100), nullptr) == NgxSkip::ModelsPath);
    CHECK(NgxClassifyModule(L"C:\\x\\real.dll", false, reinterpret_cast<void*>(0x2000),
                            reinterpret_cast<void*>(0x2000), nullptr) == NgxSkip::SharedAddress);
    CHECK(NgxClassifyModule(L"C:\\x\\real.dll", false, reinterpret_cast<void*>(0x3000),
                            reinterpret_cast<void*>(0x3100), reinterpret_cast<void*>(0x3200)) ==
          NgxSkip::None);
}

TEST(NgxFillerStub_DetectsSleds) {
    void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    REQUIRE(page != nullptr);
    uint8_t* p = static_cast<uint8_t*>(page);
    memset(p, 0x90, 14);
    CHECK(NgxIsFillerStub(p));
    memset(p, 0xCC, 14);
    CHECK(NgxIsFillerStub(p));
    // A real prologue is not a sled.
    const uint8_t real[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x56, 0x57, 0x41, 0x56};
    memcpy(p, real, sizeof(real));
    CHECK(!NgxIsFillerStub(p));
    CHECK(!NgxIsFillerStub(nullptr));
    VirtualFree(page, 0, MEM_RELEASE);
}

// Read-only: run the length decoder over the real _nvngx.dll export prologues on
// this machine (never calling them), proving the decoder covers them. Skipped
// when no NVIDIA NGX loader is present.
TEST(NgxDecoder_CoversRealNvngxOnThisMachine) {
    wchar_t base[MAX_PATH] = {};
    DWORD n = sizeof(base);
    LONG r = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore",
                          L"FullPath", RRF_RT_REG_SZ, nullptr, base, &n);
    if (r != ERROR_SUCCESS) {
        std::printf("  NGXCore not present; skipping\n");
        return;
    }
    std::wstring path = std::wstring(base) + L"\\_nvngx.dll";
    HMODULE mod = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!mod) {
        std::printf("  could not map %ls; skipping\n", path.c_str());
        return;
    }
    const char* names[] = {"NVSDK_NGX_D3D11_CreateFeature", "NVSDK_NGX_D3D11_EvaluateFeature"};
    int checked = 0;
    for (const char* name : names) {
        auto* fn = reinterpret_cast<const uint8_t*>(GetProcAddress(mod, name));
        if (!fn) continue;
        // Follow an E9 thunk once, as the hook would, before measuring.
        if (fn[0] == 0xE9) {
            int32_t rel;
            memcpy(&rel, fn + 1, sizeof(rel));
            fn = fn + 5 + rel;
        }
        std::string why;
        const size_t len = PrologueLength(fn, 14, &why);
        if (len == 0) std::printf("  %s not covered: %s\n", name, why.c_str());
        CHECK(len >= 14);
        ++checked;
    }
    CHECK(checked > 0);
    FreeLibrary(mod);
}
