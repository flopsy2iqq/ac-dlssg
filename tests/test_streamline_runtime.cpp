// StreamlineRuntime with the real Streamline 2.14.1 DLLs, which CMakeLists.txt
// copies to <test exe dir>\sl (the layout of <game>\ac-dlssg\sl). Streamline
// allows one slInit/slShutdown lifetime per process, so every test runs in a
// child process; each one ends with slShutdown before any D3D12/DXGI object
// is released (spec 6.3) and must exit cleanly.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include "camera_channel.h"
#include "camera_layout.h"
#include "camera_writer.h"
#include "child_process.h"
#include "config.h"
#include "d3d12_presenter.h"
#include "factory_hook.h"
#include "frame_constants.h"
#include "gpu_info.h"
#include "gpu_test_devices.h"
#include "log.h"
#include "ngx_hook.h"
#include "pcl_sequencer.h"
#include "proxy_swapchain.h"
#include "streamline_runtime.h"
#include "temp_dir.h"
#include "test_framework.h"

#ifdef ACDB_SL_BIN_DIR

using Microsoft::WRL::ComPtr;
using namespace acdb;

namespace {

constexpr DWORD kChildTimeoutMs = 30000;
constexpr UINT kNvidiaVendorId = 0x10DE;

std::wstring ExeDir() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return std::filesystem::path(exe).parent_path().wstring();
}

std::wstring SlDir() { return ExeDir() + L"\\sl"; }

// Streamline's own log file and NGX data; kept after the run for inspection.
std::wstring SlLogDir() {
    const std::wstring tmp = ExeDir() + L"\\test_tmp";
    CreateDirectoryW(tmp.c_str(), nullptr);
    return tmp + L"\\sl-logs";
}

ULONG RefCount(IUnknown* obj) {
    obj->AddRef();
    return obj->Release();
}

// The RTX 3080 of the reference machine has no DLSS-G without the spoof, so
// presenters that must exist there keep the M2 behaviour (spec criterion 5
// otherwise passes the chain through).
Config ProxyWithoutFg() {
    Config c;
    c.proxy_without_fg = true;
    return c;
}

// Our log for the lifetime of the object, in a scratch directory.
class LogCapture {
public:
    explicit LogCapture(const wchar_t* tag) : dir_(tag), path_(dir_.Path() / L"bridge.log") {
        LogOpen(path_.wstring(), LogLevel::Debug);
    }
    ~LogCapture() { LogClose(); }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    std::vector<std::string> Lines() const {
        std::vector<std::string> out;
        const std::string all = acdb_test::ReadAll(path_);
        size_t start = 0;
        while (start < all.size()) {
            size_t end = all.find('\n', start);
            if (end == std::string::npos) end = all.size();
            out.push_back(all.substr(start, end - start));
            start = end + 1;
        }
        return out;
    }

    // Lines that contain every one of the given pieces.
    std::vector<std::string> Matching(std::initializer_list<const char*> pieces) const {
        std::vector<std::string> out;
        for (const auto& line : Lines()) {
            bool all = true;
            for (const char* p : pieces) all = all && line.find(p) != std::string::npos;
            if (all) out.push_back(line);
        }
        return out;
    }

private:
    acdb_test::TempDir dir_;
    std::filesystem::path path_;
};

void Print(const char* title, const std::vector<std::string>& lines) {
    std::printf("  %s: %zu\n", title, lines.size());
    for (const auto& l : lines) std::printf("    %s\n", l.c_str());
}

// A hidden, never shown top-level window for the Streamline chain.
class HiddenWindow {
public:
    HiddenWindow(int width, int height) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"acdbSlTest";
        RegisterClassExW(&wc);
        RECT r{0, 0, width, height};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"acdb sl test", WS_OVERLAPPEDWINDOW, 0, 0, r.right - r.left,
                                r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    }
    ~HiddenWindow() {
        if (hwnd_) DestroyWindow(hwnd_);
        UnregisterClassW(L"acdbSlTest", GetModuleHandleW(nullptr));
    }
    HiddenWindow(const HiddenWindow&) = delete;
    HiddenWindow& operator=(const HiddenWindow&) = delete;
    HWND Get() const { return hwnd_; }
    void Pump() const {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

private:
    HWND hwnd_ = nullptr;
};

// Everything the Streamline path of D3D12Presenter creates (spec 6.4 steps
// 1-7), in that order. Release() follows spec 6.3: GPU idle, slShutdown,
// then the D3D12/DXGI objects.
struct SlSession {
    static constexpr UINT kWidth = 320;
    static constexpr UINT kHeight = 180;
    static constexpr UINT kBuffers = 3;

    acdb_test::GpuTestDevices dev;
    LUID luid{};
    ComPtr<ID3D12Device> native;    // what slSetD3DDevice gets
    ComPtr<ID3D12Device> upgraded;  // Streamline's device proxy
    ComPtr<ID3D12CommandQueue> queue;  // created from the proxy: a queue proxy
    ComPtr<IDXGIFactory4> nativeFactory;
    ComPtr<IDXGIFactory4> factory;  // Streamline's factory proxy
    ComPtr<IDXGISwapChain4> chain;
    bool tearing = false;
    std::unique_ptr<HiddenWindow> window;

    // Returns false (with a printed reason) when the machine cannot run it.
    bool Create(StreamlineRuntime& rt, bool* skipped) {
        *skipped = false;
        if (!acdb_test::CreateGpuTestDevices(&dev)) {
            *skipped = true;
            return false;
        }
        DXGI_ADAPTER_DESC1 desc{};
        dev.adapter->GetDesc1(&desc);
        if (dev.warp || desc.VendorId != kNvidiaVendorId) {
            std::printf("  SKIP: first hardware adapter is not NVIDIA (vendor 0x%04X)\n", desc.VendorId);
            *skipped = true;
            return false;
        }
        luid = desc.AdapterLuid;
        std::printf("  adapter: %ls\n", desc.Description);
        native = dev.device12;

        std::string err;
        if (!rt.SetDevice(native.Get(), &err)) {
            std::printf("  SetDevice failed: %s\n", err.c_str());
            return false;
        }
        std::string why;
        const bool fg = rt.DlssgSupported(luid, &why);
        std::printf("  DlssgSupported: %s (%s)\n", fg ? "yes" : "no", why.c_str());
        CHECK(!why.empty());

        // Upgrade leaves our reference to the native device alone and hands
        // us one reference to the proxy.
        void* p = native.Get();
        if (!rt.Upgrade(&p) || p == native.Get()) {
            std::printf("  Upgrade(device) failed\n");
            return false;
        }
        upgraded.Attach(static_cast<ID3D12Device*>(p));

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        HRESULT hr = upgraded->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
        if (FAILED(hr)) {
            std::printf("  CreateCommandQueue on the proxy failed: 0x%08lX\n", static_cast<unsigned long>(hr));
            return false;
        }

        hr = dev.adapter->GetParent(IID_PPV_ARGS(&nativeFactory));
        if (FAILED(hr)) return false;
        p = nativeFactory.Get();
        if (!rt.Upgrade(&p) || p == nativeFactory.Get()) {
            std::printf("  Upgrade(factory) failed\n");
            return false;
        }
        factory.Attach(static_cast<IDXGIFactory4*>(p));

        ComPtr<IDXGIFactory5> f5;
        BOOL allow = FALSE;
        if (SUCCEEDED(nativeFactory.As(&f5)) &&
            SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow)))) {
            tearing = allow != FALSE;
        }

        window = std::make_unique<HiddenWindow>(kWidth, kHeight);
        if (!window->Get()) return false;
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = kWidth;
        sd.Height = kHeight;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kBuffers;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        sd.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;  // never the waitable flag
        ComPtr<IDXGISwapChain1> chain1;
        hr = factory->CreateSwapChainForHwnd(queue.Get(), window->Get(), &sd, nullptr, nullptr, &chain1);
        if (FAILED(hr)) {
            std::printf("  CreateSwapChainForHwnd on the proxy factory failed: 0x%08lX\n",
                        static_cast<unsigned long>(hr));
            return false;
        }
        hr = chain1.As(&chain);
        return SUCCEEDED(hr);
    }

    // Spec 6.3 order: the chain's frames are done, then slShutdown, then the
    // releases (chain, queue, factories, devices), then the window.
    void Release(StreamlineRuntime& rt) {
        rt.Shutdown();
        chain.Reset();
        queue.Reset();
        factory.Reset();
        nativeFactory.Reset();
        upgraded.Reset();
        native.Reset();
        dev = acdb_test::GpuTestDevices();
        window.reset();
    }
};

}  // namespace

// --- Init, the log callback, the loaded modules and Shutdown -----------------

TEST(SlRuntime_RealDllsInitLogAndShutDown) {
    CHECK_EQ(acdb_test::RunChildTest("Child_SlRuntime_RealDllsInitLogAndShutDown", kChildTimeoutMs), 0);
}

TEST(Child_SlRuntime_RealDllsInitLogAndShutDown) {
    LogCapture log(L"sl_real_init");
    StreamlineRuntime& rt = StreamlineRuntime::Get();
    std::string err;
    const bool ok = rt.Init(SlDir(), SlLogDir(), &err);
    if (!ok) std::printf("  Init error: %s\n", err.c_str());
    REQUIRE(ok);
    CHECK(err.empty());
    CHECK(rt.Initialized());
    CHECK(!rt.IsShutDown());
    CHECK(GetModuleHandleW(L"sl.interposer.dll") != nullptr);

    // Streamline's lines reach our log through the callback.
    const auto slLines = log.Matching({" sl: "});
    std::printf("  Streamline log lines after slInit: %zu\n", slLines.size());
    CHECK(!slLines.empty());
    CHECK_EQ(rt.ErrorsLogged(), 0u);

    // Every Streamline module comes from the plugin directory.
    CHECK_EQ(rt.LogLoadedModules(), 0);
    Print("modules", log.Matching({"Streamline module"}));

    rt.Shutdown();
    CHECK(rt.IsShutDown());
    CHECK(rt.Initialized());
    CHECK(!log.Matching({"slShutdown eOk"}).empty());
    // Closed for the rest of the process: Init refuses, nothing reaches Streamline.
    std::string again;
    CHECK(!rt.Init(SlDir(), SlLogDir(), &again));
    std::printf("  second Init: %s\n", again.c_str());
    CHECK(again.find("shut down") != std::string::npos);
    CHECK(rt.NewFrameToken(1) == nullptr);
    CHECK(!rt.ReflexLowLatencyAvailable());
    rt.Shutdown();  // no second slShutdown
    CHECK_EQ(log.Matching({"slShutdown"}).size(), 1u);

    CHECK_EQ(rt.ErrorsLogged(), 0u);
    std::printf("  Streamline warnings: %u\n", rt.WarningsLogged());
    Print("warnings and errors in our log", log.Matching({" WARN "}));
    Print("errors in our log", log.Matching({" ERROR "}));
    CHECK(log.Matching({" ERROR "}).empty());
}

// --- Device, feature functions, proxies and IsProxied ------------------------

TEST(SlRuntime_RealDllsDeviceProxiesAndRefCounts) {
    CHECK_EQ(acdb_test::RunChildTest("Child_SlRuntime_RealDllsDeviceProxiesAndRefCounts", kChildTimeoutMs), 0);
}

TEST(Child_SlRuntime_RealDllsDeviceProxiesAndRefCounts) {
    LogCapture log(L"sl_real_device");
    StreamlineRuntime& rt = StreamlineRuntime::Get();
    std::string err;
    REQUIRE(rt.Init(SlDir(), SlLogDir(), &err));

    SlSession s;
    bool skipped = false;
    const bool created = s.Create(rt, &skipped);
    if (skipped) {
        s.Release(rt);
        return;
    }
    REQUIRE(created);
    CHECK(!log.Matching({"Streamline: device set, Reflex and PCL functions resolved"}).empty());
    const auto dlssg = log.Matching({"Streamline: DLSS-G functions"});
    Print("DLSS-G functions", dlssg);
    CHECK_EQ(dlssg.size(), 1u);
    CHECK(rt.ReflexLowLatencyAvailable());
    CHECK(rt.EnableReflexLowLatency());

    // What slGetNativeInterface does (Streamline 2.14.1, production build):
    // for a proxy it returns the base object with one reference added; for
    // anything else it returns the same pointer, also with one reference added.
    auto* getNative = reinterpret_cast<PFun_slGetNativeInterface*>(
        GetProcAddress(GetModuleHandleW(L"sl.interposer.dll"), "slGetNativeInterface"));
    REQUIRE(getNative != nullptr);
    {
        const ULONG before = RefCount(s.native.Get());
        void* out = nullptr;
        CHECK(getNative(s.native.Get(), &out) == sl::Result::eOk);
        CHECK(out == s.native.Get());
        CHECK_EQ(RefCount(s.native.Get()), before + 1);
        if (out) static_cast<IUnknown*>(out)->Release();
        CHECK_EQ(RefCount(s.native.Get()), before);
    }
    ComPtr<IDXGISwapChain> nativeChain;
    {
        void* out = nullptr;
        CHECK(getNative(s.chain.Get(), &out) == sl::Result::eOk);
        REQUIRE(out != nullptr);
        CHECK(out != static_cast<void*>(s.chain.Get()));
        nativeChain.Attach(static_cast<IDXGISwapChain*>(out));  // takes the added reference
    }

    // IsProxied: right answer, and no reference gained or lost on the object
    // or on the native object behind a proxy.
    struct Case {
        const char* name;
        IUnknown* obj;
        IUnknown* behind;  // the native object behind a proxy, or nullptr
        bool proxied;
    } cases[] = {
        {"native device", s.native.Get(), nullptr, false},
        {"native factory", s.nativeFactory.Get(), nullptr, false},
        {"upgraded device", s.upgraded.Get(), s.native.Get(), true},
        {"upgraded factory", s.factory.Get(), s.nativeFactory.Get(), true},
        {"queue from the upgraded device", s.queue.Get(), nullptr, true},
        {"chain from the upgraded factory", s.chain.Get(), nativeChain.Get(), true},
    };
    for (const auto& c : cases) {
        const ULONG objBefore = RefCount(c.obj);
        const ULONG behindBefore = c.behind ? RefCount(c.behind) : 0;
        const bool proxied = rt.IsProxied(c.obj);
        const ULONG objAfter = RefCount(c.obj);
        const ULONG behindAfter = c.behind ? RefCount(c.behind) : 0;
        std::printf("  IsProxied(%s) = %d, refs %lu -> %lu, native refs %lu -> %lu\n", c.name, proxied,
                    static_cast<unsigned long>(objBefore), static_cast<unsigned long>(objAfter),
                    static_cast<unsigned long>(behindBefore), static_cast<unsigned long>(behindAfter));
        CHECK_EQ(proxied, c.proxied);
        CHECK_EQ(objAfter, objBefore);
        CHECK_EQ(behindAfter, behindBefore);
    }
    CHECK(!rt.IsProxied(nullptr));
    nativeChain.Reset();

    s.Release(rt);
    CHECK(rt.IsShutDown());
    CHECK_EQ(rt.ErrorsLogged(), 0u);
    std::printf("  Streamline warnings: %u\n", rt.WarningsLogged());
    Print("warnings in our log", log.Matching({" WARN "}));
    Print("errors in our log", log.Matching({" ERROR "}));
    CHECK(log.Matching({" ERROR "}).empty());
}

// --- 120 frames with Reflex and the PCL marker sequence -------------------------

TEST(SlRuntime_RealDllsFrameLoopWithReflexAndMarkers) {
    CHECK_EQ(acdb_test::RunChildTest("Child_SlRuntime_RealDllsFrameLoop", kChildTimeoutMs), 0);
}

TEST(Child_SlRuntime_RealDllsFrameLoop) {
    LogCapture log(L"sl_real_frames");
    StreamlineRuntime& rt = StreamlineRuntime::Get();
    std::string err;
    REQUIRE(rt.Init(SlDir(), SlLogDir(), &err));

    SlSession s;
    bool skipped = false;
    const bool created = s.Create(rt, &skipped);
    if (skipped) {
        s.Release(rt);
        return;
    }
    REQUIRE(created);
    REQUIRE(rt.EnableReflexLowLatency());
    CHECK(rt.IsProxied(s.chain.Get()));

    // Back buffers through the proxy chain; everything else on the native device.
    ComPtr<ID3D12Resource> buffers[SlSession::kBuffers];
    for (UINT i = 0; i < SlSession::kBuffers; ++i) REQUIRE(SUCCEEDED(s.chain->GetBuffer(i, IID_PPV_ARGS(&buffers[i]))));
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = SlSession::kBuffers;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    REQUIRE(SUCCEEDED(s.native->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap))));
    const UINT rtvStep = s.native->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ComPtr<ID3D12CommandAllocator> allocators[SlSession::kBuffers];
    ComPtr<ID3D12GraphicsCommandList> lists[SlSession::kBuffers];
    for (UINT i = 0; i < SlSession::kBuffers; ++i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(i) * rtvStep;
        s.native->CreateRenderTargetView(buffers[i].Get(), nullptr, h);
        REQUIRE(SUCCEEDED(
            s.native->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i]))));
        REQUIRE(SUCCEEDED(s.native->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[i].Get(), nullptr,
                                                      IID_PPV_ARGS(&lists[i]))));
        lists[i]->Close();
    }
    ComPtr<ID3D12Fence> fence;
    REQUIRE(SUCCEEDED(s.native->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    REQUIRE(event != nullptr);
    UINT64 fenceValue = 0;
    UINT64 bufferValue[SlSession::kBuffers] = {};
    auto waitFor = [&](UINT64 value) {
        if (fence->GetCompletedValue() >= value) return true;
        fence->SetEventOnCompletion(value, event);
        return WaitForSingleObject(event, 2000) == WAIT_OBJECT_0;
    };

    constexpr uint32_t kFrames = 120;
    PclSequencer seq;
    uint32_t markers = 0;
    uint32_t occluded = 0;
    uint32_t presentFailures = 0;
    bool indexSeen[SlSession::kBuffers] = {};
    auto emit = [&](const std::vector<PclMarker>& list, const sl::FrameToken& token) {
        for (PclMarker m : list) {
            rt.Marker(m, token);
            ++markers;
        }
    };
    const ULONGLONG start = GetTickCount64();
    for (uint32_t frame = 1; frame <= kFrames; ++frame) {
        s.window->Pump();
        // Spec 7 step 1: token, Reflex sleep, SimulationStart.
        sl::FrameToken* token = rt.NewFrameToken(frame);
        REQUIRE(token != nullptr);
        CHECK_EQ(static_cast<uint32_t>(*token), frame);
        rt.ReflexSleep(*token);
        emit(seq.BeginFrame(frame), *token);

        const UINT idx = s.chain->GetCurrentBackBufferIndex();
        REQUIRE(idx < SlSession::kBuffers);
        indexSeen[idx] = true;
        REQUIRE(waitFor(bufferValue[idx]));
        REQUIRE(SUCCEEDED(allocators[idx]->Reset()));
        ID3D12GraphicsCommandList* cl = lists[idx].Get();
        REQUIRE(SUCCEEDED(cl->Reset(allocators[idx].Get(), nullptr)));
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = buffers[idx].Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cl->ResourceBarrier(1, &b);
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(idx) * rtvStep;
        const float color[4] = {static_cast<float>(frame % 60) / 60.0f, 0.2f, 0.4f, 1.0f};
        cl->ClearRenderTargetView(h, color, 0, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cl->ResourceBarrier(1, &b);
        REQUIRE(SUCCEEDED(cl->Close()));
        ID3D12CommandList* submit[] = {cl};
        s.queue->ExecuteCommandLists(1, submit);

        // Spec 7 steps 4-6: markers around the Present on the proxy chain.
        emit(seq.BeforePresent(), *token);
        const HRESULT hr = s.chain->Present(0, s.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
        if (hr == DXGI_STATUS_OCCLUDED) ++occluded;
        if (FAILED(hr)) {
            if (presentFailures++ == 0) std::printf("  Present failed: 0x%08lX\n", static_cast<unsigned long>(hr));
        }
        emit(seq.AfterPresent(), *token);

        bufferValue[idx] = ++fenceValue;
        REQUIRE(SUCCEEDED(s.queue->Signal(fence.Get(), fenceValue)));
    }
    const ULONGLONG elapsed = GetTickCount64() - start;
    CHECK(waitFor(fenceValue));
    std::printf("  %u frames in %llu ms, %u markers, %u occluded, tearing %d\n", kFrames, elapsed, markers, occluded,
                s.tearing);
    CHECK_EQ(presentFailures, 0u);
    CHECK_EQ(markers, kFrames * 6u);
    CHECK_EQ(seq.AbandonedFrames(), 0u);
    CHECK_EQ(seq.OutOfOrderCalls(), 0u);
    for (bool seen : indexSeen) CHECK(seen);
    // None of the per-frame calls failed (each logs its first failure).
    CHECK(log.Matching({"slGetNewFrameToken failed"}).empty());
    CHECK(log.Matching({"slReflexSleep failed"}).empty());
    CHECK(log.Matching({"slPCLSetMarker failed"}).empty());

    // Every Streamline module, NGX's DLSS-G snippet included, is ours.
    CHECK_EQ(rt.LogLoadedModules(), 0);
    Print("modules", log.Matching({"Streamline module"}));

    // Spec 6.3: slShutdown before any D3D12/DXGI object is released.
    rt.Shutdown();
    CloseHandle(event);
    for (auto& l : lists) l.Reset();
    for (auto& a : allocators) a.Reset();
    for (auto& b : buffers) b.Reset();
    rtvHeap.Reset();
    fence.Reset();
    s.Release(rt);
    CHECK(rt.IsShutDown());
    std::string again;
    CHECK(!rt.Init(SlDir(), SlLogDir(), &again));
    CHECK(again.find("shut down") != std::string::npos);

    CHECK_EQ(rt.ErrorsLogged(), 0u);
    std::printf("  Streamline warnings: %u\n", rt.WarningsLogged());
    Print("warnings in our log", log.Matching({" WARN "}));
    Print("errors in our log", log.Matching({" ERROR "}));
    CHECK(log.Matching({" ERROR "}).empty());
}

// --- Tags, constants and the DLSS-G calls (M3) ------------------------------------

namespace {

// A camera at the origin looking down +z, 1280x720 render size.
CameraLayout TestCamera(uint32_t frame) {
    CameraLayout c{};
    c.magic = kCameraMagic;
    c.version = kCameraVersion;
    c.frame = frame;
    c.pos[2] = static_cast<float>(frame) * 0.1f;
    c.fwd[2] = 1.0f;
    c.up[1] = 1.0f;
    c.side[0] = 1.0f;
    c.fovVDeg = 60.0f;
    c.clipNear = 0.1f;
    c.clipFar = 1000.0f;
    c.renderW = 160.0f;
    c.renderH = 90.0f;
    return c;
}

ComPtr<ID3D12Resource> CommittedTexture(ID3D12Device* dev, DXGI_FORMAT format, UINT w, UINT h,
                                        D3D12_RESOURCE_FLAGS flags) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = format;
    rd.SampleDesc.Count = 1;
    rd.Flags = flags;
    ComPtr<ID3D12Resource> r;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                 IID_PPV_ARGS(&r));
    return r;
}

}  // namespace

TEST(SlRuntime_RealDllsTagsConstantsAndDlssgCalls) {
    CHECK_EQ(acdb_test::RunChildTest("Child_SlRuntime_RealDllsTagsConstantsAndDlssgCalls", kChildTimeoutMs), 0);
}

TEST(Child_SlRuntime_RealDllsTagsConstantsAndDlssgCalls) {
    LogCapture log(L"sl_real_tags");
    StreamlineRuntime& rt = StreamlineRuntime::Get();
    std::string err;
    REQUIRE(rt.Init(SlDir(), SlLogDir(), &err));

    SlSession s;
    bool skipped = false;
    const bool created = s.Create(rt, &skipped);
    if (skipped) {
        s.Release(rt);
        return;
    }
    REQUIRE(created);
    REQUIRE(rt.EnableReflexLowLatency());

    // Stand-ins for the capture slots: D3D12 textures in COMMON, as the
    // shared slot textures are.
    ComPtr<ID3D12Resource> depth =
        CommittedTexture(s.native.Get(), DXGI_FORMAT_R32_FLOAT, 160, 90, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> mvec = CommittedTexture(s.native.Get(), DXGI_FORMAT_R16G16_FLOAT, 160, 90,
                                                   D3D12_RESOURCE_FLAG_NONE);
    REQUIRE(depth && mvec);
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    REQUIRE(SUCCEEDED(s.native->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
    REQUIRE(SUCCEEDED(
        s.native->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))));

    const CameraLayout prev = TestCamera(1);
    const CameraLayout cur = TestCamera(2);
    ConstantsInput in;
    in.cur = &cur;
    in.prev = &prev;
    in.capture.mvScaleX = -160.0f;
    in.capture.mvScaleY = -90.0f;
    in.capture.renderW = 160;
    in.capture.renderH = 90;
    in.capture.createFlags = dlss_create_flags::kMVLowRes;
    sl::Constants consts;
    std::string why;
    REQUIRE(BuildFrameConstants(in, &consts, &why));

    sl::FrameToken* token = rt.NewFrameToken(1);
    REQUIRE(token != nullptr);
    // The open command list of the frame, as open-shaders passes it.
    const sl::Extent extent{0, 0, 160, 90};
    CHECK(rt.SetTagsForFrame(*token, 0, depth.Get(), mvec.Get(), extent, list.Get()) == sl::Result::eOk);
    CHECK(rt.SetConstants(consts, *token, 0) == sl::Result::eOk);
    // eValidUntilPresent tags need no command list (verified in sl.common's
    // ResourceTaggingForFrame::setTag, which only copies other lifecycles).
    sl::FrameToken* token2 = rt.NewFrameToken(2);
    REQUIRE(token2 != nullptr);
    CHECK(rt.SetTagsForFrame(*token2, 0, depth.Get(), mvec.Get(), extent, nullptr) == sl::Result::eOk);
    CHECK(rt.SetConstants(consts, *token2, 0) == sl::Result::eOk);
    CHECK(rt.SetNullTags(*token2, 0) == sl::Result::eOk);

    // DLSS-G mode and state: eOk where DLSS-G is supported; refused without a
    // call into Streamline where it is not (the RTX 3080 without the spoof).
    const bool dlssg = rt.DlssgSupported(s.luid, nullptr) && rt.DlssgFunctionsResolved();
    std::printf("  DLSS-G functions resolved: %s\n", dlssg ? "yes" : "no");
    DlssgSizeHints hints;
    hints.numBackBuffers = SlSession::kBuffers;
    hints.mvecDepthWidth = 160;
    hints.mvecDepthHeight = 90;
    hints.colorWidth = SlSession::kWidth;
    hints.colorHeight = SlSession::kHeight;
    hints.colorBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    hints.mvecBufferFormat = DXGI_FORMAT_R16G16_FLOAT;
    hints.depthBufferFormat = DXGI_FORMAT_R32_FLOAT;
    sl::DLSSGState state{};
    const sl::Result setOff = rt.SetDlssgOptions(false, 1, hints);
    const sl::Result estimate = rt.GetDlssgState(true, 1, hints, &state);
    std::printf("  SetDlssgOptions(eOff): %s, GetDlssgState(estimate): %s, estimate %llu bytes, status %u\n",
                SlResultName(setOff), SlResultName(estimate),
                static_cast<unsigned long long>(state.estimatedVRAMUsageInBytes), static_cast<unsigned>(state.status));
    if (dlssg) {
        CHECK(setOff == sl::Result::eOk);
        CHECK(estimate == sl::Result::eOk);
    } else {
        CHECK(setOff == sl::Result::eErrorFeatureMissing);
        CHECK(estimate == sl::Result::eErrorFeatureMissing);
    }

    CHECK(SUCCEEDED(list->Close()));
    ID3D12CommandList* lists[] = {list.Get()};
    s.queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    REQUIRE(SUCCEEDED(s.native->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    REQUIRE(SUCCEEDED(s.queue->Signal(fence.Get(), 1)));
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, event);
    CHECK_EQ(WaitForSingleObject(event, 5000), static_cast<DWORD>(WAIT_OBJECT_0));
    CloseHandle(event);

    rt.Shutdown();
    list.Reset();
    alloc.Reset();
    depth.Reset();
    mvec.Reset();
    fence.Reset();
    s.Release(rt);
    CHECK(log.Matching({"slSetTagForFrame failed"}).empty());
    CHECK(log.Matching({"slSetConstants failed"}).empty());
    CHECK_EQ(rt.ErrorsLogged(), 0u);
    Print("warnings in our log", log.Matching({" WARN "}));
    Print("errors in our log", log.Matching({" ERROR "}));
    CHECK(log.Matching({" ERROR "}).empty());
}

// --- D3D12Presenter and ProxySwapChain on the Streamline path ------------------

namespace {

// The test devices when the first hardware adapter is NVIDIA; false (skip) otherwise.
bool NvidiaDevices(acdb_test::GpuTestDevices* d) {
    if (!acdb_test::CreateGpuTestDevices(d)) return false;
    DXGI_ADAPTER_DESC1 desc{};
    d->adapter->GetDesc1(&desc);
    if (d->warp || desc.VendorId != kNvidiaVendorId) {
        std::printf("  SKIP: first hardware adapter is not NVIDIA (vendor 0x%04X)\n", desc.VendorId);
        return false;
    }
    return true;
}

// CSP's main swap chain (spec section 4): waitable, tearing.
DXGI_SWAP_CHAIN_DESC1 CspDesc(UINT w, UINT h) {
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = w;
    d.Height = h;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_SHADER_INPUT | DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    d.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    return d;
}

// Stands in for the hidden chain's buffer 0.
struct Source11 {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
};

Source11 CreateSource11(ID3D11Device* dev, UINT w, UINT h) {
    Source11 s;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &s.tex))) dev->CreateRenderTargetView(s.tex.Get(), nullptr, &s.rtv);
    return s;
}

bool PresentOk(HRESULT hr) { return hr == S_OK || hr == DXGI_STATUS_OCCLUDED; }

size_t FirstLine(const std::vector<std::string>& lines, const char* piece) {
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].find(piece) != std::string::npos) return i;
    }
    return std::string::npos;
}

// Per-frame Streamline calls never failed (the runtime logs each one's first failure).
void CheckNoPerFrameFailures(const LogCapture& log) {
    CHECK(log.Matching({"slGetNewFrameToken failed"}).empty());
    CHECK(log.Matching({"slReflexSleep failed"}).empty());
    CHECK(log.Matching({"slPCLSetMarker failed"}).empty());
}

}  // namespace

TEST(SlPresenter_FramesMarkersResizeAndShutdown) {
    CHECK_EQ(acdb_test::RunChildTest("Child_SlPresenter_FramesMarkersResizeAndShutdown", kChildTimeoutMs), 0);
}

TEST(Child_SlPresenter_FramesMarkersResizeAndShutdown) {
    LogCapture log(L"sl_presenter");
    StreamlineRuntime& rt = StreamlineRuntime::Get();
    std::string err;
    REQUIRE(rt.Init(SlDir(), SlLogDir(), &err));
    acdb_test::GpuTestDevices d;
    if (!NvidiaDevices(&d)) {
        rt.Shutdown();
        return;
    }
    HiddenWindow window(640, 360);
    REQUIRE(window.Get() != nullptr);
    {
        PresenterCreateInfo info;
        info.device11 = d.device11.Get();
        info.hwnd = window.Get();
        info.game_desc = CspDesc(640, 360);
        info.streamline = &rt;
        info.config = ProxyWithoutFg();
        auto p = D3D12Presenter::Create(info, &err);
        if (!p) std::printf("  D3D12Presenter::Create: %s\n", err.c_str());
        REQUIRE(p != nullptr);
        CHECK(p->UsesStreamline());
        CHECK(rt.IsProxied(p->Chain()));
        DXGI_SWAP_CHAIN_DESC1 cd{};
        CHECK(SUCCEEDED(p->Chain()->GetDesc1(&cd)));
        CHECK_EQ(cd.BufferCount, 3u);
        CHECK_EQ(cd.Format, DXGI_FORMAT_R8G8B8A8_UNORM);
        // CSP's chain is waitable; the Streamline chain never is (spec 6.3).
        CHECK((cd.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) == 0);
        CHECK(!p->HasLatencyWaitable());
        CHECK(!log.Matching({"Streamline: Reflex low latency on"}).empty());
        CHECK_EQ(p->FramesWithMarkers(), 0u);  // creation only started frame 1

        auto frames = [&](const Source11& s, int n) {
            int bad = 0;
            for (int i = 0; i < n; ++i) {
                const float color[4] = {static_cast<float>(i % 60) / 60.0f, 0.25f, 0.75f, 1.0f};
                d.ctx11->ClearRenderTargetView(s.rtv.Get(), color);
                const HRESULT hr = p->PresentFrame(d.ctx11.Get(), s.tex.Get(), 0, 0);
                window.Pump();
                if (!PresentOk(hr) && bad++ == 0)
                    std::printf("  PresentFrame: 0x%08lX\n", static_cast<unsigned long>(hr));
            }
            return bad;
        };
        Source11 src = CreateSource11(d.device11.Get(), 640, 360);
        REQUIRE(src.rtv);
        const ULONGLONG start = GetTickCount64();
        CHECK_EQ(frames(src, 120), 0);
        std::printf("  120 frames in %llu ms\n", GetTickCount64() - start);
        CHECK_EQ(p->FramesWithMarkers(), 120u);
        CHECK_EQ(p->MarkerProblems(), 0u);

        // Resize through the proxy chain; frames and markers continue.
        CHECK(SUCCEEDED(p->Resize(800, 450)));
        CHECK(SUCCEEDED(p->Chain()->GetDesc1(&cd)));
        CHECK_EQ(cd.Width, 800u);
        CHECK_EQ(cd.Height, 450u);
        CHECK((cd.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) == 0);
        Source11 big = CreateSource11(d.device11.Get(), 800, 450);
        REQUIRE(big.rtv);
        CHECK_EQ(frames(big, 30), 0);
        // One more frame after a second, so that a stats line is written.
        Sleep(1000);
        CHECK_EQ(frames(big, 1), 0);
        CHECK_EQ(p->FramesWithMarkers(), 151u);
        CHECK_EQ(p->MarkerProblems(), 0u);
        CHECK(!p->Stopped());
        CHECK(!rt.IsShutDown());

        p->ShutdownStreamlineOnRelease();
        p.reset();
    }
    CHECK(rt.IsShutDown());
    d.ctx11->ClearState();
    d.ctx11->Flush();

    // Spec 6.3: slShutdown after the drain and before the D3D12/DXGI releases.
    const auto lines = log.Lines();
    const size_t shutdownAt = FirstLine(lines, "Streamline: slShutdown eOk");
    const size_t releasedAt = FirstLine(lines, "presenter released");
    CHECK(shutdownAt != std::string::npos);
    CHECK(releasedAt != std::string::npos);
    CHECK(shutdownAt < releasedAt);
    const auto stats = log.Matching({" stats: "});
    Print("stats lines", stats);
    CHECK(!stats.empty());
    for (const auto& l : stats) CHECK(l.find(" streamline=on reflex=on pcl_problems=0") != std::string::npos);
    CheckNoPerFrameFailures(log);
    CHECK_EQ(rt.ErrorsLogged(), 0u);
    std::printf("  Streamline warnings: %u\n", rt.WarningsLogged());
    Print("errors in our log", log.Matching({" ERROR "}));
    CHECK(log.Matching({" ERROR "}).empty());
}

TEST(SlProxy_FinalReleaseShutsStreamlineDown) {
    CHECK_EQ(acdb_test::RunChildTest("Child_SlProxy_FinalReleaseShutsStreamlineDown", kChildTimeoutMs), 0);
}

TEST(Child_SlProxy_FinalReleaseShutsStreamlineDown) {
    LogCapture log(L"sl_proxy");
    StreamlineRuntime& rt = StreamlineRuntime::Get();
    std::string err;
    REQUIRE(rt.Init(SlDir(), SlLogDir(), &err));
    acdb_test::GpuTestDevices d;
    if (!NvidiaDevices(&d)) {
        rt.Shutdown();
        return;
    }
    HiddenWindow window(640, 360);
    REQUIRE(window.Get() != nullptr);
    ComPtr<IDXGIFactory2> factory;
    REQUIRE(SUCCEEDED(d.factory.As(&factory)));
    CHECK_EQ(ProxySwapChain::LiveCount(), 0);

    ComPtr<IDXGISwapChain1> chain;
    HRESULT hr = ProxySwapChain::Create(factory.Get(), d.device11.Get(), window.Get(), CspDesc(640, 360), nullptr,
                                        ProxyWithoutFg(), &rt, chain.GetAddressOf(), &err);
    if (FAILED(hr)) std::printf("  ProxySwapChain::Create: 0x%08lX %s\n", static_cast<unsigned long>(hr), err.c_str());
    REQUIRE(SUCCEEDED(hr) && chain);
    {
        ComPtr<ID3D11Texture2D> buffer;
        ComPtr<ID3D11RenderTargetView> rtv;
        REQUIRE(SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&buffer))));
        REQUIRE(SUCCEEDED(d.device11->CreateRenderTargetView(buffer.Get(), nullptr, &rtv)));
        int bad = 0;
        for (int i = 0; i < 20; ++i) {
            const float color[4] = {static_cast<float>(i) / 20.0f, 0.5f, 0.25f, 1.0f};
            d.ctx11->ClearRenderTargetView(rtv.Get(), color);
            if (!PresentOk(chain->Present(0, 0))) ++bad;
            window.Pump();
        }
        CHECK_EQ(bad, 0);
        d.ctx11->ClearState();
        d.ctx11->Flush();
    }

    // While the chain lives, a main-window chain would be proxied (the
    // devices are on an NVIDIA adapter, see NvidiaDevices above).
    ProxyDecisionInputs in;
    in.is_d3d11_device = in.is_main_window = in.bootstrap_possible = in.compat_ok = in.nvidia_adapter = true;
    in.streamline_shut_down = rt.IsShutDown();
    CHECK(ShouldProxy(in));

    // Only the final Release shuts Streamline down.
    ComPtr<IDXGISwapChain4> extra;
    CHECK(SUCCEEDED(chain.As(&extra)));
    chain.Reset();
    CHECK(!rt.IsShutDown());
    extra.Reset();
    CHECK_EQ(ProxySwapChain::LiveCount(), 0);
    CHECK(rt.IsShutDown());
    const auto lines = log.Lines();
    const size_t shutdownAt = FirstLine(lines, "Streamline: slShutdown eOk");
    const size_t releasedAt = FirstLine(lines, "presenter released");
    CHECK(shutdownAt != std::string::npos);
    CHECK(shutdownAt < releasedAt);
    CheckNoPerFrameFailures(log);
    CHECK_EQ(rt.ErrorsLogged(), 0u);
    Print("errors in our log", log.Matching({" ERROR "}));
    CHECK(log.Matching({" ERROR "}).empty());

    // Afterwards the hook passes every chain through (spec 6.3, 8).
    in.streamline_shut_down = rt.IsShutDown();
    CHECK(!ShouldProxy(in));
    // A proxy that tried Streamline anyway fails cleanly and creates nothing.
    ComPtr<IDXGISwapChain1> second;
    std::string secondErr;
    hr = ProxySwapChain::Create(factory.Get(), d.device11.Get(), window.Get(), CspDesc(640, 360), nullptr, ProxyWithoutFg(), &rt,
                                second.GetAddressOf(), &secondErr);
    std::printf("  second proxy after shutdown: 0x%08lX %s\n", static_cast<unsigned long>(hr), secondErr.c_str());
    CHECK(FAILED(hr));
    CHECK(!second);
    CHECK(secondErr.find("shut down") != std::string::npos);
    CHECK_EQ(ProxySwapChain::LiveCount(), 0);
    CHECK_EQ(rt.ErrorsLogged(), 0u);
}

// A creation failure after slSetD3DDevice must not destroy the device while
// Streamline still holds it (Streamline's guide: slShutdown before destroying
// devices): the failed presenter shuts Streamline down before its releases,
// and later chains pass through (spec 6.3, 8, 9).
TEST(SlPresenter_CreationFailureAfterSetDeviceShutsStreamlineDown) {
    CHECK_EQ(acdb_test::RunChildTest("Child_SlPresenter_CreationFailureAfterSetDevice", kChildTimeoutMs), 0);
}

TEST(Child_SlPresenter_CreationFailureAfterSetDevice) {
    LogCapture log(L"sl_create_fail");
    StreamlineRuntime& rt = StreamlineRuntime::Get();
    std::string err;
    REQUIRE(rt.Init(SlDir(), SlLogDir(), &err));
    acdb_test::GpuTestDevices d;
    if (!NvidiaDevices(&d)) {
        rt.Shutdown();
        return;
    }
    HiddenWindow window(640, 360);
    REQUIRE(window.Get() != nullptr);
    // A flip-model chain already on the window makes the D3D12 chain creation
    // (step 5, after slSetD3DDevice and the device proxy) fail.
    ComPtr<IDXGIFactory2> factory;
    REQUIRE(SUCCEEDED(d.factory.As(&factory)));
    DXGI_SWAP_CHAIN_DESC1 cd = CspDesc(640, 360);
    cd.Flags = 0;
    ComPtr<IDXGISwapChain1> occupant;
    REQUIRE(SUCCEEDED(factory->CreateSwapChainForHwnd(d.device11.Get(), window.Get(), &cd, nullptr, nullptr,
                                                      occupant.GetAddressOf())));
    {
        PresenterCreateInfo info;
        info.device11 = d.device11.Get();
        info.hwnd = window.Get();
        info.game_desc = CspDesc(640, 360);
        info.streamline = &rt;
        info.config = ProxyWithoutFg();  // get past the DLSS-G support rule to step 5
        auto p = D3D12Presenter::Create(info, &err);
        std::printf("  D3D12Presenter::Create on an occupied window: %s\n", err.c_str());
        CHECK(p == nullptr);
        CHECK(err.find("CreateSwapChainForHwnd") != std::string::npos);
    }
    CHECK(rt.IsShutDown());
    const auto lines = log.Lines();
    const size_t shutdownAt = FirstLine(lines, "Streamline: slShutdown eOk");
    const size_t releasedAt = FirstLine(lines, "presenter released");
    CHECK(shutdownAt != std::string::npos);
    CHECK(releasedAt != std::string::npos);
    CHECK(shutdownAt < releasedAt);
    occupant.Reset();
    d.ctx11->ClearState();
    d.ctx11->Flush();
}

// --- M3: DLSS-G support rule, capture, tags and constants ------------------------

// Spec criterion 5: when Streamline refuses DLSS-G and proxy_without_fg=0 (the
// default), the presenter is not created, Streamline is shut down after the
// drain and before the releases, and the chain passes through.
TEST(SlPresenter_WithoutDlssgIsNotCreatedByDefault) {
    CHECK_EQ(acdb_test::RunChildTest("Child_SlPresenter_WithoutDlssgIsNotCreatedByDefault", kChildTimeoutMs), 0);
}

TEST(Child_SlPresenter_WithoutDlssgIsNotCreatedByDefault) {
    LogCapture log(L"sl_no_fg");
    StreamlineRuntime& rt = StreamlineRuntime::Get();
    std::string err;
    REQUIRE(rt.Init(SlDir(), SlLogDir(), &err));
    acdb_test::GpuTestDevices d;
    if (!NvidiaDevices(&d)) {
        rt.Shutdown();
        return;
    }
    HiddenWindow window(640, 360);
    REQUIRE(window.Get() != nullptr);
    DXGI_ADAPTER_DESC1 ad{};
    d.adapter->GetDesc1(&ad);
    {
        PresenterCreateInfo info;
        info.device11 = d.device11.Get();
        info.hwnd = window.Get();
        info.game_desc = CspDesc(640, 360);
        info.streamline = &rt;
        auto p = D3D12Presenter::Create(info, &err);
        if (p && p->DlssgSupported()) {
            std::printf("  SKIP: DLSS-G is supported on this adapter\n");
            p->ShutdownStreamlineOnRelease();
            p.reset();
            return;
        }
        std::printf("  D3D12Presenter::Create without DLSS-G: %s\n", err.c_str());
        CHECK(p == nullptr);
        CHECK(err.find("DLSS-G is not supported on this adapter (") == 0);
        CHECK(err.find("eErrorNoSupportedAdapterFound") != std::string::npos ||
              err.find("eError") != std::string::npos);
        // No spoof in the test process: an SM86 GPU gets the hint.
        const bool sm86 = IsAmpereSm86(ad.VendorId, ad.DeviceId);
        CHECK_EQ(err.find("RTX 30 needs dlssg_for_sm86 (version.dll) in the game folder") != std::string::npos, sm86);
    }
    CHECK(rt.IsShutDown());
    const auto lines = log.Lines();
    const size_t shutdownAt = FirstLine(lines, "Streamline: slShutdown eOk");
    const size_t releasedAt = FirstLine(lines, "presenter released");
    CHECK(shutdownAt != std::string::npos);
    CHECK(shutdownAt < releasedAt);
    // The NGX hook is attached only by presenters that were created.
    CHECK(log.Matching({"NGX hook: installed"}).empty());
    d.ctx11->ClearState();
    d.ctx11->Flush();
}

namespace {

// CSP's DLSS inputs (spec 4), render size 320x180.
struct CaptureSources {
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11Texture2D> mvec;
};

CaptureSources MakeCaptureSources(ID3D11Device* dev, UINT w, UINT h) {
    CaptureSources s;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    dev->CreateTexture2D(&td, nullptr, &s.depth);
    td.Format = DXGI_FORMAT_R16G16_FLOAT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    dev->CreateTexture2D(&td, nullptr, &s.mvec);
    return s;
}

NgxEvaluateInputs CspEvaluate(ID3D11DeviceContext* ctx, const CaptureSources& s, UINT w, UINT h) {
    NgxEvaluateInputs in;
    in.ctx = ctx;
    in.depth = s.depth.Get();
    in.mvec = s.mvec.Get();
    in.jitterX = 0.25f;
    in.jitterY = -0.25f;
    in.mvScaleX = -static_cast<float>(w);
    in.mvScaleY = -static_cast<float>(h);
    in.subrectW = w;
    in.subrectH = h;
    in.createFlags = dlss_create_flags::kMVLowRes;
    in.featureKey = 0x4000;
    return in;
}

}  // namespace

// The test app's path (tag_without_fg=1, proxy_without_fg=1) on a machine
// without DLSS-G: a fake CSP evaluate through the capture coordinator is
// copied, paired with the Present, tagged and given constants, and Streamline
// accepts both (spec 7 step 5.4), while DLSS-G itself stays off.
TEST(SlPresenter_TagWithoutFgTagsCapturesAndSetsConstants) {
    CHECK_EQ(acdb_test::RunChildTest("Child_SlPresenter_TagWithoutFgTagsCaptures", kChildTimeoutMs), 0);
}

TEST(Child_SlPresenter_TagWithoutFgTagsCaptures) {
    LogCapture log(L"sl_tags");
    StreamlineRuntime& rt = StreamlineRuntime::Get();
    std::string err;
    REQUIRE(rt.Init(SlDir(), SlLogDir(), &err));
    acdb_test::GpuTestDevices d;
    if (!NvidiaDevices(&d)) {
        rt.Shutdown();
        return;
    }
    HiddenWindow window(640, 360);
    REQUIRE(window.Get() != nullptr);
    acdb_test::TestCameraWriter writer;
    CameraChannel camera(writer.Name().c_str());
    REQUIRE(writer.Ok());
    REQUIRE(camera.Create(&err));
    constexpr int kFrames = 40;
    bool supported = false;
    {
        PresenterCreateInfo info;
        info.device11 = d.device11.Get();
        info.hwnd = window.Get();
        info.game_desc = CspDesc(640, 360);
        info.streamline = &rt;
        info.config = ProxyWithoutFg();
        info.config.tag_without_fg = true;
        info.config.start_with_fg = false;  // DLSS-G stays off where it is supported
        info.env.camera = &camera;
        auto p = D3D12Presenter::Create(info, &err);
        if (!p) std::printf("  D3D12Presenter::Create: %s\n", err.c_str());
        REQUIRE(p != nullptr);
        supported = p->DlssgSupported();
        NgxEvaluateSink* sink = p->CaptureSink();
        REQUIRE(sink != nullptr);
        Source11 src = CreateSource11(d.device11.Get(), 640, 360);
        CaptureSources cs = MakeCaptureSources(d.device11.Get(), 320, 180);
        REQUIRE(src.rtv && cs.depth && cs.mvec);
        int bad = 0;
        for (int i = 1; i <= kFrames; ++i) {
            // The Lua app writes from render.onSceneReady, before CSP's DLSS pass.
            writer.Write(acdb_test::TestCameraWriter::Camera(static_cast<uint32_t>(i), 0, 320.0f, 180.0f));
            sink->OnEvaluate(CspEvaluate(d.ctx11.Get(), cs, 320, 180));
            const float color[4] = {static_cast<float>(i) / kFrames, 0.5f, 0.25f, 1.0f};
            d.ctx11->ClearRenderTargetView(src.rtv.Get(), color);
            if (!PresentOk(p->PresentFrame(d.ctx11.Get(), src.tex.Get(), 0, 0))) ++bad;
            window.Pump();
            if (i == 20) {
                // A resize in the middle: DLSS-G is off, so no extra Present;
                // the camera latch restarts, the slots stay.
                CHECK(SUCCEEDED(p->Resize(800, 450)));
                src = CreateSource11(d.device11.Get(), 800, 450);
            }
        }
        Sleep(1000);  // one more frame after a second writes a stats line
        writer.Write(acdb_test::TestCameraWriter::Camera(kFrames + 1, 0, 320.0f, 180.0f));
        sink->OnEvaluate(CspEvaluate(d.ctx11.Get(), cs, 320, 180));
        if (!PresentOk(p->PresentFrame(d.ctx11.Get(), src.tex.Get(), 0, 0))) ++bad;
        CHECK_EQ(bad, 0);
        const D3D12Presenter::FgTotals t = p->Totals();
        std::printf("  captures %u, camera fresh %u, tagged %u, fg frames %u\n", t.captures, t.camera_fresh, t.tagged,
                    t.fg_frames);
        CHECK_EQ(t.captures, static_cast<uint32_t>(kFrames + 1));
        // Frame 1 and the first frame after the resize have no fresh camera.
        CHECK_EQ(t.camera_fresh, static_cast<uint32_t>(kFrames - 1));
        CHECK_EQ(t.tagged, static_cast<uint32_t>(kFrames - 1));
        CHECK_EQ(t.fg_frames, 0u);
        CHECK_EQ(p->FramesWithMarkers(), static_cast<uint32_t>(kFrames + 1));
        CHECK_EQ(p->MarkerProblems(), 0u);
        p->ShutdownStreamlineOnRelease();
    }
    CHECK(rt.IsShutDown());
    d.ctx11->ClearState();
    d.ctx11->Flush();

    Print("fg lines", log.Matching({" fg: "}));
    CHECK(!log.Matching({"capture: first counted evaluate: depth 39 320x180, mvec 34 320x180, subrect 320x180, "
                         "create flags 0x2, mv scale -320.000,-180.000, jitter 0.2500,-0.2500"})
               .empty());
    CHECK(!log.Matching({"camera: first fresh snapshot: pos (0.000, 0.000, 0.200) fwd (0.0000, 0.0000, 1.0000)"})
               .empty());
    CHECK_EQ(log.Matching({" constants: "}).size(), 1u);
    // DLSS-G is off for a lasting reason, but with tag_without_fg the frames
    // that could not be tagged (frame 1: no fresh camera yet) are named by
    // the throttled per-frame WARN; the first one of its reason is logged.
    CHECK(!log.Matching({"] WARN fg: frame without DLSS-G: camera not fresh"}).empty());
    CHECK_EQ(log.Matching({"fg: first tags and constants set (frame 2)"}).size(), 1u);
    const char* offReason = supported ? "fg: DLSS-G off (off by the user" : "fg: DLSS-G off (not supported on this adapter)";
    CHECK_EQ(log.Matching({offReason}).size(), 1u);
    CHECK(log.Matching({"fg: DLSS-G on"}).empty());
    Print("tag and constants lines", log.Matching({"slSetTagForFrame"}));
    CHECK(log.Matching({"slSetTagForFrame failed"}).empty());
    CHECK(log.Matching({"slSetConstants failed"}).empty());
    const auto stats = log.Matching({" stats: "});
    Print("stats lines", stats);
    REQUIRE(!stats.empty());
    const std::regex fields(" fg=off stalls=0 streamline=on reflex=on pcl_problems=0 captures=[0-9]+ camera_fresh=[0-9]+ "
                            "tagged=[0-9]+ fg_frames=0 generated=n/a double_evaluates=0 fg_mult=2 vram_mib=");
    for (const auto& l : stats) CHECK(std::regex_search(l, fields));
    CheckNoPerFrameFailures(log);
    CHECK_EQ(rt.ErrorsLogged(), 0u);
    Print("errors in our log", log.Matching({" ERROR "}));
    CHECK(log.Matching({" ERROR "}).empty());
}

#endif  // ACDB_SL_BIN_DIR
