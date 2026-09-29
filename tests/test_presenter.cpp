// D3D12Presenter on its own: creation, frames, resize, pixels reaching the
// D3D12 back buffer, statistics, the ACDLSSG_DEBUG_STALL_MS hook and the 4 s
// give-up rule. Every test uses a hidden (never shown) window of class "acsW".
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <regex>
#include <string>
#include <utility>

#include "child_process.h"
#include "d3d12_presenter.h"
#include "fake_panel.h"
#include "fake_nvngx/fake_nvngx.h"
#include "gpu_test_devices.h"
#include "log.h"
#include "ngx_hook.h"
#include "panel_status.h"
#include "temp_dir.h"
#include "test_framework.h"

using Microsoft::WRL::ComPtr;
using namespace acdb;

namespace {

// Sets an environment variable for the lifetime of the object, then restores it.
class EnvOverride {
public:
    EnvOverride(const wchar_t* name, const wchar_t* value) : name_(name) {
        const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
        if (n > 0) {
            old_.assign(n, L'\0');
            const DWORD got = GetEnvironmentVariableW(name, old_.data(), n);
            old_.resize(got < n ? got : 0);
            had_ = true;
        }
        SetEnvironmentVariableW(name, value);
    }
    ~EnvOverride() { SetEnvironmentVariableW(name_.c_str(), had_ ? old_.c_str() : nullptr); }
    EnvOverride(const EnvOverride&) = delete;
    EnvOverride& operator=(const EnvOverride&) = delete;

private:
    std::wstring name_;
    std::wstring old_;
    bool had_ = false;
};

// A hidden top-level window of class "acsW", like AC's main window.
class GameWindow {
public:
    GameWindow(int width, int height) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"acsW";
        registered_ = RegisterClassExW(&wc) != 0;
        RECT rc{0, 0, width, height};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd_ = CreateWindowExW(0, L"acsW", L"acdb presenter test", WS_OVERLAPPEDWINDOW, 0, 0, rc.right - rc.left,
                                rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    }
    ~GameWindow() {
        if (hwnd_) DestroyWindow(hwnd_);
        if (registered_) UnregisterClassW(L"acsW", GetModuleHandleW(nullptr));
    }
    GameWindow(const GameWindow&) = delete;
    GameWindow& operator=(const GameWindow&) = delete;
    HWND Get() const { return hwnd_; }

private:
    bool registered_ = false;
    HWND hwnd_ = nullptr;
};

void PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

DXGI_SWAP_CHAIN_DESC1 GameDesc(UINT w, UINT h, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) {
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = w;
    d.Height = h;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_SHADER_INPUT | DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    d.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    return d;
}

bool GetDevices(acdb_test::GpuTestDevices* d) {
    if (acdb_test::CreateGpuTestDevices(d)) return true;
    std::printf("  SKIP: no adapter creates the test devices\n");
    return false;
}

// Stands in for the hidden chain's buffer 0.
struct Source {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
};

Source CreateSource(ID3D11Device* dev, UINT w, UINT h) {
    Source s;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &s.tex)))
        dev->CreateRenderTargetView(s.tex.Get(), nullptr, &s.rtv);
    return s;
}

HRESULT Frame(D3D12Presenter* p, ID3D11DeviceContext* ctx, const Source& src, int i) {
    const float color[4] = {static_cast<float>(i % 60) / 60.0f, 0.25f, 0.75f, 1.0f};
    ctx->ClearRenderTargetView(src.rtv.Get(), color);
    const HRESULT hr = p->PresentFrame(ctx, src.tex.Get(), 0, 0);
    PumpMessages();
    return hr;
}

bool PresentOk(HRESULT hr) { return hr == S_OK || hr == DXGI_STATUS_OCCLUDED; }

// CSP's DLSS inputs (spec 4).
struct CaptureSources {
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11Texture2D> mvec;
};

CaptureSources CreateCaptureSources(ID3D11Device* dev, UINT w, UINT h) {
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

// What NgxHook reports for CSP's evaluate.
NgxEvaluateInputs EvaluateInputs(ID3D11DeviceContext* ctx, const CaptureSources& s) {
    D3D11_TEXTURE2D_DESC desc{};
    s.depth->GetDesc(&desc);
    NgxEvaluateInputs in;
    in.ctx = ctx;
    in.depth = s.depth.Get();
    in.mvec = s.mvec.Get();
    in.mvScaleX = -static_cast<float>(desc.Width);
    in.mvScaleY = -static_cast<float>(desc.Height);
    in.subrectW = desc.Width;
    in.subrectH = desc.Height;
    in.createFlags = 2;
    in.featureKey = 0x9000;
    return in;
}

std::unique_ptr<D3D12Presenter> CreatePresenter(const acdb_test::GpuTestDevices& d, HWND hwnd, UINT w, UINT h) {
    PresenterCreateInfo info;
    info.device11 = d.device11.Get();
    info.hwnd = hwnd;
    info.game_desc = GameDesc(w, h);
    std::string err;
    auto p = D3D12Presenter::Create(info, &err);
    if (!p) std::printf("  D3D12Presenter::Create: %s\n", err.c_str());
    return p;
}

}  // namespace

TEST(Presenter_CreatePresentAndResize) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    GameWindow window(1280, 720);
    REQUIRE(window.Get() != nullptr);
    {
        auto p = CreatePresenter(d, window.Get(), 1280, 720);
        REQUIRE(p != nullptr);
        REQUIRE(p->Chain() != nullptr);
        DXGI_SWAP_CHAIN_DESC1 cd{};
        CHECK(SUCCEEDED(p->Chain()->GetDesc1(&cd)));
        CHECK_EQ(cd.Width, 1280u);
        CHECK_EQ(cd.Height, 720u);
        CHECK_EQ(cd.BufferCount, 3u);
        CHECK_EQ(cd.Format, DXGI_FORMAT_R8G8B8A8_UNORM);
        CHECK_EQ(cd.SwapEffect, DXGI_SWAP_EFFECT_FLIP_DISCARD);
        // CSP's chain is waitable, so the D3D12 chain is too (M1 pacing).
        CHECK((cd.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0);
        CHECK(p->HasLatencyWaitable());
        // A plain D3D12 chain on CSP's adapter: D3D12 devices are per-adapter
        // singletons, so it is the test's own D3D12 device.
        ComPtr<ID3D12Device> dev12;
        CHECK(SUCCEEDED(p->Chain()->GetDevice(IID_PPV_ARGS(&dev12))));
        CHECK(dev12.Get() == d.device12.Get());

        Source src = CreateSource(d.device11.Get(), 1280, 720);
        REQUIRE(src.rtv);
        int bad = 0;
        for (int i = 0; i < 60; ++i) {
            const HRESULT hr = Frame(p.get(), d.ctx11.Get(), src, i);
            if (!PresentOk(hr)) {
                if (bad++ == 0) std::printf("  PresentFrame: 0x%08lX\n", static_cast<unsigned long>(hr));
            }
        }
        CHECK_EQ(bad, 0);
        CHECK_EQ(p->TestPresent(), S_OK);
        CHECK(!p->Stopped());

        CHECK(SUCCEEDED(p->Resize(1600, 900)));
        CHECK(SUCCEEDED(p->Chain()->GetDesc1(&cd)));
        CHECK_EQ(cd.Width, 1600u);
        CHECK_EQ(cd.Height, 900u);
        Source big = CreateSource(d.device11.Get(), 1600, 900);
        REQUIRE(big.rtv);
        for (int i = 0; i < 30; ++i) CHECK(PresentOk(Frame(p.get(), d.ctx11.Get(), big, i)));
        // A source that no longer matches is skipped, not fatal.
        CHECK(PresentOk(Frame(p.get(), d.ctx11.Get(), src, 0)));
        CHECK(!p->Stopped());

        // Invalid resizes are refused without harm.
        CHECK_EQ(p->Resize(0, 900), DXGI_ERROR_INVALID_CALL);
        CHECK(PresentOk(Frame(p.get(), d.ctx11.Get(), big, 1)));
    }
    d.ctx11->ClearState();
    d.ctx11->Flush();
}

// M3: the presenter owns the capture coordinator; an evaluate pairs with the
// next Present, whose own fence value also covers the capture's copy, and
// the counters see captures and double evaluates. The plain path has no
// Streamline, so nothing is tagged.
TEST(Presenter_CaptureSinkPairsEachCaptureWithThePresent) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    GameWindow window(640, 360);
    REQUIRE(window.Get() != nullptr);
    {
        auto p = CreatePresenter(d, window.Get(), 640, 360);
        REQUIRE(p != nullptr);
        NgxEvaluateSink* sink = p->CaptureSink();
        REQUIRE(sink != nullptr);
        Source src = CreateSource(d.device11.Get(), 640, 360);
        CaptureSources cs = CreateCaptureSources(d.device11.Get(), 320, 180);
        REQUIRE(src.rtv && cs.depth && cs.mvec);
        for (int i = 0; i < 10; ++i) {
            sink->OnEvaluate(EvaluateInputs(d.ctx11.Get(), cs));
            if (i == 4) sink->OnEvaluate(EvaluateInputs(d.ctx11.Get(), cs));  // a double evaluate
            CHECK(PresentOk(Frame(p.get(), d.ctx11.Get(), src, i)));
        }
        CHECK(PresentOk(Frame(p.get(), d.ctx11.Get(), src, 10)));  // a frame without an evaluate
        const D3D12Presenter::FgTotals t = p->Totals();
        CHECK_EQ(t.captures, 10u);
        CHECK_EQ(t.double_evaluates, 1u);
        CHECK_EQ(t.tagged, 0u);
        CHECK_EQ(t.fg_frames, 0u);
        CHECK(!p->Stopped());
    }
    d.ctx11->ClearState();
    d.ctx11->Flush();
}

// M3 production wiring: with install_ngx_hook the presenter installs NgxHook
// (once per process), attaches its coordinator, logs the hooked modules, and
// detaches before it is destroyed. The fake NGX module stands in for
// _nvngx.dll. A child process, because NgxHook is process-wide.
TEST(Presenter_NgxHookReachesTheCoordinatorAndIsDetachedAtRelease) {
    CHECK_EQ(acdb_test::RunChildTest("Child_Presenter_NgxHookReachesTheCoordinator"), 0);
}

TEST(Child_Presenter_NgxHookReachesTheCoordinator) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    acdb_test::TempDir dir(L"presenter_ngx");
    const std::wstring logPath = dir.Str() + L"\\bridge.log";
    REQUIRE(LogOpen(logPath, LogLevel::Info));
    HMODULE fake = LoadLibraryW(FAKE_NVNGX_PATH);
    REQUIRE(fake != nullptr);
    auto state = reinterpret_cast<FakeNgxState* (*)()>(GetProcAddress(fake, "FakeNgxGetState"));
    using CreateFn = int(__cdecl*)(void*, unsigned int, void*, void**);
    using EvalFn = int(__cdecl*)(void*, const void*, const void*, void*);
    auto create = reinterpret_cast<CreateFn>(GetProcAddress(fake, "NVSDK_NGX_D3D11_CreateFeature"));
    auto eval = reinterpret_cast<EvalFn>(GetProcAddress(fake, "NVSDK_NGX_D3D11_EvaluateFeature"));
    REQUIRE(state && create && eval);
    *state() = FakeNgxState{};
    state()->nextHandle = reinterpret_cast<void*>(0x340000);

    GameWindow window(640, 360);
    REQUIRE(window.Get() != nullptr);
    Source src = CreateSource(d.device11.Get(), 640, 360);
    CaptureSources cs = CreateCaptureSources(d.device11.Get(), 320, 180);
    REQUIRE(src.rtv && cs.depth && cs.mvec);
    FakeNgxParam cp;
    cp.SetU(ngxkey::kWidth, 320u);
    cp.SetU(ngxkey::kHeight, 180u);
    cp.SetU(ngxkey::kOutWidth, 640u);
    cp.SetU(ngxkey::kOutHeight, 360u);
    cp.SetI(ngxkey::kCreateFlags, static_cast<int>(kNgxDlssFlagMVLowRes));
    FakeNgxParam ep;
    ep.SetRes(ngxkey::kDepth, cs.depth.Get());
    ep.SetRes(ngxkey::kMotionVectors, cs.mvec.Get());
    ep.SetF(ngxkey::kMvScaleX, -320.0f);
    ep.SetF(ngxkey::kMvScaleY, -180.0f);
    ep.SetU(ngxkey::kSubrectWidth, 320u);
    ep.SetU(ngxkey::kSubrectHeight, 180u);
    {
        PresenterCreateInfo info;
        info.device11 = d.device11.Get();
        info.hwnd = window.Get();
        info.game_desc = GameDesc(640, 360);
        info.env.install_ngx_hook = true;
        std::string err;
        auto p = D3D12Presenter::Create(info, &err);
        REQUIRE(p != nullptr);
        void* h = nullptr;
        CHECK_EQ(create(d.ctx11.Get(), kNgxFeatureSuperSampling, &cp, &h), kNgxSuccess);
        for (int i = 0; i < 5; ++i) {
            CHECK_EQ(eval(d.ctx11.Get(), h, &ep, nullptr), kNgxSuccess);
            CHECK(PresentOk(Frame(p.get(), d.ctx11.Get(), src, i)));
        }
        CHECK_EQ(p->Totals().captures, 5u);
        p.reset();
        // Detached: the next evaluate is forwarded and reaches nothing freed.
        CHECK_EQ(eval(d.ctx11.Get(), h, &ep, nullptr), kNgxSuccess);
        CHECK_EQ(state()->evalCalls, 6L);
    }
    NgxHook::Get().Uninstall();
    FreeLibrary(fake);
    LogClose();
    d.ctx11->ClearState();
    d.ctx11->Flush();
    const std::string log = acdb_test::ReadAll(logPath);
    CHECK(log.find("NGX hook: installed on 1 module(s): fake_nvngx.dll") != std::string::npos);
    CHECK(log.find("capture: first counted evaluate: depth 39 320x180, mvec 34 320x180, subrect 320x180") !=
          std::string::npos);
    if (log.find("NGX hook: installed on 1 module(s): fake_nvngx.dll") == std::string::npos)
        std::printf("  log:\n%s\n", log.c_str());
}

// M1 pacing: the D3D12 chain gets a frame-latency object exactly when the
// game's chain has one, with the game's latency.
TEST(Presenter_LatencyWaitableFollowsTheGame) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    GameWindow window(640, 360);
    REQUIRE(window.Get() != nullptr);
    {
        auto p = CreatePresenter(d, window.Get(), 640, 360);
        REQUIRE(p != nullptr);
        REQUIRE(p->HasLatencyWaitable());
        UINT latency = 0;
        CHECK(SUCCEEDED(p->Chain()->GetMaximumFrameLatency(&latency)));
        CHECK_EQ(latency, 1u);  // DXGI's default until the game sets one
        p->SetMaximumFrameLatency(3);
        CHECK(SUCCEEDED(p->Chain()->GetMaximumFrameLatency(&latency)));
        CHECK_EQ(latency, 3u);
        p->SetMaximumFrameLatency(0);
        CHECK(SUCCEEDED(p->Chain()->GetMaximumFrameLatency(&latency)));
        CHECK_EQ(latency, 1u);
        p->SetMaximumFrameLatency(99);
        CHECK(SUCCEEDED(p->Chain()->GetMaximumFrameLatency(&latency)));
        CHECK_EQ(latency, 16u);
        p->SetMaximumFrameLatency(2);
        Source src = CreateSource(d.device11.Get(), 640, 360);
        REQUIRE(src.rtv);
        for (int i = 0; i < 30; ++i) CHECK(PresentOk(Frame(p.get(), d.ctx11.Get(), src, i)));
        CHECK(!p->Stopped());
        // A resize keeps the object (ResizeBuffers passes the chain's flags on).
        CHECK(SUCCEEDED(p->Resize(800, 450)));
        DXGI_SWAP_CHAIN_DESC1 cd{};
        CHECK(SUCCEEDED(p->Chain()->GetDesc1(&cd)));
        CHECK((cd.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0);
        Source big = CreateSource(d.device11.Get(), 800, 450);
        REQUIRE(big.rtv);
        for (int i = 0; i < 10; ++i) CHECK(PresentOk(Frame(p.get(), d.ctx11.Get(), big, i)));
    }
    {
        PresenterCreateInfo info;
        info.device11 = d.device11.Get();
        info.hwnd = window.Get();
        info.game_desc = GameDesc(640, 360);
        info.game_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        std::string err;
        auto p = D3D12Presenter::Create(info, &err);
        REQUIRE(p != nullptr);
        CHECK(!p->HasLatencyWaitable());
        DXGI_SWAP_CHAIN_DESC1 cd{};
        CHECK(SUCCEEDED(p->Chain()->GetDesc1(&cd)));
        CHECK((cd.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) == 0);
        p->SetMaximumFrameLatency(3);  // no waitable object: nothing to set
        Source src = CreateSource(d.device11.Get(), 640, 360);
        REQUIRE(src.rtv);
        for (int i = 0; i < 10; ++i) CHECK(PresentOk(Frame(p.get(), d.ctx11.Get(), src, i)));
    }
    d.ctx11->ClearState();
    d.ctx11->Flush();
}

// The game waits on its frame-latency object once before its first frame
// (DXGI requires it), and that wait is served by the bridge's own semaphore.
// The presenter must take the D3D12 chain's initial count itself; otherwise
// every later post-Present wait lets one frame more through than the game's
// own chain would, which is one refresh of extra latency with VSync.
TEST(Presenter_TakesTheGamesFirstLatencyWait) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    GameWindow window(640, 360);
    REQUIRE(window.Get() != nullptr);
    auto p = CreatePresenter(d, window.Get(), 640, 360);
    REQUIRE(p != nullptr);
    REQUIRE(p->HasLatencyWaitable());
    const HANDLE h = p->Chain()->GetFrameLatencyWaitableObject();
    REQUIRE(h != nullptr);
    CHECK_EQ(WaitForSingleObject(h, 0), static_cast<DWORD>(WAIT_TIMEOUT));
    CloseHandle(h);
}

TEST(Presenter_CreateRefusesUnsupportedInput) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    GameWindow window(320, 240);
    REQUIRE(window.Get() != nullptr);

    PresenterCreateInfo info;
    info.hwnd = window.Get();
    info.game_desc = GameDesc(320, 240);
    std::string err;
    CHECK(D3D12Presenter::Create(info, &err) == nullptr);  // no device
    CHECK(!err.empty());

    info.device11 = d.device11.Get();
    info.game_desc = GameDesc(320, 240, DXGI_FORMAT_B8G8R8A8_UNORM);
    err.clear();
    CHECK(D3D12Presenter::Create(info, &err) == nullptr);
    CHECK(err.find("R8G8B8A8_UNORM") != std::string::npos);
}

TEST(Presenter_LogsStatisticsEverySecond) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    acdb_test::TempDir dir(L"presenter_stats");
    const std::wstring logPath = dir.Str() + L"\\bridge.log";
    REQUIRE(LogOpen(logPath, LogLevel::Info));
    {
        GameWindow window(640, 360);
        REQUIRE(window.Get() != nullptr);
        auto p = CreatePresenter(d, window.Get(), 640, 360);
        REQUIRE(p != nullptr);
        Source src = CreateSource(d.device11.Get(), 640, 360);
        REQUIRE(src.rtv);
        const ULONGLONG start = GetTickCount64();
        for (int i = 0; GetTickCount64() - start < 2300; ++i) {
            CHECK(PresentOk(Frame(p.get(), d.ctx11.Get(), src, i)));
            Sleep(2);
        }
    }
    LogClose();
    d.ctx11->ClearState();
    d.ctx11->Flush();

    const std::string log = acdb_test::ReadAll(logPath);
    size_t lines = 0;
    const char* const statsLine = " INFO stats: base_fps=";
    for (size_t pos = log.find(statsLine); pos != std::string::npos; pos = log.find(statsLine, pos + 1)) ++lines;
    CHECK(lines >= 2);
    // Every field of spec 6.10 that M2 has, in order; the plain path has no
    // Streamline. The render adapter's local video memory, usage/budget in
    // MiB, comes last.
    const std::string vram = d.warp ? "([0-9]+/[0-9]+|n/a)" : "[0-9]+/[1-9][0-9]*";
    const std::regex full(
        " INFO stats: base_fps=[0-9]+\\.[0-9] presented_fps=[0-9]+\\.[0-9] skipped=0 failed=0 occluded=[0-9]+ "
        "uncopied=0 max_frame_ms=[0-9]+\\.[0-9] max_present_ms=[0-9]+\\.[0-9] bridge_gpu_ms d3d11=[0-9.na/]+ "
        "d3d12=[0-9.na/]+ fg=off stalls=0 streamline=off reflex=off pcl_problems=0 captures=0 camera_fresh=0 "
        "tagged=0 fg_frames=0 generated=n/a double_evaluates=0 vram_mib=" + vram + "\n");
    CHECK(std::regex_search(log, full));
    // Budget and usage are logged once at creation.
    const std::regex created(d.warp ? " INFO presenter: VRAM " : " INFO presenter: VRAM \\(local\\) budget [1-9][0-9]* MiB, "
                                                                  "usage [0-9]+ MiB\n");
    CHECK(std::regex_search(log, created));
    // Both copies were timed at least once in a second of frames.
    CHECK(log.find("d3d11=n/a d3d12=n/a") == std::string::npos);
    // The present mode is logged once, when it is first seen.
    const char* const mode = "present mode: CSP Present(0, 0x0) -> D3D12 Present(0, 0x0), windowed, chain tearing ";
    CHECK(log.find(mode) != std::string::npos);
    CHECK_EQ(log.find(mode), log.rfind(mode));
    CHECK(log.find("presenter created") != std::string::npos);
    CHECK(log.find("presenter released") != std::string::npos);
    if (lines < 2 || !std::regex_search(log, full)) std::printf("  log:\n%s\n", log.c_str());
}

TEST(Presenter_StopsAfterFourSecondsWithoutProgress) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    acdb_test::TempDir dir(L"presenter_giveup");
    const std::wstring logPath = dir.Str() + L"\\bridge.log";
    REQUIRE(LogOpen(logPath, LogLevel::Info));
    {
        GameWindow window(640, 360);
        REQUIRE(window.Get() != nullptr);
        std::unique_ptr<D3D12Presenter> p;
        {
            // Read at creation only; far longer than the 4 s limit.
            EnvOverride env(L"ACDLSSG_DEBUG_STALL_MS", L"9000");
            p = CreatePresenter(d, window.Get(), 640, 360);
        }
        REQUIRE(p != nullptr);
        Source src = CreateSource(d.device11.Get(), 640, 360);
        REQUIRE(src.rtv);

        HRESULT last = S_OK;
        ULONGLONG maxFrameMs = 0;
        const ULONGLONG start = GetTickCount64();
        int frames = 0;
        while (!p->Stopped() && GetTickCount64() - start < 9000) {
            const ULONGLONG t0 = GetTickCount64();
            last = Frame(p.get(), d.ctx11.Get(), src, frames++);
            const ULONGLONG dt = GetTickCount64() - t0;
            if (dt > maxFrameMs) maxFrameMs = dt;
            Sleep(5);
        }
        const ULONGLONG stoppedAfter = GetTickCount64() - start;
        std::printf("  stopped after %llu ms, %d frames, longest frame %llu ms\n", stoppedAfter, frames, maxFrameMs);
        CHECK(p->Stopped());
        CHECK_EQ(last, DXGI_ERROR_DEVICE_HUNG);
        CHECK(stoppedAfter < 8000);
        CHECK(maxFrameMs < 1500);
        // Every later call reports the same device error, test presents included.
        CHECK_EQ(Frame(p.get(), d.ctx11.Get(), src, 0), DXGI_ERROR_DEVICE_HUNG);
        CHECK_EQ(p->TestPresent(), DXGI_ERROR_DEVICE_HUNG);
        CHECK_EQ(p->Resize(800, 600), DXGI_ERROR_DEVICE_HUNG);

        // The destructor releases the debug wait, drains and returns promptly.
        const ULONGLONG t0 = GetTickCount64();
        p.reset();
        const ULONGLONG releaseMs = GetTickCount64() - t0;
        std::printf("  released in %llu ms\n", releaseMs);
        CHECK(releaseMs < 2000);
    }
    LogClose();
    d.ctx11->ClearState();
    d.ctx11->Flush();

    const std::string log = acdb_test::ReadAll(logPath);
    CHECK(log.find("debug stall") != std::string::npos);
    CHECK(log.find("D3D12 stall:") != std::string::npos);
    CHECK(log.find("presenter stopped: no D3D12 progress for 4 s") != std::string::npos);
    CHECK(log.find("did not drain") == std::string::npos);
}

namespace {

// Reads texel (x, y) of a D3D12 R8G8B8A8 texture in PRESENT state through a
// queue of the test's own; 0 on failure.
uint32_t ReadTexel12(ID3D12Device* dev, ID3D12Resource* tex, UINT x, UINT y) {
    const D3D12_RESOURCE_DESC desc = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&readback))) ||
        FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
        FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return 0;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    list->ResourceBarrier(1, &b);
    list->Close();
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence.Get(), 1);
    for (int i = 0; i < 200 && fence->GetCompletedValue() < 1; ++i) Sleep(5);
    if (fence->GetCompletedValue() < 1) return 0;
    void* data = nullptr;
    if (FAILED(readback->Map(0, nullptr, &data)) || !data) return 0;
    uint32_t texel = 0;
    const auto* row = static_cast<const uint8_t*>(data) + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch;
    std::memcpy(&texel, row + x * 4, sizeof(texel));
    const D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    return texel;
}

}  // namespace

// Reads the presented buffers back. That relies on a flip-discard buffer
// keeping its content after Present, which holds for a never-shown window
// because nothing composes it.
TEST(Presenter_FrameReachesTheD3D12BackBuffer) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    GameWindow window(320, 180);
    REQUIRE(window.Get() != nullptr);
    {
        auto p = CreatePresenter(d, window.Get(), 320, 180);
        REQUIRE(p != nullptr);
        Source src = CreateSource(d.device11.Get(), 320, 180);
        REQUIRE(src.rtv);
        // Exact n/255 values, so float-to-UNORM rounding cannot move a byte.
        const float colors[2][4] = {{51 / 255.0f, 102 / 255.0f, 153 / 255.0f, 1.0f},
                                    {204 / 255.0f, 26 / 255.0f, 77 / 255.0f, 1.0f}};
        const uint32_t expect[2] = {0xFF996633u, 0xFF4D1ACCu};  // R8G8B8A8 bytes, little-endian
        UINT idx[2] = {};
        for (int f = 0; f < 2; ++f) {
            idx[f] = p->Chain()->GetCurrentBackBufferIndex();
            d.ctx11->ClearRenderTargetView(src.rtv.Get(), colors[f]);
            CHECK(PresentOk(p->PresentFrame(d.ctx11.Get(), src.tex.Get(), 0, 0)));
        }
        CHECK(idx[0] != idx[1]);
        Sleep(100);
        for (int f = 0; f < 2; ++f) {
            ComPtr<ID3D12Resource> buffer;
            REQUIRE(SUCCEEDED(p->Chain()->GetBuffer(idx[f], IID_PPV_ARGS(&buffer))));
            const uint32_t texel = ReadTexel12(d.device12.Get(), buffer.Get(), 160, 90);
            std::printf("  frame %d buffer %u texel 0x%08X expect 0x%08X\n", f, idx[f], texel, expect[f]);
            CHECK_EQ(texel, expect[f]);
        }
    }
    d.ctx11->ClearState();
    d.ctx11->Flush();
}

// Review finding F1, end to end: with a capture in every frame, the frames
// stay in order while the D3D12 queue lags. The queue is held on frame 30
// (ACDLSSG_DEBUG_STALL_MS, below the watchdog's 500 ms) while frame 31 is
// captured and presented. D3D11 may copy frame 31 into the shared back
// buffer only after the D3D12 queue copied frame 30 out of it, so frame 30's
// D3D12 back buffer shows frame 30, and both frames reach the chain. (On the
// reference RTX 3080 this held before the fix too, apparently because the
// driver orders the two APIs' accesses to the shared texture; the fence
// behaviour itself is tested in Coordinator_ACaptureDoesNotReleaseTheWait-
// ForTheD3D12Queue.)
TEST(Presenter_ACaptureKeepsCspWaitingForTheD3D12Copy) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    acdb_test::TempDir dir(L"presenter_capture_wait");
    const std::wstring logPath = dir.Str() + L"\\bridge.log";
    REQUIRE(LogOpen(logPath, LogLevel::Info));
    uint32_t texel[2] = {};
    uint32_t expect[2] = {};
    {
        GameWindow window(320, 180);
        REQUIRE(window.Get() != nullptr);
        std::unique_ptr<D3D12Presenter> p;
        {
            EnvOverride env(L"ACDLSSG_DEBUG_STALL_MS", L"250");  // read at creation only
            p = CreatePresenter(d, window.Get(), 320, 180);
        }
        REQUIRE(p != nullptr);
        NgxEvaluateSink* sink = p->CaptureSink();
        REQUIRE(sink != nullptr);
        Source src = CreateSource(d.device11.Get(), 320, 180);
        CaptureSources cs = CreateCaptureSources(d.device11.Get(), 160, 90);
        REQUIRE(src.rtv && cs.depth && cs.mvec);
        UINT idx[2] = {};
        for (int f = 1; f <= 31; ++f) {
            // Exact n/255 values, so float-to-UNORM rounding cannot move a byte.
            const float color[4] = {f / 255.0f, (255 - f) / 255.0f, 128 / 255.0f, 1.0f};
            d.ctx11->ClearRenderTargetView(src.rtv.Get(), color);
            sink->OnEvaluate(EvaluateInputs(d.ctx11.Get(), cs));
            if (f >= 30) {
                idx[f - 30] = p->Chain()->GetCurrentBackBufferIndex();
                expect[f - 30] = 0xFF800000u | (static_cast<uint32_t>(255 - f) << 8) | static_cast<uint32_t>(f);
            }
            CHECK(PresentOk(p->PresentFrame(d.ctx11.Get(), src.tex.Get(), 0, 0)));
        }
        Sleep(700);  // the debug stall is over and the queue ran
        for (int f = 0; f < 2; ++f) {
            ComPtr<ID3D12Resource> buffer;
            REQUIRE(SUCCEEDED(p->Chain()->GetBuffer(idx[f], IID_PPV_ARGS(&buffer))));
            texel[f] = ReadTexel12(d.device12.Get(), buffer.Get(), 160, 90);
            std::printf("  frame %d buffer %u texel 0x%08X expect 0x%08X\n", 30 + f, idx[f], texel[f], expect[f]);
        }
        CHECK(!p->Stopped());
    }
    LogClose();
    d.ctx11->ClearState();
    d.ctx11->Flush();
    CHECK_EQ(texel[0], expect[0]);
    CHECK_EQ(texel[1], expect[1]);
    const std::string log = acdb_test::ReadAll(logPath);
    CHECK(log.find("debug stall: the D3D12 queue now waits 250 ms") != std::string::npos);
    CHECK(log.find("D3D12 stall:") == std::string::npos);  // the watchdog never fired
}

// ---------------------------------------------------------------- the panel (spec 6.9)

namespace {

std::wstring PanelSectionName(const wchar_t* kind) {
    static std::atomic<unsigned> counter{0};
    return std::wstring(L"Local\\AcDlssg.") + kind + L".presenter." + std::to_wstring(GetCurrentProcessId()) + L"." +
           std::to_wstring(counter++);
}

size_t Count(const std::string& text, const std::string& piece) {
    size_t n = 0;
    for (size_t pos = text.find(piece); pos != std::string::npos; pos = text.find(piece, pos + 1)) ++n;
    return n;
}

}  // namespace

// The Lua app's requests reach the presenter through the control section
// at the start of every frame; the presenter publishes its state into the
// status section. The plain path has no Streamline, so DLSS-G is
// unavailable (kPanelProxyNoFg), but the switches and the save work the same.
TEST(Presenter_AppliesPanelRequestsAndPublishesItsStatus) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    acdb_test::TempDir dir(L"presenter_panel");
    const std::wstring logPath = dir.Str() + L"\\bridge.log";
    const std::wstring iniPath =
        dir.Write(L"ac-dlssg.ini", "[bridge]\r\nstart_with_fg=1\r\nlog_level=info\r\n").wstring();
    const std::wstring statusName = PanelSectionName(L"Status");
    const std::wstring controlName = PanelSectionName(L"Control");
    PanelStatusChannel status(statusName.c_str());
    PanelControlChannel control(controlName.c_str());
    std::string err;
    REQUIRE(status.Create(&err));
    REQUIRE(control.Create(&err));
    testapp::FakePanel panel;
    REQUIRE(panel.Open(statusName.c_str(), controlName.c_str(), &err));
    // A request from before the presenter existed is never applied.
    panel.Request(false, true, true, true);
    REQUIRE(LogOpen(logPath, LogLevel::Info));
    StatusLayout s{};
    uint32_t beatBefore = 0;
    uint32_t beatAfter = 0;
    {
        GameWindow window(320, 180);
        REQUIRE(window.Get() != nullptr);
        PresenterCreateInfo info;
        info.device11 = d.device11.Get();
        info.hwnd = window.Get();
        info.game_desc = GameDesc(320, 180);
        info.config.start_with_fg = true;
        info.env.status = &status;
        info.env.control = &control;
        info.env.config_path = iniPath;
        auto p = D3D12Presenter::Create(info, &err);
        if (!p) std::printf("  D3D12Presenter::Create: %s\n", err.c_str());
        REQUIRE(p != nullptr);
        Source src = CreateSource(d.device11.Get(), 320, 180);
        REQUIRE(src.rtv);
        const auto frames = [&](int n) {
            for (int i = 0; i < n; ++i) CHECK(PresentOk(Frame(p.get(), d.ctx11.Get(), src, i)));
        };

        REQUIRE(panel.ReadStatus(&s));
        CHECK_EQ(s.bridgeState, static_cast<uint32_t>(kPanelProxyNoFg));
        CHECK_EQ(s.fgUserOn, 1u);
        CHECK_EQ(s.fgOn, 0u);
        CHECK_EQ(s.startWithFg, 1u);
        CHECK_EQ(s.controlApplied, panel.Counter());  // the baseline, not applied
        CHECK(!TextOf(s.stateReason).empty());
        CHECK(!TextOf(s.gpuName).empty());
        CHECK(TextOf(s.hotkey) == "Ctrl+F10");
        CHECK(TextOf(s.reason) == "not supported on this adapter");
        frames(3);
        REQUIRE(panel.ReadStatus(&s));
        CHECK_EQ(s.fgUserOn, 1u);
        CHECK_EQ(s.cameraFlipHandedness, 0u);

        // Off, as the switch in the window does it.
        panel.Request(false, false, false, false);
        frames(1);
        REQUIRE(panel.ReadStatus(&s));
        CHECK_EQ(s.controlApplied, panel.Counter());
        CHECK_EQ(s.fgUserOn, 0u);
        CHECK(TextOf(s.reason) == "off by the user (panel)");

        // A camera switch; the DLSS-G switch stays off.
        panel.Request(false, true, false, false);
        frames(1);
        REQUIRE(panel.ReadStatus(&s));
        CHECK_EQ(s.cameraFlipHandedness, 1u);
        CHECK_EQ(s.cameraNegateSide, 0u);
        CHECK_EQ(s.fgUserOn, 0u);

        // A request the app is still writing (odd seq) waits for the next frame.
        panel.Request(false, true, true, false);
        const uint32_t seq = panel.ControlSeq();
        panel.SetControlSeqForTest(seq - 1);
        frames(2);
        REQUIRE(panel.ReadStatus(&s));
        CHECK_EQ(s.cameraNegateSide, 0u);
        CHECK(s.controlApplied != panel.Counter());
        panel.SetControlSeqForTest(seq);
        frames(1);
        REQUIRE(panel.ReadStatus(&s));
        CHECK_EQ(s.cameraNegateSide, 1u);
        panel.Request(false, true, false, false);
        frames(1);

        // Save as default: the current switches go into ac-dlssg.ini.
        panel.Request(false, true, false, true);
        frames(1);
        REQUIRE(panel.ReadStatus(&s));
        CHECK_EQ(s.saveCounter, panel.Counter());
        CHECK_EQ(s.saveOk, 1u);
        CHECK_EQ(s.startWithFg, 0u);
        CHECK(acdb_test::ReadAll(iniPath) ==
              "[bridge]\r\nstart_with_fg=0\r\nlog_level=info\r\ncamera_flip_handedness=1\r\ncamera_negate_side=0\r\n");

        // On again; then the same state once more changes nothing.
        panel.Request(true, true, false, false);
        frames(1);
        panel.Request(true, true, false, false);
        frames(1);
        REQUIRE(panel.ReadStatus(&s));
        CHECK_EQ(s.controlApplied, panel.Counter());
        CHECK_EQ(s.fgUserOn, 1u);
        CHECK_EQ(s.cameraFlipHandedness, 1u);

        // Without requests the statistics path keeps the heartbeat going.
        beatBefore = s.heartbeat;
        const ULONGLONG start = GetTickCount64();
        for (int i = 0; GetTickCount64() - start < 1300; ++i) {
            CHECK(PresentOk(Frame(p.get(), d.ctx11.Get(), src, i)));
            Sleep(2);
        }
        REQUIRE(panel.ReadStatus(&s));
        beatAfter = s.heartbeat;
        CHECK(s.baseFps > 0.0f);
    }
    LogClose();
    d.ctx11->ClearState();
    d.ctx11->Flush();
    std::printf("  heartbeat %u -> %u\n", beatBefore, beatAfter);
    CHECK(beatAfter > beatBefore);
    // Released: the next chain passes through (spec 6.3 "Final Release").
    REQUIRE(panel.ReadStatus(&s));
    CHECK_EQ(s.bridgeState, static_cast<uint32_t>(kPanelPassThrough));
    CHECK_EQ(s.fgOn, 0u);
    CHECK(TextOf(s.stateReason) == "the game's swap chain was released");

    const std::string log = acdb_test::ReadAll(logPath);
    CHECK_EQ(Count(log, " INFO fg: panel -> off\n"), 1u);
    CHECK_EQ(Count(log, " INFO fg: panel -> on\n"), 1u);
    CHECK_EQ(Count(log, " INFO panel: camera_flip_handedness 0 -> 1\n"), 1u);
    CHECK_EQ(Count(log, " INFO panel: camera_negate_side 0 -> 1\n"), 1u);
    CHECK_EQ(Count(log, " INFO panel: camera_negate_side 1 -> 0\n"), 1u);
    std::string narrowIni;
    for (wchar_t c : iniPath) narrowIni.push_back(static_cast<char>(c));
    CHECK_EQ(Count(log, " INFO panel: saved start_with_fg=0 camera_flip_handedness=1 camera_negate_side=0 to " +
                            narrowIni + "\n"),
             1u);
    if (Count(log, "fg: panel -> ") != 2) std::printf("  log:\n%s\n", log.c_str());
}

// A failed save is reported in the status and the log, and changes nothing.
TEST(Presenter_ReportsAFailedSaveAsDefault) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    acdb_test::TempDir dir(L"presenter_panel_save");
    const std::wstring logPath = dir.Str() + L"\\bridge.log";
    const std::wstring iniPath = dir.Str() + L"\\ac-dlssg.ini";
    REQUIRE(CreateDirectoryW(iniPath.c_str(), nullptr));  // a folder where the file should be
    const std::wstring statusName = PanelSectionName(L"Status");
    const std::wstring controlName = PanelSectionName(L"Control");
    PanelStatusChannel status(statusName.c_str());
    PanelControlChannel control(controlName.c_str());
    std::string err;
    REQUIRE(status.Create(&err));
    REQUIRE(control.Create(&err));
    testapp::FakePanel panel;
    REQUIRE(panel.Open(statusName.c_str(), controlName.c_str(), &err));
    REQUIRE(LogOpen(logPath, LogLevel::Info));
    StatusLayout s{};
    {
        GameWindow window(160, 90);
        REQUIRE(window.Get() != nullptr);
        PresenterCreateInfo info;
        info.device11 = d.device11.Get();
        info.hwnd = window.Get();
        info.game_desc = GameDesc(160, 90);
        info.env.status = &status;
        info.env.control = &control;
        info.env.config_path = iniPath;
        auto p = D3D12Presenter::Create(info, &err);
        REQUIRE(p != nullptr);
        Source src = CreateSource(d.device11.Get(), 160, 90);
        REQUIRE(src.rtv);
        panel.Request(true, false, false, true);
        CHECK(PresentOk(Frame(p.get(), d.ctx11.Get(), src, 0)));
        REQUIRE(panel.ReadStatus(&s));
        CHECK_EQ(s.saveCounter, panel.Counter());
        CHECK_EQ(s.saveOk, 0u);
        CHECK_EQ(s.startWithFg, 1u);
    }
    LogClose();
    d.ctx11->ClearState();
    d.ctx11->Flush();
    const std::string log = acdb_test::ReadAll(logPath);
    CHECK_EQ(Count(log, " WARN panel: Save as default failed: "), 1u);
}

// Without the channels (the unit tests' default) the presenter publishes
// nothing and reads no requests.
TEST(Presenter_PanelChannelsAreOffByDefault) {
    PresenterEnvironment env;
    CHECK(env.status == nullptr);
    CHECK(env.control == nullptr);
    CHECK(env.config_path.empty());
}
