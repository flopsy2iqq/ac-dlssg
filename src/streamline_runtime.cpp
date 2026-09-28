#include "streamline_runtime.h"

// sl_security.h defines its functions and globals in the header, so this must
// stay the only file that includes it. It loads wintrust.dll and crypt32.dll
// from System32 at runtime; the bridge imports neither.
#include <sl_security.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <new>
#include <string_view>

#include "fg_policy.h"
#include "internal_call.h"
#include "log.h"
#include "module_version.h"

namespace acdb {

struct StreamlineRuntime::Api {
    PFun_slInit* slInit = nullptr;
    PFun_slShutdown* slShutdown = nullptr;
    PFun_slIsFeatureSupported* slIsFeatureSupported = nullptr;
    PFun_slGetFeatureRequirements* slGetFeatureRequirements = nullptr;
    PFun_slSetTagForFrame* slSetTagForFrame = nullptr;
    PFun_slSetConstants* slSetConstants = nullptr;
    PFun_slUpgradeInterface* slUpgradeInterface = nullptr;
    PFun_slGetNativeInterface* slGetNativeInterface = nullptr;
    PFun_slGetFeatureFunction* slGetFeatureFunction = nullptr;
    PFun_slGetNewFrameToken* slGetNewFrameToken = nullptr;
    PFun_slSetD3DDevice* slSetD3DDevice = nullptr;
    // Feature functions, resolved by SetDevice.
    PFun_slReflexSetOptions* slReflexSetOptions = nullptr;
    PFun_slReflexGetState* slReflexGetState = nullptr;
    PFun_slReflexSleep* slReflexSleep = nullptr;
    PFun_slPCLSetMarker* slPCLSetMarker = nullptr;
    PFun_slDLSSGSetOptions* slDLSSGSetOptions = nullptr;
    PFun_slDLSSGGetState* slDLSSGGetState = nullptr;
};

namespace {

constexpr wchar_t kInterposerName[] = L"sl.interposer.dll";

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

const char* ResultName(sl::Result r) {
    switch (r) {
        case sl::Result::eOk: return "eOk";
        case sl::Result::eErrorIO: return "eErrorIO";
        case sl::Result::eErrorDriverOutOfDate: return "eErrorDriverOutOfDate";
        case sl::Result::eErrorOSOutOfDate: return "eErrorOSOutOfDate";
        case sl::Result::eErrorOSDisabledHWS: return "eErrorOSDisabledHWS";
        case sl::Result::eErrorDeviceNotCreated: return "eErrorDeviceNotCreated";
        case sl::Result::eErrorNoSupportedAdapterFound: return "eErrorNoSupportedAdapterFound";
        case sl::Result::eErrorAdapterNotSupported: return "eErrorAdapterNotSupported";
        case sl::Result::eErrorNoPlugins: return "eErrorNoPlugins";
        case sl::Result::eErrorVulkanAPI: return "eErrorVulkanAPI";
        case sl::Result::eErrorDXGIAPI: return "eErrorDXGIAPI";
        case sl::Result::eErrorD3DAPI: return "eErrorD3DAPI";
        case sl::Result::eErrorNRDAPI: return "eErrorNRDAPI";
        case sl::Result::eErrorNVAPI: return "eErrorNVAPI";
        case sl::Result::eErrorReflexAPI: return "eErrorReflexAPI";
        case sl::Result::eErrorNGXFailed: return "eErrorNGXFailed";
        case sl::Result::eErrorJSONParsing: return "eErrorJSONParsing";
        case sl::Result::eErrorMissingProxy: return "eErrorMissingProxy";
        case sl::Result::eErrorMissingResourceState: return "eErrorMissingResourceState";
        case sl::Result::eErrorInvalidIntegration: return "eErrorInvalidIntegration";
        case sl::Result::eErrorMissingInputParameter: return "eErrorMissingInputParameter";
        case sl::Result::eErrorNotInitialized: return "eErrorNotInitialized";
        case sl::Result::eErrorComputeFailed: return "eErrorComputeFailed";
        case sl::Result::eErrorInitNotCalled: return "eErrorInitNotCalled";
        case sl::Result::eErrorExceptionHandler: return "eErrorExceptionHandler";
        case sl::Result::eErrorInvalidParameter: return "eErrorInvalidParameter";
        case sl::Result::eErrorMissingConstants: return "eErrorMissingConstants";
        case sl::Result::eErrorDuplicatedConstants: return "eErrorDuplicatedConstants";
        case sl::Result::eErrorMissingOrInvalidAPI: return "eErrorMissingOrInvalidAPI";
        case sl::Result::eErrorCommonConstantsMissing: return "eErrorCommonConstantsMissing";
        case sl::Result::eErrorUnsupportedInterface: return "eErrorUnsupportedInterface";
        case sl::Result::eErrorFeatureMissing: return "eErrorFeatureMissing";
        case sl::Result::eErrorFeatureNotSupported: return "eErrorFeatureNotSupported";
        case sl::Result::eErrorFeatureMissingHooks: return "eErrorFeatureMissingHooks";
        case sl::Result::eErrorFeatureFailedToLoad: return "eErrorFeatureFailedToLoad";
        case sl::Result::eErrorFeatureWrongPriority: return "eErrorFeatureWrongPriority";
        case sl::Result::eErrorFeatureMissingDependency: return "eErrorFeatureMissingDependency";
        case sl::Result::eErrorFeatureManagerInvalidState: return "eErrorFeatureManagerInvalidState";
        case sl::Result::eErrorInvalidState: return "eErrorInvalidState";
        case sl::Result::eWarnOutOfVRAM: return "eWarnOutOfVRAM";
    }
    return "unknown sl::Result";
}

// Why slIsFeatureSupported refused, in words for the log and the panel.
const char* UnsupportedReason(sl::Result r) {
    switch (r) {
        case sl::Result::eErrorOSOutOfDate: return "Windows is too old";
        case sl::Result::eErrorDriverOutOfDate: return "the NVIDIA driver is too old";
        case sl::Result::eErrorOSDisabledHWS: return "hardware-accelerated GPU scheduling is off";
        case sl::Result::eErrorNoSupportedAdapterFound:
        case sl::Result::eErrorAdapterNotSupported: return "the GPU does not support DLSS Frame Generation";
        case sl::Result::eErrorFeatureMissing:
        case sl::Result::eErrorFeatureFailedToLoad: return "the DLSS-G plugin (sl.dlss_g.dll) did not load";
        case sl::Result::eErrorFeatureNotSupported: return "DLSS Frame Generation is not supported on this system";
        case sl::Result::eErrorNGXFailed: return "NGX failed to initialise";
        default: return "";
    }
}

std::wstring NormalizeDir(const std::wstring& dir) {
    std::wstring out(dir);
    for (auto& c : out) {
        if (c == L'/') c = L'\\';
    }
    const DWORD n = GetFullPathNameW(out.c_str(), 0, nullptr, nullptr);
    if (n > 0) {
        std::wstring full(n, L'\0');
        const DWORD m = GetFullPathNameW(out.c_str(), n, full.data(), nullptr);
        if (m > 0 && m < n) {
            full.resize(m);
            out.swap(full);
        }
    }
    while (out.size() > 3 && out.back() == L'\\') out.pop_back();
    return out;
}

// "C:\..." or "\\server\share\..."; relative paths would depend on the
// current directory, which the game does not control for us.
bool IsAbsolutePath(const std::wstring& p) {
    if (p.size() >= 3 && iswalpha(p[0]) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) return true;
    return p.size() >= 3 && (p[0] == L'\\' || p[0] == L'/') && (p[1] == L'\\' || p[1] == L'/');
}

bool SamePathNoCase(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
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
        buf.resize(buf.size() * 2);
    }
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

bool StartsWith(const std::wstring& s, const wchar_t* prefix) { return s.rfind(prefix, 0) == 0; }

bool EndsWith(const std::wstring& s, const wchar_t* suffix) {
    const size_t n = wcslen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

template <typename Fn>
bool ResolveExport(HMODULE module, const char* name, Fn** out) {
    *out = reinterpret_cast<Fn*>(GetProcAddress(module, name));
    return *out != nullptr;
}

template <typename Fn>
sl::Result ResolveFeatureFunction(PFun_slGetFeatureFunction* get, sl::Feature feature, const char* name, Fn** out) {
    void* fn = nullptr;
    const sl::Result r = get(feature, name, fn);
    *out = r == sl::Result::eOk ? reinterpret_cast<Fn*>(fn) : nullptr;
    return r == sl::Result::eOk && !*out ? sl::Result::eErrorMissingOrInvalidAPI : r;
}

ULONG RefCount(IUnknown* obj) {
    obj->AddRef();
    return obj->Release();
}

// Logs the first failure of each per-frame call; later ones would flood the log.
void LogFirstFailure(std::atomic<bool>& logged, const char* what, sl::Result r) {
    if (!logged.exchange(true)) LOGW("%s failed: %s (further failures are not logged)", what, ResultName(r));
}

std::atomic<bool> g_token_failed{false};
std::atomic<bool> g_sleep_failed{false};
std::atomic<bool> g_marker_failed{false};

// The M3 calls run every frame; each distinct failure is logged at most once
// per 10 s.
void LogThrottledFailure(const char* what, sl::Result r) {
    static std::mutex mu;
    static ReasonThrottle throttle(10000);
    const std::string key = std::string(what) + ": " + ResultName(r);
    bool log = false;
    {
        std::lock_guard<std::mutex> lock(mu);
        log = throttle.ShouldLog(key, GetTickCount64());
    }
    if (log) LOGW("%s failed: %s (logged at most every 10 s)", what, ResultName(r));
}

}  // namespace

const char* SlResultName(sl::Result r) { return ResultName(r); }

sl::DLSSGOptions BuildDlssgOptions(bool on, const DlssgSizeHints& hints, bool requestVramEstimate) {
    sl::DLSSGOptions o;
    o.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    o.numFramesToGenerate = 1;
    o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    if (requestVramEstimate) o.flags = o.flags | sl::DLSSGFlags::eRequestVRAMEstimate;
    o.numBackBuffers = hints.numBackBuffers;
    o.mvecDepthWidth = hints.mvecDepthWidth;
    o.mvecDepthHeight = hints.mvecDepthHeight;
    o.colorWidth = hints.colorWidth;
    o.colorHeight = hints.colorHeight;
    o.colorBufferFormat = hints.colorBufferFormat;
    o.mvecBufferFormat = hints.mvecBufferFormat;
    o.depthBufferFormat = hints.depthBufferFormat;
    return o;
}

sl::Preferences BuildPreferences(const std::wstring& pluginDir, const std::wstring& logDir,
                                 const std::string& engineVersion, sl::PFun_LogMessageCallback* callback,
                                 SlPreferencesStorage* storage) {
    sl::Preferences p;
    p.showConsole = false;
    p.logLevel = sl::LogLevel::eDefault;
    p.logMessageCallback = callback;
    // OTA updates and downloaded plugins stay off: only the pinned files load.
    p.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging |
              sl::PreferenceFlags::eDisableCLStateTracking;
    p.applicationId = 0;
    p.engine = sl::EngineType::eCustom;
    p.projectId = kSlProjectId;
    p.renderAPI = sl::RenderAPI::eD3D12;
    if (!storage) return p;

    storage->plugin_dir = pluginDir;
    storage->log_dir = logDir;
    storage->engine_version = engineVersion;
    storage->plugin_paths[0] = storage->plugin_dir.c_str();
    storage->features[0] = sl::kFeatureDLSS_G;
    storage->features[1] = sl::kFeatureReflex;
    storage->features[2] = sl::kFeaturePCL;
    p.pathsToPlugins = storage->plugin_paths;
    p.numPathsToPlugins = 1;
    p.pathToLogsAndData = storage->log_dir.c_str();
    p.featuresToLoad = storage->features;
    p.numFeaturesToLoad = 3;
    p.engineVersion = storage->engine_version.c_str();
    return p;
}

StreamlineRuntime& StreamlineRuntime::Get() {
    static StreamlineRuntime instance;
    return instance;
}

bool StreamlineRuntime::Init(const std::wstring& pluginDir, const std::wstring& logDir, std::string* error) {
    std::lock_guard<std::mutex> lock(mu_);
    // Streamline allows one lifetime per process.
    if (shut_down_) {
        if (error) *error = "Streamline was already shut down in this process";
        return false;
    }
    if (!init_done_) {
        init_done_ = true;
        std::string err;
        try {
            do {
                if (shut_down_) {
                    err = "already shut down in this process";
                    break;
                }
                if (!IsAbsolutePath(pluginDir)) {
                    err = "plugin directory is not an absolute path: " + ToUtf8(pluginDir);
                    break;
                }
                plugin_dir_ = NormalizeDir(pluginDir);
                const std::wstring path = plugin_dir_ + L"\\" + kInterposerName;
                const std::string path8 = ToUtf8(path);

                // Opened without write or delete sharing, so the file cannot be
                // swapped between the signature check and the load.
                const HANDLE pin = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                               FILE_ATTRIBUTE_NORMAL, nullptr);
                if (pin == INVALID_HANDLE_VALUE) {
                    const DWORD e = GetLastError();
                    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
                        err = "missing " + path8;
                    } else {
                        err = "cannot open " + path8 + " (error " + std::to_string(e) + ")";
                    }
                    break;
                }
                const bool verified = sl::security::verifyEmbeddedSignature(path.c_str());
                DWORD loadError = 0;
                if (verified) {
                    InternalCallScope internal;
                    interposer_ = LoadLibraryW(path.c_str());
                    loadError = interposer_ ? 0 : GetLastError();
                }
                CloseHandle(pin);
                if (!verified) {
                    err = path8 + " failed the Streamline signature check (it must carry NVIDIA's signature)";
                    break;
                }
                if (!interposer_) {
                    err = "LoadLibrary failed for " + path8 + " (error " + std::to_string(loadError) + ")";
                    break;
                }
                LOGI("Streamline: %s verified and loaded (%s)", path8.c_str(), ModuleVersionText(interposer_).c_str());

                auto* api = new (std::nothrow) Api();
                if (!api) {
                    err = "out of memory";
                    break;
                }
                const char* missing = nullptr;
                auto need = [&](const char* name, auto** out) {
                    if (!missing && !ResolveExport(interposer_, name, out)) missing = name;
                };
                need("slInit", &api->slInit);
                need("slShutdown", &api->slShutdown);
                need("slIsFeatureSupported", &api->slIsFeatureSupported);
                need("slGetFeatureRequirements", &api->slGetFeatureRequirements);
                need("slSetTagForFrame", &api->slSetTagForFrame);
                need("slSetConstants", &api->slSetConstants);
                need("slUpgradeInterface", &api->slUpgradeInterface);
                need("slGetNativeInterface", &api->slGetNativeInterface);
                need("slGetFeatureFunction", &api->slGetFeatureFunction);
                need("slGetNewFrameToken", &api->slGetNewFrameToken);
                need("slSetD3DDevice", &api->slSetD3DDevice);
                if (missing) {
                    delete api;
                    FreeLibrary(interposer_);
                    interposer_ = nullptr;
                    err = path8 + " has no export " + missing;
                    break;
                }
                api_ = api;

                CreateDirectoryW(logDir.c_str(), nullptr);  // Streamline writes its own log there
                const sl::Preferences prefs =
                    BuildPreferences(plugin_dir_, logDir, ACDB_VERSION, &StreamlineRuntime::OnLogMessage, &storage_);
                sl::Result r;
                {
                    InternalCallScope internal;
                    r = api_->slInit(prefs, sl::kSDKVersion);
                }
                if (r != sl::Result::eOk) {
                    // The interposer stays loaded: Streamline may have started
                    // threads or hooks before it failed.
                    err = std::string("slInit failed: ") + ResultName(r);
                    break;
                }
                initialized_ = true;
                LOGI("Streamline: slInit ok (SDK %d.%d.%d, plugins from %s)", SL_VERSION_MAJOR, SL_VERSION_MINOR,
                     SL_VERSION_PATCH, ToUtf8(plugin_dir_).c_str());
            } while (false);
        } catch (...) {
            err = "unexpected exception during Streamline initialisation";
            initialized_ = false;
        }
        init_error_ = initialized_ ? std::string() : err;
    }
    if (error) *error = init_error_;
    return initialized_;
}

bool StreamlineRuntime::SetDevice(ID3D12Device* nativeDevice, std::string* error) {
    std::lock_guard<std::mutex> lock(mu_);
    std::string err;
    do {
        if (!initialized_ || !api_) {
            err = "Streamline is not initialised";
            break;
        }
        if (shut_down_) {
            err = "Streamline was shut down";
            break;
        }
        if (!nativeDevice) {
            err = "no D3D12 device";
            break;
        }
        InternalCallScope internal;
        sl::Result r = api_->slSetD3DDevice(nativeDevice);
        if (r != sl::Result::eOk) {
            err = std::string("slSetD3DDevice failed: ") + ResultName(r);
            break;
        }
        struct Required {
            sl::Feature feature;
            const char* name;
            sl::Result result;
        } required[] = {
            {sl::kFeatureReflex, "slReflexSetOptions",
             ResolveFeatureFunction(api_->slGetFeatureFunction, sl::kFeatureReflex, "slReflexSetOptions",
                                    &api_->slReflexSetOptions)},
            {sl::kFeatureReflex, "slReflexGetState",
             ResolveFeatureFunction(api_->slGetFeatureFunction, sl::kFeatureReflex, "slReflexGetState",
                                    &api_->slReflexGetState)},
            {sl::kFeatureReflex, "slReflexSleep",
             ResolveFeatureFunction(api_->slGetFeatureFunction, sl::kFeatureReflex, "slReflexSleep",
                                    &api_->slReflexSleep)},
            {sl::kFeaturePCL, "slPCLSetMarker",
             ResolveFeatureFunction(api_->slGetFeatureFunction, sl::kFeaturePCL, "slPCLSetMarker",
                                    &api_->slPCLSetMarker)},
        };
        for (const auto& fn : required) {
            if (fn.result != sl::Result::eOk && err.empty()) {
                err = std::string("slGetFeatureFunction(") + fn.name + ") failed: " + ResultName(fn.result);
            }
        }
        if (!err.empty()) break;
        // The DLSS-G functions are optional here: the presenter decides what
        // an adapter without them means (proxy_without_fg). Requesting them while the plugin is not loaded (unsupported GPU)
        // makes Streamline log an error, and so does slIsFeatureLoaded;
        // slIsFeatureSupported on the device's adapter does not.
        const LUID luid = nativeDevice->GetAdapterLuid();
        LUID luidCopy = luid;
        sl::AdapterInfo adapter;
        adapter.deviceLUID = reinterpret_cast<uint8_t*>(&luidCopy);
        adapter.deviceLUIDSizeInBytes = sizeof(luidCopy);
        const bool dlssgLoaded = api_->slIsFeatureSupported(sl::kFeatureDLSS_G, adapter) == sl::Result::eOk;
        if (!dlssgLoaded) {
            api_->slDLSSGSetOptions = nullptr;
            api_->slDLSSGGetState = nullptr;
            LOGI("Streamline: DLSS-G functions not resolved: DLSS-G is not supported on this adapter");
        } else {
            const sl::Result setOptions = ResolveFeatureFunction(api_->slGetFeatureFunction, sl::kFeatureDLSS_G,
                                                                 "slDLSSGSetOptions", &api_->slDLSSGSetOptions);
            const sl::Result getState = ResolveFeatureFunction(api_->slGetFeatureFunction, sl::kFeatureDLSS_G,
                                                               "slDLSSGGetState", &api_->slDLSSGGetState);
            if (setOptions != sl::Result::eOk || getState != sl::Result::eOk) {
                LOGI("Streamline: DLSS-G functions unavailable (slDLSSGSetOptions %s, slDLSSGGetState %s)",
                     ResultName(setOptions), ResultName(getState));
            } else {
                LOGI("Streamline: DLSS-G functions resolved");
            }
        }
        LOGI("Streamline: device set, Reflex and PCL functions resolved");
    } while (false);
    if (error) *error = err;
    return err.empty();
}

bool StreamlineRuntime::DlssgSupported(const LUID& luid, std::string* why) const {
    Api* api = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (initialized_ && !shut_down_) api = api_;
    }
    if (!api) {
        if (why) *why = "Streamline is not initialised";
        return false;
    }
    LUID copy = luid;
    sl::AdapterInfo info;
    info.deviceLUID = reinterpret_cast<uint8_t*>(&copy);
    info.deviceLUIDSizeInBytes = sizeof(copy);
    InternalCallScope internal;
    const sl::Result r = api->slIsFeatureSupported(sl::kFeatureDLSS_G, info);
    if (why) {
        std::string text = ResultName(r);
        if (r != sl::Result::eOk) {
            const char* reason = UnsupportedReason(r);
            if (*reason) text += std::string(": ") + reason;
            sl::FeatureRequirements req;
            if (api->slGetFeatureRequirements(sl::kFeatureDLSS_G, req) == sl::Result::eOk) {
                char buf[160];
                std::snprintf(buf, sizeof(buf), " (driver %u.%u.%u, needs %u.%u.%u; OS %u.%u.%u, needs %u.%u.%u)",
                              req.driverVersionDetected.major, req.driverVersionDetected.minor,
                              req.driverVersionDetected.build, req.driverVersionRequired.major,
                              req.driverVersionRequired.minor, req.driverVersionRequired.build,
                              req.osVersionDetected.major, req.osVersionDetected.minor, req.osVersionDetected.build,
                              req.osVersionRequired.major, req.osVersionRequired.minor, req.osVersionRequired.build);
                text += buf;
            }
        }
        *why = text;
    }
    return r == sl::Result::eOk;
}

bool StreamlineRuntime::Upgrade(void** iface) const {
    Api* api = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (initialized_ && !shut_down_) api = api_;
    }
    if (!api || !iface || !*iface) return false;
    InternalCallScope internal;
    const sl::Result r = api->slUpgradeInterface(iface);
    if (r != sl::Result::eOk) LOGW("slUpgradeInterface failed: %s", ResultName(r));
    return r == sl::Result::eOk;
}

bool StreamlineRuntime::IsProxied(IUnknown* obj) const {
    Api* api = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (initialized_ && !shut_down_) api = api_;
    }
    if (!api || !obj) return false;
    InternalCallScope internal;
    const ULONG before = RefCount(obj);
    void* native = nullptr;
    const sl::Result r = api->slGetNativeInterface(obj, &native);
    if (r != sl::Result::eOk || !native) {
        LOGW("slGetNativeInterface failed: %s", ResultName(r));
        return false;
    }
    if (native != obj) {
        static_cast<IUnknown*>(native)->Release();  // the reference the call added
        return true;
    }
    // Not a proxy: drop the extra reference only if the call added one.
    if (RefCount(obj) > before) obj->Release();
    return false;
}

bool StreamlineRuntime::EnableReflexLowLatency() {
    Api* api = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (initialized_ && !shut_down_) api = api_;
    }
    if (!api || !api->slReflexSetOptions) return false;
    sl::ReflexOptions options;
    options.mode = sl::ReflexMode::eLowLatency;
    InternalCallScope internal;
    const sl::Result r = api->slReflexSetOptions(options);
    if (r != sl::Result::eOk) LOGW("slReflexSetOptions(eLowLatency) failed: %s", ResultName(r));
    return r == sl::Result::eOk;
}

bool StreamlineRuntime::ReflexLowLatencyAvailable() const {
    Api* api = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (initialized_ && !shut_down_) api = api_;
    }
    if (!api || !api->slReflexGetState) return false;
    sl::ReflexState state;
    InternalCallScope internal;
    const sl::Result r = api->slReflexGetState(state);
    if (r != sl::Result::eOk) {
        LOGW("slReflexGetState failed: %s", ResultName(r));
        return false;
    }
    return state.lowLatencyAvailable;
}

sl::FrameToken* StreamlineRuntime::NewFrameToken(uint32_t frameIndex) {
    Api* api = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (initialized_ && !shut_down_) api = api_;
    }
    if (!api) return nullptr;
    sl::FrameToken* token = nullptr;
    const uint32_t index = frameIndex;
    InternalCallScope internal;
    const sl::Result r = api->slGetNewFrameToken(token, &index);
    if (r != sl::Result::eOk || !token) {
        LogFirstFailure(g_token_failed, "slGetNewFrameToken", r);
        return nullptr;
    }
    return token;
}

void StreamlineRuntime::ReflexSleep(const sl::FrameToken& token) {
    Api* api = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (initialized_ && !shut_down_) api = api_;
    }
    // Called without the lock: it blocks for Reflex pacing.
    if (!api || !api->slReflexSleep) return;
    InternalCallScope internal;
    const sl::Result r = api->slReflexSleep(token);
    if (r != sl::Result::eOk) LogFirstFailure(g_sleep_failed, "slReflexSleep", r);
}

void StreamlineRuntime::Marker(PclMarker marker, const sl::FrameToken& token) {
    Api* api = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (initialized_ && !shut_down_) api = api_;
    }
    if (!api || !api->slPCLSetMarker) return;
    InternalCallScope internal;
    const sl::Result r = api->slPCLSetMarker(static_cast<sl::PCLMarker>(static_cast<uint32_t>(marker)), token);
    if (r != sl::Result::eOk) LogFirstFailure(g_marker_failed, "slPCLSetMarker", r);
}

bool StreamlineRuntime::DlssgFunctionsResolved() const {
    std::lock_guard<std::mutex> lock(mu_);
    return initialized_ && !shut_down_ && api_ && api_->slDLSSGSetOptions && api_->slDLSSGGetState;
}

sl::Result StreamlineRuntime::SetDlssgOptions(bool on, const DlssgSizeHints& hints) {
    Api* api = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (initialized_ && !shut_down_) api = api_;
    }
    if (!api) return sl::Result::eErrorNotInitialized;
    if (!api->slDLSSGSetOptions) return sl::Result::eErrorFeatureMissing;
    const sl::DLSSGOptions options = BuildDlssgOptions(on, hints, false);
    InternalCallScope internal;
    const sl::Result r = api->slDLSSGSetOptions(sl::ViewportHandle(0u), options);
    if (r != sl::Result::eOk) LogThrottledFailure(on ? "slDLSSGSetOptions(eOn)" : "slDLSSGSetOptions(eOff)", r);
    return r;
}

sl::Result StreamlineRuntime::GetDlssgState(bool requestVramEstimate, const DlssgSizeHints& hints,
                                            sl::DLSSGState* state) {
    Api* api = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (initialized_ && !shut_down_) api = api_;
    }
    if (!api) return sl::Result::eErrorNotInitialized;
    if (!api->slDLSSGGetState) return sl::Result::eErrorFeatureMissing;
    if (!state) return sl::Result::eErrorMissingInputParameter;
    // Options only for the estimate: with them DLSS-G computes it, which is
    // too expensive for a status poll (DLSS-G programming guide, 13.0).
    const sl::DLSSGOptions options = BuildDlssgOptions(true, hints, true);
    sl::DLSSGState out;
    InternalCallScope internal;
    const sl::Result r =
        api->slDLSSGGetState(sl::ViewportHandle(0u), out, requestVramEstimate ? &options : nullptr);
    if (r == sl::Result::eOk) {
        *state = out;
    } else {
        LogThrottledFailure(requestVramEstimate ? "slDLSSGGetState(eRequestVRAMEstimate)" : "slDLSSGGetState", r);
    }
    return r;
}

sl::Result StreamlineRuntime::SetTagsForFrame(const sl::FrameToken& token, uint32_t viewport, ID3D12Resource* depth,
                                              ID3D12Resource* mvec, const sl::Extent& extent,
                                              ID3D12GraphicsCommandList* cmdList) {
    Api* api = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (initialized_ && !shut_down_) api = api_;
    }
    if (!api) return sl::Result::eErrorNotInitialized;
    if (!depth || !mvec) return sl::Result::eErrorMissingInputParameter;
    sl::Resource depthRes(sl::ResourceType::eTex2d, depth, static_cast<uint32_t>(D3D12_RESOURCE_STATE_COMMON));
    sl::Resource mvecRes(sl::ResourceType::eTex2d, mvec, static_cast<uint32_t>(D3D12_RESOURCE_STATE_COMMON));
    const sl::ResourceTag tags[] = {
        sl::ResourceTag(&depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &extent),
        sl::ResourceTag(&mvecRes, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &extent),
    };
    InternalCallScope internal;
    const sl::Result r = api->slSetTagForFrame(token, sl::ViewportHandle(viewport), tags, 2,
                                               static_cast<sl::CommandBuffer*>(cmdList));
    if (r != sl::Result::eOk) LogThrottledFailure("slSetTagForFrame", r);
    return r;
}

sl::Result StreamlineRuntime::SetNullTags(const sl::FrameToken& token, uint32_t viewport) {
    Api* api = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (initialized_ && !shut_down_) api = api_;
    }
    if (!api) return sl::Result::eErrorNotInitialized;
    const sl::ResourceTag tags[] = {
        sl::ResourceTag(nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent),
    };
    InternalCallScope internal;
    const sl::Result r = api->slSetTagForFrame(token, sl::ViewportHandle(viewport), tags, 2, nullptr);
    if (r != sl::Result::eOk) LogThrottledFailure("slSetTagForFrame(null tags)", r);
    return r;
}

sl::Result StreamlineRuntime::SetConstants(const sl::Constants& constants, const sl::FrameToken& token,
                                           uint32_t viewport) {
    Api* api = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (initialized_ && !shut_down_) api = api_;
    }
    if (!api) return sl::Result::eErrorNotInitialized;
    InternalCallScope internal;
    const sl::Result r = api->slSetConstants(constants, token, sl::ViewportHandle(viewport));
    if (r != sl::Result::eOk) LogThrottledFailure("slSetConstants", r);
    return r;
}

int StreamlineRuntime::LogLoadedModules() const {
    std::wstring pluginDir;
    {
        std::lock_guard<std::mutex> lock(mu_);
        pluginDir = plugin_dir_;
    }
    HANDLE snap = INVALID_HANDLE_VALUE;
    // ERROR_BAD_LENGTH: the module list changed while it was read; retry.
    for (int attempt = 0; attempt < 5 && snap == INVALID_HANDLE_VALUE; ++attempt) {
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
        if (snap == INVALID_HANDLE_VALUE && GetLastError() != ERROR_BAD_LENGTH) break;
    }
    if (snap == INVALID_HANDLE_VALUE) {
        LOGW("Streamline modules: cannot list the loaded modules (error %lu)", GetLastError());
        return 0;
    }
    int outside = 0;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Module32FirstW(snap, &entry); ok; ok = Module32NextW(snap, &entry)) {
        const std::wstring name = Lower(entry.szModule);
        const bool slModule = StartsWith(name, L"sl.") && EndsWith(name, L".dll");
        const bool dlssgModule = StartsWith(name, L"nvngx_dlssg") && EndsWith(name, L".dll");
        if (!slModule && !dlssgModule) continue;
        std::wstring path = ModuleFileName(entry.hModule);
        if (path.empty()) path = entry.szExePath;
        const size_t sep = path.find_last_of(L"\\/");
        const std::wstring dir = sep == std::wstring::npos ? std::wstring() : path.substr(0, sep);
        const std::string version = ModuleVersionText(entry.hModule);
        if (!pluginDir.empty() && SamePathNoCase(dir, pluginDir)) {
            LOGI("Streamline module: %s (%s)", ToUtf8(path).c_str(), version.c_str());
        } else {
            ++outside;
            LOGW("Streamline module not from the plugin directory (%s): %s (%s); an override or another mod's "
                 "copy is in use",
                 pluginDir.empty() ? "none" : ToUtf8(pluginDir).c_str(), ToUtf8(path).c_str(), version.c_str());
        }
    }
    CloseHandle(snap);
    return outside;
}

void StreamlineRuntime::Shutdown() {
    Api* api = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (shut_down_) return;
        shut_down_ = true;
        if (initialized_) api = api_;
    }
    if (!api) return;
    InternalCallScope internal;
    const sl::Result r = api->slShutdown();
    LOGI("Streamline: slShutdown %s", ResultName(r));
}

void StreamlineRuntime::OnLogMessage(sl::LogType type, const char* message) {
    if (!message) return;
    std::string_view text(message);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.remove_suffix(1);
    const int len = static_cast<int>(text.size());
    StreamlineRuntime& rt = Get();
    switch (type) {
        case sl::LogType::eError:
            rt.errors_.fetch_add(1);
            LogWrite(LogLevel::Error, "sl: %.*s", len, text.data());
            break;
        case sl::LogType::eWarn:
            rt.warnings_.fetch_add(1);
            LogWrite(LogLevel::Warn, "sl: %.*s", len, text.data());
            break;
        default:
            LogWrite(LogLevel::Info, "sl: %.*s", len, text.data());
            break;
    }
}

}  // namespace acdb
