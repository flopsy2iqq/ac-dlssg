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

#include "bootstrap.h"
#include "factory_hook.h"
#include "gpu_test_devices.h"
#include "internal_call.h"
#include "log.h"
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

}  // namespace

// ---------------------------------------------------------------- decision

TEST(FactoryHook_ShouldProxyTruthTable) {
    int trueCount = 0;
    for (unsigned bits = 0; bits < 64; ++bits) {
        ProxyDecisionInputs in;
        in.internal_call = (bits & 1) != 0;
        in.is_d3d11_device = (bits & 2) != 0;
        in.is_main_window = (bits & 4) != 0;
        in.bootstrap_possible = (bits & 8) != 0;
        in.compat_ok = (bits & 16) != 0;
        in.streamline_shut_down = (bits & 32) != 0;
        const bool expected = !in.internal_call && in.is_d3d11_device && in.is_main_window &&
                              in.bootstrap_possible && in.compat_ok && !in.streamline_shut_down;
        if (ShouldProxy(in) != expected) std::printf("  mismatch for bits 0x%02X\n", bits);
        CHECK_EQ(ShouldProxy(in), expected);
        if (ShouldProxy(in)) ++trueCount;
    }
    CHECK_EQ(trueCount, 1);
    CHECK(!ShouldProxy(ProxyDecisionInputs()));  // defaults never proxy
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
        CHECK(s.possible);
        CHECK(s.reason.empty());
    } else {
        CHECK(!s.possible);
        CHECK(s.reason == "disabled in ac-dlssg.ini");
    }
    // The redirected Documents folder is empty and the test dir has no CSP config.
    CHECK(!s.compat.fsr_active.has_value());
    CHECK(!s.compat.video_width.has_value());

    const std::string log = acdb_test::ReadAll(s.data_dir + L"\\logs\\bridge.log");
    CHECK(log.find("ac-dlssg " ACDB_VERSION) != std::string::npos);
    CHECK(log.find("host: ") != std::string::npos);
    CHECK(log.find("Windows ") != std::string::npos);
    CHECK(log.find("adapter 0: ") != std::string::npos);
    CHECK(log.find("LUID ") != std::string::npos);
    CHECK(log.find("compat: graphics_adjustments.ini [FSR] ACTIVE=unset") != std::string::npos);
    CHECK(log.find("compat: HAGS ") != std::string::npos);
    CHECK(log.find("bridge: ") != std::string::npos);
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
    CHECK(log.find("pass-through: CSP upscaler is not DLSS") != std::string::npos);
    CHECK(log.find("proxy swap chain created") == std::string::npos);
    if (lines != 1) std::printf("  log:\n%s\n", log.c_str());
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
    CHECK(log.find("ac-dlssg " ACDB_VERSION) != std::string::npos);
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
