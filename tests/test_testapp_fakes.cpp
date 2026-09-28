// The fakes the test application uses to stand in for CSP (tools/testapp):
// the parameter blocks of the fake NGX module (tests/fake_nvngx), and the
// test app's CSP stand-ins in tools/testapp/fake_csp.h (the camera writer of
// the Lua app, the Halton jitter and the DLSS calls). They are checked through
// the bridge's real CameraChannel and NgxHook, because the test app's
// --fake-camera and --fake-ngx modes feed exactly those.
#include <windows.h>
#include <d3d11.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "camera_channel.h"
#include "fake_csp.h"
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

// ------------------------------------------------------------ fake_csp.h

namespace {

std::wstring TestCameraSection(const wchar_t* tag) {
    return L"Local\\AcDlssg.Camera.testapp." + std::to_wstring(GetCurrentProcessId()) + L"." + tag + L"." +
           std::to_wstring(GetTickCount64());
}

float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
bool Near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

using ComPtrTexture2D = Microsoft::WRL::ComPtr<ID3D11Texture2D>;

ComPtrTexture2D AsTexture(ID3D11Resource* r) {
    ComPtrTexture2D t;
    if (r) r->QueryInterface(IID_PPV_ARGS(&t));
    return t;
}

D3D11_TEXTURE2D_DESC DescOf(ID3D11Resource* r) {
    D3D11_TEXTURE2D_DESC d{};
    if (const auto t = AsTexture(r)) t->GetDesc(&d);
    return d;
}

}  // namespace

// The test app writes the section the bridge reads.
TEST(TestappFakes_CameraSectionIsTheBridges) {
    CHECK(std::wstring(testapp::kCameraSectionName) == acdb::kCameraSectionName);
}

// The 8-phase Halton(2,3) sequence, centred on the pixel: phase i uses the
// Halton index i + 1, and frame 8 starts over.
TEST(TestappFakes_JitterIsTheEightPhaseHalton23Sequence) {
    const float x[8] = {0.0f, -0.25f, 0.25f, -0.375f, 0.125f, -0.125f, 0.375f, -0.4375f};
    const float y[8] = {1.0f / 3 - 0.5f, 2.0f / 3 - 0.5f, 1.0f / 9 - 0.5f, 4.0f / 9 - 0.5f,
                        7.0f / 9 - 0.5f, 2.0f / 9 - 0.5f, 5.0f / 9 - 0.5f, 8.0f / 9 - 0.5f};
    for (uint32_t f = 0; f < 16; ++f) {
        const testapp::Jitter j = testapp::HaltonJitter(f);
        CHECK(Near(j.x, x[f % 8]));
        CHECK(Near(j.y, y[f % 8]));
    }
}

// The path camera: smooth motion at driving speed, an orthonormal basis, and
// side = cross(look, up) as CSP builds it.
TEST(TestappFakes_PathCameraMovesSmoothlyWithCspsBasis) {
    for (int f = 0; f < 600; f += 37) {
        const testapp::CameraPose a = testapp::PathPose(f / 60.0);
        const testapp::CameraPose b = testapp::PathPose((f + 1) / 60.0);
        const float d[3] = {b.pos[0] - a.pos[0], b.pos[1] - a.pos[1], b.pos[2] - a.pos[2]};
        const float step = std::sqrt(Dot(d, d));
        CHECK(step > 0.05f && step < 1.0f);  // moving, and far below the Lua app's 1 m jump limit
        CHECK(Near(Dot(a.fwd, a.fwd), 1.0f) && Near(Dot(a.up, a.up), 1.0f) && Near(Dot(a.side, a.side), 1.0f));
        CHECK(Near(Dot(a.fwd, a.up), 0.0f) && Near(Dot(a.fwd, a.side), 0.0f) && Near(Dot(a.up, a.side), 0.0f));
        const float cross[3] = {a.fwd[1] * a.up[2] - a.fwd[2] * a.up[1], a.fwd[2] * a.up[0] - a.fwd[0] * a.up[2],
                                a.fwd[0] * a.up[1] - a.fwd[1] * a.up[0]};
        CHECK(Near(cross[0], a.side[0]) && Near(cross[1], a.side[1]) && Near(cross[2], a.side[2]));
        CHECK(Dot(a.fwd, b.fwd) > 0.99f);  // far from the Lua app's 30 degree cut
    }
}

// What the writer publishes is what the bridge's reader accepts: a stable
// record with our magic and version, a frame counter that makes every write
// fresh, and the fields of the pose and the render size.
TEST(TestappFakes_CameraWriterPublishesRecordsTheBridgeReads) {
    const std::wstring name = TestCameraSection(L"writer");
    CameraChannel channel(name.c_str());
    std::string err;
    REQUIRE(channel.Create(&err));
    testapp::CameraWriter writer;
    REQUIRE(writer.Open(name.c_str(), &err));

    CameraLatch latch;
    uint32_t previousFrame = 0;
    for (int f = 0; f < 5; ++f) {
        const testapp::CameraPose pose = testapp::PathPose(f / 60.0);
        writer.Write(pose, 853.0f, 480.0f, 1.0f / 60, f * 1000.0 / 60);
        CameraLayout snap{};
        REQUIRE(channel.Read(&snap) == CameraChannel::ReadResult::Ok);
        CHECK_EQ(snap.magic, kCameraMagic);
        CHECK_EQ(snap.version, kCameraVersion);
        CHECK_EQ(snap.seq % 2, 0u);
        if (f > 0) CHECK_EQ(snap.frame, previousFrame + 1);
        previousFrame = snap.frame;
        CHECK_EQ(writer.Frame(), snap.frame);
        // The first write has nothing to compare with, like the Lua app's first.
        CHECK_EQ(snap.flags, f == 0 ? static_cast<uint32_t>(kCamJumped) : 0u);
        CHECK_EQ(latch.Latch(snap), f > 0);  // the first latch is never fresh
        for (int i = 0; i < 3; ++i) {
            CHECK(snap.pos[i] == pose.pos[i] && snap.fwd[i] == pose.fwd[i] && snap.up[i] == pose.up[i] &&
                  snap.side[i] == pose.side[i] && snap.originShift[i] == 0.0f);
        }
        CHECK(snap.fovVDeg == 56.0f && snap.clipNear == 0.1f && snap.clipFar == 20000.0f);
        CHECK(snap.renderW == 853.0f && snap.renderH == 480.0f);
        CHECK(Near(snap.dt, 1.0f / 60));
        CHECK(snap.simTimeMs == f * 1000.0 / 60);
    }
}

// Like the Lua app after a reload: a new writer continues the counter of the
// section instead of starting over, so the bridge sees fresh records at once.
TEST(TestappFakes_CameraWriterContinuesTheCounterOfAnExistingSection) {
    const std::wstring name = TestCameraSection(L"continue");
    CameraChannel channel(name.c_str());
    std::string err;
    REQUIRE(channel.Create(&err));
    uint32_t last = 0;
    {
        testapp::CameraWriter first;
        REQUIRE(first.Open(name.c_str(), &err));
        for (int f = 0; f < 3; ++f) first.Write(testapp::PathPose(f / 60.0), 640, 360, 0.016f, 0);
        last = first.Frame();
    }
    testapp::CameraWriter second;
    REQUIRE(second.Open(name.c_str(), &err));
    second.Write(testapp::PathPose(0.1), 640, 360, 0.016f, 0);
    CameraLayout snap{};
    REQUIRE(channel.Read(&snap) == CameraChannel::ReadResult::Ok);
    CHECK_EQ(snap.frame, last + 1);
    CHECK_EQ(snap.flags, static_cast<uint32_t>(kCamJumped));
}

// A seq left odd by an interrupted write heals with the next one (forced
// parity, as the Lua app does it), and the record is readable again.
TEST(TestappFakes_CameraWriterForcesTheSeqParity) {
    const std::wstring name = TestCameraSection(L"parity");
    CameraChannel channel(name.c_str());
    std::string err;
    REQUIRE(channel.Create(&err));
    testapp::CameraWriter writer;
    REQUIRE(writer.Open(name.c_str(), &err));
    writer.Write(testapp::PathPose(0), 640, 360, 0.016f, 0);
    writer.SetSeqForTest(7);  // odd: a writer died between its two seq stores
    CameraLayout snap{};
    CHECK(channel.Read(&snap) == CameraChannel::ReadResult::Torn);
    writer.Write(testapp::PathPose(0.02), 640, 360, 0.016f, 0);
    REQUIRE(channel.Read(&snap) == CameraChannel::ReadResult::Ok);
    CHECK_EQ(snap.seq, 8u);
}

// The test app's DLSS calls, as CSP makes them, seen through the bridge's
// hook: one SuperSampling create with MVLowRes, then per frame an evaluate on
// the immediate context with an R32_TYPELESS depth texture and two ping-ponged
// R16G16_FLOAT motion-vector textures at the render size, the Halton jitter,
// MV.Scale = -(render size), the subrect and Reset on the first frame only.
// A re-create (a resize) starts over with a new handle and Reset.
TEST(TestappFakes_NgxDriverPlaysCspsCallsThroughTheHook) {
    GpuTestDevices d;
    if (!CreateGpuTestDevices(&d)) {
        std::printf("  no D3D device; skipping\n");
        return;
    }
    FakeNgxFixture fx;
    REQUIRE(fx.module != nullptr && fx.state != nullptr);
    testapp::FakeNgx ngx;
    std::string err;
    REQUIRE(ngx.Attach(fx.module, &err));
    CHECK_EQ(fx.state->allocatedParams, 1L);
    REQUIRE(ngx.CreateFeature(d.device11.Get(), d.ctx11.Get(), 853, 480, 1280, 720, &err));
    REQUIRE(fx.sink.creates.size() == 1);
    CHECK_EQ(fx.sink.creates[0].featureId, kNgxFeatureSuperSampling);
    CHECK_EQ(fx.sink.creates[0].width, 853u);
    CHECK_EQ(fx.sink.creates[0].height, 480u);
    CHECK_EQ(fx.sink.creates[0].outWidth, 1280u);
    CHECK_EQ(fx.sink.creates[0].outHeight, 720u);
    CHECK_EQ(fx.sink.creates[0].createFlags, kNgxDlssFlagMVLowRes);

    for (uint32_t f = 0; f < 10; ++f) REQUIRE(ngx.Evaluate(d.ctx11.Get(), f, &err));
    CHECK_EQ(fx.state->createCalls, 1L);
    CHECK_EQ(fx.state->evalCalls, 10L);
    CHECK_EQ(ngx.Evaluates(), 10u);
    REQUIRE(fx.sink.evals.size() == 10);
    for (size_t i = 0; i < fx.sink.evals.size(); ++i) {
        const NgxEvaluateInputs& in = fx.sink.evals[i];
        CHECK(in.ctx == d.ctx11.Get());
        CHECK(in.createObserved);
        CHECK_EQ(in.createFlags, kNgxDlssFlagMVLowRes);
        const D3D11_TEXTURE2D_DESC dd = DescOf(in.depth);
        CHECK(dd.Format == DXGI_FORMAT_R32_TYPELESS && dd.Width == 853 && dd.Height == 480);
        CHECK((dd.BindFlags & D3D11_BIND_DEPTH_STENCIL) && (dd.BindFlags & D3D11_BIND_SHADER_RESOURCE));
        const D3D11_TEXTURE2D_DESC md = DescOf(in.mvec);
        CHECK(md.Format == DXGI_FORMAT_R16G16_FLOAT && md.Width == 853 && md.Height == 480);
        CHECK(md.BindFlags & D3D11_BIND_SHADER_RESOURCE);
        if (i + 1 < fx.sink.evals.size()) CHECK(in.mvec != fx.sink.evals[i + 1].mvec);
        if (i + 2 < fx.sink.evals.size()) CHECK(in.mvec == fx.sink.evals[i + 2].mvec);
        const testapp::Jitter j = testapp::HaltonJitter(static_cast<uint32_t>(i));
        CHECK(in.jitterX == j.x && in.jitterY == j.y);
        CHECK(in.mvScaleX == -853.0f && in.mvScaleY == -480.0f);
        CHECK_EQ(in.subrectW, 853u);
        CHECK_EQ(in.subrectH, 480u);
        CHECK_EQ(in.reset, i == 0);
    }
    const uint64_t firstKey = fx.sink.evals[0].featureKey;

    REQUIRE(ngx.CreateFeature(d.device11.Get(), d.ctx11.Get(), 1067, 600, 1600, 900, &err));
    REQUIRE(fx.sink.creates.size() == 2);
    CHECK_EQ(fx.sink.creates[1].outWidth, 1600u);
    REQUIRE(ngx.Evaluate(d.ctx11.Get(), 10, &err));
    REQUIRE(ngx.Evaluate(d.ctx11.Get(), 11, &err));
    REQUIRE(fx.sink.evals.size() == 12);
    CHECK(fx.sink.evals[10].featureKey != firstKey);
    CHECK(fx.sink.evals[10].reset && !fx.sink.evals[11].reset);
    CHECK_EQ(DescOf(fx.sink.evals[10].depth).Width, 1067u);
    CHECK(fx.sink.evals[10].mvScaleX == -1067.0f && fx.sink.evals[10].mvScaleY == -600.0f);
    CHECK_EQ(fx.sink.evals[10].subrectW, 1067u);

    ngx.Release();
    CHECK_EQ(fx.state->destroyedParams, 1L);
}