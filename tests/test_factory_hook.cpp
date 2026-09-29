// FactoryHook, Bootstrap and the DLL's DXGI exports.
//
// Side effects on the rest of the test run: the factory hook patches the DXGI
// factory class vtable of this process for good, and the DLL test leaves
// ac-dlssg.dll loaded (its hooks sit in that vtable too). Both pass any
// swap chain that is not on an "acsW" window straight through. The in-process
// bootstrap runs once with ACDLSSG_DOCS_DIR pointing at an empty directory, so
// it never reads the real Documents folder, and it writes its log under
// <test exe dir>\ac-dlssg\logs.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <dxgidebug.h>
#include <wrl/client.h>

#include <cstdio>
#include <regex>
#include <string>

#include "adapter_caps.h"
#include "bootstrap.h"
#include "compat.h"
#include "factory_hook.h"
#include "gpu_info.h"
#include "gpu_test_devices.h"
#include "internal_call.h"
#include "log.h"
#include "panel_control.h"
#include "panel_status.h"
#include "system_dxgi.h"
#include "temp_dir.h"
#include "test_framework.h"

using Microsoft::WRL::ComPtr;
using namespace acdb;

namespace {

// Sets an environment variable for the lifetime of the object, then restores it.
class EnvOverride {
public:
    EnvOverride(const wchar_t* name, const std::wstring& value) : name_(name) {
        const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
        if (n > 0) {
            old_.assign(n, L'\0');
            const DWORD got = GetEnvironmentVariableW(name, old_.data(), n);
            old_.resize(got < n ? got : 0);
            had_ = true;
        }
        SetEnvironmentVariableW(name, value.c_str());
    }
    ~EnvOverride() { SetEnvironmentVariableW(name_.c_str(), had_ ? old_.c_str() : nullptr); }
    EnvOverride(const EnvOverride&) = delete;
    EnvOverride& operator=(const EnvOverride&) = delete;

private:
    std::wstring name_;
    std::wstring old_;
    bool had_ = false;
};

// A hidden top-level window of its own class; the class is unregistered again
// so that a later test can register the same name with different case.
class TestWindow {
public:
    explicit TestWindow(const wchar_t* className, int width = 320, int height = 240) : class_(className) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = className;
        registered_ = RegisterClassExW(&wc) != 0;
        hwnd_ = CreateWindowExW(0, className, L"acdb test", WS_OVERLAPPEDWINDOW, 0, 0, width, height, nullptr,
                                nullptr, wc.hInstance, nullptr);
    }
    ~TestWindow() {
        if (hwnd_) DestroyWindow(hwnd_);
        if (registered_) UnregisterClassW(class_.c_str(), GetModuleHandleW(nullptr));
    }
    TestWindow(const TestWindow&) = delete;
    TestWindow& operator=(const TestWindow&) = delete;

    HWND Get() const { return hwnd_; }

private:
    std::wstring class_;
    bool registered_ = false;
    HWND hwnd_ = nullptr;
};

HMODULE ModuleOf(const void* address) {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(address), &module);
    return module;
}

void** VtableOf(IUnknown* object) { return *reinterpret_cast<void***>(object); }

std::wstring ExeDir() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path, n);
    const size_t sep = p.find_last_of(L"\\/");
    return sep == std::wstring::npos ? std::wstring() : p.substr(0, sep);
}

ComPtr<IDXGIFactory2> NewSystemFactory() {
    ComPtr<IDXGIFactory2> factory;
    const SystemDxgi& sys = GetSystemDxgi();
    if (sys.CreateDXGIFactory1)
        sys.CreateDXGIFactory1(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory.GetAddressOf()));
    return factory;
}

// The factory that created the device's adapter: swap chains must come from it.
ComPtr<IDXGIFactory2> FactoryOf(ID3D11Device* device) {
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgiDevice.GetAddressOf()))) &&
        SUCCEEDED(dxgiDevice->GetAdapter(&adapter)))
        adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory.GetAddressOf()));
    return factory;
}

DXGI_SWAP_CHAIN_DESC1 SmallDesc(UINT width, UINT height) {
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = width;
    d.Height = height;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
    d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    return d;
}

// A real DXGI chain (not ours) whose buffer 0 is a D3D11 texture.
bool IsRealChain(IDXGISwapChain1* chain) {
    if (!chain) return false;
    if (ModuleOf(VtableOf(chain)) == GetModuleHandleW(nullptr)) return false;
    ComPtr<ID3D11Texture2D> buffer;
    return SUCCEEDED(chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(buffer.GetAddressOf()))) &&
           buffer;
}

std::wstring g_bootstrap_docs;

// Runs the in-process bootstrap once with the Documents folder redirected to
// an empty directory, then closes the log it opened so that later tests start
// with the log closed.
const BootstrapState& EnsureBootstrap() {
    static bool done = false;
    if (!done) {
        done = true;
        acdb_test::TempDir docs(L"bootstrap_docs");
        EnvOverride env(L"ACDLSSG_DOCS_DIR", docs.Str());
        g_bootstrap_docs = docs.Str();
        BootstrapRunOnce();
        LogClose();
    }
    return BootstrapRunOnce();
}

// Bootstrap first, so that the hook never triggers it with the real Documents folder.
bool EnsureHookInstalled() {
    EnsureBootstrap();
    if (!FactoryHookInstalled()) {
        ComPtr<IDXGIFactory2> factory = NewSystemFactory();
        if (factory) FactoryHookInstallOnce(factory.Get());
    }
    return FactoryHookInstalled();
}

// The adapter of a D3D11 device, the way CSP's device is asked.
bool AdapterDescOf(ID3D11Device* device, DXGI_ADAPTER_DESC* out) {
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    return SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgiDevice.GetAddressOf()))) &&
           SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(out));
}

// One swap chain on a new "acsW" window through the hooked factory, with the
// log open at Debug; returns the log. The chain is released before it returns.
std::string MainWindowChainLog(IDXGIFactory2* factory, const acdb_test::GpuTestDevices& dev, const wchar_t* tag) {
    acdb_test::TempDir dir(tag);
    const std::wstring logPath = dir.Str() + L"\\hook.log";
    if (!LogOpen(logPath, LogLevel::Debug)) return {};
    {
        TestWindow window(L"acsW", 400, 300);
        const DXGI_SWAP_CHAIN_DESC1 desc = SmallDesc(0, 0);
        ComPtr<IDXGISwapChain1> chain;
        const HRESULT hr = factory->CreateSwapChainForHwnd(dev.device11.Get(), window.Get(), &desc, nullptr, nullptr,
                                                           chain.GetAddressOf());
        if (FAILED(hr)) std::printf("  CreateSwapChainForHwnd: 0x%08lX\n", static_cast<unsigned long>(hr));
        CHECK(SUCCEEDED(hr) && IsRealChain(chain.Get()));
    }
    dev.ctx11->ClearState();
    dev.ctx11->Flush();
    LogClose();
    return acdb_test::ReadAll(logPath);
}

LUID MakeLuid(DWORD low, LONG high) {
    LUID l{};
    l.LowPart = low;
    l.HighPart = high;
    return l;
}

}  // namespace

// ---------------------------------------------------------------- decision

TEST(FactoryHook_ShouldProxyTruthTable) {
    int trueCount = 0;
    for (unsigned bits = 0; bits < 128; ++bits) {
        ProxyDecisionInputs in;
        in.internal_call = (bits & 1) != 0;
        in.is_d3d11_device = (bits & 2) != 0;
        in.is_main_window = (bits & 4) != 0;
        in.bootstrap_possible = (bits & 8) != 0;
        in.compat_ok = (bits & 16) != 0;
        in.streamline_shut_down = (bits & 32) != 0;
        in.nvidia_adapter = (bits & 64) != 0;
        const bool expected = !in.internal_call && in.is_d3d11_device && in.is_main_window &&
                              in.bootstrap_possible && in.compat_ok && !in.streamline_shut_down && in.nvidia_adapter;
        if (ShouldProxy(in) != expected) std::printf("  mismatch for bits 0x%02X\n", bits);
        CHECK_EQ(ShouldProxy(in), expected);
        if (ShouldProxy(in)) ++trueCount;
    }
    CHECK_EQ(trueCount, 1);
    CHECK(!ShouldProxy(ProxyDecisionInputs()));  // defaults never proxy
}

// The Optimus laptop: CSP's device on the Intel iGPU (acs.exe not set to
// High performance) passes through with the adapter named.
TEST(FactoryHook_RenderAdapterMustBeNvidia) {
    RenderAdapter a;
    a.known = true;
    a.vendor_id = 0x10DE;
    a.device_id = 0x25A0;
    a.description = "NVIDIA GeForce RTX 3050 Ti Laptop GPU";
    CHECK_EQ(RenderAdapterRefusal(a), "");

    a.vendor_id = 0x8086;
    a.device_id = 0x9A68;
    a.description = "Intel(R) UHD Graphics";
    CHECK_EQ(RenderAdapterRefusal(a),
             "CSP renders on Intel(R) UHD Graphics (vendor 0x8086), not an NVIDIA GPU: set acs.exe to High "
             "performance in Windows Settings > System > Display > Graphics, and check SELECT_ADAPTER in CSP's "
             "dxgi_tweaks.ini");

    a.vendor_id = 0x1414;
    a.description = "Microsoft Basic Render Driver";
    CHECK_EQ(RenderAdapterRefusal(a),
             "CSP renders on Microsoft Basic Render Driver (vendor 0x1414), not an NVIDIA GPU: set acs.exe to High "
             "performance in Windows Settings > System > Display > Graphics, and check SELECT_ADAPTER in CSP's "
             "dxgi_tweaks.ini");

    RenderAdapter unknown;
    unknown.error = "IDXGIDevice::GetAdapter/GetDesc failed: 0x887A0005";
    CHECK_EQ(RenderAdapterRefusal(unknown),
             "the adapter of CSP's device is unknown (IDXGIDevice::GetAdapter/GetDesc failed: 0x887A0005)");
}

// Reasons in order: a disabled or impossible bridge first, then the adapter,
// then the compatibility rules.
TEST(FactoryHook_PassThroughReasonOrder) {
    ProxyDecisionInputs in;
    in.is_d3d11_device = true;
    in.is_main_window = true;
    in.bootstrap_possible = true;
    in.compat_ok = true;
    in.nvidia_adapter = true;
    CHECK_EQ(PassThroughReason(in, "boot", "adapter", "compat"), "");
    in.compat_ok = false;
    CHECK_EQ(PassThroughReason(in, "boot", "adapter", "compat"), "compat");
    in.nvidia_adapter = false;
    CHECK_EQ(PassThroughReason(in, "boot", "adapter", "compat"), "adapter");
    in.streamline_shut_down = true;
    CHECK_EQ(PassThroughReason(in, "boot", "adapter", "compat"), "Streamline already shut down");
    in.bootstrap_possible = false;
    CHECK_EQ(PassThroughReason(in, "boot", "adapter", "compat"), "boot");
    in.is_main_window = false;
    CHECK_EQ(PassThroughReason(in, "boot", "adapter", "compat"), "not the main game window (class acsW)");
}

TEST(FactoryHook_MainGameWindowClassName) {
    CHECK(IsMainGameWindowClassName(L"acsW", 4));
    // Case-sensitive, although class registration is not.
    CHECK(!IsMainGameWindowClassName(L"ACSW", 4));
    CHECK(!IsMainGameWindowClassName(L"acsw", 4));
    // A longer name starting with acsW, or a shorter one, must not match.
    CHECK(!IsMainGameWindowClassName(L"acsWindow", 9));
    CHECK(!IsMainGameWindowClassName(L"acsW", 3));
    CHECK(!IsMainGameWindowClassName(L"acs", 3));
    CHECK(!IsMainGameWindowClassName(L"", 0));
    CHECK(!IsMainGameWindowClassName(nullptr, 4));
}

// The case and truncation rules are tested on strings above: the class name a
// window reports is the spelling of the session-wide atom, so an "ACSW" window
// reads back as "acsW" whenever another process (the game, a running testapp)
// has registered "acsW".
TEST(FactoryHook_IsMainGameWindow) {
    CHECK(!IsMainGameWindow(nullptr));
    {
        TestWindow acs(L"acsW");
        REQUIRE(acs.Get() != nullptr);
        CHECK(IsMainGameWindow(acs.Get()));
    }
    {
        TestWindow other(L"acdbFactoryHookOther");
        REQUIRE(other.Get() != nullptr);
        CHECK(!IsMainGameWindow(other.Get()));
    }
    HWND destroyed = nullptr;
    {
        TestWindow gone(L"acsW");
        destroyed = gone.Get();
    }
    CHECK(!IsMainGameWindow(destroyed));
}

// ---------------------------------------------------------------- HAGS per adapter

TEST(FactoryHook_DecideHagsPrefersD3dkmtOverTheRegistry) {
    const LUID luid = MakeLuid(0x140D9, 0);
    AdapterHags kmt;
    kmt.state = HagsState::On;
    kmt.supported = true;
    HagsDecision d = DecideHags(luid, kmt, false);
    CHECK(d.on);
    CHECK_EQ(d.source, "D3DKMT for LUID 00000000:000140D9");

    // The laptop case the other way round: the registry says on, the adapter off.
    kmt.state = HagsState::Off;
    d = DecideHags(luid, kmt, true);
    CHECK(!d.on);
    CHECK_EQ(d.source, "D3DKMT for LUID 00000000:000140D9");
}

TEST(FactoryHook_DecideHagsFallsBackToTheRegistryOnlyWithoutAnAnswer) {
    const LUID luid = MakeLuid(0x2A, 1);
    AdapterHags kmt;  // Unknown
    kmt.reason = "D3DKMTQueryAdapterInfo(KMTQAITYPE_WDDM_2_7_CAPS) failed: NTSTATUS 0xC00000BB";
    HagsDecision d = DecideHags(luid, kmt, true);
    CHECK(d.on);
    CHECK_EQ(d.source, "registry fallback (D3DKMT for LUID 00000001:0000002A: "
                       "D3DKMTQueryAdapterInfo(KMTQAITYPE_WDDM_2_7_CAPS) failed: NTSTATUS 0xC00000BB)");
    d = DecideHags(luid, kmt, false);
    CHECK(!d.on);
    CHECK(d.source.rfind("registry fallback (", 0) == 0);
}

TEST(FactoryHook_ChainCompatTakesHagsFromTheDecision) {
    const char* const kHags = "hardware-accelerated GPU scheduling is off";
    CompatInputs in;
    in.fsr_active = 1;
    in.fsr_old_implementation = 3;
    in.hags_on = false;  // the bootstrap's registry value: must not decide
    HagsDecision on;
    on.on = true;
    on.source = "D3DKMT for LUID 00000000:000140D9";
    CompatResult r = EvaluateChainCompat(in, on, 1920, 1080);
    CHECK(r.ok);
    CHECK(r.reason.empty());

    // Rule 10 keeps its text and names the source that decided.
    in.hags_on = true;
    HagsDecision off;
    off.on = false;
    off.source = "D3DKMT for LUID 00000000:000140D9";
    r = EvaluateChainCompat(in, off, 1920, 1080);
    CHECK(!r.ok);
    CHECK_EQ(r.reason, std::string(kHags) + " (D3DKMT for LUID 00000000:000140D9)");
    off.source = "registry fallback (D3DKMT for LUID 00000000:000140D9: failed)";
    r = EvaluateChainCompat(in, off, 1920, 1080);
    CHECK_EQ(r.reason, std::string(kHags) + " (registry fallback (D3DKMT for LUID 00000000:000140D9: failed))");

    // Earlier rules still come first, with their own text.
    in.fsr_active = 0;
    r = EvaluateChainCompat(in, off, 1920, 1080);
    CHECK_EQ(r.reason, "CSP upscaler is not DLSS (need [FSR] ACTIVE=1, OLD_IMPLEMENTATION=3)");
}

// Through the hook: the main window's chain is decided with HAGS from the
// LUID of the device's own adapter.
TEST(FactoryHook_MainWindowTakesHagsFromTheDevicesAdapter) {
    REQUIRE(EnsureHookInstalled());
    acdb_test::GpuTestDevices dev;
    if (!acdb_test::CreateGpuTestDevices(&dev)) {
        std::printf("  no GPU device; skipped\n");
        return;
    }
    ComPtr<IDXGIFactory2> factory = FactoryOf(dev.device11.Get());
    REQUIRE(factory);
    DXGI_ADAPTER_DESC ad{};
    REQUIRE(AdapterDescOf(dev.device11.Get(), &ad));
    const std::string luid = LuidText(ad.AdapterLuid);
    const AdapterKmtInfo kmt = QueryAdapterKmt(ad.AdapterLuid);

    const std::string log = MainWindowChainLog(factory.Get(), dev, L"hook_hags");
    char ids[64];
    std::snprintf(ids, sizeof(ids), ", vendor 0x%04X device 0x%04X, LUID ", ad.VendorId, ad.DeviceId);
    CHECK(log.find(" INFO render adapter: ") != std::string::npos);
    CHECK(log.find(ids + luid + "; HAGS ") != std::string::npos);
    if (kmt.hags.state != HagsState::Unknown) {
        CHECK(log.find(std::string("; HAGS ") + HagsStateName(kmt.hags.state) + " (D3DKMT for LUID " + luid + ")\n") !=
              std::string::npos);
    } else {
        CHECK(log.find("(registry fallback (D3DKMT for LUID " + luid + ": ") != std::string::npos);
    }
    CHECK(log.find("ACDLSSG_DEBUG_HAGS") == std::string::npos);
    if (log.find(" INFO render adapter: ") == std::string::npos) std::printf("  log:\n%s\n", log.c_str());
}

// The render adapter is the adapter of the device CSP passes, never adapter
// 0: a device on WARP (adapter 1 on a machine with a GPU) is reported as WARP.
TEST(FactoryHook_RenderAdapterIsTheDevicesOwn) {
    REQUIRE(EnsureHookInstalled());
    ComPtr<IDXGIFactory4> factory4;
    REQUIRE(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory4))));
    ComPtr<IDXGIAdapter1> warp;
    REQUIRE(SUCCEEDED(factory4->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
    DXGI_ADAPTER_DESC1 wd{};
    REQUIRE(SUCCEEDED(warp->GetDesc1(&wd)));
    acdb_test::GpuTestDevices dev;
    const D3D_FEATURE_LEVEL fl11 = D3D_FEATURE_LEVEL_11_0;
    REQUIRE(SUCCEEDED(D3D11CreateDevice(warp.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl11, 1, D3D11_SDK_VERSION,
                                        &dev.device11, nullptr, &dev.ctx11)));
    ComPtr<IDXGIFactory2> factory = FactoryOf(dev.device11.Get());
    REQUIRE(factory);

    const std::string log = MainWindowChainLog(factory.Get(), dev, L"hook_warp");
    char ids[64];
    std::snprintf(ids, sizeof(ids), ", vendor 0x%04X device 0x%04X, LUID ", wd.VendorId, wd.DeviceId);
    CHECK(log.find(ids + LuidText(wd.AdapterLuid) + "; HAGS ") != std::string::npos);
    if (log.find(ids) == std::string::npos) std::printf("  log:\n%s\n", log.c_str());
}

// ACDLSSG_DEBUG_HAGS, the test hook: "fail" makes the D3DKMT query count as
// failed, so the registry value read at bootstrap decides.
TEST(FactoryHook_DebugHagsFailFallsBackToTheRegistry) {
    REQUIRE(EnsureHookInstalled());
    acdb_test::GpuTestDevices dev;
    if (!acdb_test::CreateGpuTestDevices(&dev)) {
        std::printf("  no GPU device; skipped\n");
        return;
    }
    ComPtr<IDXGIFactory2> factory = FactoryOf(dev.device11.Get());
    REQUIRE(factory);
    DXGI_ADAPTER_DESC ad{};
    REQUIRE(AdapterDescOf(dev.device11.Get(), &ad));
    const std::string luid = LuidText(ad.AdapterLuid);
    const bool registry = EnsureBootstrap().compat.hags_on;

    std::string log;
    {
        EnvOverride env(L"ACDLSSG_DEBUG_HAGS", L"fail");
        log = MainWindowChainLog(factory.Get(), dev, L"hook_hags_fail");
    }
    CHECK(log.find(" WARN ACDLSSG_DEBUG_HAGS=fail: ") != std::string::npos);
    CHECK(log.find(std::string("; HAGS ") + (registry ? "on" : "off") + " (registry fallback (D3DKMT for LUID " + luid +
                   ": ACDLSSG_DEBUG_HAGS=fail))\n") != std::string::npos);
    if (log.find("registry fallback") == std::string::npos) std::printf("  log:\n%s\n", log.c_str());
}

// "off" replaces the D3DKMT answer with off; any other value is ignored.
TEST(FactoryHook_DebugHagsOffReplacesTheD3dkmtAnswer) {
    REQUIRE(EnsureHookInstalled());
    acdb_test::GpuTestDevices dev;
    if (!acdb_test::CreateGpuTestDevices(&dev)) {
        std::printf("  no GPU device; skipped\n");
        return;
    }
    ComPtr<IDXGIFactory2> factory = FactoryOf(dev.device11.Get());
    REQUIRE(factory);
    DXGI_ADAPTER_DESC ad{};
    REQUIRE(AdapterDescOf(dev.device11.Get(), &ad));
    const std::string luid = LuidText(ad.AdapterLuid);
    const AdapterKmtInfo kmt = QueryAdapterKmt(ad.AdapterLuid);

    std::string log;
    {
        EnvOverride env(L"ACDLSSG_DEBUG_HAGS", L"off");
        log = MainWindowChainLog(factory.Get(), dev, L"hook_hags_off");
    }
    CHECK(log.find(" WARN ACDLSSG_DEBUG_HAGS=off: ") != std::string::npos);
    CHECK(log.find("; HAGS off (D3DKMT for LUID " + luid + ")\n") != std::string::npos);
    {
        EnvOverride env(L"ACDLSSG_DEBUG_HAGS", L"on");
        log = MainWindowChainLog(factory.Get(), dev, L"hook_hags_other");
    }
    CHECK(log.find(" WARN ACDLSSG_DEBUG_HAGS=on is not \"off\" or \"fail\"; ignored") != std::string::npos);
    if (kmt.hags.state != HagsState::Unknown)
        CHECK(log.find(std::string("; HAGS ") + HagsStateName(kmt.hags.state) + " (D3DKMT for LUID " + luid + ")\n") !=
              std::string::npos);
}

// ---------------------------------------------------------------- bootstrap

TEST(Bootstrap_StateAndBanner) {
    const HMODULE before = BridgeModule();
    SetBridgeModule(GetModuleHandleW(nullptr));
    CHECK(BridgeModule() == GetModuleHandleW(nullptr));
    SetBridgeModule(before);

    const BootstrapState& s = EnsureBootstrap();
    CHECK(&BootstrapRunOnce() == &s);  // runs once
    CHECK(s.game_dir == ExeDir());
    CHECK(s.data_dir == ExeDir() + L"\\ac-dlssg");
    CHECK(s.docs_ac_dir == g_bootstrap_docs);
    if (s.config.enabled) {
        // M2: possible needs Streamline from <data_dir>\sl, which the test
        // exe dir does not have.
        CHECK_EQ(s.possible, s.streamline_ok);
        if (s.streamline_ok) {
            CHECK(s.reason.empty());
        } else {
            std::printf("  reason: %s\n", s.reason.c_str());
            CHECK(!s.streamline_error.empty());
            CHECK(s.reason == "Streamline: " + s.streamline_error);
        }
    } else {
        CHECK(!s.possible);
        CHECK(s.reason == "disabled in ac-dlssg.ini");
    }
    // The redirected Documents folder is empty and the test dir has no CSP config.
    CHECK(!s.compat.fsr_active.has_value());
    CHECK(!s.compat.video_width.has_value());

    const std::string log = acdb_test::ReadAll(s.data_dir + L"\\logs\\bridge.log");
    CHECK(log.find(BannerLine() + "\n") != std::string::npos);
    CHECK(log.find("host: ") != std::string::npos);
    CHECK(log.find("Windows ") != std::string::npos);
    CHECK(log.find("adapter 0: ") != std::string::npos);
    CHECK(log.find("LUID ") != std::string::npos);
    CHECK(log.find("compat: graphics_adjustments.ini [FSR] ACTIVE=unset") != std::string::npos);
    // The registry value is only the fallback for the per-adapter state.
    CHECK(log.find("compat: HAGS registry fallback (HwSchMode) ") != std::string::npos);
    CHECK(log.find("bridge: ") != std::string::npos);
    CHECK(log.find("spoof: ") != std::string::npos);
    CHECK(log.find("driver profile: ") != std::string::npos);
    CHECK(log.find("Streamline: ") != std::string::npos);
    // The driver profile is read before Streamline and before any device.
    CHECK(log.find("driver profile: ") < log.find("Streamline: "));
    // Hybrid diagnostics for every DXGI adapter: D3DKMT adapter type, HAGS
    // and the number of outputs; driver warnings for NVIDIA adapters.
    ComPtr<IDXGIFactory1> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i, adapter.Reset()) {
        DXGI_ADAPTER_DESC1 d{};
        REQUIRE(SUCCEEDED(adapter->GetDesc1(&d)));
        UINT outputs = 0;
        for (ComPtr<IDXGIOutput> o; outputs < 64 && SUCCEEDED(adapter->EnumOutputs(outputs, &o)); o.Reset()) ++outputs;
        const std::string line =
            "adapter " + std::to_string(i) + " D3DKMT: " + AdapterKmtText(QueryAdapterKmt(d.AdapterLuid), outputs) + "\n";
        if (log.find(line) == std::string::npos) std::printf("  missing: %s", line.c_str());
        CHECK(log.find(line) != std::string::npos);
        LARGE_INTEGER umd{};
        if (d.VendorId == 0x10DE && SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
            for (const std::string& w : NvidiaDriverWarnings(NvidiaDriverVersion(umd.QuadPart)))
                CHECK(log.find(" WARN adapter " + std::to_string(i) + ": " + w + "\n") != std::string::npos);
            if (NvidiaDriverWarnings(NvidiaDriverVersion(umd.QuadPart)).empty())
                CHECK(log.find(" WARN adapter " + std::to_string(i) + ": NVIDIA driver ") == std::string::npos);
        }
    }
    if (log.find("adapter 0: ") == std::string::npos) std::printf("  log:\n%s\n", log.c_str());
}

// ---------------------------------------------------------------- vtable patch

TEST(FactoryHook_PatchesFactoryClassAndPassesThrough) {
    REQUIRE(EnsureHookInstalled());
    const HMODULE self = GetModuleHandleW(nullptr);

    // Class-wide: a factory created after the patch already has our slots.
    ComPtr<IDXGIFactory2> second = NewSystemFactory();
    REQUIRE(second);
    void** vtable = VtableOf(second.Get());
    CHECK(ModuleOf(vtable[kSlotCreateSwapChain]) == self);
    CHECK(ModuleOf(vtable[kSlotCreateSwapChainForHwnd]) == self);
    CHECK(ModuleOf(vtable[kSlotCreateSwapChainForCoreWindow]) == self);
    CHECK(ModuleOf(vtable[kSlotCreateSwapChainForComposition]) == self);
    // Untouched neighbours still point into DXGI.
    CHECK(ModuleOf(vtable[kSlotCreateSwapChainForHwnd - 1]) != self);
    CHECK(ModuleOf(vtable[kSlotCreateSwapChainForCoreWindow + 1]) != self);

    // A second install is a no-op.
    FactoryHookInstallOnce(second.Get());
    CHECK(ModuleOf(vtable[kSlotCreateSwapChainForHwnd]) == self);

    acdb_test::GpuTestDevices dev;
    if (!acdb_test::CreateGpuTestDevices(&dev)) {
        std::printf("  no GPU device; swap chain part skipped\n");
        return;
    }
    ComPtr<IDXGIFactory2> factory = FactoryOf(dev.device11.Get());
    REQUIRE(factory);
    CHECK(VtableOf(factory.Get()) == vtable);  // the device's own factory is patched too

    // The saved original creates a real chain on a window of another class.
    {
        TestWindow window(L"acdbFactoryHookPassA");
        REQUIRE(window.Get() != nullptr);
        const DXGI_SWAP_CHAIN_DESC1 desc = SmallDesc(64, 48);
        ComPtr<IDXGISwapChain1> chain;
        const HRESULT hr = CallOriginalCreateSwapChainForHwnd(factory.Get(), dev.device11.Get(), window.Get(), &desc,
                                                              nullptr, nullptr, chain.GetAddressOf());
        if (FAILED(hr)) std::printf("  CallOriginalCreateSwapChainForHwnd: 0x%08lX\n", static_cast<unsigned long>(hr));
        REQUIRE(SUCCEEDED(hr));
        CHECK(IsRealChain(chain.Get()));
        DXGI_SWAP_CHAIN_DESC1 got{};
        CHECK(SUCCEEDED(chain->GetDesc1(&got)));
        CHECK_EQ(got.Width, 64u);
        CHECK_EQ(got.Height, 48u);
        HWND hwnd = nullptr;
        CHECK(SUCCEEDED(chain->GetHwnd(&hwnd)));
        CHECK(hwnd == window.Get());
    }

    // The hooked slot passes a non-main window through as well.
    {
        TestWindow window(L"acdbFactoryHookPassB");
        REQUIRE(window.Get() != nullptr);
        const DXGI_SWAP_CHAIN_DESC1 desc = SmallDesc(64, 48);
        ComPtr<IDXGISwapChain1> chain;
        const HRESULT hr = factory->CreateSwapChainForHwnd(dev.device11.Get(), window.Get(), &desc, nullptr, nullptr,
                                                           chain.GetAddressOf());
        REQUIRE(SUCCEEDED(hr));
        CHECK(IsRealChain(chain.Get()));
    }

    // Legacy CreateSwapChain (slot 10) passes through.
    {
        TestWindow window(L"acdbFactoryHookPassC");
        REQUIRE(window.Get() != nullptr);
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Width = 64;
        desc.BufferDesc.Height = 48;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.OutputWindow = window.Get();
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain> chain;
        CHECK(SUCCEEDED(factory->CreateSwapChain(dev.device11.Get(), &desc, chain.GetAddressOf())));
        CHECK(chain);
    }
    dev.ctx11->ClearState();
    dev.ctx11->Flush();
}

TEST(FactoryHook_MainWindowDecisionIsLoggedAndPassesThroughOnRefusal) {
    REQUIRE(EnsureHookInstalled());
    acdb_test::GpuTestDevices dev;
    if (!acdb_test::CreateGpuTestDevices(&dev)) {
        std::printf("  no GPU device; skipped\n");
        return;
    }
    ComPtr<IDXGIFactory2> factory = FactoryOf(dev.device11.Get());
    REQUIRE(factory);

    // The bootstrap saw no CSP config, so compatibility refuses (rule 4) and the
    // hook must hand back the real chain.
    acdb_test::TempDir dir(L"factory_hook_log");
    const std::wstring logPath = dir.Str() + L"\\hook.log";
    REQUIRE(LogOpen(logPath, LogLevel::Debug));
    {
        TestWindow window(L"acsW", 400, 300);
        REQUIRE(window.Get() != nullptr);
        const DXGI_SWAP_CHAIN_DESC1 desc = SmallDesc(0, 0);  // 0 = client size
        ComPtr<IDXGISwapChain1> chain;
        const HRESULT hr = factory->CreateSwapChainForHwnd(dev.device11.Get(), window.Get(), &desc, nullptr, nullptr,
                                                           chain.GetAddressOf());
        if (FAILED(hr)) std::printf("  CreateSwapChainForHwnd: 0x%08lX\n", static_cast<unsigned long>(hr));
        REQUIRE(SUCCEEDED(hr));
        CHECK(IsRealChain(chain.Get()));
        RECT rc{};
        GetClientRect(window.Get(), &rc);
        DXGI_SWAP_CHAIN_DESC1 got{};
        CHECK(SUCCEEDED(chain->GetDesc1(&got)));
        CHECK_EQ(got.Width, static_cast<UINT>(rc.right - rc.left));
    }
    // Under InternalCallScope the hook neither decides nor logs.
    {
        TestWindow window(L"acsW", 400, 300);
        REQUIRE(window.Get() != nullptr);
        const DXGI_SWAP_CHAIN_DESC1 desc = SmallDesc(32, 32);
        ComPtr<IDXGISwapChain1> chain;
        InternalCallScope internal;
        CHECK(SUCCEEDED(factory->CreateSwapChainForHwnd(dev.device11.Get(), window.Get(), &desc, nullptr, nullptr,
                                                        chain.GetAddressOf())));
        CHECK(IsRealChain(chain.Get()));
    }
    dev.ctx11->ClearState();
    dev.ctx11->Flush();
    LogClose();

    const std::string log = acdb_test::ReadAll(logPath);
    size_t lines = 0;
    for (size_t pos = log.find("CreateSwapChainForHwnd: "); pos != std::string::npos;
         pos = log.find("CreateSwapChainForHwnd: ", pos + 1))
        ++lines;
    CHECK_EQ(lines, static_cast<size_t>(1));  // only the non-internal call
    CHECK(log.find(" INFO CreateSwapChainForHwnd: ") != std::string::npos);
    CHECK(log.find("(main window)") != std::string::npos);
    // Without <exe dir>\ac-dlssg\sl the bootstrap refuses first (M2), with its reason.
    const BootstrapState& bs = EnsureBootstrap();
    const std::string refusal = bs.possible ? std::string("CSP upscaler is not DLSS") : bs.reason;
    CHECK(log.find("pass-through: " + refusal) != std::string::npos);
    CHECK(log.find("proxy swap chain created") == std::string::npos);
    if (lines != 1) std::printf("  log:\n%s\n", log.c_str());
}

// ---------------------------------------------------------------- the panel (spec 6.9)

// The bootstrap creates the panel's two sections and publishes what it
// knows. In this process the section is Local\AcDlssg.Status.v1 itself: a
// game running at the same time owns it, and then this process never
// writes it.
TEST(Bootstrap_CreatesThePanelSectionsAndPublishesItsPart) {
    const BootstrapState& bs = EnsureBootstrap();
    CHECK(bs.config_path == bs.data_dir + L"\\ac-dlssg.ini");
    PanelStatusChannel& panel = PanelStatusChannel::Get();
    StatusLayout s{};
    REQUIRE(panel.Read(&s) == PanelStatusChannel::ReadResult::Ok);
    if (!panel.Owned()) {
        std::printf("  the status section belongs to process %u (a running game); not checked\n", panel.ForeignOwner());
        CHECK(panel.ForeignOwner() != 0);
        CHECK_EQ(s.ownerPid, panel.ForeignOwner());
        return;
    }
    CHECK(PanelControlChannel::Get().Ready());
    CHECK_EQ(s.ownerPid, static_cast<uint32_t>(GetCurrentProcessId()));
    // The test exe is not dxgi.dll: proxy (ReShade) mode.
    CHECK_EQ(s.mode, static_cast<uint32_t>(kPanelModeReShade));
    CHECK(TextOf(s.bridgeVersion) == ACDB_VERSION);
    CHECK(TextOf(s.hotkey) == HotkeyText(bs.config.hotkey));
    CHECK_EQ(s.startWithFg, bs.config.start_with_fg ? 1u : 0u);
    CHECK_EQ(s.spoofLoaded, bs.spoof_loaded ? 1u : 0u);
    CHECK_EQ(s.driverWarning, bs.driver_warnings.empty() ? 0u : 1u);
    // Before any main-window chain: waiting, or passing everything through
    // when the bootstrap already refused (no Streamline next to the test exe).
    // A later test's pass-through may have been published since.
    if (bs.possible) {
        CHECK(s.bridgeState == kPanelNotLoaded || s.bridgeState == kPanelPassThrough);
    } else {
        CHECK_EQ(s.bridgeState, static_cast<uint32_t>(kPanelPassThrough));
        CHECK(TextOf(s.stateReason).find(bs.reason) != std::string::npos);
    }
    const std::string log = acdb_test::ReadAll(bs.data_dir + L"\\logs\\bridge.log");
    CHECK(log.find(" INFO panel: status section Local\\AcDlssg.Status.v1 and control section "
                   "Local\\AcDlssg.Control.v1 ready\n") != std::string::npos);
}

// A main-window chain that passes through is published with its reason.
TEST(FactoryHook_PublishesAPassThroughToThePanel) {
    REQUIRE(EnsureHookInstalled());
    acdb_test::GpuTestDevices dev;
    if (!acdb_test::CreateGpuTestDevices(&dev)) {
        std::printf("  SKIP: no GPU device\n");
        return;
    }
    PanelStatusChannel& panel = PanelStatusChannel::Get();
    if (!panel.Owned()) {
        std::printf("  SKIP: the status section belongs to process %u\n", panel.ForeignOwner());
        return;
    }
    ComPtr<IDXGIFactory2> factory = NewSystemFactory();
    REQUIRE(factory);
    const std::string log = MainWindowChainLog(factory.Get(), dev, L"hook_panel");
    const BootstrapState& bs = EnsureBootstrap();
    const std::string refusal = bs.possible ? std::string("CSP upscaler is not DLSS") : bs.reason;
    CHECK(log.find("pass-through: " + refusal) != std::string::npos);
    StatusLayout s{};
    REQUIRE(panel.Read(&s) == PanelStatusChannel::ReadResult::Ok);
    CHECK_EQ(s.bridgeState, static_cast<uint32_t>(kPanelPassThrough));
    CHECK_EQ(s.fgOn, 0u);
    CHECK(TextOf(s.stateReason).find(refusal) == 0);
    DXGI_ADAPTER_DESC ad{};
    REQUIRE(AdapterDescOf(dev.device11.Get(), &ad));
    char name[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, static_cast<int>(sizeof(name)) - 1, nullptr, nullptr);
    CHECK(TextOf(s.gpuName) == std::string(name).substr(0, sizeof(s.gpuName) - 1));
    CHECK_EQ(s.rtx30, IsAmpereSm86(ad.VendorId, ad.DeviceId) ? 1u : 0u);
}

// ---------------------------------------------------------------- the DLL

TEST(DllExports_LoadExportsAndCreateFactory) {
    std::wstring path = L"" ACDB_DLL_PATH;
    for (auto& c : path) {
        if (c == L'/') c = L'\\';
    }
    // The DLL's own bootstrap must not read the real Documents folder either.
    acdb_test::TempDir docs(L"dll_docs");
    EnvOverride env(L"ACDLSSG_DOCS_DIR", docs.Str());

    // A log of an earlier run, which the DLL's bootstrap must keep.
    const std::wstring logDir = ExeDir() + L"\\ac-dlssg\\logs";
    const std::string marker = "earlier run " + std::to_string(GetTickCount64());
    CreateDirectoryW((ExeDir() + L"\\ac-dlssg").c_str(), nullptr);
    CreateDirectoryW(logDir.c_str(), nullptr);
    {
        const HANDLE f = CreateFileW((logDir + L"\\bridge.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(f != INVALID_HANDLE_VALUE);
        DWORD written = 0;
        WriteFile(f, marker.data(), static_cast<DWORD>(marker.size()), &written, nullptr);
        CloseHandle(f);
    }

    // Never freed: its factory hook stays in the DXGI class vtable for the
    // rest of the process, exactly as in the game.
    const HMODULE dll = LoadLibraryW(path.c_str());
    if (!dll) std::printf("  LoadLibraryW(%ls) failed: %lu\n", path.c_str(), GetLastError());
    REQUIRE(dll != nullptr);

    const char* const names[] = {
        "CreateDXGIFactory",
        "CreateDXGIFactory1",
        "CreateDXGIFactory2",
        "DXGIGetDebugInterface1",
        "DXGIDeclareAdapterRemovalSupport",
        "DXGIDisableVBlankVirtualization",
        "DXGIReportAdapterConfiguration",
        "DXGIDumpJournal",
        "CompatValue",
        "CompatString",
        "DXGID3D10CreateDevice",
        "DXGID3D10CreateLayeredDevice",
        "DXGID3D10GetLayeredDeviceSize",
        "DXGID3D10RegisterLayers",
    };
    for (const char* name : names) {
        if (!GetProcAddress(dll, name)) std::printf("  missing export %s\n", name);
        CHECK(GetProcAddress(dll, name) != nullptr);
    }
    // ReShade refuses to start next to a module exporting its names.
    CHECK(GetProcAddress(dll, "ReShadeVersion") == nullptr);
    CHECK(GetProcAddress(dll, "ReShadeRegisterAddon") == nullptr);
    CHECK(GetProcAddress(dll, "ReShadeUnregisterAddon") == nullptr);

    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    using CreateFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    using GetDebugFn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    const auto create1 = reinterpret_cast<CreateFactoryFn>(GetProcAddress(dll, "CreateDXGIFactory1"));
    REQUIRE(create1 != nullptr);
    ComPtr<IDXGIFactory1> factory;
    REQUIRE(SUCCEEDED(create1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf()))));
    REQUIRE(factory);
    ComPtr<IDXGIAdapter1> adapter;
    CHECK(SUCCEEDED(factory->EnumAdapters1(0, &adapter)));

    // The export hooked the factory class before returning it.
    ComPtr<IDXGIFactory2> factory2;
    REQUIRE(SUCCEEDED(factory.As(&factory2)));
    CHECK(ModuleOf(VtableOf(factory2.Get())[kSlotCreateSwapChainForHwnd]) == dll);

    const auto create0 = reinterpret_cast<CreateFactoryFn>(GetProcAddress(dll, "CreateDXGIFactory"));
    const auto create2 = reinterpret_cast<CreateFactory2Fn>(GetProcAddress(dll, "CreateDXGIFactory2"));
    REQUIRE(create0 != nullptr);
    REQUIRE(create2 != nullptr);
    ComPtr<IDXGIFactory> f0;
    CHECK(SUCCEEDED(create0(__uuidof(IDXGIFactory), reinterpret_cast<void**>(f0.GetAddressOf()))));
    ComPtr<IDXGIFactory4> f4;
    CHECK(SUCCEEDED(create2(0, __uuidof(IDXGIFactory4), reinterpret_cast<void**>(f4.GetAddressOf()))));
    CHECK(f0 && f4);

    // Forwarding: the same answer as System32's export.
    const auto getDebug = reinterpret_cast<GetDebugFn>(GetProcAddress(dll, "DXGIGetDebugInterface1"));
    REQUIRE(getDebug != nullptr);
    REQUIRE(GetSystemDxgi().DXGIGetDebugInterface1 != nullptr);
    ComPtr<IUnknown> ours;
    ComPtr<IUnknown> theirs;
    const HRESULT hrOurs = getDebug(0, __uuidof(IDXGIInfoQueue), reinterpret_cast<void**>(ours.GetAddressOf()));
    const HRESULT hrTheirs =
        GetSystemDxgi().DXGIGetDebugInterface1(0, __uuidof(IDXGIInfoQueue), reinterpret_cast<void**>(theirs.GetAddressOf()));
    CHECK_EQ(hrOurs, hrTheirs);

    // The DLL's bootstrap wrote its banner next to the host exe, after moving
    // the earlier log aside.
    const std::string log = acdb_test::ReadAll(logDir + L"\\bridge.log");
    CHECK(log.find(BannerLine() + "\n") != std::string::npos);
    CHECK(log.find("factory hook installed") != std::string::npos);
    CHECK(log.find(marker) == std::string::npos);
    CHECK(log.find("the previous log was kept as ") != std::string::npos);
    CHECK(acdb_test::ReadAll(logDir + L"\\bridge.prev.log") == marker);
    // Versions (spec 6.10): the test exe has no ReShade or CSP next to it.
    CHECK(log.find("ReShade: dxgi.dll is not loaded from the game folder") != std::string::npos);
    CHECK(log.find("CSP: dwrite.dll is not loaded from the game folder") != std::string::npos);
    const bool driver = std::regex_search(log, std::regex("adapter 0: [^\n]*, driver [0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+"));
    CHECK(driver);
    if (!driver) std::printf("  log:\n%s\n", log.c_str());
}
