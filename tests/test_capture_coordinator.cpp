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
#include "temp_dir.h"
#include "test_framework.h"
#include "triple_diag.h"

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
    TripleDiagLog* tripleDiag = nullptr;  // set before Create

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
        deps.tripleDiag = tripleDiag;
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

bool Has(const std::string& s, const std::string& piece) { return Has(s, piece.c_str()); }

size_t Count(const std::string& s, const char* piece) {
    size_t n = 0;
    for (size_t pos = s.find(piece); pos != std::string::npos; pos = s.find(piece, pos + 1)) ++n;
    return n;
}

std::string Ptr(const void* p) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%p", p);
    return buf;
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

// Diagnostics (GitHub issue 10): every evaluate the coordinator is handed
// counts as seen, also one it ignores for its context or its device, so that
// the stats line can count the Presents that had no DLSS evaluate at all.
TEST(Coordinator_EveryEvaluateCountsAsSeenAlsoAnIgnoredOne) {
    Rig r;
    if (!r.Create()) return;
    CspSources s = MakeSources(r.d.device11.Get(), 64, 36);
    REQUIRE(s.depth && s.mvec);
    FrameCapture f = r.coord->EndFrame();
    CHECK_EQ(f.allEvaluates, 0u);
    r.coord->OnEvaluate(r.Inputs(s));
    r.coord->OnEvaluate(r.Inputs(s));
    f = r.coord->EndFrame();
    CHECK_EQ(f.evaluates, 2u);
    CHECK_EQ(f.allEvaluates, 2u);

    ComPtr<ID3D11DeviceContext> deferred;
    REQUIRE(SUCCEEDED(r.d.device11->CreateDeferredContext(0, &deferred)));
    r.coord->OnEvaluate(r.Inputs(s, deferred.Get()));
    acdb_test::GpuTestDevices other;
    REQUIRE(acdb_test::CreateGpuTestDevices(&other));
    CspSources os = MakeSources(other.device11.Get(), 64, 36);
    r.coord->OnEvaluate(r.Inputs(os, other.ctx11.Get()));
    f = r.coord->EndFrame();
    CHECK_EQ(f.evaluates, 0u);
    CHECK_EQ(f.allEvaluates, 2u);
    CHECK_EQ(r.coord->EndFrame().allEvaluates, 0u);
}

// Diagnostics (GitHub issue 10): the first evaluate whose device is not the
// game's logs, once, what the two devices are, whose immediate context the
// evaluate's context is, and which device owns its depth and motion vectors.
TEST(Coordinator_FirstEvaluateOnAnotherDeviceDescribesBothDevicesOnce) {
    Rig r;
    if (!r.Create()) return;
    acdb_test::GpuTestDevices other;
    REQUIRE(acdb_test::CreateGpuTestDevices(&other));
    CspSources os = MakeSources(other.device11.Get(), 64, 36);
    REQUIRE(os.depth && os.mvec);
    acdb_test::TempDir dir(L"coordinator_mismatch");
    const std::wstring logPath = dir.Str() + L"\\bridge.log";
    REQUIRE(LogOpen(logPath, LogLevel::Info));
    for (int i = 0; i < 2; ++i) {
        r.coord->OnEvaluate(r.Inputs(os, other.ctx11.Get()));
        CHECK(!r.coord->EndFrame().captured);
    }
    LogClose();
    const std::string log = acdb_test::ReadAll(logPath);
    CHECK_EQ(Count(log, " INFO diag: first DLSS evaluate on another device: ctx "), static_cast<size_t>(1));
    CHECK(Has(log, "ctx " + Ptr(other.ctx11.Get()) + " (immediate, vtable "));
    CHECK(Has(log, "the evaluate's device: " + Ptr(other.device11.Get()) + " (identity "));
    CHECK(Has(log, "the game's device: " + Ptr(r.d.device11.Get()) + " (identity "));
    CHECK(Has(log, "; its immediate context " + Ptr(other.ctx11.Get()) + " (this ctx); ReShade unwrap 0x"));
    CHECK(Has(log, "; its immediate context " + Ptr(r.d.ctx11.Get()) + " (not this ctx); ReShade unwrap 0x"));
    CHECK(Has(log, "depth " + Ptr(os.depth.Get()) + " (device " + Ptr(other.device11.Get()) +
                       "): the game's device no, the evaluate's device yes"));
    CHECK(Has(log, "motion vectors " + Ptr(os.mvec.Get()) + " (device " + Ptr(other.device11.Get()) +
                       "): the game's device no, the evaluate's device yes"));
    // Whether it is a pass-through chain's device; the factory hook tests'
    // released devices may share its address, so only the line is checked.
    CHECK(Has(log, "diag:   the evaluate's device is "));
    if (log.find(" diag: first DLSS evaluate") == std::string::npos) std::printf("  log:\n%s\n", log.c_str());
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

// The triple-screen diagnostics see every counted create and evaluate, the
// evaluate with its textures and its create, whatever the capture makes of it.
TEST(Coordinator_ReportsCreatesAndEvaluatesToTheTripleDiagnostics) {
    std::vector<std::string> lines;
    TripleDiagLog diag([&lines](const std::string& l) { lines.push_back(l); }, 1, 1);
    Rig r;
    r.tripleDiag = &diag;
    if (!r.Create()) return;
    CspSources s = MakeSources(r.d.device11.Get(), 64, 36);
    REQUIRE(s.depth && s.mvec);
    NgxCreateInfo info;
    info.featureId = 1;
    info.width = 64;
    info.height = 36;
    info.outWidth = 128;
    info.outHeight = 72;
    info.createFlags = 2;
    r.coord->OnCreateFeature(0x5000, info);
    r.coord->OnEvaluate(r.Inputs(s));
    NgxEvaluateInputs nulls = r.Inputs(CspSources());
    r.coord->OnEvaluate(nulls);
    // Review finding B-2: an evaluate on another device is reported as such,
    // although the coordinator ignores it.
    acdb_test::GpuTestDevices other;
    REQUIRE(acdb_test::CreateGpuTestDevices(&other));
    CspSources os = MakeSources(other.device11.Get(), 64, 36);
    REQUIRE(os.depth && os.mvec);
    r.coord->OnEvaluate(r.Inputs(os, other.ctx11.Get()));
    // Review finding B-5: an evaluate the hook did not count.
    NgxUncountedEvaluate deferred;
    deferred.featureKey = 0x5000;
    deferred.deferred = true;
    r.coord->OnUncountedEvaluate(deferred);
    r.coord->EndFrame();
    diag.EndFrame(128, 72, 28);
    std::string all;
    for (const std::string& l : lines) all += l + "\n";
    std::printf("%s", all.c_str());
    CHECK(Has(all, "triple-diag: DLSS feature 0x5000 created: render 64x36, output 128x72"));
    CHECK(Has(all, "triple-diag: frame 1 evaluate 1: feature 0x5000; context " + Ptr(r.d.ctx11.Get()) +
                       " immediate, on the game's device; create render 64x36 output 128x72 flags 0x2"));
    CHECK(Has(all, "Depth " + Ptr(s.depth.Get()) + " 64x36 format 39"));         // DXGI_FORMAT_R32_TYPELESS
    CHECK(Has(all, "MotionVectors " + Ptr(s.mvec.Get()) + " 64x36 format 34"));  // DXGI_FORMAT_R16G16_FLOAT
    CHECK(Has(all, "frame 1 evaluate 2: feature 0x5000"));
    CHECK(Has(all, "Depth none; MotionVectors none"));
    CHECK(Has(all, "frame 1 evaluate 3: feature 0x5000; context " + Ptr(other.ctx11.Get()) +
                       " immediate, not on the game's device; "));
    CHECK(Has(all, "Depth " + Ptr(os.depth.Get()) + " 64x36 format 39"));
    CHECK(Has(all, "triple-diag: frame 1: 3 DLSS evaluates; not counted: 1 on a deferred context; swap chain 128x72 "
                   "format 28"));
}

// Review finding B-1: with the triple-screen diagnostics on, the coordinator
// captures nothing. A frame with evaluates of two sizes would otherwise
// recreate its capture slot at every turn. The markers and the camera latch
// of the first evaluate stay, and the diagnostics still see every evaluate.
TEST(Coordinator_TripleDiagnosticsCaptureNothing) {
    std::vector<std::string> lines;
    TripleDiagLog diag([&lines](const std::string& l) { lines.push_back(l); }, 3, 1);
    Rig r;
    r.tripleDiag = &diag;
    if (!r.Create()) return;
    CspSources narrow = MakeSources(r.d.device11.Get(), 64, 36);
    CspSources wide = MakeSources(r.d.device11.Get(), 96, 54);
    REQUIRE(narrow.depth && narrow.mvec && wide.depth && wide.mvec);
    acdb_test::TempDir dir(L"coordinator_triple_diag");
    const std::wstring logPath = dir.Str() + L"\\bridge.log";
    REQUIRE(LogOpen(logPath, LogLevel::Info));
    constexpr int kFrames = 6;
    for (int i = 0; i < kFrames; ++i) {
        r.coord->OnEvaluate(r.Inputs(narrow));
        r.coord->OnEvaluate(r.Inputs(wide));
        const FrameCapture f = r.coord->EndFrame();
        CHECK(!f.captured);
        CHECK(!f.forcedOff);
        CHECK(Has(f.reason, "no capture: triple-screen diagnostics"));
        diag.EndFrame(128, 72, 28);
    }
    LogClose();
    for (uint32_t slot = 0; slot < CaptureSlots::kSlots; ++slot) CHECK(!r.slots->HasTextures(slot));
    CHECK_EQ(r.markers, kFrames);
    const std::string log = acdb_test::ReadAll(logPath);
    // Neither the coordinator's "capture: ..." lines nor the slots' "capture
    // slot N: ..." lines of a recreate.
    CHECK_EQ(Count(log, " capture"), static_cast<size_t>(0));
    if (Count(log, " capture") != 0) std::printf("  log:\n%s\n", log.c_str());
    std::string all;
    for (const std::string& l : lines) all += l + "\n";
    CHECK(Has(all, "triple-diag: frame 3: 2 DLSS evaluates"));

    // Without the diagnostics the same frames capture.
    Rig plain;
    if (!plain.Create()) return;
    CspSources ps = MakeSources(plain.d.device11.Get(), 64, 36);
    CspSources pl = MakeSources(plain.d.device11.Get(), 96, 54);
    REQUIRE(ps.depth && ps.mvec && pl.depth && pl.mvec);
    plain.coord->OnEvaluate(plain.Inputs(ps));
    plain.coord->OnEvaluate(plain.Inputs(pl));
    const FrameCapture f = plain.coord->EndFrame();
    CHECK(f.captured);
    CHECK(plain.slots->HasTextures(f.slot));
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

// Review finding F4 (and phase-1 item S5): when CSP stops calling DLSS, the
// source-view cache must not keep CSP's depth texture alive for the rest of
// the session. After 60 frames without a counted evaluate it is released,
// and the next evaluate captures as before.
TEST(Coordinator_ReleasesCspsDepthTextureWhenDlssStops) {
    Rig r;
    if (!r.Create()) return;
    CspSources s = MakeSources(r.d.device11.Get(), 64, 36);
    REQUIRE(s.depth && s.mvec);
    const auto refs = [](IUnknown* p) {
        p->AddRef();
        return p->Release();
    };
    const ULONG alone = refs(s.depth.Get());
    r.coord->OnEvaluate(r.Inputs(s));
    REQUIRE(r.coord->EndFrame().captured);
    CHECK_EQ(r.slots->CachedSourceViews(), 1u);
    CHECK(refs(s.depth.Get()) > alone);
    for (int i = 0; i < 59; ++i) CHECK(!r.coord->EndFrame().captured);
    CHECK_EQ(r.slots->CachedSourceViews(), 1u);  // a short gap keeps the view
    CHECK(!r.coord->EndFrame().captured);          // the 60th frame without an evaluate
    CHECK_EQ(r.slots->CachedSourceViews(), 0u);
    CHECK_EQ(refs(s.depth.Get()), alone);
    r.coord->OnEvaluate(r.Inputs(s));
    CHECK(r.coord->EndFrame().captured);
    CHECK_EQ(r.slots->CachedSourceViews(), 1u);
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
