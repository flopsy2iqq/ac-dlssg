// ProxySwapChain created directly (not through the factory hook) with CSP's
// swap chain description, on a hidden window of class "acsW".
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <regex>
#include <string>

#include "config.h"
#include "gpu_test_devices.h"
#include "log.h"
#include "proxy_swapchain.h"
#include "temp_dir.h"
#include "test_framework.h"

using Microsoft::WRL::ComPtr;
using namespace acdb;

namespace {

constexpr UINT kCspFlags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

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

// A hidden top-level window of class "acsW" with the given client size.
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
        hwnd_ = CreateWindowExW(0, L"acsW", L"acdb proxy test", WS_OVERLAPPEDWINDOW, 0, 0, rc.right - rc.left,
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

// CSP's main swap chain (spec section 4).
DXGI_SWAP_CHAIN_DESC1 CspDesc(UINT w = 1280, UINT h = 720, UINT flags = kCspFlags) {
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
    d.Flags = flags;
    return d;
}

bool PresentOk(HRESULT hr) { return hr == S_OK || hr == DXGI_STATUS_OCCLUDED; }

// Devices, a game window and the proxy on it.
struct Fixture {
    acdb_test::GpuTestDevices dev;
    std::unique_ptr<GameWindow> window;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGISwapChain1> chain;

    bool Init(const DXGI_SWAP_CHAIN_DESC1& desc, const Config& config = Config()) {
        if (!acdb_test::CreateGpuTestDevices(&dev)) {
            std::printf("  SKIP: no adapter creates the test devices\n");
            return false;
        }
        window = std::make_unique<GameWindow>(static_cast<int>(desc.Width), static_cast<int>(desc.Height));
        if (!window->Get() || FAILED(dev.factory.As(&factory))) return false;
        std::string err;
        const HRESULT hr = ProxySwapChain::Create(factory.Get(), dev.device11.Get(), window->Get(), desc, nullptr,
                                                  config, nullptr, chain.GetAddressOf(), &err);
        if (FAILED(hr))
            std::printf("  ProxySwapChain::Create:0x%08lX %s\n", static_cast<unsigned long>(hr), err.c_str());
        return SUCCEEDED(hr) && chain;
    }
    ~Fixture() {
        chain.Reset();
        if (dev.ctx11) {
            dev.ctx11->ClearState();
            dev.ctx11->Flush();
        }
    }
};

ComPtr<ID3D11RenderTargetView> BufferRtv(IDXGISwapChain1* chain, ID3D11Device* dev) {
    ComPtr<ID3D11Texture2D> buffer;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&buffer)))) dev->CreateRenderTargetView(buffer.Get(), nullptr, &rtv);
    return rtv;
}

HRESULT Frame(IDXGISwapChain1* chain, ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, int i) {
    const float color[4] = {static_cast<float>(i % 60) / 60.0f, 0.5f, 0.25f, 1.0f};
    ctx->ClearRenderTargetView(rtv, color);
    const HRESULT hr = chain->Present(0, 0);
    PumpMessages();
    return hr;
}

// Threads whose start address lies in this exe: with the static CRT, every
// std::thread of the bridge code starts in the CRT's thread_start here.
int ThreadsStartedInExe() {
    using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    const auto query = reinterpret_cast<NtQueryInformationThreadFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    if (!query) return -1;
    const auto* base = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const BYTE* end = base + nt->OptionalHeader.SizeOfImage;

    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    int count = 0;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        const HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        void* start = nullptr;
        constexpr int kThreadQuerySetWin32StartAddress = 9;
        if (query(h, kThreadQuerySetWin32StartAddress, &start, sizeof(start), nullptr) == 0) {
            const auto* p = static_cast<const BYTE*>(start);
            if (p >= base && p < end) ++count;
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    return count;
}

int CountHiddenWindows() {
    int count = 0;
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            wchar_t cls[64] = {};
            if (pid == GetCurrentProcessId() && GetClassNameW(hwnd, cls, 64) > 0 &&
                std::wcscmp(cls, L"acdb_hidden") == 0)
                ++*reinterpret_cast<int*>(lp);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&count));
    return count;
}

}  // namespace

TEST(ProxySwapChain_InterfacesAndDescriptions) {
    Fixture f;
    if (!f.Init(CspDesc())) {
        REQUIRE(!f.dev.device11);  // only a missing GPU may skip
        return;
    }
    IDXGISwapChain1* chain = f.chain.Get();

    const IID accepted[] = {__uuidof(IUnknown),       __uuidof(IDXGIObject),     __uuidof(IDXGIDeviceSubObject),
                            __uuidof(IDXGISwapChain), __uuidof(IDXGISwapChain1), __uuidof(IDXGISwapChain2),
                            __uuidof(IDXGISwapChain3), __uuidof(IDXGISwapChain4)};
    for (const IID& iid : accepted) {
        ComPtr<IUnknown> out;
        CHECK(SUCCEEDED(chain->QueryInterface(iid, reinterpret_cast<void**>(out.GetAddressOf()))));
        CHECK(out.Get() == static_cast<IUnknown*>(chain));
    }
    ComPtr<IUnknown> none;
    CHECK_EQ(chain->QueryInterface(__uuidof(ID3D11Device), reinterpret_cast<void**>(none.GetAddressOf())),
             E_NOINTERFACE);
    CHECK(!none);
    CHECK_EQ(chain->QueryInterface(__uuidof(IDXGIFactory), reinterpret_cast<void**>(none.GetAddressOf())),
             E_NOINTERFACE);

    DXGI_SWAP_CHAIN_DESC1 d1{};
    CHECK(SUCCEEDED(chain->GetDesc1(&d1)));
    CHECK_EQ(d1.Width, 1280u);
    CHECK_EQ(d1.Height, 720u);
    CHECK_EQ(d1.Format, DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK_EQ(d1.BufferCount, 2u);
    CHECK_EQ(d1.SwapEffect, DXGI_SWAP_EFFECT_FLIP_DISCARD);
    CHECK_EQ(d1.Flags, 0x840u);

    DXGI_SWAP_CHAIN_DESC d{};
    CHECK(SUCCEEDED(chain->GetDesc(&d)));
    CHECK_EQ(d.BufferDesc.Width, 1280u);
    CHECK_EQ(d.BufferDesc.Format, DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK_EQ(d.BufferCount, 2u);
    CHECK_EQ(d.OutputWindow, f.window->Get());
    CHECK(d.Windowed == TRUE);
    CHECK_EQ(d.Flags, 0x840u);

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs{};
    CHECK(SUCCEEDED(chain->GetFullscreenDesc(&fs)));
    CHECK(fs.Windowed == TRUE);
    BOOL fullscreen = TRUE;
    CHECK(SUCCEEDED(chain->GetFullscreenState(&fullscreen, nullptr)));
    CHECK(fullscreen == FALSE);

    HWND hwnd = nullptr;
    CHECK(SUCCEEDED(chain->GetHwnd(&hwnd)));
    CHECK_EQ(hwnd, f.window->Get());

    ComPtr<IDXGIFactory2> parent;
    CHECK(SUCCEEDED(chain->GetParent(IID_PPV_ARGS(&parent))));
    CHECK(parent.Get() == f.factory.Get());
    ComPtr<ID3D11Device> device;
    CHECK(SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&device))));
    CHECK(device.Get() == f.dev.device11.Get());

    ComPtr<IDXGISwapChain4> chain4;
    REQUIRE(SUCCEEDED(f.chain.As(&chain4)));
    CHECK_EQ(chain4->GetCurrentBackBufferIndex(), 0u);
    ComPtr<ID3D11Texture2D> other;
    CHECK_EQ(chain->GetBuffer(1, IID_PPV_ARGS(&other)), DXGI_ERROR_INVALID_CALL);
    CHECK_EQ(chain4->ResizeBuffers1(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0, nullptr, nullptr), DXGI_ERROR_INVALID_CALL);
    UINT support = 0;
    CHECK(SUCCEEDED(chain4->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &support)));
    CHECK((support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0);
    CHECK(SUCCEEDED(chain4->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)));

    // Private data lives in the proxy (ReShade keeps its wrapper pointer there).
    const GUID key = {0x1b2c3d4e, 0x1111, 0x2222, {1, 2, 3, 4, 5, 6, 7, 8}};
    const GUID ikey = {0x1b2c3d4e, 0x1111, 0x2223, {1, 2, 3, 4, 5, 6, 7, 8}};
    const unsigned value = 0xC0FFEE;
    CHECK(SUCCEEDED(chain->SetPrivateData(key, sizeof(value), &value)));
    UINT size = 0;
    CHECK(SUCCEEDED(chain->GetPrivateData(key, &size, nullptr)));
    CHECK_EQ(size, static_cast<UINT>(sizeof(value)));
    unsigned got = 0;
    size = 1;
    CHECK_EQ(chain->GetPrivateData(key, &size, &got), DXGI_ERROR_MORE_DATA);
    size = sizeof(got);
    CHECK(SUCCEEDED(chain->GetPrivateData(key, &size, &got)));
    CHECK_EQ(got, value);
    CHECK(SUCCEEDED(chain->SetPrivateData(key, 0, nullptr)));
    size = sizeof(got);
    CHECK_EQ(chain->GetPrivateData(key, &size, &got), DXGI_ERROR_NOT_FOUND);
    CHECK(SUCCEEDED(chain->SetPrivateDataInterface(ikey, f.dev.device11.Get())));
    IUnknown* stored = nullptr;
    size = sizeof(stored);
    CHECK(SUCCEEDED(chain->GetPrivateData(ikey, &size, &stored)));
    CHECK(stored != nullptr);
    if (stored) {
        ComPtr<ID3D11Device> same;
        CHECK(SUCCEEDED(stored->QueryInterface(IID_PPV_ARGS(&same))));
        CHECK(same.Get() == f.dev.device11.Get());
        stored->Release();
    }
    CHECK(SUCCEEDED(chain->SetPrivateDataInterface(ikey, nullptr)));
}

TEST(ProxySwapChain_Buffer0TakesNullAndSrgbViews) {
    Fixture f;
    if (!f.Init(CspDesc())) {
        REQUIRE(!f.dev.device11);
        return;
    }
    ComPtr<ID3D11Texture2D> buffer;
    REQUIRE(SUCCEEDED(f.chain->GetBuffer(0, IID_PPV_ARGS(&buffer))));
    D3D11_TEXTURE2D_DESC td{};
    buffer->GetDesc(&td);
    CHECK_EQ(td.Width, 1280u);
    CHECK_EQ(td.Height, 720u);
    CHECK_EQ(td.Format, DXGI_FORMAT_R8G8B8A8_UNORM);

    // CSP: NULL description. ReShade: UNORM and UNORM_SRGB views.
    ComPtr<ID3D11RenderTargetView> rtvNull;
    CHECK(SUCCEEDED(f.dev.device11->CreateRenderTargetView(buffer.Get(), nullptr, &rtvNull)));
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    ComPtr<ID3D11RenderTargetView> rtvSrgb;
    CHECK(SUCCEEDED(f.dev.device11->CreateRenderTargetView(buffer.Get(), &rd, &rtvSrgb)));
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D11RenderTargetView> rtvUnorm;
    CHECK(SUCCEEDED(f.dev.device11->CreateRenderTargetView(buffer.Get(), &rd, &rtvUnorm)));

    // The same buffer on every call, also through another interface.
    ComPtr<IDXGISurface> surface;
    CHECK(SUCCEEDED(f.chain->GetBuffer(0, IID_PPV_ARGS(&surface))));
    ComPtr<ID3D11Texture2D> again;
    CHECK(SUCCEEDED(f.chain->GetBuffer(0, IID_PPV_ARGS(&again))));
    CHECK(again.Get() == buffer.Get());
}

TEST(ProxySwapChain_120FramesNeverBlock) {
    Fixture f;
    if (!f.Init(CspDesc())) {
        REQUIRE(!f.dev.device11);
        return;
    }
    ComPtr<ID3D11RenderTargetView> rtv = BufferRtv(f.chain.Get(), f.dev.device11.Get());
    REQUIRE(rtv);
    ComPtr<IDXGISwapChain2> chain2;
    REQUIRE(SUCCEEDED(f.chain.As(&chain2)));
    const HANDLE waitable = chain2->GetFrameLatencyWaitableObject();
    REQUIRE(waitable != nullptr);

    ULONGLONG longest = 0;
    int timeouts = 0;
    int bad = 0;
    const ULONGLONG start = GetTickCount64();
    for (int i = 0; i < 120; ++i) {
        const ULONGLONG t0 = GetTickCount64();
        if (WaitForSingleObject(waitable, 1000) != WAIT_OBJECT_0) ++timeouts;
        const HRESULT hr = Frame(f.chain.Get(), f.dev.ctx11.Get(), rtv.Get(), i);
        if (!PresentOk(hr) && bad++ == 0) std::printf("  Present: 0x%08lX\n", static_cast<unsigned long>(hr));
        const ULONGLONG dt = GetTickCount64() - t0;
        if (dt > longest) longest = dt;
    }
    std::printf("  120 frames in %llu ms, longest %llu ms\n", GetTickCount64() - start, longest);
    CHECK_EQ(timeouts, 0);
    CHECK_EQ(bad, 0);
    CHECK(longest < 1000);
    UINT count = 0;
    CHECK(SUCCEEDED(f.chain->GetLastPresentCount(&count)));
    CHECK_EQ(count, 120u);
    CloseHandle(waitable);
}

TEST(ProxySwapChain_TestPresentDoesNotReleaseTheSemaphore) {
    Fixture f;
    if (!f.Init(CspDesc())) {
        REQUIRE(!f.dev.device11);
        return;
    }
    ComPtr<ID3D11RenderTargetView> rtv = BufferRtv(f.chain.Get(), f.dev.device11.Get());
    REQUIRE(rtv);
    ComPtr<IDXGISwapChain2> chain2;
    REQUIRE(SUCCEEDED(f.chain.As(&chain2)));
    const HANDLE waitable = chain2->GetFrameLatencyWaitableObject();
    REQUIRE(waitable != nullptr);

    // Latency 1: the initial count lets the first wait through at once.
    CHECK_EQ(WaitForSingleObject(waitable, 0), static_cast<DWORD>(WAIT_OBJECT_0));
    CHECK_EQ(WaitForSingleObject(waitable, 0), static_cast<DWORD>(WAIT_TIMEOUT));
    CHECK_EQ(f.chain->Present(0, DXGI_PRESENT_TEST), S_OK);
    CHECK_EQ(f.chain->Present(1, DXGI_PRESENT_TEST), S_OK);
    CHECK_EQ(WaitForSingleObject(waitable, 0), static_cast<DWORD>(WAIT_TIMEOUT));
    UINT count = 99;
    CHECK(SUCCEEDED(f.chain->GetLastPresentCount(&count)));
    CHECK_EQ(count, 0u);

    CHECK(PresentOk(Frame(f.chain.Get(), f.dev.ctx11.Get(), rtv.Get(), 0)));
    CHECK_EQ(WaitForSingleObject(waitable, 0), static_cast<DWORD>(WAIT_OBJECT_0));
    CHECK_EQ(WaitForSingleObject(waitable, 0), static_cast<DWORD>(WAIT_TIMEOUT));
    // Present1 is a frame too.
    DXGI_PRESENT_PARAMETERS params{};
    CHECK(PresentOk(f.chain->Present1(0, 0, &params)));
    CHECK_EQ(WaitForSingleObject(waitable, 0), static_cast<DWORD>(WAIT_OBJECT_0));
    CHECK(SUCCEEDED(f.chain->GetLastPresentCount(&count)));
    CHECK_EQ(count, 2u);
    CloseHandle(waitable);
}

TEST(ProxySwapChain_ResizeBuffersKeepsPresenting) {
    Fixture f;
    if (!f.Init(CspDesc())) {
        REQUIRE(!f.dev.device11);
        return;
    }
    ComPtr<ID3D11RenderTargetView> rtv = BufferRtv(f.chain.Get(), f.dev.device11.Get());
    REQUIRE(rtv);
    for (int i = 0; i < 10; ++i) CHECK(PresentOk(Frame(f.chain.Get(), f.dev.ctx11.Get(), rtv.Get(), i)));

    // Like a game: every view on the old buffer goes before the resize.
    rtv.Reset();
    f.dev.ctx11->ClearState();
    f.dev.ctx11->Flush();
    const HRESULT hr = f.chain->ResizeBuffers(0, 1600, 900, DXGI_FORMAT_UNKNOWN, 0x840);
    if (FAILED(hr)) std::printf("  ResizeBuffers: 0x%08lX\n", static_cast<unsigned long>(hr));
    REQUIRE(SUCCEEDED(hr));

    DXGI_SWAP_CHAIN_DESC1 d1{};
    CHECK(SUCCEEDED(f.chain->GetDesc1(&d1)));
    CHECK_EQ(d1.Width, 1600u);
    CHECK_EQ(d1.Height, 900u);
    CHECK_EQ(d1.Format, DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK_EQ(d1.BufferCount, 2u);
    CHECK_EQ(d1.Flags, 0x840u);
    DXGI_SWAP_CHAIN_DESC d{};
    CHECK(SUCCEEDED(f.chain->GetDesc(&d)));
    CHECK_EQ(d.BufferDesc.Width, 1600u);
    CHECK_EQ(d.BufferDesc.Height, 900u);

    ComPtr<ID3D11Texture2D> buffer;
    REQUIRE(SUCCEEDED(f.chain->GetBuffer(0, IID_PPV_ARGS(&buffer))));
    D3D11_TEXTURE2D_DESC td{};
    buffer->GetDesc(&td);
    CHECK_EQ(td.Width, 1600u);
    CHECK_EQ(td.Height, 900u);
    buffer.Reset();

    rtv = BufferRtv(f.chain.Get(), f.dev.device11.Get());
    REQUIRE(rtv);
    for (int i = 0; i < 20; ++i) CHECK(PresentOk(Frame(f.chain.Get(), f.dev.ctx11.Get(), rtv.Get(), i)));

    // Zero size takes the window's client area.
    rtv.Reset();
    f.dev.ctx11->ClearState();
    f.dev.ctx11->Flush();
    RECT rc{};
    GetClientRect(f.window->Get(), &rc);
    CHECK(SUCCEEDED(f.chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0x840)));
    CHECK(SUCCEEDED(f.chain->GetDesc1(&d1)));
    CHECK_EQ(d1.Width, static_cast<UINT>(rc.right - rc.left));
    CHECK_EQ(d1.Height, static_cast<UINT>(rc.bottom - rc.top));
    rtv = BufferRtv(f.chain.Get(), f.dev.device11.Get());
    REQUIRE(rtv);
    for (int i = 0; i < 5; ++i) CHECK(PresentOk(Frame(f.chain.Get(), f.dev.ctx11.Get(), rtv.Get(), i)));
}

// A view on buffer 0 still alive: DXGI refuses the resize of the hidden chain,
// and then nothing may change, as with a real chain; frames keep being copied
// at the old size (ReShade treats this error as recoverable).
TEST(ProxySwapChain_FailedResizeChangesNothing) {
    acdb_test::TempDir dir(L"proxy_resize_fail");
    const std::wstring logPath = dir.Str() + L"\\bridge.log";
    REQUIRE(LogOpen(logPath, LogLevel::Info));
    bool ran = false;
    {
        Fixture f;
        if (f.Init(CspDesc())) {
            ran = true;
            ComPtr<ID3D11RenderTargetView> rtv = BufferRtv(f.chain.Get(), f.dev.device11.Get());
            REQUIRE(rtv);
            for (int i = 0; i < 5; ++i) CHECK(PresentOk(Frame(f.chain.Get(), f.dev.ctx11.Get(), rtv.Get(), i)));
            f.dev.ctx11->ClearState();
            f.dev.ctx11->Flush();

            CHECK_EQ(f.chain->ResizeBuffers(0, 1600, 900, DXGI_FORMAT_UNKNOWN, 0x840), DXGI_ERROR_INVALID_CALL);
            DXGI_SWAP_CHAIN_DESC1 d1{};
            CHECK(SUCCEEDED(f.chain->GetDesc1(&d1)));
            CHECK_EQ(d1.Width, 1280u);
            CHECK_EQ(d1.Height, 720u);
            ComPtr<ID3D11Texture2D> buffer;
            REQUIRE(SUCCEEDED(f.chain->GetBuffer(0, IID_PPV_ARGS(&buffer))));
            D3D11_TEXTURE2D_DESC td{};
            buffer->GetDesc(&td);
            CHECK_EQ(td.Width, 1280u);
            CHECK_EQ(td.Height, 720u);
            buffer.Reset();
            // GetSourceSize is forwarded to the D3D12 chain.
            ComPtr<IDXGISwapChain2> chain2;
            REQUIRE(SUCCEEDED(f.chain.As(&chain2)));
            UINT sw = 0;
            UINT sh = 0;
            CHECK(SUCCEEDED(chain2->GetSourceSize(&sw, &sh)));
            CHECK_EQ(sw, 1280u);
            CHECK_EQ(sh, 720u);
            for (int i = 0; i < 10; ++i) CHECK(PresentOk(Frame(f.chain.Get(), f.dev.ctx11.Get(), rtv.Get(), i)));

            // Without the view the same resize succeeds on both sides.
            rtv.Reset();
            f.dev.ctx11->ClearState();
            f.dev.ctx11->Flush();
            CHECK(SUCCEEDED(f.chain->ResizeBuffers(0, 1600, 900, DXGI_FORMAT_UNKNOWN, 0x840)));
            CHECK(SUCCEEDED(chain2->GetSourceSize(&sw, &sh)));
            CHECK_EQ(sw, 1600u);
            CHECK_EQ(sh, 900u);
            rtv = BufferRtv(f.chain.Get(), f.dev.device11.Get());
            REQUIRE(rtv);
            for (int i = 0; i < 5; ++i) CHECK(PresentOk(Frame(f.chain.Get(), f.dev.ctx11.Get(), rtv.Get(), i)));
        } else {
            REQUIRE(!f.dev.device11);
        }
    }
    LogClose();
    if (!ran) return;
    const std::string log = acdb_test::ReadAll(logPath);
    CHECK(log.find("hidden chain resize failed: 0x887A0001") != std::string::npos);
    CHECK(log.find("frames are not copied") == std::string::npos);
    CHECK(log.find("presenter resized to 1600x900") != std::string::npos);
    CHECK(log.find("presenter stopped") == std::string::npos);
    // The failed call must not have touched the presenter.
    CHECK_EQ(log.find("presenter resized"), log.rfind("presenter resized"));
    if (log.find("frames are not copied") != std::string::npos) std::printf("  log:\n%s\n", log.c_str());
}

TEST(ProxySwapChain_FrameLatency) {
    acdb_test::TempDir dir(L"proxy_latency");
    const std::wstring logPath = dir.Str() + L"\\bridge.log";
    REQUIRE(LogOpen(logPath, LogLevel::Info));
    bool ran = false;
    {
        Fixture f;
        if (f.Init(CspDesc())) {
            ran = true;
            ComPtr<IDXGISwapChain2> chain2;
            REQUIRE(SUCCEEDED(f.chain.As(&chain2)));
            UINT latency = 0;
            CHECK(SUCCEEDED(chain2->GetMaximumFrameLatency(&latency)));
            CHECK_EQ(latency, 1u);
            CHECK(SUCCEEDED(chain2->SetMaximumFrameLatency(3)));
            CHECK(SUCCEEDED(chain2->GetMaximumFrameLatency(&latency)));
            CHECK_EQ(latency, 3u);
            // The initial count of 1 plus the 2 released by the raise.
            const HANDLE waitable = chain2->GetFrameLatencyWaitableObject();
            REQUIRE(waitable != nullptr);
            for (int i = 0; i < 3; ++i) CHECK_EQ(WaitForSingleObject(waitable, 0), static_cast<DWORD>(WAIT_OBJECT_0));
            CHECK_EQ(WaitForSingleObject(waitable, 0), static_cast<DWORD>(WAIT_TIMEOUT));
            CloseHandle(waitable);
            // Frames still flow with the D3D12 chain paced at latency 3.
            ComPtr<ID3D11RenderTargetView> rtv = BufferRtv(f.chain.Get(), f.dev.device11.Get());
            REQUIRE(rtv);
            for (int i = 0; i < 10; ++i) CHECK(PresentOk(Frame(f.chain.Get(), f.dev.ctx11.Get(), rtv.Get(), i)));
        } else {
            REQUIRE(!f.dev.device11);
        }
    }
    LogClose();
    if (!ran) return;
    // The game's latency reaches the D3D12 chain's own waitable object.
    const std::string log = acdb_test::ReadAll(logPath);
    CHECK(log.find("frame-latency object yes") != std::string::npos);
    CHECK(log.find("presenter: D3D12 chain frame latency 3") != std::string::npos);
    CHECK(log.find(" WARN ") == std::string::npos);
}

TEST(ProxySwapChain_WithoutWaitableFlagHasNoLatencyObject) {
    Fixture f;
    if (!f.Init(CspDesc(640, 360, DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING))) {
        REQUIRE(!f.dev.device11);
        return;
    }
    ComPtr<IDXGISwapChain2> chain2;
    REQUIRE(SUCCEEDED(f.chain.As(&chain2)));
    CHECK(chain2->GetFrameLatencyWaitableObject() == nullptr);
    UINT latency = 0;
    CHECK_EQ(chain2->GetMaximumFrameLatency(&latency), DXGI_ERROR_INVALID_CALL);
    CHECK_EQ(chain2->SetMaximumFrameLatency(2), DXGI_ERROR_INVALID_CALL);
    ComPtr<ID3D11RenderTargetView> rtv = BufferRtv(f.chain.Get(), f.dev.device11.Get());
    REQUIRE(rtv);
    for (int i = 0; i < 5; ++i) CHECK(PresentOk(Frame(f.chain.Get(), f.dev.ctx11.Get(), rtv.Get(), i)));
}

TEST(ProxySwapChain_DebugStallIsReleasedAndFramesContinue) {
    acdb_test::TempDir dir(L"proxy_stall");
    const std::wstring logPath = dir.Str() + L"\\bridge.log";
    REQUIRE(LogOpen(logPath, LogLevel::Info));
    bool ran = false;
    {
        Fixture f;
        bool ok = false;
        {
            EnvOverride env(L"ACDLSSG_DEBUG_STALL_MS", L"1500");
            ok = f.Init(CspDesc());
        }
        if (ok) {
            ran = true;
            ComPtr<ID3D11RenderTargetView> rtv = BufferRtv(f.chain.Get(), f.dev.device11.Get());
            REQUIRE(rtv);
            ComPtr<IDXGISwapChain2> chain2;
            REQUIRE(SUCCEEDED(f.chain.As(&chain2)));
            const HANDLE waitable = chain2->GetFrameLatencyWaitableObject();
            REQUIRE(waitable != nullptr);

            ULONGLONG longest = 0;
            int frames = 0;
            int bad = 0;
            HRESULT last = S_OK;
            const ULONGLONG start = GetTickCount64();
            while (GetTickCount64() - start < 4000) {
                const ULONGLONG t0 = GetTickCount64();
                WaitForSingleObject(waitable, 3000);
                last = Frame(f.chain.Get(), f.dev.ctx11.Get(), rtv.Get(), frames++);
                if (!PresentOk(last) && bad++ == 0)
                    std::printf("  Present: 0x%08lX\n", static_cast<unsigned long>(last));
                const ULONGLONG dt = GetTickCount64() - t0;
                if (dt > longest) longest = dt;
                Sleep(2);
            }
            std::printf("  %d frames in 4 s, longest %llu ms\n", frames, longest);
            CHECK_EQ(bad, 0);
            CHECK(PresentOk(last));
            CHECK(longest < 2500);  // released, not blocked for the whole stall
            CHECK(frames > 60);
            CloseHandle(waitable);
        } else {
            REQUIRE(!f.dev.device11);
        }
    }
    LogClose();
    if (!ran) return;
    const std::string log = acdb_test::ReadAll(logPath);
    CHECK(log.find("debug stall") != std::string::npos);
    CHECK(log.find("D3D12 stall:") != std::string::npos);
    CHECK(log.find("D3D12 stall over") != std::string::npos);
    CHECK(log.find("presenter stopped") == std::string::npos);
    CHECK(log.find("did not drain") == std::string::npos);
    // The frames CSP presented during the stall are counted as not delivered.
    CHECK(std::regex_search(log, std::regex(" stats: base_fps=[0-9.]+ presented_fps=[0-9.]+ skipped=[1-9]")));
    if (log.find("D3D12 stall:") == std::string::npos) std::printf("  log:\n%s\n", log.c_str());
}

TEST(ProxySwapChain_ReleaseDestroysEverythingAndJoinsThreads) {
    const int threadsBefore = ThreadsStartedInExe();
    const int windowsBefore = CountHiddenWindows();
    const long liveBefore = ProxySwapChain::LiveCount();
    CHECK_EQ(liveBefore, 0);
    {
        Fixture f;
        if (!f.Init(CspDesc())) {
            REQUIRE(!f.dev.device11);
            return;
        }
        CHECK_EQ(ProxySwapChain::LiveCount(), 1);
        ComPtr<ID3D11RenderTargetView> rtv = BufferRtv(f.chain.Get(), f.dev.device11.Get());
        REQUIRE(rtv);
        for (int i = 0; i < 5; ++i) CHECK(PresentOk(Frame(f.chain.Get(), f.dev.ctx11.Get(), rtv.Get(), i)));
        // Window thread and watchdog.
        const int threadsDuring = ThreadsStartedInExe();
        std::printf("  threads started in the exe: %d before, %d during\n", threadsBefore, threadsDuring);
        CHECK(threadsDuring >= threadsBefore + 2);
        CHECK_EQ(CountHiddenWindows(), windowsBefore + 1);

        ComPtr<IDXGISwapChain4> extra;
        CHECK(SUCCEEDED(f.chain.As(&extra)));
        f.chain.Reset();
        CHECK_EQ(ProxySwapChain::LiveCount(), 1);  // still referenced
        rtv.Reset();
        extra.Reset();
        CHECK_EQ(ProxySwapChain::LiveCount(), 0);
        CHECK_EQ(ThreadsStartedInExe(), threadsBefore);
        CHECK_EQ(CountHiddenWindows(), windowsBefore);
    }
}
