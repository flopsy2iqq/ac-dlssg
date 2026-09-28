#include "bootstrap.h"

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>

#include "gpu_info.h"
#include "internal_call.h"
#include "log.h"
#include "module_version.h"
#include "streamline_runtime.h"
#include "system_dxgi.h"

using Microsoft::WRL::ComPtr;

namespace acdb {
namespace {

std::atomic<HMODULE> g_module{nullptr};
std::once_flag g_once;
BootstrapState g_state;  // written once inside g_once, read-only afterwards

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring ModuleFileName(HMODULE module) {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        if (buf.size() >= 32768) return {};
        buf.resize(buf.size() * 2);  // truncated: GetModuleFileNameW returns the buffer size
    }
}

std::wstring ParentDir(const std::wstring& path) {
    const size_t sep = path.find_last_of(L"\\/");
    return sep == std::wstring::npos ? std::wstring() : path.substr(0, sep);
}

bool FileExists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// GetVersionEx reports what the exe manifest allows; RtlGetVersion does not.
std::string WindowsBuild() {
    using RtlGetVersionFn = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto rtlGetVersion =
        ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    if (!rtlGetVersion || rtlGetVersion(&vi) != 0) return "unknown";

    DWORD ubr = 0;
    DWORD size = sizeof(ubr);
    const bool haveUbr = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR",
                                      RRF_RT_REG_DWORD, nullptr, &ubr, &size) == ERROR_SUCCESS;
    char buf[64];
    if (haveUbr) {
        std::snprintf(buf, sizeof(buf), "%lu.%lu.%lu.%lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber, ubr);
    } else {
        std::snprintf(buf, sizeof(buf), "%lu.%lu.%lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
    }
    return buf;
}

const char* LevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Error: return "error";
        case LogLevel::Warn: return "warn";
        case LogLevel::Info: return "info";
        case LogLevel::Debug: return "debug";
    }
    return "?";
}

std::string Opt(const std::optional<long long>& v) { return v ? std::to_string(*v) : std::string("unset"); }

bool SamePathNoCase(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                                              static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::string Drs(const DrsValue& v) {
    if (!v.found) return "unset";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(v.value));
    return buf;
}

void LogDriverProfile(const DriverProfileReport& r, const std::vector<std::string>& warnings) {
    if (!r.nvapi_ok) {
        LOGI("driver profile: not read: %s", r.error.c_str());
    } else {
        LOGI("driver profile: app profile %s; 0x%08X (DLSS-G override mode) app=%s global=%s; 0x%08X (Streamline "
             "override) app=%s global=%s",
             r.app_profile.empty() ? "(none)" : ("\"" + ToUtf8(r.app_profile) + "\"").c_str(), kDrsNgxDlssgMode,
             Drs(r.dlssg_mode_app).c_str(), Drs(r.dlssg_mode_base).c_str(), kDrsSlDlssOverride,
             Drs(r.sl_override_app).c_str(), Drs(r.sl_override_base).c_str());
    }
    for (const auto& w : warnings) LOGW("driver profile: %s", w.c_str());
}

void LogAdapters() {
    const SystemDxgi& sys = GetSystemDxgi();
    if (!sys.CreateDXGIFactory1) {
        LOGW("adapters: System32 dxgi.dll or its CreateDXGIFactory1 is not available");
        return;
    }
    InternalCallScope internal;
    ComPtr<IDXGIFactory1> factory;
    const HRESULT hr =
        sys.CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf()));
    if (FAILED(hr) || !factory) {
        LOGW("adapters: CreateDXGIFactory1 failed (0x%08lX)", static_cast<unsigned long>(hr));
        return;
    }
    UINT i = 0;
    for (; i < 64; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT ehr = factory->EnumAdapters1(i, &adapter);
        if (ehr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(ehr) || !adapter) {
            LOGW("adapter %u: EnumAdapters1 failed (0x%08lX)", i, static_cast<unsigned long>(ehr));
            break;
        }
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(adapter->GetDesc1(&d))) {
            LOGW("adapter %u: GetDesc1 failed", i);
            continue;
        }
        LARGE_INTEGER umd{};
        const std::string driver = SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))
                                       ? DriverVersionText(d.VendorId, umd.QuadPart)
                                       : std::string("unknown");
        LOGI("adapter %u: %s, vendor 0x%04X device 0x%04X subsys 0x%08X rev %u, LUID %08lX:%08lX, %s%s%s, "
             "%llu MB dedicated, driver %s",
             i, ToUtf8(d.Description).c_str(), d.VendorId, d.DeviceId, d.SubSysId, d.Revision,
             static_cast<unsigned long>(d.AdapterLuid.HighPart), static_cast<unsigned long>(d.AdapterLuid.LowPart),
             ArchName(ArchFromIds(d.VendorId, d.DeviceId)),
             IsRtx30From3070(d.VendorId, d.DeviceId) ? " (RTX 3070 or higher)" : "",
             (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? ", software" : "",
             static_cast<unsigned long long>(d.DedicatedVideoMemory / (1024 * 1024)), driver.c_str());
    }
    if (i == 0) LOGW("adapters: none found");
}

void LogCompatInputs(const CompatInputs& c) {
    LOGI("compat: dxgi_tweaks.ini OLD_SWAPCHAIN=%s EXCLUSIVE_FULLSCREEN=%s ALLOW_STRETCHING=%s [HDR] ENABLED=%s",
         Opt(c.old_swapchain).c_str(), Opt(c.exclusive_fullscreen).c_str(), Opt(c.allow_stretching).c_str(),
         Opt(c.hdr_enabled).c_str());
    LOGI("compat: graphics_adjustments.ini [FSR] ACTIVE=%s OLD_IMPLEMENTATION=%s", Opt(c.fsr_active).c_str(),
         Opt(c.fsr_old_implementation).c_str());
    LOGI("compat: video.ini AASAMPLES=%s WIDTH=%s HEIGHT=%s [CAMERA] MODE=%s", Opt(c.aasamples).c_str(),
         Opt(c.video_width).c_str(), Opt(c.video_height).c_str(),
         c.camera_mode ? c.camera_mode->c_str() : "unset");
    LOGI("compat: HAGS %s, dlss5-bridge.addon64 %s, renodx-dlss5.addon64 %s", c.hags_on ? "on" : "off",
         c.dlss5_bridge_loaded ? "loaded" : "not loaded", c.renodx_dlss5_loaded ? "loaded" : "not loaded");
}

void Run(BootstrapState* s) {
    const std::wstring exe = ModuleFileName(nullptr);
    s->game_dir = ParentDir(exe);
    s->data_dir = s->game_dir.empty() ? std::wstring(L"ac-dlssg") : s->game_dir + L"\\ac-dlssg";

    const std::wstring iniPath = s->data_dir + L"\\ac-dlssg.ini";
    const bool iniFound = FileExists(iniPath);
    s->config = LoadConfig(iniPath);

    // The previous run's log survives one relaunch, so a crash or freeze can
    // still be analysed after the game was started again.
    const std::wstring logPath = s->data_dir + L"\\logs\\bridge.log";
    const std::wstring prevLogPath = s->data_dir + L"\\logs\\bridge.prev.log";
    const bool keptPrevious = MoveFileExW(logPath.c_str(), prevLogPath.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
    LogOpen(logPath, s->config.log_level);

    LOGI("ac-dlssg %s (M2: Streamline proxy chain, Reflex and PCL markers; DLSS-G off)", ACDB_VERSION);
    LOGI("host: %s (pid %lu)", ToUtf8(exe).c_str(), GetCurrentProcessId());
    const HMODULE bridge = g_module.load();
    const std::wstring bridgePath = bridge ? ModuleFileName(bridge) : std::wstring();
    s->mode = BridgeModeFromPath(bridgePath);
    LOGI("bridge module: %s", bridge ? ToUtf8(bridgePath).c_str() : "(not set)");
    if (s->mode == BridgeMode::Standalone) {
        LOGI("mode: standalone: the bridge is the process's dxgi.dll and forwards to System32\\dxgi.dll");
    } else {
        const size_t sep = bridgePath.find_last_of(L"\\/");
        LOGI("mode: proxy: the bridge is loaded as %s, not as dxgi.dll (ReShade's [PROXY] ProxyLibrary)",
             ToUtf8(sep == std::wstring::npos ? bridgePath : bridgePath.substr(sep + 1)).c_str());
    }
    LOGI("Windows %s", WindowsBuild().c_str());
    if (const HMODULE version = GetModuleHandleW(L"version.dll"))
        LOGI("version.dll loaded from %s, version %s", ToUtf8(ModuleFileName(version)).c_str(),
             ModuleVersionText(version).c_str());
    // By full path: System32's dxgi.dll is loaded as well. In standalone mode
    // the game folder's dxgi.dll is this bridge. CSP is dwrite.dll.
    const HMODULE gameDxgi = s->game_dir.empty() ? nullptr : GetModuleHandleW((s->game_dir + L"\\dxgi.dll").c_str());
    LOGI("%s", GameFolderDxgiLine(gameDxgi != nullptr, gameDxgi != nullptr && gameDxgi == bridge,
                                  gameDxgi ? ToUtf8(ModuleFileName(gameDxgi)) : std::string(),
                                  gameDxgi ? ModuleVersionText(gameDxgi) : std::string())
                   .c_str());
    const HMODULE csp = s->game_dir.empty() ? nullptr : GetModuleHandleW((s->game_dir + L"\\dwrite.dll").c_str());
    if (csp) {
        LOGI("CSP: %s, version %s", ToUtf8(ModuleFileName(csp)).c_str(), ModuleVersionText(csp).c_str());
    } else {
        LOGI("CSP: dwrite.dll is not loaded from the game folder");
    }
    LOGI("data dir: %s", ToUtf8(s->data_dir).c_str());
    if (keptPrevious) LOGI("the previous log was kept as %s", ToUtf8(prevLogPath).c_str());

    const Config& c = s->config;
    LOGI("config: %s%s", ToUtf8(iniPath).c_str(), iniFound ? "" : " (not found, using defaults)");
    LOGI("config: enabled=%d start_with_fg=%d hotkey=%s%s%svk 0x%02X max_frame_latency=%u log_level=%s",
         c.enabled ? 1 : 0, c.start_with_fg ? 1 : 0, c.hotkey.ctrl ? "ctrl+" : "", c.hotkey.shift ? "shift+" : "",
         c.hotkey.alt ? "alt+" : "", c.hotkey.vk, c.max_frame_latency, LevelName(c.log_level));
    for (const auto& w : c.warnings) LOGW("config: %s", w.c_str());

    LogAdapters();

    s->docs_ac_dir = DocumentsAcDir();
    LOGI("game dir: %s", ToUtf8(s->game_dir).c_str());
    LOGI("documents dir: %s", s->docs_ac_dir.empty() ? "(not found)" : ToUtf8(s->docs_ac_dir).c_str());
    s->compat = ReadCompatInputs(s->game_dir, s->docs_ac_dir);
    LogCompatInputs(s->compat);

    // Spoof: dlssg_for_sm86's version.dll next to acs.exe (spec 10).
    if (const HMODULE version = GetModuleHandleW(L"version.dll"))
        s->spoof_loaded = SamePathNoCase(ParentDir(ModuleFileName(version)), s->game_dir);
    LOGI("spoof: %s", s->spoof_loaded ? "version.dll is loaded from the game folder (dlssg_for_sm86)"
                                      : "no version.dll from the game folder");

    // Driver profile before any D3D device exists (spec 6.8).
    s->driver_profile = ReadDriverProfile(exe);
    s->driver_warnings = DriverProfileWarnings(s->driver_profile);
    LogDriverProfile(s->driver_profile, s->driver_warnings);

    // Streamline is loaded only for an enabled bridge: disabled means the
    // game runs as without the mod.
    if (c.enabled) {
        const std::wstring slDir = s->data_dir + L"\\sl";
        s->streamline_ok = StreamlineRuntime::Get().Init(slDir, s->data_dir + L"\\logs", &s->streamline_error);
        if (s->streamline_ok) {
            LOGI("Streamline: initialised from %s", ToUtf8(slDir).c_str());
        } else {
            LOGW("Streamline: not available: %s", s->streamline_error.c_str());
        }
    } else {
        s->streamline_error = "not loaded: the bridge is disabled";
        LOGI("Streamline: %s", s->streamline_error.c_str());
    }

    s->possible = c.enabled && s->streamline_ok;
    if (!c.enabled) {
        s->reason = "disabled in ac-dlssg.ini";
    } else if (!s->streamline_ok) {
        s->reason = "Streamline: " + s->streamline_error;
    }
    if (s->possible) {
        LOGI("bridge: possible; compatibility is checked per swap chain");
    } else {
        LOGI("bridge: not possible: %s", s->reason.c_str());
    }
}

}  // namespace

BridgeMode BridgeModeFromPath(const std::wstring& modulePath) {
    const size_t sep = modulePath.find_last_of(L"\\/");
    const std::wstring file = sep == std::wstring::npos ? modulePath : modulePath.substr(sep + 1);
    constexpr wchar_t kDxgi[] = L"dxgi.dll";
    return CompareStringOrdinal(file.c_str(), static_cast<int>(file.size()), kDxgi, -1, TRUE) == CSTR_EQUAL
               ? BridgeMode::Standalone
               : BridgeMode::Proxy;
}

const char* BridgeModeName(BridgeMode mode) { return mode == BridgeMode::Standalone ? "standalone" : "proxy"; }

std::string GameFolderDxgiLine(bool loaded, bool isThisBridge, const std::string& path, const std::string& version) {
    if (isThisBridge) return "dxgi.dll: " + path + " is this bridge (standalone), not ReShade";
    if (loaded) return "ReShade: " + path + ", version " + version;
    return "ReShade: dxgi.dll is not loaded from the game folder";
}

void SetBridgeModule(HMODULE module) { g_module.store(module); }

HMODULE BridgeModule() { return g_module.load(); }

const BootstrapState& BootstrapRunOnce() {
    try {
        std::call_once(g_once, [] {
            try {
                Run(&g_state);
            } catch (...) {
                g_state.possible = false;
                g_state.reason = "bootstrap failed with an exception";
                LOGE("bootstrap failed with an exception; the bridge passes through");
            }
        });
    } catch (...) {
        // call_once itself failed; g_state keeps possible == false.
    }
    return g_state;
}

}  // namespace acdb
