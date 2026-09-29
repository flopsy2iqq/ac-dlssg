// NgxHook: module scan, inline detours, feature filter, parameter reading, the
// loader-notification rescan and unload paths, and the skip rules (spec 6.5).
// The fake NGX DLL (tests/fake_nvngx) stands in for _nvngx.dll; the read-only
// case at the end runs the length decoder over the real driver export prologues.
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "child_process.h"
#include "gpu_test_devices.h"
#include "inline_hook.h"
#include "log.h"
#include "ngx_hook.h"
#include "temp_dir.h"
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
    explicit HookFixture(HMODULE m) : fake(m) {
        NgxHook::Get().Uninstall();  // defensive: no state from a previous test
        std::string err;
        NgxHook::Get().Install(&sink, &err);
    }
    ~HookFixture() {
        NgxHook::Get().Uninstall();
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

// A render subrect that is present but 0 means "not set": NVIDIA's evaluate
// helper always writes the key from InRenderSubrectDimensions, which an app
// without dynamic resolution leaves at 0, and the DLSS guide (3.17) then
// assumes the input dimensions given at creation (ngx review F3). The subrect
// falls back to the evaluate block's Width/Height, then to the create's.
TEST(NgxHook_ZeroSubrectFallsBackToInputSize) {
    GpuTestDevices d;
    if (!ImmediateDevices(&d)) return;
    HMODULE fake = LoadFake();
    REQUIRE(fake != nullptr);
    FakeNgxState* s = State(fake);
    *s = FakeNgxState{};
    s->nextHandle = reinterpret_cast<void*>(0x640000);
    HookFixture fx(fake);
    FakeNgxParam cp = SuperSamplingCreate();  // Width/Height 1920x1080
    void* h = nullptr;
    Create(fake)(d.ctx11.Get(), kNgxFeatureSuperSampling, &cp, &h);

    // Zero subrect, Width/Height in the evaluate block: those win.
    FakeNgxParam ep = CspEvaluate();
    ep.SetU(ngxkey::kSubrectWidth, 0u);
    ep.SetU(ngxkey::kSubrectHeight, 0u);
    ep.SetU(ngxkey::kWidth, 1280u);
    ep.SetU(ngxkey::kHeight, 720u);
    Eval(fake)(d.ctx11.Get(), h, &ep, nullptr);
    REQUIRE(fx.sink.evals.size() == 1);
    CHECK_EQ(fx.sink.evals[0].subrectW, 1280u);
    CHECK_EQ(fx.sink.evals[0].subrectH, 720u);

    // Zero subrect and no Width/Height in the block: the create's input size.
    FakeNgxParam bare = CspEvaluate();
    bare.SetU(ngxkey::kSubrectWidth, 0u);
    bare.SetU(ngxkey::kSubrectHeight, 0u);
    Eval(fake)(d.ctx11.Get(), h, &bare, nullptr);
    REQUIRE(fx.sink.evals.size() == 2);
    CHECK_EQ(fx.sink.evals[1].subrectW, 1920u);
    CHECK_EQ(fx.sink.evals[1].subrectH, 1080u);

    // A real subrect still wins over both.
    FakeNgxParam dyn = CspEvaluate();  // 1600x900
    dyn.SetU(ngxkey::kWidth, 1280u);
    dyn.SetU(ngxkey::kHeight, 720u);
    Eval(fake)(d.ctx11.Get(), h, &dyn, nullptr);
    REQUIRE(fx.sink.evals.size() == 3);
    CHECK_EQ(fx.sink.evals[2].subrectW, 1600u);
    CHECK_EQ(fx.sink.evals[2].subrectH, 900u);
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
    REQUIRE(GetModuleHandleW(L"fake_nvngx.dll") == nullptr);  // not resident from a prior test
    NgxHook::Get().Uninstall();
    RecSink sink;
    std::string err;
    REQUIRE(NgxHook::Get().Install(&sink, &err));

    HMODULE fake = LoadFake();  // loaded AFTER Install
    REQUIRE(fake != nullptr);
    // The load notification only queued the event; nothing is hooked yet.
    CHECK_EQ(NgxHook::Get().HookedModules(), 0u);
    NgxHook::Get().ProcessPendingRescan();
    CHECK(NgxHook::Get().HookedModules() >= 1);

    FreeLibrary(fake);  // its queued unload drops the layer, no memory write
    NgxHook::Get().ProcessPendingRescan();
    CHECK_EQ(NgxHook::Get().HookedModules(), 0u);

    NgxHook::Get().Uninstall();
}

// The loader-notification callback runs under the loader lock and must never
// wait on one of our locks (spec 6.5, ngx review F1). One thread holds every
// internal lock while another loads and unloads a module; both loader calls must
// return promptly. Runs in a child process so a regression hangs only the child.
namespace {
struct LoadUnloadCtx {
    HANDLE done = nullptr;
    bool loaded = false;
};
DWORD WINAPI LoadAndUnloadFake(void* p) {
    auto* c = static_cast<LoadUnloadCtx*>(p);
    HMODULE m = LoadLibraryW(FAKE_NVNGX_PATH);
    c->loaded = m != nullptr;
    if (m) FreeLibrary(m);
    SetEvent(c->done);
    return 0;
}
}  // namespace

TEST(Child_NgxHook_LoadCompletesWhileStateLocked) {
    NgxHook::Get().Uninstall();
    RecSink sink;
    std::string err;
    REQUIRE(NgxHook::Get().Install(&sink, &err));
    REQUIRE(GetModuleHandleW(L"fake_nvngx.dll") == nullptr);  // the load below is a real load

    LoadUnloadCtx ctx;
    ctx.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    REQUIRE(ctx.done != nullptr);
    NgxHook::Get().LockStateForTest();
    HANDLE th = CreateThread(nullptr, 0, &LoadAndUnloadFake, &ctx, 0, nullptr);
    const DWORD w = th ? WaitForSingleObject(ctx.done, 5000) : WAIT_FAILED;
    NgxHook::Get().UnlockStateForTest();
    if (th) {
        WaitForSingleObject(th, INFINITE);
        CloseHandle(th);
    }
    CloseHandle(ctx.done);
    CHECK(w == WAIT_OBJECT_0);  // the load and the unload returned while we held the locks
    CHECK(ctx.loaded);

    // The queued load and unload are finished from the render thread: the module
    // is gone again, so nothing stays hooked.
    NgxHook::Get().ProcessPendingRescan();
    CHECK_EQ(NgxHook::Get().HookedModules(), 0u);
    NgxHook::Get().Uninstall();
}

TEST(NgxHook_LoadCompletesWhileStateLocked) {
    CHECK_EQ(RunChildTest("Child_NgxHook_LoadCompletesWhileStateLocked", 60000), 0);
}

// A module that loads after Install's first scan but before the notification is
// registered produces no event; the first ProcessPendingRescan must still hook
// it (ngx review F5).
namespace {
HMODULE g_gapLoaded = nullptr;
void LoadFakeInInstallGap() { g_gapLoaded = LoadLibraryW(FAKE_NVNGX_PATH); }
}  // namespace

TEST(NgxHook_ModuleLoadedDuringInstallIsHooked) {
    REQUIRE(GetModuleHandleW(L"fake_nvngx.dll") == nullptr);
    NgxHook::Get().Uninstall();
    RecSink sink;
    std::string err;
    g_gapLoaded = nullptr;
    NgxHook::Get().SetInstallGapHookForTest(&LoadFakeInInstallGap);
    const bool installed = NgxHook::Get().Install(&sink, &err);
    NgxHook::Get().SetInstallGapHookForTest(nullptr);
    REQUIRE(installed);
    REQUIRE(g_gapLoaded != nullptr);
    CHECK_EQ(NgxHook::Get().HookedModules(), 0u);  // the first scan ran before the load

    NgxHook::Get().ProcessPendingRescan();
    CHECK(NgxHook::Get().HookedModules() >= 1);
    NgxHook::Get().Uninstall();
    FreeLibrary(g_gapLoaded);
    g_gapLoaded = nullptr;
}

// More loader events than the queue holds: the unload that falls off the end is
// still noticed, because an overflow makes the next ProcessPendingRescan
// re-verify every layer (ngx review F1).
TEST(NgxHook_QueueOverflowStillDropsUnloadedLayer) {
    REQUIRE(GetModuleHandleW(L"fake_nvngx.dll") == nullptr);
    HMODULE fake = LoadFake();
    REQUIRE(fake != nullptr);
    NgxHook::Get().Uninstall();
    RecSink sink;
    std::string err;
    REQUIRE(NgxHook::Get().Install(&sink, &err));
    REQUIRE(NgxHook::Get().HookedModules() >= 1);

    // 2 events per cycle, 100 cycles: far more than the queue holds.
    for (int i = 0; i < 100; ++i) {
        HMODULE a = LoadLibraryW(FAKE_NVNGX_ALIAS_PATH);
        if (a) FreeLibrary(a);
    }
    FreeLibrary(fake);  // this unload's event is dropped: the queue is full
    NgxHook::Get().ProcessPendingRescan();
    CHECK_EQ(NgxHook::Get().HookedModules(), 0u);
    NgxHook::Get().Uninstall();
}

// A hooked module unloads and a different module loads at the same base while
// the loader events are lost (queue overflow). The stale layer must be
// recognised by its size and path: the new module gets hooked, and Uninstall
// never writes the old module's saved bytes into it (ngx review F6).
TEST(NgxHook_ReusedBaseIsNotTakenForTheOldModule) {
    REQUIRE(GetModuleHandleW(L"fake_nvngx_reuse_a.dll") == nullptr);
    REQUIRE(GetModuleHandleW(L"fake_nvngx_reuse_b.dll") == nullptr);
    HMODULE a = LoadLibraryW(FAKE_NVNGX_REUSE_A_PATH);
    REQUIRE(a != nullptr);
    const uintptr_t aCreate = reinterpret_cast<uintptr_t>(Create(a));
    NgxHook::Get().Uninstall();
    RecSink sink;
    std::string err;
    REQUIRE(NgxHook::Get().Install(&sink, &err));
    REQUIRE(NgxHook::Get().HookedModules() >= 1);

    for (int i = 0; i < 100; ++i) {  // overflow the event queue
        HMODULE x = LoadLibraryW(FAKE_NVNGX_ALIAS_PATH);
        if (x) FreeLibrary(x);
    }
    FreeLibrary(a);  // its unload event is lost
    HMODULE b = LoadLibraryW(FAKE_NVNGX_REUSE_B_PATH);
    REQUIRE(b != nullptr);
    if (b != a) {
        std::printf("  the second fake did not load at the first one's base; skipping\n");
        NgxHook::Get().Uninstall();
        FreeLibrary(b);
        return;
    }
    // B's bytes where A's CreateFeature was: Uninstall must leave them alone.
    uint8_t before[16];
    memcpy(before, reinterpret_cast<const void*>(aCreate), sizeof(before));

    NgxHook::Get().ProcessPendingRescan();
    CHECK(NgxHook::Get().HookedModules() >= 1);
    FakeNgxState* sb = State(b);
    REQUIRE(sb != nullptr);
    *sb = FakeNgxState{};
    sb->nextHandle = reinterpret_cast<void*>(0x330000);
    FakeNgxParam cp = SuperSamplingCreate();
    void* h = nullptr;
    CHECK_EQ(Create(b)(nullptr, kNgxFeatureSuperSampling, &cp, &h), kNgxSuccess);
    CHECK_EQ(sb->createCalls, 1L);
    CHECK_EQ(sink.creates.size(), size_t{1});  // B itself is hooked

    NgxHook::Get().Uninstall();
    uint8_t after[16];
    memcpy(after, reinterpret_cast<const void*>(aCreate), sizeof(after));
    CHECK(memcmp(before, after, sizeof(before)) == 0);
    CHECK_EQ(Create(b)(nullptr, kNgxFeatureSuperSampling, &cp, &h), kNgxSuccess);  // B still runs
    CHECK_EQ(sb->createCalls, 2L);
    FreeLibrary(b);
}

// SetSink swaps the sink, and when it returns no call into the old sink is in
// progress: the presenter calls SetSink(nullptr) and is then destroyed.
namespace {
struct SlowSink : NgxEvaluateSink {
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<bool> inside{false};
    std::atomic<int> evals{0};
    ~SlowSink() override { CloseHandle(entered); }
    void OnCreateFeature(uint64_t, const NgxCreateInfo&) override {}
    void OnEvaluate(const NgxEvaluateInputs&) override {
        inside = true;
        ++evals;
        SetEvent(entered);
        Sleep(300);
        inside = false;
    }
};
struct EvalCallCtx {
    EvalFn eval = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    void* handle = nullptr;
    FakeNgxParam* params = nullptr;
};
DWORD WINAPI EvaluateOnThread(void* p) {
    auto* c = static_cast<EvalCallCtx*>(p);
    c->eval(c->ctx, c->handle, c->params, nullptr);
    return 0;
}
}  // namespace

TEST(NgxHook_SetSinkWaitsForCallsInProgress) {
    GpuTestDevices d;
    if (!ImmediateDevices(&d)) return;
    HMODULE fake = LoadFake();
    REQUIRE(fake != nullptr);
    FakeNgxState* s = State(fake);
    *s = FakeNgxState{};
    s->nextHandle = reinterpret_cast<void*>(0x610000);
    SlowSink slow;
    NgxHook::Get().Uninstall();
    std::string err;
    REQUIRE(NgxHook::Get().Install(&slow, &err));
    FakeNgxParam cp = SuperSamplingCreate();
    void* h = nullptr;
    Create(fake)(d.ctx11.Get(), kNgxFeatureSuperSampling, &cp, &h);
    FakeNgxParam ep = CspEvaluate();

    EvalCallCtx call{Eval(fake), d.ctx11.Get(), h, &ep};
    HANDLE th = CreateThread(nullptr, 0, &EvaluateOnThread, &call, 0, nullptr);
    REQUIRE(th != nullptr);
    const bool entered = WaitForSingleObject(slow.entered, 5000) == WAIT_OBJECT_0;
    NgxHook::Get().SetSink(nullptr);
    const bool stillInside = slow.inside;  // must be false: SetSink waited for the call
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    CHECK(entered);
    CHECK(!stillInside);

    // Later evaluates never reach the old sink; a new sink gets them.
    Eval(fake)(d.ctx11.Get(), h, &ep, nullptr);
    CHECK_EQ(slow.evals.load(), 1);
    RecSink rec;
    NgxHook::Get().SetSink(&rec);
    Eval(fake)(d.ctx11.Get(), h, &ep, nullptr);
    CHECK_EQ(rec.evals.size(), size_t{1});
    NgxHook::Get().Uninstall();
    FreeLibrary(fake);
}

// A sink that clears itself from inside its own OnEvaluate must not wait on its
// own call (that would deadlock the render thread). Child process: a regression
// hangs only the child.
namespace {
struct SelfClearingSink : NgxEvaluateSink {
    int evals = 0;
    void OnCreateFeature(uint64_t, const NgxCreateInfo&) override {}
    void OnEvaluate(const NgxEvaluateInputs&) override {
        ++evals;
        NgxHook::Get().SetSink(nullptr);
    }
};
}  // namespace

TEST(Child_NgxHook_SetSinkFromInsideTheSink) {
    GpuTestDevices d;
    if (!ImmediateDevices(&d)) return;
    HMODULE fake = LoadFake();
    REQUIRE(fake != nullptr);
    FakeNgxState* s = State(fake);
    *s = FakeNgxState{};
    s->nextHandle = reinterpret_cast<void*>(0x620000);
    SelfClearingSink sink;
    NgxHook::Get().Uninstall();
    std::string err;
    REQUIRE(NgxHook::Get().Install(&sink, &err));
    FakeNgxParam cp = SuperSamplingCreate();
    void* h = nullptr;
    Create(fake)(d.ctx11.Get(), kNgxFeatureSuperSampling, &cp, &h);
    FakeNgxParam ep = CspEvaluate();
    Eval(fake)(d.ctx11.Get(), h, &ep, nullptr);  // returns: no self-wait
    Eval(fake)(d.ctx11.Get(), h, &ep, nullptr);  // the sink is gone now
    CHECK_EQ(sink.evals, 1);
    NgxHook::Get().Uninstall();
    FreeLibrary(fake);
}

TEST(NgxHook_SetSinkFromInsideTheSink) {
    CHECK_EQ(RunChildTest("Child_NgxHook_SetSinkFromInsideTheSink", 60000), 0);
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

// A dispatcher whose slot has no original (never hooked, removed or detached)
// must not invent a result: NGX's generic failure goes back, never Success
// with an unwritten *outHandle (ngx review F2).
TEST(NgxHook_NoOriginalReturnsFail) {
    NgxHook::Get().Uninstall();
    NgxHandle* handle = reinterpret_cast<NgxHandle*>(0x1234);
    const NgxResult rc = NgxHook::Get().DispatchCreate(0, nullptr, kNgxFeatureSuperSampling, nullptr, &handle);
    CHECK_EQ(static_cast<uint32_t>(rc), 0xBAD00000u);
    CHECK(handle == reinterpret_cast<NgxHandle*>(0x1234));  // untouched
    CHECK_EQ(static_cast<uint32_t>(NgxHook::Get().DispatchEvaluate(0, false, nullptr, nullptr, nullptr, nullptr)),
             0xBAD00000u);
    CHECK_EQ(static_cast<uint32_t>(NgxHook::Get().DispatchEvaluate(0, true, nullptr, nullptr, nullptr, nullptr)),
             0xBAD00000u);
    // An out-of-range slot is refused the same way rather than indexing past the table.
    CHECK_EQ(static_cast<uint32_t>(NgxHook::Get().DispatchCreate(99, nullptr, 1, nullptr, &handle)), 0xBAD00000u);
    CHECK_EQ(static_cast<uint32_t>(NgxHook::Get().DispatchEvaluate(-1, false, nullptr, nullptr, nullptr, nullptr)),
             0xBAD00000u);
}
static_assert(static_cast<uint32_t>(kNgxFail) == 0xBAD00000u, "NVSDK_NGX_Result_Fail");

// ---- CSP's DLSS NR ("Neural Rendering") and DLSS RR are never counted (ngx
// review F4). CSP creates its NR feature through the signed nvngx_dlssnr.dll
// with feature ids 18, 16, 17, 19, 20 and DLSSNR.* keys (static analysis of
// CSP 0.3.0-preview622 dwrite.dll).

TEST(NgxIsDenoiserModule_Names) {
    CHECK(NgxIsDenoiserModule(L"H:\\game\\nvngx_dlssnr.dll"));
    CHECK(NgxIsDenoiserModule(L"C:\\x\\NVNGX_DLSSD.DLL"));
    CHECK(NgxIsDenoiserModule(L"nvngx_dlssnr.dll"));
    CHECK(!NgxIsDenoiserModule(L"C:\\x\\nvngx_dlss.dll"));
    CHECK(!NgxIsDenoiserModule(L"C:\\x\\_nvngx.dll"));
    CHECK(!NgxIsDenoiserModule(L"C:\\x\\my_nvngx_dlssnr.dll"));  // the base name, not a suffix
    CHECK(!NgxIsDenoiserModule(L""));
    CHECK(!NgxIsDenoiserModule(nullptr));
}

// Calls whose outermost layer is the NR snippet are forwarded but never
// counted, even a feature-id-1 create with plain DLSS parameters; the handle it
// returns is recorded as not ours, so an evaluate on it through another layer
// is not counted either.
TEST(NgxHook_DenoiserSnippetNeverCounts) {
    GpuTestDevices d;
    if (!ImmediateDevices(&d)) return;
    REQUIRE(GetModuleHandleW(L"nvngx_dlssnr.dll") == nullptr);
    HMODULE plain = LoadFake();
    REQUIRE(plain != nullptr);
    HMODULE nr = LoadLibraryW(FAKE_NVNGX_DLSSNR_PATH);
    REQUIRE(nr != nullptr);
    FakeNgxState* s = State(nr);
    REQUIRE(s != nullptr && s != State(plain));
    *s = FakeNgxState{};
    s->nextHandle = reinterpret_cast<void*>(0x710000);
    *State(plain) = FakeNgxState{};
    HookFixture fx(plain);
    CHECK(NgxHook::Get().HookedModules() >= 2);  // hooked, so nesting is tracked

    FakeNgxParam cp = SuperSamplingCreate();
    void* h = nullptr;
    CHECK_EQ(Create(nr)(d.ctx11.Get(), kNgxFeatureSuperSampling, &cp, &h), kNgxSuccess);
    CHECK_EQ(s->createCalls, 1L);
    CHECK(fx.sink.creates.empty());

    FakeNgxParam ep = CspEvaluate();
    CHECK_EQ(Eval(nr)(d.ctx11.Get(), h, &ep, nullptr), kNgxSuccess);
    CHECK_EQ(s->evalCalls, 1L);
    ep.SetI(ngxkey::kCreateFlags, static_cast<int>(kNgxDlssFlagMVLowRes));  // unobserved-style block
    Eval(nr)(d.ctx11.Get(), reinterpret_cast<void*>(0x720000), &ep, nullptr);
    CHECK(fx.sink.evals.empty());

    Eval(plain)(d.ctx11.Get(), h, &ep, nullptr);  // the NR handle through the other layer
    CHECK(fx.sink.evals.empty());

    NgxHook::Get().Uninstall();
    FreeLibrary(nr);
}

// CSP's NR ids are not SuperSampling; a feature-id-1 create that carries a
// DLSSNR or DLSSD input resource is not either, but one with only NR scalar
// hints is; and an evaluate on an unobserved handle
// that carries a DLSSNR resource is not counted even when Depth, MotionVectors
// and the create flags are present too.
TEST(NgxHook_NeuralRenderingIdsAndKeysNotCounted) {
    GpuTestDevices d;
    if (!ImmediateDevices(&d)) return;
    HMODULE fake = LoadFake();
    REQUIRE(fake != nullptr);
    FakeNgxState* s = State(fake);
    *s = FakeNgxState{};
    HookFixture fx(fake);
    FakeNgxParam ep = CspEvaluate();

    const uint32_t nrIds[] = {18, 16, 17, 19, 20};
    for (uint32_t id : nrIds) {
        s->nextHandle = reinterpret_cast<void*>(static_cast<uintptr_t>(0x730000 + id * 0x100));
        FakeNgxParam nrc;
        nrc.SetU("DLSSNR.Width", 1920u);
        nrc.SetU("DLSSNR.Height", 1080u);
        nrc.SetU(ngxkey::kWidth, 1920u);
        nrc.SetU(ngxkey::kHeight, 1080u);
        void* h = nullptr;
        Create(fake)(d.ctx11.Get(), id, &nrc, &h);
        Eval(fake)(d.ctx11.Get(), h, &ep, nullptr);
    }
    CHECK(fx.sink.creates.empty());
    CHECK(fx.sink.evals.empty());

    // A SuperSampling create that carries only NR scalar hints is still CSP's
    // DLSS: CSP 0.3.0-preview622 sets DLSSNR.Hint.Render.Preset on its
    // SuperSampling create block (seen in game on 2026-09-29). Only a real
    // denoiser input resource disqualifies a feature-id-1 create.
    s->nextHandle = reinterpret_cast<void*>(0x740000);
    FakeNgxParam nrScalars = SuperSamplingCreate();
    nrScalars.SetU("DLSSNR.Hint.Render.Preset", 10u);
    nrScalars.SetU("DLSSNR.Width", 1920u);
    void* h1 = nullptr;
    Create(fake)(d.ctx11.Get(), kNgxFeatureSuperSampling, &nrScalars, &h1);
    CHECK_EQ(fx.sink.creates.size(), size_t{1});
    fx.sink.creates.clear();

    s->nextHandle = reinterpret_cast<void*>(0x750000);
    FakeNgxParam rr = SuperSamplingCreate();
    rr.SetRes("DLSSD.DiffuseHitDistance", reinterpret_cast<ID3D11Resource*>(0xD00D));
    void* h2 = nullptr;
    Create(fake)(d.ctx11.Get(), kNgxFeatureSuperSampling, &rr, &h2);
    CHECK(fx.sink.creates.empty());

    FakeNgxParam unobserved;
    unobserved.SetRes(ngxkey::kDepth, reinterpret_cast<ID3D11Resource*>(0x11));
    unobserved.SetRes(ngxkey::kMotionVectors, reinterpret_cast<ID3D11Resource*>(0x22));
    unobserved.SetI(ngxkey::kCreateFlags, static_cast<int>(kNgxDlssFlagMVLowRes));
    unobserved.SetRes("DLSSNR.Color", reinterpret_cast<ID3D11Resource*>(0xC010));
    Eval(fake)(d.ctx11.Get(), reinterpret_cast<void*>(0x7F0000), &unobserved, nullptr);
    CHECK(fx.sink.evals.empty());

    // Control: the same block without the NR key is counted.
    FakeNgxParam plainBlock;
    plainBlock.SetRes(ngxkey::kDepth, reinterpret_cast<ID3D11Resource*>(0x11));
    plainBlock.SetRes(ngxkey::kMotionVectors, reinterpret_cast<ID3D11Resource*>(0x22));
    plainBlock.SetI(ngxkey::kCreateFlags, static_cast<int>(kNgxDlssFlagMVLowRes));
    Eval(fake)(d.ctx11.Get(), reinterpret_cast<void*>(0x7F0000), &plainBlock, nullptr);
    CHECK_EQ(fx.sink.evals.size(), size_t{1});
}

// A module whose NGX exports forward into an already-hooked module resolves to
// the same code. It is not hooked a second time: a second patch would chain
// two detours, and restoring them later would leave a jump into freed memory.
TEST(NgxHook_ForwardedExportsNotHookedTwice) {
    GpuTestDevices d;
    if (!ImmediateDevices(&d)) return;
    REQUIRE(GetModuleHandleW(L"nvngx_dlssnr.dll") == nullptr);
    HMODULE nr = LoadLibraryW(FAKE_NVNGX_DLSSNR_PATH);
    REQUIRE(nr != nullptr);
    HMODULE fwd = LoadLibraryW(FAKE_NVNGX_FWD_PATH);
    REQUIRE(fwd != nullptr);
    CreateFn nrCreate = Create(nr);
    REQUIRE(reinterpret_cast<void*>(Create(fwd)) == reinterpret_cast<void*>(nrCreate));
    uint8_t original[16];
    memcpy(original, reinterpret_cast<const void*>(nrCreate), sizeof(original));

    RecSink sink;
    std::string err;
    NgxHook::Get().Uninstall();
    TempDir tmp(L"ngx_fwd");
    const std::filesystem::path logPath = tmp.Path() / L"bridge.log";
    REQUIRE(LogOpen(logPath.wstring(), LogLevel::Debug));
    const bool installed = NgxHook::Get().Install(&sink, &err);
    LogClose();
    REQUIRE(installed);
    CHECK_EQ(NgxHook::Get().HookedModules(), 1u);
    // Not even attempted: patching already-patched code only works by luck of
    // how our jump's address bytes decode.
    const std::string log = ReadAll(logPath);
    CHECK(log.find("hook failed for") == std::string::npos);
    CHECK(log.find("already hooked") != std::string::npos);

    FakeNgxState* s = State(nr);
    *s = FakeNgxState{};
    s->nextHandle = reinterpret_cast<void*>(0x760000);
    FakeNgxParam cp = SuperSamplingCreate();
    void* h = nullptr;
    CHECK_EQ(Create(fwd)(d.ctx11.Get(), kNgxFeatureSuperSampling, &cp, &h), kNgxSuccess);
    CHECK_EQ(s->createCalls, 1L);

    NgxHook::Get().Uninstall();
    CHECK(memcmp(original, reinterpret_cast<const void*>(nrCreate), sizeof(original)) == 0);
    FreeLibrary(fwd);
    FreeLibrary(nr);
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
