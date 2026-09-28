// Test application for ac-dlssg (spec 11 "Test application").
//
// Towards DXGI it behaves like Assetto Corsa with CSP: a window of class
// "acsW", a D3D11 device on an explicit adapter at feature level 11_0 without
// flags, and CSP's exact CreateSwapChainForHwnd description (spec 4). It loads
// the bridge the way ReShade loads its ProxyLibrary: LoadLibraryW by path, then
// the exported CreateDXGIFactory1. With --via-dxgi it runs the production
// chain instead: ReShade's dxgi.dll next to the exe (loaded at process start
// as d3d11.dll's dxgi.dll import, as in the game) with ac-dlssg.dll as its
// [PROXY] ProxyLibrary, and the factory comes from ReShade's
// CreateDXGIFactory1. With --standalone the bridge itself is dxgi.dll next to
// the exe, as a standalone install puts it next to acs.exe: the loader binds
// d3d11.dll's dxgi.dll import to it at process start, the app takes its
// factory from LoadLibraryW(L"dxgi.dll") by name, and D3D12 and Streamline
// bind to it the same way; the app checks those bindings in the import
// tables. It renders a moving rectangle with
// ClearView through a NULL-description view and an sRGB view of buffer 0, and
// paces itself on the frame-latency waitable object like CSP's
// ADVANCED_PACING.
//
// Whether the bridge proxied a swap chain is read from the bridge log, which
// is the same evidence a user has in the game. The build puts the Streamline
// DLLs in <exe dir>\ac-dlssg\sl, as the installer does in the game folder.
//
// M3: --fake-ngx plays CSP's DLSS calls against fake_nvngx.dll (the build's
// fake NGX module, next to the exe) and --fake-camera plays the CSP Lua app
// that publishes the camera (tools/testapp/fake_csp.h). With --expect-fg the
// bridge log must then show the capture -> camera -> constants -> tags path
// (the log lines of the M3 contract). DLSS-G itself never runs here.
//
// Exit codes: 0 every check passed, 1 a check failed (the reason is printed),
// 2 usage or set-up error.
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "fake_csp.h"
#include "log_checks.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[] = L"acsW";
constexpr wchar_t kBridgeDll[] = L"ac-dlssg.dll";
constexpr wchar_t kDxgiDll[] = L"dxgi.dll";  // --via-dxgi: ReShade; --standalone: the bridge; next to the exe
// ReShade's IID_UnwrappedObject (its source/com_utils.hpp; the bridge declares
// the same one): QueryInterface on a ReShade wrapper returns the wrapped object.
constexpr GUID kReShadeUnwrappedObject = {0x7F2C9A11, 0x3B4E, 0x4D6A, {0x81, 0x2F, 0x5E, 0x9C, 0xD3, 0x7A, 0x1B, 0x42}};
constexpr UINT kWidth = 1280;
constexpr UINT kHeight = 720;
constexpr UINT kResizeWidth = 1600;
constexpr UINT kResizeHeight = 900;
constexpr int kResizeUpFrame = 200;
constexpr int kResizeBackFrame = 400;
constexpr int kRecreateFrame = 300;
constexpr int kTestPresentEvery = 10;
constexpr int kVerifyDelay = 3;  // frames after an event before the buffer is read back
constexpr UINT kLatency = 2;
constexpr int kQueueDepthFirstFrame = 60;  // VSync queue depth is sampled from this frame on
constexpr DWORD kFrameLimitMs = 2000;
constexpr DWORD kStallFrameLimitMs = 5000;
constexpr double kSlowFrameMs = 25.0;  // reported, not failed
constexpr int kSlowFramesPrinted = 10;
constexpr wchar_t kStallMs[] = L"1500";
// Stalled frames return at once, so without a cap the whole run could end
// before the 1.5 s debug stall does. CSP has the same kind of CPU limiter.
constexpr int kStallFpsCap = 200;
// --stall: frames with a blocking read-back, around the bridge's debug stall
// (its 30th delivered frame, d3d12_presenter.cpp). The watchdog releases a
// held D3D11 queue after 500 ms; the debug stall itself ends after 1500 ms.
constexpr int kStallProbeFirst = 20;
constexpr int kStallProbeLast = 60;
constexpr double kStallProbeHeldMs = 300.0;
constexpr double kStallProbeWatchdogMs = 1200.0;
constexpr UINT kCspFlags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
constexpr UINT kCspUsage = DXGI_USAGE_SHADER_INPUT | DXGI_USAGE_RENDER_TARGET_OUTPUT;

// Background through the UNORM view, rectangle through the sRGB view.
constexpr float kBackground[4] = {0.10f, 0.20f, 0.40f, 1.0f};
constexpr float kRectangle[4] = {1.0f, 0.5f, 0.0f, 1.0f};
constexpr unsigned char kBackgroundBytes[4] = {26, 51, 102, 255};
constexpr unsigned char kRectangleBytes[4] = {255, 188, 0, 255};  // 0.5 linear is 188 in sRGB

// --fake-ngx: the DLSS render size, the Quality ratio (2/3) of the window, so
// that the render and output sizes differ as in CSP's upscaling modes.
constexpr UINT kRenderScaleNum = 2;
constexpr UINT kRenderScaleDen = 3;
constexpr wchar_t kFakeNgxDll[] = L"fake_nvngx.dll";
constexpr double kFakeCameraFps = 60.0;  // simulated time step of the fake drive
// --expect-fg: the capture, camera and tag counts of each statistics line
// after the first must reach this share of base_fps.
constexpr double kFgMinShare = 0.9;
// The bridge logs each distinct per-frame DLSS-G-off reason at most once per
// 10 s (M3 contract); the timestamps have millisecond resolution.
constexpr double kFgReasonPeriodMs = 10000.0 - 50.0;

enum class Expect { Any, Proxy, Passthrough };
enum class ExpectFg { None, Pipeline, NoCamera };

struct Options {
    int frames = 600;
    bool vsync = false;
    bool resize = false;
    bool test_present = false;
    bool recreate = false;
    bool stall = false;
    bool hidden = false;
    bool via_dxgi = false;  // the factory from <exe dir>\dxgi.dll (ReShade) instead of the bridge
    bool standalone = false;  // the bridge is <exe dir>\dxgi.dll, bound by name like the game's dxgi.dll
    int fps_cap = 0;        // 0: none
    int gpu_load = 0;       // extra 4096x4096 RGBA16F copies per frame, to make frames GPU-bound
    int cpu_load_ms = 0;    // CPU busy time per frame, to make frames CPU-bound
    Expect expect = Expect::Any;
    std::string passthrough_reason;  // --expect-passthrough-reason: the text the pass-through must name
    bool fake_ngx = false;           // CSP's DLSS calls against fake_nvngx.dll
    bool fake_camera = false;        // the CSP Lua app's camera writer
    ExpectFg expect_fg = ExpectFg::None;
    std::wstring fixture = L"default";
};

// ------------------------------------------------------------------ output

std::string g_failure;  // the first failure; empty while everything passes

void Print(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::fputs("testapp: ", stdout);
    std::vprintf(fmt, args);
    std::fputc('\n', stdout);
    va_end(args);
}

// Records the first failure and returns false, so checks read "if (!x) return Fail(...)".
bool Fail(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (g_failure.empty()) g_failure = buf;
    std::printf("testapp: CHECK FAILED: %s\n", buf);
    return false;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

const char* HrName(HRESULT hr) {
    switch (hr) {
        case S_OK: return "S_OK";
        case DXGI_STATUS_OCCLUDED: return "DXGI_STATUS_OCCLUDED";
        case DXGI_ERROR_INVALID_CALL: return "DXGI_ERROR_INVALID_CALL";
        case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED";
        case DXGI_ERROR_DEVICE_HUNG: return "DXGI_ERROR_DEVICE_HUNG";
        case DXGI_ERROR_DEVICE_RESET: return "DXGI_ERROR_DEVICE_RESET";
        case E_ACCESSDENIED: return "E_ACCESSDENIED";
        case E_INVALIDARG: return "E_INVALIDARG";
        case E_FAIL: return "E_FAIL";
        default: return "?";
    }
}

// ------------------------------------------------------------------ bridge log

// One CreateSwapChainForHwnd decision for the main window, in log order.
struct Decision {
    bool resolved = false;
    bool proxy = false;
    bool creation_failed = false;  // the hook wanted a proxy but creating it failed
    std::string line;
};

struct BridgeLog {
    bool found = false;
    std::vector<std::string> lines;
    std::vector<Decision> decisions;

    int Count(const char* needle) const {
        int n = 0;
        for (const auto& l : lines) n += l.find(needle) != std::string::npos ? 1 : 0;
        return n;
    }
    bool Has(const char* needle) const { return Count(needle) > 0; }
};

// The bridge keeps the log open for writing and shares it for reading.
BridgeLog ReadBridgeLog(const std::wstring& path) {
    BridgeLog log;
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return log;
    log.found = true;
    std::string text;
    char buf[65536];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) text.append(buf, got);
    CloseHandle(h);

    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        log.lines.push_back(std::move(line));
        pos = end + 1;
    }

    // Line formats come from factory_hook.cpp.
    for (const auto& line : log.lines) {
        if (line.find("CreateSwapChainForHwnd: hwnd") != std::string::npos &&
            line.find("(main window)") != std::string::npos) {
            Decision d;
            d.line = line;
            if (line.find(": pass-through: ") != std::string::npos) {
                d.resolved = true;
                d.proxy = false;
            }
            log.decisions.push_back(d);
        } else if (line.find("proxy swap chain created") != std::string::npos) {
            if (!log.decisions.empty() && !log.decisions.back().resolved) {
                log.decisions.back().resolved = true;
                log.decisions.back().proxy = true;
            }
        } else if (line.find("proxy swap chain creation failed") != std::string::npos) {
            if (!log.decisions.empty() && !log.decisions.back().resolved) {
                log.decisions.back().resolved = true;
                log.decisions.back().proxy = false;
                log.decisions.back().creation_failed = true;
                log.decisions.back().line += " / " + line;
            }
        }
    }
    return log;
}

void PrintLogTail(const std::wstring& path, size_t count) {
    const BridgeLog log = ReadBridgeLog(path);
    if (!log.found) {
        Print("bridge log not found: %s", Narrow(path).c_str());
        return;
    }
    const size_t first = log.lines.size() > count ? log.lines.size() - count : 0;
    Print("last %zu lines of %s:", log.lines.size() - first, Narrow(path).c_str());
    for (size_t i = first; i < log.lines.size(); ++i) std::printf("  | %s\n", log.lines[i].c_str());
}

// ------------------------------------------------------------------ window

bool g_close_requested = false;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLOSE:
            // The swap chain still uses the window; the main loop ends the run.
            g_close_requested = true;
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

constexpr DWORD kWindowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

SIZE WindowSizeForClient(UINT width, UINT height) {
    RECT rc{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    AdjustWindowRectEx(&rc, kWindowStyle, FALSE, 0);
    return SIZE{rc.right - rc.left, rc.bottom - rc.top};
}

HWND CreateGameWindow(bool show) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) return nullptr;
    const SIZE size = WindowSizeForClient(kWidth, kHeight);
    const HWND hwnd = CreateWindowExW(0, kWindowClass, L"ac-dlssg testapp", kWindowStyle, CW_USEDEFAULT,
                                      CW_USEDEFAULT, size.cx, size.cy, nullptr, nullptr, wc.hInstance, nullptr);
    // Shown without activation so that a scripted run does not take the focus.
    if (hwnd && show) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    return hwnd;
}

bool PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !g_close_requested;
}

UINT RefreshRateOf(HWND hwnd) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return 0;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0;
    return dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 0;
}

// ------------------------------------------------------------------ timing

double NowMs() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

// ------------------------------------------------------------------ the app

struct App {
    Options opt;
    std::wstring exe_dir;
    std::wstring log_path;
    HWND hwnd = nullptr;
    HMODULE factory_module = nullptr;  // ac-dlssg.dll, ReShade's dxgi.dll with --via-dxgi, the bridge's with
                                       // --standalone

    ComPtr<IDXGIFactory2> factory;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11DeviceContext1> ctx1;
    bool clear_view = false;
    bool tearing = false;
    UINT chain_flags = kCspFlags;

    // The current swap chain and what hangs off it.
    ComPtr<IDXGISwapChain1> chain;
    ComPtr<IDXGISwapChain2> chain2;
    ComPtr<IDXGISwapChain3> chain3;
    ComPtr<ID3D11Texture2D> buffer;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11RenderTargetView> rtv_srgb;
    ComPtr<ID3D11Texture2D> load_src, load_dst;  // --gpu-load
    HANDLE waitable = nullptr;
    UINT width = 0;
    UINT height = 0;
    bool is_proxy = false;  // from the bridge log

    int chains_created = 0;
    int proxies_created = 0;
    int proxies_released = 0;
    int occluded = 0;
    int test_presents = 0;
    double max_frame_ms = 0;
    int slow_frames = 0;
    int max_frame_index = -1;
    std::vector<std::string> proxy_resizes;  // "presenter resized to WxH" for each resize of a proxy

    // --stall: 1x1 staging texture for the blocking read-back.
    ComPtr<ID3D11Texture2D> probe;
    double queue_depth_sum = 0;
    int queue_depth_samples = 0;
    double probe_max_ms = 0;
    int probe_max_frame = -1;

    // M3 (--fake-ngx, --fake-camera).
    HMODULE ngx_module = nullptr;      // fake_nvngx.dll
    testapp::FakeNgx ngx;
    UINT ngx_first_w = 0, ngx_first_h = 0;  // render size of the first DLSS feature
    testapp::CameraWriter camera;
    uint32_t camera_writes = 0;
    double camera_last_ms = 0;
    bool frames_done = false;          // RunFrames presented every frame
};

std::wstring ExeDir() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) return {};
        if (n < path.size()) {
            path.resize(n);
            break;
        }
        path.resize(path.size() * 2);
    }
    const size_t sep = path.find_last_of(L"\\/");
    return sep == std::wstring::npos ? std::wstring(L".") : path.substr(0, sep);
}

bool DirExists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool FileExists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring DocsDirFor(const std::wstring& exeDir, const std::wstring& fixture) {
    if (fixture == L"default") return exeDir + L"\\fixture\\docs";
    return exeDir + L"\\fixture\\" + fixture + L"\\docs";
}

// ---- device and factory, the ReShade ProxyLibrary way

std::wstring ModulePath(HMODULE module) {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    return n > 0 && n < MAX_PATH ? std::wstring(path, n) : std::wstring();
}

HMODULE ImplementingModule(IUnknown* object);
std::string ModuleFileNameOf(HMODULE module);

// ---- import bindings (--standalone)

struct Binding {
    bool found = false;        // the importer has this import
    bool delay = false;        // a delay-load import
    HMODULE target = nullptr;  // the module its import address points into
};

HMODULE ModuleOfAddress(const void* address) {
    HMODULE module = nullptr;
    if (!address ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &module))
        return nullptr;
    return module;
}

// Where the loaded module importer's import dll!name points: the import
// address table of a static import, or of a delay-load import (which points
// into the importer's own thunk until its first call).
Binding ImportBinding(HMODULE importer, const char* dll, const char* name) {
    Binding b;
    if (!importer) return b;
    const auto* base = reinterpret_cast<const BYTE*>(importer);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto find = [&](DWORD namesRva, DWORD iatRva) {
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + namesRva);
        const auto* iat = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + iatRva);
        for (size_t i = 0; names[i].u1.AddressOfData; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL64(names[i].u1.Ordinal)) continue;
            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), name) != 0) continue;
            b.found = true;
            b.target = ModuleOfAddress(reinterpret_cast<const void*>(iat[i].u1.Function));
            return true;
        }
        return false;
    };
    const IMAGE_DATA_DIRECTORY& imp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imp.VirtualAddress) {
        for (const auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imp.VirtualAddress); d->Name;
             ++d) {
            if (_stricmp(reinterpret_cast<const char*>(base + d->Name), dll) == 0 &&
                find(d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk, d->FirstThunk))
                return b;
        }
    }
    const IMAGE_DATA_DIRECTORY& delay = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delay.VirtualAddress) {
        for (const auto* d = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(base + delay.VirtualAddress);
             d->DllNameRVA; ++d) {
            if (d->Attributes.RvaBased && _stricmp(reinterpret_cast<const char*>(base + d->DllNameRVA), dll) == 0 &&
                find(d->ImportNameTableRVA, d->ImportAddressTableRVA)) {
                b.delay = true;
                return b;
            }
        }
    }
    return b;
}

// The dxgi.dll import `name` of the loaded module importerName must be bound
// to the bridge; a delay-load import only once it has been called.
bool CheckDxgiBinding(HMODULE bridge, const wchar_t* importerName, const char* name) {
    const HMODULE importer = GetModuleHandleW(importerName);
    if (!importer) return Fail("standalone: %s is not loaded", Narrow(importerName).c_str());
    const Binding b = ImportBinding(importer, "dxgi.dll", name);
    const std::string where = Narrow(ModulePath(importer));
    if (!b.found) return Fail("standalone: %s has no dxgi.dll!%s import", where.c_str(), name);
    if (b.delay && b.target == importer) {
        Print("standalone: %s's delay-load dxgi.dll!%s is not bound yet", where.c_str(), name);
        return true;
    }
    Print("standalone: %s's %sdxgi.dll!%s is bound to %s", where.c_str(), b.delay ? "delay-load " : "", name,
          b.target ? Narrow(ModulePath(b.target)).c_str() : "(no module)");
    if (b.target != bridge)
        return Fail("standalone: %s's dxgi.dll!%s is not bound to the bridge", where.c_str(), name);
    return true;
}

bool CreateDevice(App& a) {
    // Without --via-dxgi or --standalone the test app plays ReShade: it loads
    // the bridge by path. With either, the dxgi.dll next to the exe (ReShade,
    // or the bridge itself) is already in the process: testapp.exe imports
    // d3d11.dll, which imports dxgi.dll, and the loader found that in the exe
    // directory, as it does for acs.exe.
    const std::wstring dllPath = a.exe_dir + L"\\" + (a.opt.via_dxgi || a.opt.standalone ? kDxgiDll : kBridgeDll);
    const std::wstring bridgePath = a.exe_dir + L"\\" + kBridgeDll;
    if (a.opt.via_dxgi || a.opt.standalone) {
        if (!GetModuleHandleW(dllPath.c_str()))
            return Fail("%s is not loaded: d3d11.dll's dxgi.dll import did not resolve to the exe directory",
                        Narrow(dllPath).c_str());
        if (GetModuleHandleW(bridgePath.c_str()))
            return Fail("%s is loaded before the first DXGI call", Narrow(bridgePath).c_str());
        Print("%s was loaded at process start (d3d11.dll's dxgi.dll import)", Narrow(dllPath).c_str());
    }
    if (a.opt.standalone) {
        const HMODULE bridge = GetModuleHandleW(dllPath.c_str());
        if (!CheckDxgiBinding(bridge, L"d3d11.dll", "CreateDXGIFactory2")) return false;
        // By name, as nvngx_dlssg.dll and Streamline do: the loader returns the
        // dxgi.dll that is already in the process, the bridge.
        a.factory_module = LoadLibraryW(kDxgiDll);
        if (a.factory_module != bridge)
            return Fail("LoadLibraryW(L\"dxgi.dll\") returned %s, not the bridge %s",
                        Narrow(ModulePath(a.factory_module)).c_str(), Narrow(dllPath).c_str());
        Print("LoadLibraryW(L\"dxgi.dll\") by name returned the bridge %s", Narrow(dllPath).c_str());
    } else {
        a.factory_module = LoadLibraryW(dllPath.c_str());
    }
    if (!a.factory_module)
        return Fail("LoadLibraryW(%s) failed: error %lu", Narrow(dllPath).c_str(), GetLastError());
    using PFN_CreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);
    const auto createFactory1 =
        reinterpret_cast<PFN_CreateDXGIFactory1>(GetProcAddress(a.factory_module, "CreateDXGIFactory1"));
    if (!createFactory1) return Fail("%s does not export CreateDXGIFactory1", Narrow(dllPath).c_str());

    ComPtr<IDXGIFactory1> factory1;
    HRESULT hr = createFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory1.GetAddressOf()));
    if (FAILED(hr) || !factory1)
        return Fail("CreateDXGIFactory1 of %s failed: 0x%08lX", Narrow(dllPath).c_str(),
                    static_cast<unsigned long>(hr));
    if (a.opt.via_dxgi) {
        // ReShade loads its ProxyLibrary on the first call to a DXGI export.
        const HMODULE bridge = GetModuleHandleW(bridgePath.c_str());
        if (!bridge)
            return Fail("%s did not load %s as its ProxyLibrary (see [PROXY] in ReShade.ini and ReShade.log)",
                        Narrow(dllPath).c_str(), Narrow(bridgePath).c_str());
        Print("CreateDXGIFactory1 through %s, which loaded its ProxyLibrary %s",
              Narrow(ModulePath(a.factory_module)).c_str(), Narrow(ModulePath(bridge)).c_str());
    }
    if (a.opt.standalone) {
        // The bridge forwards to System32's dxgi.dll, loaded by full path next to it.
        wchar_t sys[MAX_PATH] = {};
        const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        const HMODULE real = GetModuleHandleW((std::wstring(sys, n) + L"\\dxgi.dll").c_str());
        if (!real || real == a.factory_module)
            return Fail("standalone: System32's dxgi.dll is not loaded next to the bridge after CreateDXGIFactory1");
        if (ImplementingModule(factory1.Get()) != real)
            return Fail("standalone: the factory is implemented by %s, not by System32's dxgi.dll",
                        ModuleFileNameOf(ImplementingModule(factory1.Get())).c_str());
        Print("standalone: CreateDXGIFactory1 through the bridge returned a factory of %s",
              Narrow(ModulePath(real)).c_str());
    }
    if (FAILED(factory1.As(&a.factory))) return Fail("the factory does not implement IDXGIFactory2");

    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory1.As(&factory5))) {
        BOOL allow = FALSE;
        a.tearing =
            SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))) &&
            allow;
    }
    if (!a.tearing) {
        // CSP only asks for tearing where DXGI supports it.
        a.chain_flags &= ~static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
        Print("tearing is not supported here; ALLOW_TEARING is left out of the chain flags");
    }

    ComPtr<IDXGIAdapter1> adapter;
    hr = factory1->EnumAdapters1(0, &adapter);
    if (FAILED(hr)) return Fail("EnumAdapters1(0) failed: 0x%08lX", static_cast<unsigned long>(hr));
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    Print("adapter 0: %s (vendor 0x%04X device 0x%04X)", Narrow(ad.Description).c_str(), ad.VendorId, ad.DeviceId);

    // CSP: explicit adapter, UNKNOWN driver type, no flags, feature level 11_0.
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    D3D_FEATURE_LEVEL got{};
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                           &a.device, &got, &a.ctx);
    if (FAILED(hr)) return Fail("D3D11CreateDevice failed: 0x%08lX", static_cast<unsigned long>(hr));
    if (FAILED(a.ctx.As(&a.ctx1))) return Fail("the immediate context does not implement ID3D11DeviceContext1");

    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    a.clear_view = SUCCEEDED(a.device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))) &&
                   options.ClearView;
    if (!a.clear_view) Print("ClearView is not supported; the rectangle is not drawn and not verified");

    if (a.opt.gpu_load > 0) {
        D3D11_TEXTURE2D_DESC ld{};
        ld.Width = ld.Height = 4096;
        ld.MipLevels = ld.ArraySize = 1;
        ld.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ld.SampleDesc.Count = 1;
        ld.Usage = D3D11_USAGE_DEFAULT;
        ld.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(a.device->CreateTexture2D(&ld, nullptr, &a.load_src)) ||
            FAILED(a.device->CreateTexture2D(&ld, nullptr, &a.load_dst)))
            return Fail("could not create the GPU load textures");
    }
    return true;
}

// ---- swap chain

void ReleaseChain(App& a) {
    a.rtv.Reset();
    a.rtv_srgb.Reset();
    a.buffer.Reset();
    if (a.waitable) {
        CloseHandle(a.waitable);
        a.waitable = nullptr;
    }
    a.chain2.Reset();
    a.chain3.Reset();
    // Flip-model chains are destroyed lazily; a new chain on the same window
    // needs the old one gone.
    if (a.ctx) {
        a.ctx->ClearState();
        a.ctx->Flush();
    }
    if (a.chain) {
        a.chain.Reset();
        if (a.is_proxy) ++a.proxies_released;
        if (a.ctx) a.ctx->Flush();
    }
    a.is_proxy = false;
}

bool FetchBufferAndViews(App& a) {
    HRESULT hr = a.chain->GetBuffer(0, IID_PPV_ARGS(&a.buffer));
    if (FAILED(hr) || !a.buffer) return Fail("GetBuffer(0) failed: 0x%08lX", static_cast<unsigned long>(hr));
    D3D11_TEXTURE2D_DESC td{};
    a.buffer->GetDesc(&td);
    if (td.Width != a.width || td.Height != a.height || td.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
        return Fail("buffer 0 is %ux%u format %d, expected %ux%u format %d", td.Width, td.Height,
                    static_cast<int>(td.Format), a.width, a.height, static_cast<int>(DXGI_FORMAT_R8G8B8A8_UNORM));
    // CSP: NULL description. ReShade: also an sRGB view (spec 4, 6.4).
    hr = a.device->CreateRenderTargetView(a.buffer.Get(), nullptr, &a.rtv);
    if (FAILED(hr)) return Fail("CreateRenderTargetView(NULL desc) failed: 0x%08lX", static_cast<unsigned long>(hr));
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    hr = a.device->CreateRenderTargetView(a.buffer.Get(), &rd, &a.rtv_srgb);
    if (FAILED(hr)) return Fail("CreateRenderTargetView(sRGB) failed: 0x%08lX", static_cast<unsigned long>(hr));
    return true;
}

bool CheckDesc1(App& a, const char* when) {
    DXGI_SWAP_CHAIN_DESC1 d{};
    const HRESULT hr = a.chain->GetDesc1(&d);
    if (FAILED(hr)) return Fail("GetDesc1 (%s) failed: 0x%08lX", when, static_cast<unsigned long>(hr));
    if (d.Width != a.width || d.Height != a.height || d.Format != DXGI_FORMAT_R8G8B8A8_UNORM || d.BufferCount != 2 ||
        (d.BufferUsage & kCspUsage) != kCspUsage || d.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD ||
        d.Flags != a.chain_flags)
        return Fail("GetDesc1 (%s): %ux%u format %d, %u buffers, usage 0x%X, swap effect %d, flags 0x%X; expected "
                    "%ux%u format %d, 2 buffers, usage 0x%X, FLIP_DISCARD, flags 0x%X",
                    when, d.Width, d.Height, static_cast<int>(d.Format), d.BufferCount,
                    static_cast<unsigned>(d.BufferUsage), static_cast<int>(d.SwapEffect), d.Flags, a.width, a.height,
                    static_cast<int>(DXGI_FORMAT_R8G8B8A8_UNORM), kCspUsage, a.chain_flags);
    return true;
}

// Reads the bridge's decision for the chain just created.
bool ClassifyChain(App& a) {
    const BridgeLog log = ReadBridgeLog(a.log_path);
    if (!log.found) return Fail("bridge log not found: %s", Narrow(a.log_path).c_str());
    if (static_cast<int>(log.decisions.size()) != a.chains_created)
        return Fail("the bridge log has %zu main-window CreateSwapChainForHwnd decisions after %d creations",
                    log.decisions.size(), a.chains_created);
    const Decision& d = log.decisions.back();
    if (!d.resolved) return Fail("the bridge log has no outcome for the proxy decision: %s", d.line.c_str());
    a.is_proxy = d.proxy;
    if (a.is_proxy) ++a.proxies_created;
    Print("swap chain %d: %s (bridge log: %s)", a.chains_created, a.is_proxy ? "proxy" : "pass-through",
          d.line.c_str());
    // --expect-passthrough-reason: the decision line, or the line of a proxy
    // creation that failed (a presenter refused by Streamline), names it.
    const bool namesReason =
        !a.opt.passthrough_reason.empty() && d.line.find(a.opt.passthrough_reason) != std::string::npos;
    if (d.creation_failed && !namesReason) return Fail("the bridge wanted a proxy but its creation failed");
    if (!a.opt.passthrough_reason.empty() && !a.is_proxy && !namesReason)
        return Fail("swap chain %d: the pass-through does not name \"%s\"", a.chains_created,
                    a.opt.passthrough_reason.c_str());
    // Streamline allows one Init/Shutdown per process and the final Release
    // of a proxy shuts it down (spec 6.3, 8): after that, --expect-proxy
    // expects pass-through chains with that reason.
    if (a.opt.expect == Expect::Proxy && a.proxies_released > 0) {
        if (a.is_proxy)
            return Fail("swap chain %d: expected a pass-through after Streamline was shut down, the bridge proxied it",
                        a.chains_created);
        if (d.line.find(": pass-through: Streamline already shut down") == std::string::npos)
            return Fail("swap chain %d: pass-through without the reason \"Streamline already shut down\"",
                        a.chains_created);
    } else if (a.opt.expect == Expect::Proxy && !a.is_proxy) {
        return Fail("swap chain %d: expected a proxy, the bridge passed it through", a.chains_created);
    }
    if (a.opt.expect == Expect::Passthrough && a.is_proxy)
        return Fail("swap chain %d: expected a pass-through, the bridge proxied it", a.chains_created);
    // A proxy logs its presenter; a pass-through must not create one.
    const int presenters = log.Count("presenter created");
    if (presenters != a.proxies_created)
        return Fail("the bridge log shows %d presenters for %d proxies", presenters, a.proxies_created);
    return true;
}

// The bridge's hidden-chain windows in this process (class name from hidden_chain.cpp).
int CountBridgeHiddenWindows() {
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

// Evidence independent of the log: a proxy owns exactly one hidden window, and
// its buffer 0 belongs to that hidden chain rather than to the chain the game
// holds. A real chain owns its buffer and no hidden window.
bool CheckChainIdentity(App& a) {
    const int hidden = CountBridgeHiddenWindows();
    if (hidden != (a.is_proxy ? 1 : 0))
        return Fail("swap chain %d: the log says %s, but %d hidden bridge windows exist", a.chains_created,
                    a.is_proxy ? "proxy" : "pass-through", hidden);
    // With --via-dxgi the app holds ReShade's wrapper, which owns no buffer:
    // the buffer's parent is compared with the chain the wrapper holds (the
    // bridge's proxy or DXGI's chain) instead, so that the check can fail.
    ComPtr<IUnknown> chainId;
    if (a.opt.via_dxgi) {
        ComPtr<IUnknown> inner;
        const HRESULT hr =
            a.chain->QueryInterface(kReShadeUnwrappedObject, reinterpret_cast<void**>(inner.GetAddressOf()));
        if (FAILED(hr) || !inner)
            return Fail("swap chain %d: ReShade's wrapper does not return the wrapped chain (IID_UnwrappedObject: "
                        "0x%08lX)",
                        a.chains_created, static_cast<unsigned long>(hr));
        inner.As(&chainId);
    } else {
        a.chain.As(&chainId);
    }
    const char* whose = a.opt.via_dxgi ? "the chain behind ReShade's wrapper" : "the chain";
    ComPtr<IDXGISurface> surface;
    ComPtr<IDXGISwapChain> owner;
    if (SUCCEEDED(a.buffer.As(&surface)) && SUCCEEDED(surface->GetParent(IID_PPV_ARGS(&owner))) && owner) {
        ComPtr<IUnknown> ownerId;
        owner.As(&ownerId);
        const bool own = ownerId && ownerId == chainId;
        if (own == a.is_proxy)
            return Fail("swap chain %d: the log says %s, but buffer 0 %s to %s", a.chains_created,
                        a.is_proxy ? "proxy" : "pass-through", own ? "belongs" : "does not belong", whose);
        Print("swap chain %d: buffer 0 %s %s, %d hidden bridge window(s): consistent with the log",
              a.chains_created, own ? "belongs to" : "does not belong to", whose, hidden);
    } else {
        Print("swap chain %d: %d hidden bridge window(s): consistent with the log (buffer owner unknown)",
              a.chains_created, hidden);
    }
    return true;
}

// The module that holds an object's vtable, i.e. the module that implements it.
HMODULE ImplementingModule(IUnknown* object) {
    HMODULE module = nullptr;
    const void* vtable = *reinterpret_cast<void* const*>(object);
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(vtable), &module))
        return nullptr;
    return module;
}

std::string ModuleFileNameOf(HMODULE module) {
    if (!module) return "(no module)";
    const std::wstring path = ModulePath(module);
    const size_t sep = path.find_last_of(L"\\/");
    return Narrow(sep == std::wstring::npos ? path : path.substr(sep + 1));
}

// --via-dxgi: the app holds ReShade's swap-chain wrapper, and behind it is
// the bridge's proxy (or, for a pass-through, DXGI's own chain), never one of
// the bridge's internal chains.
bool CheckReShadeWrapper(App& a) {
    const HMODULE bridge = GetModuleHandleW((a.exe_dir + L"\\" + kBridgeDll).c_str());
    const HMODULE outer = ImplementingModule(a.chain.Get());
    if (outer != a.factory_module)
        return Fail("swap chain %d is implemented by %s, not by ReShade's %s", a.chains_created,
                    ModuleFileNameOf(outer).c_str(), ModuleFileNameOf(a.factory_module).c_str());
    ComPtr<IUnknown> inner;
    const HRESULT hr = a.chain->QueryInterface(kReShadeUnwrappedObject, reinterpret_cast<void**>(inner.GetAddressOf()));
    if (FAILED(hr) || !inner)
        return Fail("swap chain %d: ReShade's wrapper does not return the wrapped chain (IID_UnwrappedObject: 0x%08lX)",
                    a.chains_created, static_cast<unsigned long>(hr));
    const HMODULE wrapped = ImplementingModule(inner.Get());
    if ((wrapped == bridge) != a.is_proxy || wrapped == a.factory_module)
        return Fail("swap chain %d: the log says %s, but ReShade wraps an object of %s", a.chains_created,
                    a.is_proxy ? "proxy" : "pass-through", ModuleFileNameOf(wrapped).c_str());
    // The wrapper's address is the "this" of ReShade.log's IDXGISwapChain lines
    // (run-testapp.ps1 matches them).
    Print("swap chain %d: ReShade's wrapper %p (%s) around %s (%s)", a.chains_created,
          static_cast<void*>(a.chain.Get()), ModuleFileNameOf(outer).c_str(),
          a.is_proxy ? "the bridge's proxy" : "DXGI's chain", ModuleFileNameOf(wrapped).c_str());
    return true;
}

// --standalone: the app holds the bridge's proxy (implemented in dxgi.dll next
// to the exe) or DXGI's own chain. Once Streamline runs, its DXGI imports are
// bound to the bridge too.
bool CheckStandaloneChain(App& a) {
    const HMODULE impl = ImplementingModule(a.chain.Get());
    if ((impl == a.factory_module) != a.is_proxy)
        return Fail("swap chain %d: the log says %s, but the app's chain is implemented by %s", a.chains_created,
                    a.is_proxy ? "proxy" : "pass-through", ModuleFileNameOf(impl).c_str());
    if (a.is_proxy && a.chains_created == 1) {
        if (!CheckDxgiBinding(a.factory_module, L"sl.common.dll", "CreateDXGIFactory")) return false;
        if (GetModuleHandleW(L"D3D12Core.dll") &&
            !CheckDxgiBinding(a.factory_module, L"D3D12Core.dll", "CreateDXGIFactory2"))
            return false;
    }
    return true;
}

bool CreateChain(App& a) {
    RECT rc{};
    GetClientRect(a.hwnd, &rc);
    a.width = static_cast<UINT>(rc.right - rc.left);
    a.height = static_cast<UINT>(rc.bottom - rc.top);

    // CSP's description (spec 4).
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = a.width;
    desc.Height = a.height;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = kCspUsage;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = a.chain_flags;
    ++a.chains_created;
    const HRESULT hr = a.factory->CreateSwapChainForHwnd(a.device.Get(), a.hwnd, &desc, nullptr, nullptr, &a.chain);
    if (FAILED(hr) || !a.chain)
        return Fail("CreateSwapChainForHwnd #%d failed: 0x%08lX (%s)", a.chains_created,
                    static_cast<unsigned long>(hr), HrName(hr));
    if (!ClassifyChain(a)) return false;
    if (a.opt.via_dxgi && !CheckReShadeWrapper(a)) return false;
    if (a.opt.standalone && !CheckStandaloneChain(a)) return false;
    if (!CheckDesc1(a, "after creation")) return false;
    if (FAILED(a.chain.As(&a.chain2)) || FAILED(a.chain.As(&a.chain3)))
        return Fail("the swap chain does not implement IDXGISwapChain2 and IDXGISwapChain3");

    // CSP's ADVANCED_PACING: latency first, then the waitable object.
    HRESULT lhr = a.chain2->SetMaximumFrameLatency(kLatency);
    if (FAILED(lhr)) return Fail("SetMaximumFrameLatency(%u) failed: 0x%08lX", kLatency, static_cast<unsigned long>(lhr));
    UINT latency = 0;
    lhr = a.chain2->GetMaximumFrameLatency(&latency);
    if (FAILED(lhr) || latency != kLatency)
        return Fail("GetMaximumFrameLatency: 0x%08lX, %u (expected %u)", static_cast<unsigned long>(lhr), latency,
                    kLatency);
    a.waitable = a.chain2->GetFrameLatencyWaitableObject();
    if (!a.waitable) return Fail("GetFrameLatencyWaitableObject returned NULL");
    return FetchBufferAndViews(a) && CheckChainIdentity(a);
}

// ---- CSP stand-ins (M3)

// A process named acs.exe: a running game shares the camera section.
bool GameRunning() {
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    for (BOOL more = Process32FirstW(snap, &pe); more && !found; more = Process32NextW(snap, &pe))
        found = _wcsicmp(pe.szExeFile, L"acs.exe") == 0;
    CloseHandle(snap);
    return found;
}

UINT RenderSizeOf(UINT windowSize) { return std::max(windowSize * kRenderScaleNum / kRenderScaleDen, 1u); }

// (Re)creates the DLSS feature for the current window size, as CSP does at
// start and after a resize.
bool CreateFakeFeature(App& a) {
    const UINT rw = RenderSizeOf(a.width);
    const UINT rh = RenderSizeOf(a.height);
    std::string err;
    if (!a.ngx.CreateFeature(a.device.Get(), a.ctx.Get(), rw, rh, a.width, a.height, &err))
        return Fail("fake NGX: %s", err.c_str());
    if (a.ngx.Creates() == 1) {
        a.ngx_first_w = rw;
        a.ngx_first_h = rh;
    }
    Print("fake NGX: DLSS feature %u created: render %ux%u, output %ux%u, create flags 0x%X", a.ngx.Creates(), rw, rh,
          a.width, a.height, acdb::kNgxDlssFlagMVLowRes);
    return true;
}

// After the swap chain exists, as CSP initialises DLSS and its Lua apps.
bool SetUpCspFakes(App& a) {
    if (a.opt.fake_ngx) {
        const std::wstring path = a.exe_dir + L"\\" + kFakeNgxDll;
        a.ngx_module = LoadLibraryW(path.c_str());
        if (!a.ngx_module)
            return Fail("fake NGX: LoadLibraryW(%s) failed: error %lu", Narrow(path).c_str(), GetLastError());
        std::string err;
        if (!a.ngx.Attach(a.ngx_module, &err)) return Fail("fake NGX: %s: %s", Narrow(path).c_str(), err.c_str());
        Print("fake NGX: loaded %s", Narrow(path).c_str());
        if (!CreateFakeFeature(a)) return false;
    }
    if (a.opt.fake_camera) {
        std::string err;
        if (!a.camera.Open(testapp::kCameraSectionName, &err))
            return Fail("fake camera: %s: %s", Narrow(testapp::kCameraSectionName).c_str(), err.c_str());
        a.camera_last_ms = NowMs();
        Print("fake camera: writing %s once per frame (frame counter continues at %u)",
              Narrow(testapp::kCameraSectionName).c_str(), a.camera.Frame());
    }
    return true;
}

// Once per frame before the DLSS evaluate, like the Lua app's
// render.onSceneReady: the fake drive at kFakeCameraFps simulated frames per
// second, with the current DLSS render size (the window's without DLSS).
void WriteFakeCamera(App& a, int frame) {
    const double now = NowMs();
    const float rw = static_cast<float>(a.opt.fake_ngx ? a.ngx.RenderWidth() : a.width);
    const float rh = static_cast<float>(a.opt.fake_ngx ? a.ngx.RenderHeight() : a.height);
    const float dt = static_cast<float>((now - a.camera_last_ms) / 1000.0);
    a.camera.Write(testapp::PathPose(frame / kFakeCameraFps), rw, rh, dt, frame * 1000.0 / kFakeCameraFps);
    a.camera_last_ms = now;
    ++a.camera_writes;
}

// Every call the test app made reached the fake module, and every frame was
// evaluated once.
bool CheckCspFakes(App& a) {
    bool ok = true;
    if (a.opt.fake_ngx && a.ngx.State()) {
        const FakeNgxState* s = a.ngx.State();
        Print("fake NGX: %u create(s) and %u evaluate(s) made; fake_nvngx.dll counted %ld and %ld", a.ngx.Creates(),
              a.ngx.Evaluates(), s->createCalls, s->evalCalls);
        if (s->createCalls != static_cast<long>(a.ngx.Creates()) ||
            s->evalCalls != static_cast<long>(a.ngx.Evaluates()))
            ok = Fail("fake NGX: fake_nvngx.dll did not see every call the test app made");
        if (a.frames_done && a.ngx.Evaluates() != static_cast<uint32_t>(a.opt.frames))
            ok = Fail("fake NGX: %u evaluates for %d frames", a.ngx.Evaluates(), a.opt.frames);
    }
    if (a.opt.fake_camera) {
        Print("fake camera: %u records written, frame counter %u", a.camera_writes, a.camera.Frame());
        if (a.frames_done && a.camera_writes != static_cast<uint32_t>(a.opt.frames))
            ok = Fail("fake camera: %u records for %d frames", a.camera_writes, a.opt.frames);
    }
    return ok;
}

void ReleaseCspFakes(App& a) {
    if (a.opt.fake_ngx && a.ngx.State()) {
        const FakeNgxState* s = a.ngx.State();
        a.ngx.Release();
        if (s->allocatedParams != s->destroyedParams)
            Fail("fake NGX: %ld parameter blocks allocated, %ld destroyed", s->allocatedParams, s->destroyedParams);
    }
    a.camera.Close();
    // fake_nvngx.dll stays loaded, as NGX does in CSP: the bridge's hook
    // patched it.
}

bool ResizeChain(App& a, UINT width, UINT height) {
    Print("resize to %ux%u", width, height);
    const SIZE size = WindowSizeForClient(width, height);
    SetWindowPos(a.hwnd, nullptr, 0, 0, size.cx, size.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    PumpMessages();
    a.rtv.Reset();
    a.rtv_srgb.Reset();
    a.buffer.Reset();
    a.ctx->ClearState();
    a.ctx->Flush();
    // Zero count and UNKNOWN format keep the current ones; the flags must repeat the creation flags.
    const HRESULT hr = a.chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, a.chain_flags);
    if (FAILED(hr))
        return Fail("ResizeBuffers(0, %u, %u, UNKNOWN, 0x%X) failed: 0x%08lX (%s)", width, height, a.chain_flags,
                    static_cast<unsigned long>(hr), HrName(hr));
    a.width = width;
    a.height = height;
    if (a.is_proxy) {
        char line[64];
        std::snprintf(line, sizeof(line), "presenter resized to %ux%u", width, height);
        a.proxy_resizes.push_back(line);
    }
    if (!CheckDesc1(a, "after ResizeBuffers")) return false;
    if (!FetchBufferAndViews(a)) return false;
    return !a.opt.fake_ngx || CreateFakeFeature(a);
}

bool RecreateChain(App& a) {
    Print("release and re-create the swap chain");
    const bool wasProxy = a.is_proxy;
    ReleaseChain(a);
    if (const int hidden = CountBridgeHiddenWindows(); hidden != 0)
        return Fail("%d hidden bridge windows remain after the swap chain was released", hidden);
    if (wasProxy) {
        // The proxy's final Release runs synchronously inside Release.
        const BridgeLog log = ReadBridgeLog(a.log_path);
        const std::vector<std::string> problems = testapp::ReleaseProblems(log.lines, a.proxies_released);
        if (!problems.empty()) return Fail("after releasing the proxy: %s", problems.front().c_str());
    }
    return CreateChain(a);
}

// ---- frames

RECT RectangleFor(const App& a, int frame) {
    const UINT w = std::max(a.width / 8, 1u);
    const UINT h = std::max(a.height / 4, 1u);
    const UINT span = a.width > w ? a.width - w : 1;
    const UINT x = static_cast<UINT>(frame) * 8u % span;
    const UINT y = a.height / 3;
    return RECT{static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + w), static_cast<LONG>(y + h)};
}

void Render(App& a, const RECT& rect) {
    for (int i = 0; i < a.opt.gpu_load && a.load_src; ++i) a.ctx->CopyResource(a.load_dst.Get(), a.load_src.Get());
    if (a.opt.cpu_load_ms > 0) {
        const double until = NowMs() + a.opt.cpu_load_ms;
        while (NowMs() < until) {
        }
    }
    if (a.clear_view) {
        a.ctx1->ClearView(a.rtv.Get(), kBackground, nullptr, 0);
        a.ctx1->ClearView(a.rtv_srgb.Get(), kRectangle, &rect, 1);
    } else {
        a.ctx->ClearRenderTargetView(a.rtv.Get(), kBackground);
    }
}

bool PixelNear(const unsigned char* px, const unsigned char* expected, int tolerance) {
    for (int i = 0; i < 4; ++i)
        if (std::abs(static_cast<int>(px[i]) - static_cast<int>(expected[i])) > tolerance) return false;
    return true;
}

// Reads buffer 0 back after rendering: both views must reach the texture the
// chain hands out, with the sRGB view encoding.
bool VerifyBuffer(App& a, const RECT& rect, int frame) {
    D3D11_TEXTURE2D_DESC td{};
    a.buffer->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = a.device->CreateTexture2D(&td, nullptr, &staging);
    if (FAILED(hr)) return Fail("staging texture creation failed: 0x%08lX", static_cast<unsigned long>(hr));
    a.ctx->CopyResource(staging.Get(), a.buffer.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    hr = a.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) return Fail("Map of the read-back texture failed: 0x%08lX", static_cast<unsigned long>(hr));
    const auto pixel = [&](LONG x, LONG y) {
        return static_cast<const unsigned char*>(m.pData) + static_cast<size_t>(y) * m.RowPitch +
               static_cast<size_t>(x) * 4;
    };
    const unsigned char* bg = pixel(2, 2);
    const unsigned char* fg = pixel((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
    unsigned char bgCopy[4];
    unsigned char fgCopy[4];
    std::memcpy(bgCopy, bg, 4);
    std::memcpy(fgCopy, fg, 4);
    a.ctx->Unmap(staging.Get(), 0);

    if (!PixelNear(bgCopy, kBackgroundBytes, 2))
        return Fail("frame %d: background pixel is %u,%u,%u,%u, expected about %u,%u,%u,%u", frame, bgCopy[0],
                    bgCopy[1], bgCopy[2], bgCopy[3], kBackgroundBytes[0], kBackgroundBytes[1], kBackgroundBytes[2],
                    kBackgroundBytes[3]);
    if (a.clear_view && !PixelNear(fgCopy, kRectangleBytes, 3))
        return Fail("frame %d: rectangle pixel (sRGB view) is %u,%u,%u,%u, expected about %u,%u,%u,%u", frame,
                    fgCopy[0], fgCopy[1], fgCopy[2], fgCopy[3], kRectangleBytes[0], kRectangleBytes[1],
                    kRectangleBytes[2], kRectangleBytes[3]);
    Print("frame %d: buffer 0 read back %ux%u, background and sRGB rectangle as drawn", frame, a.width, a.height);
    return true;
}

// --stall: a blocking read-back, like a game that waits for the GPU on its
// render thread. While the D3D12 queue is stalled, the D3D11 queue is held by
// the bridge's shared-fence wait and no Present runs, so only the bridge's
// watchdog can release this Map (spec 6.4, 11).
bool BlockingReadback(App& a, int frame) {
    if (!a.probe) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = 1;
        td.Height = 1;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        const HRESULT hr = a.device->CreateTexture2D(&td, nullptr, &a.probe);
        if (FAILED(hr)) return Fail("read-back texture creation failed: 0x%08lX", static_cast<unsigned long>(hr));
    }
    const D3D11_BOX box{0, 0, 0, 1, 1, 1};
    a.ctx->CopySubresourceRegion(a.probe.Get(), 0, 0, 0, 0, a.buffer.Get(), 0, &box);
    const double t0 = NowMs();
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = a.ctx->Map(a.probe.Get(), 0, D3D11_MAP_READ, 0, &m);
    const double waited = NowMs() - t0;
    if (FAILED(hr))
        return Fail("frame %d: blocking read-back Map failed after %.0f ms: 0x%08lX (%s)", frame, waited,
                    static_cast<unsigned long>(hr), HrName(hr));
    a.ctx->Unmap(a.probe.Get(), 0);
    if (waited > a.probe_max_ms) {
        a.probe_max_ms = waited;
        a.probe_max_frame = frame;
    }
    if (waited >= 100.0) Print("frame %d: blocking read-back waited %.0f ms", frame, waited);
    return true;
}

bool PresentOk(HRESULT hr) { return hr == S_OK || hr == DXGI_STATUS_OCCLUDED; }

// Proxy only: the emulated waitable object is a semaphore that only real
// presents release (spec 6.3), so right after a Present plus a test present
// its count cannot exceed the latency. The counts taken are given back.
bool CheckLatencyCount(App& a, int frame) {
    LONG count = 0;
    while (count <= 16 && WaitForSingleObject(a.waitable, 0) == WAIT_OBJECT_0) ++count;
    if (count > 0 && !ReleaseSemaphore(a.waitable, count, nullptr))
        return Fail("frame %d: could not give back %ld latency counts: error %lu", frame, count, GetLastError());
    if (count > static_cast<LONG>(kLatency))
        return Fail("frame %d: the latency object holds %ld counts after a test present (latency %u): test presents "
                    "must not release it",
                    frame, count, kLatency);
    return true;
}

// Presents not yet displayed when a frame starts, as the game would see it
// through its own chain: GetLastPresentCount minus the last displayed
// PresentCount. Samples where DXGI has no statistics are skipped.
void SampleQueueDepth(App& a) {
    UINT last = 0;
    DXGI_FRAME_STATISTICS st{};
    if (FAILED(a.chain->GetLastPresentCount(&last)) || FAILED(a.chain->GetFrameStatistics(&st))) return;
    if (st.PresentCount == 0 || last < st.PresentCount) return;
    a.queue_depth_sum += static_cast<double>(last - st.PresentCount);
    ++a.queue_depth_samples;
}

bool RunFrames(App& a) {
    const DWORD limit = a.opt.stall ? kStallFrameLimitMs : kFrameLimitMs;
    const UINT sync = a.opt.vsync ? 1 : 0;
    const UINT flags = (!a.opt.vsync && a.tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    const double capMs = a.opt.fps_cap > 0 ? 1000.0 / a.opt.fps_cap : 0.0;
    Print("presenting %d frames with Present(%u, 0x%X)%s", a.opt.frames, sync, flags,
          a.opt.fps_cap > 0 ? ", CPU-capped" : "");

    int verifyAt = kVerifyDelay;
    const double start = NowMs();
    for (int f = 0; f < a.opt.frames; ++f) {
        if (!PumpMessages()) return Fail("the window was closed at frame %d", f);

        if (a.opt.resize && f == kResizeUpFrame) {
            if (!ResizeChain(a, kResizeWidth, kResizeHeight)) return false;
            verifyAt = f + kVerifyDelay;
        }
        if (a.opt.resize && f == kResizeBackFrame) {
            if (!ResizeChain(a, kWidth, kHeight)) return false;
            verifyAt = f + kVerifyDelay;
        }
        if (a.opt.recreate && f == kRecreateFrame) {
            if (!RecreateChain(a)) return false;
            verifyAt = f + kVerifyDelay;
        }

        const double t0 = NowMs();
        const DWORD w = WaitForSingleObjectEx(a.waitable, limit, TRUE);
        if (w != WAIT_OBJECT_0)
            return Fail("frame %d: the frame-latency wait returned %lu after %lu ms", f, w, limit);
        const double tWaited = NowMs();
        if (a.opt.vsync && f >= kQueueDepthFirstFrame) SampleQueueDepth(a);

        // CSP's order: the Lua app's camera before the scene, DLSS after it,
        // then post-processing, UI and Present.
        if (a.opt.fake_camera) WriteFakeCamera(a, f);
        const RECT rect = RectangleFor(a, f);
        Render(a, rect);
        if (a.opt.fake_ngx) {
            std::string err;
            if (!a.ngx.Evaluate(a.ctx.Get(), static_cast<uint32_t>(f), &err))
                return Fail("frame %d: fake NGX: %s", f, err.c_str());
        }
        if (f == verifyAt && !VerifyBuffer(a, rect, f)) return false;
        if (a.opt.stall && f >= kStallProbeFirst && f <= kStallProbeLast && !BlockingReadback(a, f)) return false;

        const double tRendered = NowMs();
        const HRESULT hr = a.chain->Present(sync, flags);
        const double tPresented = NowMs();
        if (!PresentOk(hr))
            return Fail("frame %d: Present(%u, 0x%X) returned 0x%08lX (%s)", f, sync, flags,
                        static_cast<unsigned long>(hr), HrName(hr));
        if (hr == DXGI_STATUS_OCCLUDED) ++a.occluded;

        const UINT index = a.chain3->GetCurrentBackBufferIndex();
        // The proxy always reports buffer 0 (spec 6.3).
        if (a.is_proxy && index != 0)
            return Fail("frame %d: the log says proxy, but GetCurrentBackBufferIndex returned %u", f, index);

        if (a.opt.test_present && f % kTestPresentEvery == 0) {
            ++a.test_presents;
            const HRESULT thr = a.chain->Present(0, DXGI_PRESENT_TEST);
            if (!PresentOk(thr))
                return Fail("frame %d: Present(0, DXGI_PRESENT_TEST) returned 0x%08lX (%s)", f,
                            static_cast<unsigned long>(thr), HrName(thr));
            if (a.is_proxy && !CheckLatencyCount(a, f)) return false;
        }

        if (capMs > 0) {
            while (NowMs() - t0 < capMs) Sleep(1);
        }
        const double dt = NowMs() - t0;
        if (dt > a.max_frame_ms) {
            a.max_frame_ms = dt;
            a.max_frame_index = f;
        }
        if (dt > kSlowFrameMs && ++a.slow_frames <= kSlowFramesPrinted)
            Print("slow frame %d: %.1f ms (latency wait %.1f, render %.1f, Present %.1f)", f, dt, tWaited - t0,
                  tRendered - tWaited, tPresented - tRendered);
        if (dt > limit) return Fail("frame %d took %.0f ms (limit %lu ms)", f, dt, limit);
    }
    a.frames_done = true;
    const double total = NowMs() - start;
    const double fps = total > 0 ? a.opt.frames * 1000.0 / total : 0.0;
    const UINT refresh = RefreshRateOf(a.hwnd);
    Print("%d frames in %.0f ms (%.1f fps, display %u Hz), longest frame %.1f ms (frame %d), %d frames over %.0f ms, "
          "%d occluded presents, %d test presents",
          a.opt.frames, total, fps, refresh, a.max_frame_ms, a.max_frame_index, a.slow_frames, kSlowFrameMs,
          a.occluded, a.test_presents);
    if (a.opt.stall)
        Print("longest blocking read-back (frames %d-%d): %.0f ms (frame %d)", kStallProbeFirst, kStallProbeLast,
              a.probe_max_ms, a.probe_max_frame);
    // VSync must still pace the game (spec 11, M1). An occluded window is not paced.
    if (a.opt.vsync && refresh > 0 && a.occluded == 0 && fps > refresh * 1.3 + 5.0)
        return Fail("VSync did not pace presentation: %.1f fps on a %u Hz display", fps, refresh);
    // Diagnostic only: DXGI frame statistics of a hidden, DWM-composed window
    // are too noisy for a pass/fail limit (the same run measured 5.1 and 6.9).
    // The queue-depth rule itself is pinned by the unit test
    // Presenter_TakesTheGamesFirstLatencyWait.
    if (a.opt.vsync && a.queue_depth_samples > 0)
        Print("VSync queue depth at frame start: %.2f Presents not yet displayed (%d samples, latency %u)",
              a.queue_depth_sum / a.queue_depth_samples, a.queue_depth_samples, kLatency);
    return true;
}

// ---- final log checks

// The per-frame DLSS-G-off warnings: at most one per distinct reason per 10 s.
bool CheckFgReasonThrottle(const BridgeLog& log) {
    bool ok = true;
    for (const auto& v : testapp::FgReasonThrottleViolations(log.lines, kFgReasonPeriodMs))
        ok = Fail("\"fg: frame without DLSS-G:\" more than once per 10 s: %s", v.c_str());
    return ok;
}

// --expect-fg: the lines of the M3 log line contract for the test app's fake
// DLSS calls (and camera), at the first DLSS feature's render size.
bool CheckFgLog(const App& a, const BridgeLog& log) {
    testapp::FgExpectation e;
    e.pipeline = a.opt.expect_fg == ExpectFg::Pipeline;
    e.renderW = a.ngx_first_w;
    e.renderH = a.ngx_first_h;
    e.fovVDeg = testapp::kFovVDeg;
    e.clipNear = testapp::kClipNear;
    e.clipFar = testapp::kClipFar;
    e.minShare = kFgMinShare;
    e.reasonPeriodMs = kFgReasonPeriodMs;
    const testapp::FgCheckResult r = testapp::CheckFgLines(log.lines, e);
    for (const auto& l : r.evidence) Print("note: fg: %s", l.c_str());
    for (const auto& p : r.problems) Fail("%s", p.c_str());
    return r.problems.empty();
}
bool CheckLogAfterRun(App& a) {
    const BridgeLog log = ReadBridgeLog(a.log_path);
    if (!log.found) return Fail("bridge log not found: %s", Narrow(a.log_path).c_str());
    bool ok = true;
    for (const auto& line : log.lines) {
        // --expect-passthrough-reason: the lines that name the expected
        // reason are the evidence of the pass-through, whatever their level.
        if (!a.opt.passthrough_reason.empty() && line.find(a.opt.passthrough_reason) != std::string::npos) {
            Print("note: expected pass-through reason: %s", line.c_str());
            continue;
        }
        // M3: the throttled per-frame DLSS-G-off reasons are expected in the
        // test app, which has no DLSS-G (checked below and with --expect-fg).
        if (line.find(testapp::kFgFrameWithoutTag) != std::string::npos) {
            Print("note: fg warning: %s", line.c_str());
            continue;
        }
        // Streamline's own lines ("sl: ") included: none may be an error.
        if (line.find("] ERROR ") != std::string::npos) ok = Fail("bridge log error: %s", line.c_str());
        // Streamline's warnings are its own (without the spoof: no DLSS-G on
        // this GPU) and only reported. The stall scenario warns on purpose (the
        // debug stall, its handling, and a resize the stall defers); any other
        // warning is a defect.
        if (line.find("] WARN sl: ") != std::string::npos) {
            Print("note: Streamline warning: %s", line.c_str());
            continue;
        }
        const bool expectedWarning =
            a.opt.stall && (line.find("stall") != std::string::npos || line.find("Stall") != std::string::npos);
        if (line.find("] WARN ") != std::string::npos && !expectedWarning)
            ok = Fail("bridge log warning: %s", line.c_str());
    }
    if (static_cast<int>(log.decisions.size()) != a.chains_created)
        ok = Fail("the bridge log has %zu decisions for %d swap chains", log.decisions.size(), a.chains_created);

    // The banner's mode (bootstrap.cpp): standalone only as dxgi.dll.
    const char* const mode = a.opt.standalone ? "] INFO mode: standalone: " : "] INFO mode: proxy: ";
    if (log.Count(mode) != 1) ok = Fail("the bridge log does not show \"%s\" once", mode + 7);
    if (a.opt.standalone) {
        if (log.Count(" is this bridge (standalone), not ReShade") != 1)
            ok = Fail("the bridge log does not say that the exe folder's dxgi.dll is the bridge");
        if (log.Has("] INFO ReShade: "))
            ok = Fail("the bridge log calls a dxgi.dll ReShade in standalone mode");
    }

    for (const auto& p : testapp::ReleaseProblems(log.lines, a.proxies_released)) ok = Fail("%s", p.c_str());
    if (log.Has("presenter stopped")) ok = Fail("the presenter stopped");

    if (a.proxies_created > 0) {
        char line[64];
        std::snprintf(line, sizeof(line), "SetMaximumFrameLatency(%u): 0x00000000", kLatency);
        if (log.Count(line) != a.proxies_created)
            ok = Fail("the bridge log does not show SetMaximumFrameLatency(%u) for every proxy", kLatency);
        // Resizes of a pass-through chain (after --recreate) never reach the presenter.
        for (const auto& resized : a.proxy_resizes)
            if (!log.Has(resized.c_str())) ok = Fail("the bridge log does not show \"%s\"", resized.c_str());

        // M2: every proxy presents through Streamline's proxy chain with Reflex
        // low latency and the PCL markers (spec 6.4, 7, 9).
        if (log.Count("Streamline: initialised from ") != 1)
            ok = Fail("the bridge log does not show \"Streamline: initialised from\" once");
        if (const int n = log.Count(", Streamline proxy chain"); n != a.proxies_created)
            ok = Fail("%d of %d presenters use the Streamline proxy chain", n, a.proxies_created);
        if (const int n = log.Count("Streamline: Reflex low latency on (lowLatencyAvailable yes)");
            n != a.proxies_created)
            ok = Fail("Reflex low latency is available and on for %d of %d presenters", n, a.proxies_created);
        int dlssg = 0;
        for (const auto& l : log.lines) {
            if (l.find("Streamline: DLSS-G ") == std::string::npos || l.find(" on this adapter (") == std::string::npos)
                continue;
            if (dlssg++ == 0) Print("note: %s", l.c_str());
        }
        if (dlssg != a.proxies_created)
            ok = Fail("the bridge log has %d DLSS-G support lines for %d presenters", dlssg, a.proxies_created);
        for (const auto& l : log.lines) {
            if (l.find(" stats: base_fps=") == std::string::npos) continue;
            const std::string problem = testapp::StatsProblem(testapp::ParseStats(l));
            if (!problem.empty()) ok = Fail("statistics line: %s: %s", problem.c_str(), l.c_str());
        }
        if (!log.Has(" presenter: VRAM (local) budget "))
            ok = Fail("the bridge log does not show the presenter's VRAM budget and usage");
        if (a.opt.stall) {
            if (!log.Has("debug stall")) ok = Fail("the bridge log does not show the debug stall");
            if (!log.Has("StallWatchdog: D3D12 progress stuck"))
                ok = Fail("the bridge log does not show the watchdog releasing the D3D11 queue");
            if (!log.Has("D3D12 stall:")) ok = Fail("the bridge log does not show the stall being handled");
            if (!log.Has("D3D12 stall over")) ok = Fail("the bridge log does not show the end of the stall");
            // Either release path is correct (spec 6.4): the watchdog, or the
            // render thread's 500 ms wait in Present, which since the latency
            // fix usually catches the stall first. Either way no read-back may
            // stay blocked until the 1500 ms debug stall ends on its own.
            if (a.probe_max_ms < kStallProbeHeldMs)
                Print("note: the longest blocking read-back was %.0f ms: the stall was released before the D3D11 "
                      "queue backed up",
                      a.probe_max_ms);
            if (a.probe_max_ms >= kStallProbeWatchdogMs)
                ok = Fail("the blocking read-back at frame %d waited %.0f ms: the watchdog did not release the "
                          "D3D11 queue within %.0f ms",
                          a.probe_max_frame, a.probe_max_ms, kStallProbeWatchdogMs);
            if (a.opt.resize && log.Has("deferred by the D3D12 stall"))
                Print("note: a resize fell into a stall; the bridge deferred it and applied it afterwards");
        }
        if (!log.Has("stats: base_fps=")) Print("note: no statistics line (the run was shorter than a second)");
    }
    // --expect-fg checks the throttle with the rest.
    if (a.opt.expect_fg == ExpectFg::None && !CheckFgReasonThrottle(log)) ok = false;
    if (a.opt.expect_fg != ExpectFg::None) {
        if (a.proxies_created == 0) {
            ok = Fail("--expect-fg needs a proxied swap chain");
        } else if (!CheckFgLog(a, log)) {
            ok = false;
        }
    }
    return ok;
}

// ---- command line

void Usage() {
    std::puts(
        "usage: testapp [--frames N] [--vsync] [--resize] [--test-present] [--recreate] [--stall]\n"
        "               [--expect-proxy | --expect-passthrough] [--fixture NAME] [--fps-cap N] [--hidden]\n"
        "               [--via-dxgi | --standalone] [--expect-passthrough-reason TEXT]\n"
        "               [--fake-ngx] [--fake-camera] [--expect-fg pipeline|no-camera]\n"
        "  --frames N            frames to present (default 600)\n"
        "  --vsync               Present(1, 0) instead of Present(0, ALLOW_TEARING)\n"
        "  --resize              ResizeBuffers to 1600x900 at frame 200 and back to 1280x720 at frame 400\n"
        "  --test-present        an extra Present(0, DXGI_PRESENT_TEST) every 10th frame\n"
        "  --recreate            release and re-create the swap chain at frame 300\n"
        "  --stall               ACDLSSG_DEBUG_STALL_MS=1500: the D3D12 queue stalls; blocking read-backs on\n"
        "                        frames 20-60 must be released by the bridge's watchdog, frames must resume\n"
        "  --expect-proxy        fail unless the bridge proxies every swap chain until a proxy is released;\n"
        "                        later chains must pass through with \"Streamline already shut down\"\n"
        "  --expect-passthrough  fail unless the bridge passes every swap chain through\n"
        "  --fixture NAME        docs fixture: default = fixture\\docs, else fixture\\NAME\\docs\n"
        "  --fps-cap N           CPU frame limiter like CSP's FPS_CAP (--stall defaults to 200)\n"
        "  --gpu-load N          N extra 4096x4096 RGBA16F copies per frame (GPU-bound frames)\n"
        "  --cpu-load-ms N       N ms of CPU busy work per frame (CPU-bound frames)\n"
        "  --hidden              never show the window\n"
        "  --via-dxgi            the factory from dxgi.dll next to the exe (ReShade with [PROXY]\n"
        "                        ProxyLibrary=ac-dlssg.dll) instead of loading the bridge directly\n"
        "  --standalone          the bridge is dxgi.dll next to the exe: bound at process start through\n"
        "                        d3d11.dll's import, the factory from LoadLibraryW(L\"dxgi.dll\") by name\n"
        "  --expect-passthrough-reason TEXT\n"
        "                        the pass-through's decision line, or the line of its failed proxy creation,\n"
        "                        names TEXT; lines naming TEXT may be warnings or errors\n"
        "  --fake-ngx            CSP's DLSS calls against fake_nvngx.dll next to the exe: one SuperSampling\n"
        "                        create (render size 2/3 of the window, MVLowRes), then per frame an evaluate\n"
        "                        with depth, ping-ponged motion vectors, Halton jitter, MV.Scale and Reset\n"
        "  --fake-camera         the CSP Lua app's camera: Local\\AcDlssg.Camera.v1 written before every frame\n"
        "                        (refused while acs.exe runs, which shares that section)\n"
        "  --expect-fg MODE      pipeline: the bridge log shows the capture, camera, constants and first-tags\n"
        "                        lines, and the statistics captures, camera_fresh and tagged at 90% of base_fps;\n"
        "                        no-camera: captures, but no fresh camera and no tags, and a warning naming\n"
        "                        the camera");
}

bool ParseInt(const wchar_t* s, int minimum, int* out) {
    wchar_t* end = nullptr;
    const long v = std::wcstol(s, &end, 10);
    if (!end || *end != L'\0' || v < minimum || v > 1000000) return false;
    *out = static_cast<int>(v);
    return true;
}

bool ParseArgs(int argc, wchar_t** argv, Options* o) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == L"--frames" && hasValue) {
            if (!ParseInt(argv[++i], 1, &o->frames)) return false;
        } else if (arg == L"--fps-cap" && hasValue) {
            if (!ParseInt(argv[++i], 1, &o->fps_cap)) return false;
        } else if (arg == L"--gpu-load" && hasValue) {
            if (!ParseInt(argv[++i], 1, &o->gpu_load)) return false;
        } else if (arg == L"--cpu-load-ms" && hasValue) {
            if (!ParseInt(argv[++i], 1, &o->cpu_load_ms)) return false;
        } else if (arg == L"--fixture" && hasValue) {
            o->fixture = argv[++i];
            if (o->fixture.empty() || o->fixture.find_first_of(L"\\/.:") != std::wstring::npos) return false;
        } else if (arg == L"--vsync") {
            o->vsync = true;
        } else if (arg == L"--resize") {
            o->resize = true;
        } else if (arg == L"--test-present") {
            o->test_present = true;
        } else if (arg == L"--recreate") {
            o->recreate = true;
        } else if (arg == L"--stall") {
            o->stall = true;
        } else if (arg == L"--hidden") {
            o->hidden = true;
        } else if (arg == L"--via-dxgi") {
            if (o->standalone) return false;
            o->via_dxgi = true;
        } else if (arg == L"--standalone") {
            if (o->via_dxgi) return false;
            o->standalone = true;
        } else if (arg == L"--expect-proxy") {
            if (o->expect == Expect::Passthrough) return false;
            o->expect = Expect::Proxy;
        } else if (arg == L"--expect-passthrough") {
            if (o->expect == Expect::Proxy) return false;
            o->expect = Expect::Passthrough;
        } else if (arg == L"--expect-passthrough-reason" && hasValue) {
            o->passthrough_reason = Narrow(argv[++i]);
            if (o->passthrough_reason.empty()) return false;
        } else if (arg == L"--fake-ngx") {
            o->fake_ngx = true;
        } else if (arg == L"--fake-camera") {
            o->fake_camera = true;
        } else if (arg == L"--expect-fg" && hasValue) {
            const std::wstring mode = argv[++i];
            if (mode == L"pipeline") {
                o->expect_fg = ExpectFg::Pipeline;
            } else if (mode == L"no-camera") {
                o->expect_fg = ExpectFg::NoCamera;
            } else {
                return false;
            }
        } else {
            return false;
        }
    }
    if (o->stall && o->fps_cap == 0) o->fps_cap = kStallFpsCap;
    // The DLSS evaluate is what --expect-fg checks; a pipeline needs the camera.
    if (o->expect_fg != ExpectFg::None && !o->fake_ngx) return false;
    if (o->expect_fg == ExpectFg::Pipeline && !o->fake_camera) return false;
    if (o->expect_fg == ExpectFg::NoCamera && o->fake_camera) return false;
    return true;
}

int Run(App& a) {
    a.exe_dir = ExeDir();
    a.log_path = a.exe_dir + L"\\ac-dlssg\\logs\\bridge.log";
    const std::wstring docs = DocsDirFor(a.exe_dir, a.opt.fixture);
    if (!FileExists(docs + L"\\cfg\\video.ini")) {
        Print("fixture \"%s\" not found: %s\\cfg\\video.ini", Narrow(a.opt.fixture).c_str(), Narrow(docs).c_str());
        return 2;
    }
    if (!DirExists(a.exe_dir + L"\\extension\\config"))
        Print("note: %s\\extension\\config does not exist", Narrow(a.exe_dir).c_str());
    // The camera section is per logon session: the game's bridge would read
    // the fake camera, and the bridge here the game's.
    if ((a.opt.fake_camera || a.opt.fake_ngx) && GameRunning()) {
        Print("acs.exe is running; --fake-camera and --fake-ngx would share %s with it",
              Narrow(testapp::kCameraSectionName).c_str());
        return 2;
    }

    // Read by the bridge's bootstrap and presenter; set before the DLL runs.
    SetEnvironmentVariableW(L"ACDLSSG_DOCS_DIR", docs.c_str());
    SetEnvironmentVariableW(L"ACDLSSG_DEBUG_STALL_MS", a.opt.stall ? kStallMs : nullptr);
    // A stale log from an earlier run must not answer for this one.
    if (FileExists(a.log_path) && !DeleteFileW(a.log_path.c_str())) {
        Print("cannot delete the old bridge log %s: error %lu", Narrow(a.log_path).c_str(), GetLastError());
        return 2;
    }
    Print("fixture %s (ACDLSSG_DOCS_DIR=%s)%s", Narrow(a.opt.fixture).c_str(), Narrow(docs).c_str(),
          a.opt.stall ? ", ACDLSSG_DEBUG_STALL_MS=1500" : "");

    a.hwnd = CreateGameWindow(!a.opt.hidden);
    if (!a.hwnd) {
        Print("cannot create the acsW window: error %lu", GetLastError());
        return 2;
    }
    PumpMessages();

    bool ok = CreateDevice(a) && CreateChain(a) && SetUpCspFakes(a) && RunFrames(a);
    if (!CheckCspFakes(a)) ok = false;
    ReleaseCspFakes(a);
    const int created = a.chains_created;
    ReleaseChain(a);
    if (const int hidden = CountBridgeHiddenWindows(); hidden != 0)
        ok = Fail("%d hidden bridge windows remain after the swap chain was released", hidden);
    if (created > 0 && !CheckLogAfterRun(a)) ok = false;

    a.probe.Reset();
    a.ctx1.Reset();
    a.ctx.Reset();
    a.device.Reset();
    a.factory.Reset();
    DestroyWindow(a.hwnd);
    PumpMessages();
    // The bridge stays loaded: it patched the DXGI factory vtable, which
    // outlives this module like it does in the game.
    if (!ok || !g_failure.empty()) {
        PrintLogTail(a.log_path, 40);
        Print("FAIL: %s", g_failure.empty() ? "unknown failure" : g_failure.c_str());
        return 1;
    }
    Print("PASS (%d swap chain(s), %d proxied)", a.chains_created, a.proxies_created);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // Unbuffered, so that a run killed on timeout still shows how far it got.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // The NGX runtime that Streamline loads here can start nvngx_update.exe
    // with inherited handles; with the pipes of run-testapp.ps1 among them,
    // the script's reads would wait for the updater to exit, minutes after
    // this process has.
    for (const DWORD std : {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE, STD_INPUT_HANDLE}) {
        const HANDLE h = GetStdHandle(std);
        if (h && h != INVALID_HANDLE_VALUE) SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0);
    }
    App app;
    if (!ParseArgs(argc, argv, &app.opt)) {
        Usage();
        return 2;
    }
    try {
        return Run(app);
    } catch (...) {
        std::puts("testapp: FAIL: unexpected C++ exception");
        return 1;
    }
}
