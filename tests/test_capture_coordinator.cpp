// CaptureCoordinator (spec 6.4 "Capture slots", 6.5, 6.6, 7 step 2) on real
// D3D11/D3D12 devices, without Streamline: the per-frame first-evaluate latch
// and markers, the copy into slot N mod 3 (without a shared-fence signal), the
// refusals (null resources, deferred context, another device, unsupported
// formats), the recreate rule, CreateFeature, the subrect and MV-scale
// fallbacks, and the camera latch at a Present without an evaluate.
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "camera_channel.h"
#include "camera_writer.h"
#include "capture_coordinator.h"
#include "capture_slots.h"
#include "fence_pair.h"
#include "gpu_test_devices.h"
#include "log.h"
#include "test_framework.h"

using Microsoft::WRL::ComPtr;
using namespace acdb;

namespace {

ComPtr<ID3D11Texture2D> Tex(ID3D11Device* device, UINT w, UINT h, DXGI_FORMAT format, UINT bind) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &tex))) return nullptr;
    return tex;
}

// CSP's DLSS inputs (spec 4): R32_TYPELESS depth, R16G16_FLOAT motion vectors.
struct CspSources {
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11Texture2D> mvec;
};

CspSources MakeSources(ID3D11Device* device, UINT w, UINT h) {
    CspSources s;
    s.depth = Tex(device, w, h, DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
    s.mvec = Tex(device, w, h, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
    return s;
}

// Everything a coordinator needs, on one set of test devices.
struct Rig {
    acdb_test::GpuTestDevices d;
    std::unique_ptr<FencePair> fences;
    std::unique_ptr<CaptureSlots> slots;
    acdb_test::TestCameraWriter writer;
    std::unique_ptr<CameraChannel> camera;
    std::unique_ptr<CaptureCoordinator> coord;
    int markers = 0;  // onFirstEvaluate calls

    bool Create() {
        if (!acdb_test::CreateGpuTestDevices(&d)) return false;
        std::string err;
        fences = FencePair::Create(d.device12.Get(), d.device11.Get(), &err);
        slots = CaptureSlots::Create(d.device11.Get(), d.device12.Get(), &err);
        if (!fences || !slots || !writer.Ok()) {
            std::printf("  rig: %s\n", err.c_str());
            return false;
        }
        camera = std::make_unique<CameraChannel>(writer.Name().c_str());
        if (!camera->Create(&err)) return false;
        CaptureCoordinator::Deps deps;
        deps.device11 = d.device11.Get();
        deps.slots = slots.get();
        deps.fences = fences.get();
        deps.camera = camera.get();
        deps.onFirstEvaluate = [this] { ++markers; };
        coord = std::make_unique<CaptureCoordinator>(deps);
        return true;
    }

    NgxEvaluateInputs Inputs(const CspSources& s, ID3D11DeviceContext* ctx = nullptr) const {
        NgxEvaluateInputs in;
        in.ctx = ctx ? ctx : d.ctx11.Get();
        in.depth = s.depth.Get();
        in.mvec = s.mvec.Get();
        in.jitterX = 0.25f;
        in.jitterY = -0.125f;
        D3D11_TEXTURE2D_DESC desc{};
        if (s.depth) s.depth->GetDesc(&desc);
        in.mvScaleX = -static_cast<float>(desc.Width);
        in.mvScaleY = -static_cast<float>(desc.Height);
        in.subrectW = desc.Width;
        in.subrectH = desc.Height;
        in.createFlags = 2;
        in.featureKey = 0x5000;
        in.createObserved = true;
        return in;
    }
};

bool Has(const std::string& s, const char* piece) {
    const bool ok = s.find(piece) != std::string::npos;
    if (!ok) std::printf("  '%s' does not contain '%s'\n", s.c_str(), piece);
    return ok;
}

}  // namespace

TEST(Coordinator_FirstEvaluateLatchesTheCameraAndEmitsMarkersOnce) {
    Rig r;
    if (!r.Create()) return;
    CspSources s = MakeSources(r.d.device11.Get(), 64, 36);
    REQUIRE(s.depth && s.mvec);

    r.writer.Write(1);
    r.coord->OnEvaluate(r.Inputs(s));
    CHECK_EQ(r.markers, 1);
    FrameCapture f = r.coord->EndFrame();
    CHECK_EQ(f.evaluates, 1u);
    CHECK(f.captured);
    CHECK(f.cameraResult == CameraChannel::ReadResult::Ok);
    CHECK(!f.cameraFresh);  // the first latch has nothing to compare with

    // Frame 2: the app wrote once; two evaluates latch once and emit one pair
    // of markers; the second evaluate wins for the resources and parameters.
    r.writer.Write(2);
    r.coord->OnEvaluate(r.Inputs(s));
    r.writer.Write(3);  // a write between the evaluates is not seen by this frame
    NgxEvaluateInputs second = r.Inputs(s);
    second.jitterX = 0.5f;
    r.coord->OnEvaluate(second);
    CHECK_EQ(r.markers, 2);
    f = r.coord->EndFrame();
    CHECK_EQ(f.evaluates, 2u);
    CHECK(f.captured);
    CHECK(f.cameraFresh);
    CHECK_EQ(f.camera.frame, 2u);
    CHECK(f.params.jitterX == 0.5f);

    // Frame 3: the write of 3 came after frame 2's latch, so frame 3 is fresh.
    r.coord->OnEvaluate(r.Inputs(s));
    f = r.coord->EndFrame();
    CHECK(f.cameraFresh);
    CHECK_EQ(f.camera.frame, 3u);
    // Frame 4 without a write: stale.
    r.coord->OnEvaluate(r.Inputs(s));
    f = r.coord->EndFrame();
    CHECK(!f.cameraFresh);
    CHECK_EQ(r.markers, 4);
}

TEST(Coordinator_CopiesIntoSlotNmod3AndLeavesTheSharedFenceAlone) {
    Rig r;
    if (!r.Create()) return;
    CspSources s = MakeSources(r.d.device11.Get(), 64, 36);
    REQUIRE(s.depth && s.mvec);
    uint32_t slots[6] = {};
    for (int i = 0; i < 6; ++i) {
        r.coord->OnEvaluate(r.Inputs(s));
        const FrameCapture f = r.coord->EndFrame();
        REQUIRE(f.captured);
        slots[i] = f.slot;
        CHECK(r.slots->HasTextures(f.slot));
        CHECK(r.slots->Depth12(f.slot) != nullptr);
        // Tag extents come from the subrect; the MV scale and flags pass through.
        CHECK_EQ(f.params.renderW, 64u);
        CHECK_EQ(f.params.renderH, 36u);
        CHECK(f.params.mvScaleX == -64.0f);
        CHECK_EQ(f.params.createFlags, 2u);
        CHECK(!f.mvScaleMissing);
    }
    CHECK(slots[0] != slots[1] && slots[1] != slots[2] && slots[0] != slots[2]);
    CHECK_EQ(slots[3], slots[0]);
    CHECK_EQ(slots[4], slots[1]);
    CHECK_EQ(slots[5], slots[2]);
    // The capture leaves the shared fence to the Present (review finding F1).
    r.d.ctx11->Flush();
    Sleep(50);
    CHECK_EQ(r.fences->Shared12()->GetCompletedValue(), 0ull);
}

// Review finding F1. The shared fence hands the shared back buffer from one
// API to the other: at Present, D3D11 waits for the value W the D3D12 queue
// signals when it is done with the previous frame (spec 7 steps 4 and 7). A
// capture during the next frame must not satisfy that wait by itself. Here
// the D3D12 queue is held before it signals W, CSP's next frame is captured,
// and D3D11's wait for W must still hold until the queue runs.
TEST(Coordinator_ACaptureDoesNotReleaseTheWaitForTheD3D12Queue) {
    Rig r;
    if (!r.Create()) return;
    CspSources s = MakeSources(r.d.device11.Get(), 64, 36);
    REQUIRE(s.depth && s.mvec);
    ComPtr<ID3D11DeviceContext4> ctx4;
    REQUIRE(SUCCEEDED(r.d.ctx11.As(&ctx4)));
    ComPtr<ID3D11Device5> device5;
    REQUIRE(SUCCEEDED(r.d.device11.As(&device5)));
    ComPtr<ID3D11Fence> marker;  // how far the D3D11 queue got
    REQUIRE(SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&marker))));
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    REQUIRE(SUCCEEDED(r.d.device12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))));
    ComPtr<ID3D12Fence> hold;
    ComPtr<ID3D12Fence> idle;
    REQUIRE(SUCCEEDED(r.d.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&hold))));
    REQUIRE(SUCCEEDED(r.d.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&idle))));
    // Releases the queue and waits for it whatever the checks say, so that the
    // devices can be torn down.
    struct Release {
        ID3D12CommandQueue* queue;
        ID3D12Fence* hold;
        ID3D12Fence* idle;
        ~Release() {
            hold->Signal(1);
            queue->Signal(idle, 1);
            for (int i = 0; i < 500 && idle->GetCompletedValue() < 1; ++i) Sleep(1);
        }
    } release{queue.Get(), hold.Get(), idle.Get()};

    // End of frame N on the D3D12 queue: held, then W.
    const uint64_t w = r.fences->Next();
    REQUIRE(SUCCEEDED(queue->Wait(hold.Get(), 1)));
    REQUIRE(SUCCEEDED(queue->Signal(r.fences->Shared12(), w)));
    // Frame N+1 on D3D11: CSP's evaluate is captured, then its Present waits for W.
    r.coord->OnEvaluate(r.Inputs(s));
    REQUIRE(SUCCEEDED(ctx4->Wait(r.fences->Shared11(), w)));
    REQUIRE(SUCCEEDED(ctx4->Signal(marker.Get(), 1)));
    r.d.ctx11->Flush();
    Sleep(200);
    const uint64_t whileHeld = marker->GetCompletedValue();
    const uint64_t sharedWhileHeld = r.fences->Shared12()->GetCompletedValue();
    std::printf("  while the D3D12 queue is held: D3D11 marker %llu, shared fence %llu (W %llu)\n",
                static_cast<unsigned long long>(whileHeld), static_cast<unsigned long long>(sharedWhileHeld),
                static_cast<unsigned long long>(w));
    CHECK(r.coord->EndFrame().captured);
    CHECK_EQ(whileHeld, 0ull);  // D3D11 still waits for the D3D12 queue
    CHECK(sharedWhileHeld < w);

    REQUIRE(SUCCEEDED(hold->Signal(1)));
    for (int i = 0; i < 5000 && marker->GetCompletedValue() < 1; ++i) Sleep(1);
    CHECK_EQ(marker->GetCompletedValue(), 1ull);
    CHECK(r.fences->Shared12()->GetCompletedValue() >= w);
}

TEST(Coordinator_NullResourcesGiveNoCaptureAndKeepTheSlot) {
    Rig r;
    if (!r.Create()) return;
    CspSources s = MakeSources(r.d.device11.Get(), 64, 36);
    REQUIRE(s.depth && s.mvec);
    for (int i = 0; i < 3; ++i) {  // fill all three slots
        r.coord->OnEvaluate(r.Inputs(s));
        REQUIRE(r.coord->EndFrame().captured);
    }
    NgxEvaluateInputs in = r.Inputs(s);
    in.depth = nullptr;
    r.coord->OnEvaluate(in);
    FrameCapture f = r.coord->EndFrame();
    CHECK_EQ(f.evaluates, 1u);  // CSP's evaluate: it counts and emits markers
    CHECK(!f.captured);
    CHECK(Has(f.reason, "no depth"));
    CHECK(r.slots->HasTextures(f.slot));  // not destroyed
    in = r.Inputs(s);
    in.mvec = nullptr;
    r.coord->OnEvaluate(in);
    f = r.coord->EndFrame();
    CHECK(!f.captured);
    CHECK(Has(f.reason, "no motion vectors"));
    CHECK(r.slots->HasTextures(f.slot));
    CHECK_EQ(r.markers, 5);
}

// Review finding F5: "the last evaluate wins" only for an evaluate that
// reaches the slot. A later counted evaluate of the same frame that is
// refused first (null depth or motion vectors, a slot still in use) keeps
// the frame's earlier capture and its parameters.
TEST(Coordinator_ARefusedLaterEvaluateKeepsTheFramesCapture) {
    Rig r;
    if (!r.Create()) return;
    CspSources s = MakeSources(r.d.device11.Get(), 64, 36);
    CspSources other = MakeSources(r.d.device11.Get(), 96, 54);
    REQUIRE(s.depth && s.mvec && other.depth && other.mvec);

    NgxEvaluateInputs noDepth = r.Inputs(s);
    noDepth.depth = nullptr;
    noDepth.jitterX = 0.5f;
    NgxEvaluateInputs noMvec = r.Inputs(s);
    noMvec.mvec = nullptr;
    for (int i = 0; i < 3; ++i) {  // every slot exists: the frames below are not forced off
        r.coord->OnEvaluate(r.Inputs(s));
        REQUIRE(r.coord->EndFrame().captured);
    }
    for (const NgxEvaluateInputs& refused : {noDepth, noMvec}) {
        r.coord->OnEvaluate(r.Inputs(s));
        r.coord->OnEvaluate(refused);
        const FrameCapture f = r.coord->EndFrame();
        CHECK_EQ(f.evaluates, 2u);
        CHECK(f.captured);
        CHECK(!f.forcedOff);
        CHECK(f.params.jitterX == 0.25f);  // the captured evaluate's
        CHECK_EQ(f.params.renderW, 64u);
    }

    // A later evaluate whose sources need a slot the GPU may still read.
    for (int i = 0; i < 3; ++i) {
        r.coord->OnEvaluate(r.Inputs(s));
        const FrameCapture f = r.coord->EndFrame();
        REQUIRE(f.captured);
        r.coord->NoteTagged(f.slot, 1000);  // progress never gets there
    }
    r.coord->OnEvaluate(r.Inputs(s));
    r.coord->OnEvaluate(r.Inputs(other));
    const FrameCapture f = r.coord->EndFrame();
    CHECK(f.captured);
    CHECK_EQ(f.params.renderW, 64u);
    CHECK(r.slots->Matches(f.slot, s.depth.Get(), s.mvec.Get()));
}

TEST(Coordinator_DeferredContextAndOtherDeviceAreNotCsp) {
    Rig r;
    if (!r.Create()) return;
    CspSources s = MakeSources(r.d.device11.Get(), 64, 36);
    REQUIRE(s.depth && s.mvec);
    ComPtr<ID3D11DeviceContext> deferred;
    REQUIRE(SUCCEEDED(r.d.device11->CreateDeferredContext(0, &deferred)));
    r.coord->OnEvaluate(r.Inputs(s, deferred.Get()));
    FrameCapture f = r.coord->EndFrame();
    CHECK_EQ(f.evaluates, 0u);
    CHECK(!f.captured);
    CHECK(Has(f.reason, "deferred context"));
    CHECK_EQ(r.markers, 0);

    acdb_test::GpuTestDevices other;
    REQUIRE(acdb_test::CreateGpuTestDevices(&other));
    CspSources os = MakeSources(other.device11.Get(), 64, 36);
    r.coord->OnEvaluate(r.Inputs(os, other.ctx11.Get()));
    f = r.coord->EndFrame();
    CHECK_EQ(f.evaluates, 0u);
    CHECK(!f.captured);
    CHECK(Has(f.reason, "not the game's D3D11 device"));
    CHECK_EQ(r.markers, 0);
}

TEST(Coordinator_UnsupportedSourceFormatGivesNoCapture) {
    Rig r;
    if (!r.Create()) return;
    CspSources s;
    s.depth = Tex(r.d.device11.Get(), 64, 36, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    s.mvec = Tex(r.d.device11.Get(), 64, 36, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    REQUIRE(s.depth && s.mvec);
    r.coord->OnEvaluate(r.Inputs(s));
    const FrameCapture f = r.coord->EndFrame();
    CHECK(!f.captured);
    CHECK(Has(f.reason, "no capture"));
    CHECK(!r.slots->HasTextures(f.slot));
}

TEST(Coordinator_ChangedSourcesWaitForTheGpuBeforeRecreating) {
    Rig r;
    if (!r.Create()) return;
    CspSources before = MakeSources(r.d.device11.Get(), 64, 36);
    CspSources after = MakeSources(r.d.device11.Get(), 96, 54);
    REQUIRE(before.depth && after.depth);
    uint32_t slot0 = 0;
    for (int i = 0; i < 3; ++i) {
        r.coord->OnEvaluate(r.Inputs(before));
        const FrameCapture f = r.coord->EndFrame();
        REQUIRE(f.captured);
        if (i == 0) slot0 = f.slot;
        // The D3D12 side "tagged" it with a value progress has not reached.
        r.coord->NoteTagged(f.slot, 1000 + static_cast<uint64_t>(i));
    }
    // Slot 0's turn with new sources: the GPU may still read it.
    r.coord->OnEvaluate(r.Inputs(after));
    FrameCapture f = r.coord->EndFrame();
    CHECK_EQ(f.slot, slot0);
    CHECK(!f.captured);
    CHECK(Has(f.reason, "still in use"));
    CHECK(r.slots->Matches(slot0, before.depth.Get(), before.mvec.Get()));  // untouched
    // Progress passes the tag value: the slot is recreated at its next turn,
    // and that frame has DLSS-G off.
    REQUIRE(SUCCEEDED(r.fences->Progress()->Signal(1002)));
    for (int i = 0; i < 2; ++i) {
        r.coord->OnEvaluate(r.Inputs(after));
        f = r.coord->EndFrame();
        CHECK(f.captured);
        CHECK(f.forcedOff);
        CHECK(Has(f.reason, "recreated"));
    }
    r.coord->OnEvaluate(r.Inputs(after));
    f = r.coord->EndFrame();
    CHECK_EQ(f.slot, slot0);
    CHECK(f.captured);
    CHECK(f.forcedOff);
    CHECK(r.slots->Matches(slot0, after.depth.Get(), after.mvec.Get()));
    // Steady again.
    r.coord->OnEvaluate(r.Inputs(after));
    f = r.coord->EndFrame();
    CHECK(f.captured);
    CHECK(!f.forcedOff);
}

TEST(Coordinator_CreateFeatureTurnsTheFrameOffAndRecreatesEverySlot) {
    Rig r;
    if (!r.Create()) return;
    CspSources s = MakeSources(r.d.device11.Get(), 64, 36);
    REQUIRE(s.depth && s.mvec);
    for (int i = 0; i < 3; ++i) {
        r.coord->OnEvaluate(r.Inputs(s));
        REQUIRE(r.coord->EndFrame().captured);
    }
    NgxCreateInfo info;
    info.featureId = 1;
    info.width = 64;
    info.height = 36;
    info.outWidth = 128;
    info.outHeight = 72;
    info.createFlags = 2;
    r.coord->OnCreateFeature(0x5000, info);
    // Every slot is recreated at its turn (progress is 0 and nothing was
    // tagged, so without a wait), and each of those frames is off.
    for (int i = 0; i < 3; ++i) {
        r.coord->OnEvaluate(r.Inputs(s));
        const FrameCapture f = r.coord->EndFrame();
        CHECK(f.captured);
        CHECK(f.forcedOff);
        if (i == 0) CHECK(Has(f.reason, "DLSS feature re-created"));
        CHECK_EQ(f.outWidth, 128u);
        CHECK_EQ(f.outHeight, 72u);
    }
    r.coord->OnEvaluate(r.Inputs(s));
    CHECK(!r.coord->EndFrame().forcedOff);
}

TEST(Coordinator_SubrectZeroFallsBackAndMvScaleZeroIsFlagged) {
    Rig r;
    if (!r.Create()) return;
    CspSources s = MakeSources(r.d.device11.Get(), 64, 36);
    REQUIRE(s.depth && s.mvec);
    // Without a create record: the depth texture's size.
    NgxEvaluateInputs in = r.Inputs(s);
    in.subrectW = 0;
    in.subrectH = 0;
    in.featureKey = 0x7000;
    in.createObserved = false;
    r.coord->OnEvaluate(in);
    FrameCapture f = r.coord->EndFrame();
    CHECK(f.captured);
    CHECK_EQ(f.params.renderW, 64u);
    CHECK_EQ(f.params.renderH, 36u);
    // With a create record: its render size.
    NgxCreateInfo info;
    info.featureId = 1;
    info.width = 60;
    info.height = 30;
    r.coord->OnCreateFeature(0x7000, info);
    r.coord->OnEvaluate(in);
    f = r.coord->EndFrame();
    CHECK_EQ(f.params.renderW, 60u);
    CHECK_EQ(f.params.renderH, 30u);
    // A missing MV scale is flagged, the capture itself still happens.
    in = r.Inputs(s);
    in.mvScaleX = 0.0f;
    r.coord->OnEvaluate(in);
    f = r.coord->EndFrame();
    CHECK(f.captured);
    CHECK(f.mvScaleMissing);
}

TEST(Coordinator_ResetIsOredAcrossTheFramesEvaluates) {
    Rig r;
    if (!r.Create()) return;
    CspSources s = MakeSources(r.d.device11.Get(), 64, 36);
    REQUIRE(s.depth && s.mvec);
    NgxEvaluateInputs in = r.Inputs(s);
    in.reset = true;
    r.coord->OnEvaluate(in);
    in.reset = false;
    r.coord->OnEvaluate(in);
    CHECK(r.coord->EndFrame().params.ngxReset);
    r.coord->OnEvaluate(in);
    CHECK(!r.coord->EndFrame().params.ngxReset);
}

TEST(Coordinator_PresentWithoutEvaluateLatchesTheCamera) {
    Rig r;
    if (!r.Create()) return;
    CspSources s = MakeSources(r.d.device11.Get(), 64, 36);
    REQUIRE(s.depth && s.mvec);
    r.writer.Write(1);
    r.coord->OnEvaluate(r.Inputs(s));
    r.coord->EndFrame();
    // A frame without an evaluate in which the app wrote.
    r.writer.Write(2);
    FrameCapture f = r.coord->EndFrame();
    CHECK_EQ(f.evaluates, 0u);
    CHECK(!f.captured);
    CHECK_EQ(r.markers, 1);
    // The next frame has an evaluate but no new write: not fresh, because
    // the write belonged to the frame before.
    r.coord->OnEvaluate(r.Inputs(s));
    f = r.coord->EndFrame();
    CHECK(!f.cameraFresh);
}

TEST(Coordinator_ResetCameraMakesTheNextLatchStale) {
    Rig r;
    if (!r.Create()) return;
    CspSources s = MakeSources(r.d.device11.Get(), 64, 36);
    REQUIRE(s.depth && s.mvec);
    r.writer.Write(1);
    r.coord->OnEvaluate(r.Inputs(s));
    r.coord->EndFrame();
    r.writer.Write(2);
    r.coord->ResetCamera();
    r.coord->OnEvaluate(r.Inputs(s));
    CHECK(!r.coord->EndFrame().cameraFresh);
    r.writer.Write(3);
    r.coord->OnEvaluate(r.Inputs(s));
    CHECK(r.coord->EndFrame().cameraFresh);
}

TEST(Coordinator_MissingCameraIsReported) {
    Rig r;
    if (!r.Create()) return;
    CspSources s = MakeSources(r.d.device11.Get(), 64, 36);
    REQUIRE(s.depth && s.mvec);
    r.coord->OnEvaluate(r.Inputs(s));  // nothing written yet
    FrameCapture f = r.coord->EndFrame();
    CHECK(f.captured);
    CHECK(f.cameraResult == CameraChannel::ReadResult::NotWritten);
    CHECK(!f.cameraFresh);
}
