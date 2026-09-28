#include "driver_profile.h"

#include <windows.h>

// NvApiDriverSettings.h needs nvapi.h's types.
#include <nvapi.h>
#include <NvApiDriverSettings.h>

#include <cstdio>
#include <memory>

namespace acdb {

static_assert(kDrsNgxDlssgMode == static_cast<uint32_t>(NGX_DLSSG_MODE_ID));
static_assert(kDrsSlDlssOverride == static_cast<uint32_t>(SL_DLSS_OVERRIDE_ID));
static_assert(kNgxDlssgModeOff == static_cast<uint32_t>(NGX_DLSSG_MODE_OFF));

namespace {

// nvapi_QueryInterface ids, from nvapi_interface.h of the pinned NVAPI commit.
constexpr unsigned kIdInitialize = 0x0150e828;
constexpr unsigned kIdUnload = 0xd22bdd7e;
constexpr unsigned kIdGetErrorMessage = 0x6c2d048c;
constexpr unsigned kIdDrsCreateSession = 0x0694d52e;
constexpr unsigned kIdDrsDestroySession = 0xdad9cff8;
constexpr unsigned kIdDrsLoadSettings = 0x375dbd6b;
constexpr unsigned kIdDrsFindApplicationByName = 0xeee566b2;
constexpr unsigned kIdDrsGetProfileInfo = 0x61cd6fd6;
constexpr unsigned kIdDrsGetBaseProfile = 0xda8466a0;
constexpr unsigned kIdDrsGetSetting = 0x73bf8338;

using QueryInterfaceFn = void*(__cdecl*)(unsigned int id);

struct Nvapi {
    decltype(&NvAPI_Initialize) Initialize = nullptr;
    decltype(&NvAPI_Unload) Unload = nullptr;
    decltype(&NvAPI_GetErrorMessage) GetErrorMessage = nullptr;
    decltype(&NvAPI_DRS_CreateSession) CreateSession = nullptr;
    decltype(&NvAPI_DRS_DestroySession) DestroySession = nullptr;
    decltype(&NvAPI_DRS_LoadSettings) LoadSettings = nullptr;
    decltype(&NvAPI_DRS_FindApplicationByName) FindApplicationByName = nullptr;
    decltype(&NvAPI_DRS_GetProfileInfo) GetProfileInfo = nullptr;
    decltype(&NvAPI_DRS_GetBaseProfile) GetBaseProfile = nullptr;
    decltype(&NvAPI_DRS_GetSetting) GetSetting = nullptr;
};

template <typename Fn>
bool Resolve(QueryInterfaceFn query, unsigned id, Fn* out) {
    *out = reinterpret_cast<Fn>(query(id));
    return *out != nullptr;
}

// "NvAPI_DRS_LoadSettings failed: -3 (NVAPI_NO_IMPLEMENTATION)".
std::string Failed(const Nvapi& api, const char* what, NvAPI_Status status) {
    NvAPI_ShortString text = {};
    const bool haveText = api.GetErrorMessage && api.GetErrorMessage(status, text) == NVAPI_OK;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s failed: %d%s%s%s", what, static_cast<int>(status), haveText ? " (" : "",
                  haveText ? text : "", haveText ? ")" : "");
    return buf;
}

enum class Scope { OwnValueOnly, AnyButDefault };

// A DWORD setting of one profile. OwnValueOnly skips values the profile only
// inherits from the global or base profile, so each is reported once.
bool ReadSetting(const Nvapi& api, NvDRSSessionHandle session, NvDRSProfileHandle profile, NvU32 id, Scope scope,
                 DrsValue* out, std::string* error) {
    *out = DrsValue();
    auto setting = std::make_unique<NVDRS_SETTING>();
    setting->version = NVDRS_SETTING_VER;
    const NvAPI_Status status = api.GetSetting(session, profile, id, setting.get());
    if (status == NVAPI_SETTING_NOT_FOUND) return true;
    if (status != NVAPI_OK) {
        char what[64];
        std::snprintf(what, sizeof(what), "NvAPI_DRS_GetSetting(0x%08X)", static_cast<unsigned>(id));
        *error = Failed(api, what, status);
        return false;
    }
    if (setting->settingType != NVDRS_DWORD_TYPE) return true;
    const NVDRS_SETTING_LOCATION loc = setting->settingLocation;
    if (scope == Scope::OwnValueOnly && loc != NVDRS_CURRENT_PROFILE_LOCATION) return true;
    if (scope == Scope::AnyButDefault && loc == NVDRS_DEFAULT_PROFILE_LOCATION) return true;
    out->found = true;
    out->value = setting->u32CurrentValue;
    return true;
}

// Destroys the DRS session on every exit, a C++ exception included.
struct SessionGuard {
    const Nvapi& api;
    NvDRSSessionHandle session;
    ~SessionGuard() { api.DestroySession(session); }
};

// Everything after NvAPI_Initialize; false with *error when a step fails.
bool ReadWithSession(const Nvapi& api, const std::wstring& exePath, DriverProfileReport* r, std::string* error) {
    NvDRSSessionHandle session = nullptr;
    NvAPI_Status status = api.CreateSession(&session);
    if (status != NVAPI_OK) {
        *error = Failed(api, "NvAPI_DRS_CreateSession", status);
        return false;
    }
    const SessionGuard guard{api, session};
    bool ok = false;
    do {
        status = api.LoadSettings(session);
        if (status != NVAPI_OK) {
            *error = Failed(api, "NvAPI_DRS_LoadSettings", status);
            break;
        }

        // The app profile: with a full path, NVAPI returns the profile the
        // driver applies to that very file.
        if (!exePath.empty()) {
            NvAPI_UnicodeString name = {};
            const size_t n = exePath.size() < NVAPI_UNICODE_STRING_MAX - 1 ? exePath.size()
                                                                           : NVAPI_UNICODE_STRING_MAX - 1;
            for (size_t i = 0; i < n; ++i) name[i] = static_cast<NvU16>(exePath[i]);
            auto app = std::make_unique<NVDRS_APPLICATION>();
            app->version = NVDRS_APPLICATION_VER;
            NvDRSProfileHandle appProfile = nullptr;
            status = api.FindApplicationByName(session, name, &appProfile, app.get());
            if (status == NVAPI_OK && appProfile) {
                auto info = std::make_unique<NVDRS_PROFILE>();
                info->version = NVDRS_PROFILE_VER;
                if (api.GetProfileInfo(session, appProfile, info.get()) == NVAPI_OK) {
                    const NvU16* p = info->profileName;
                    for (size_t i = 0; i < NVAPI_UNICODE_STRING_MAX && p[i]; ++i) {
                        r->app_profile.push_back(static_cast<wchar_t>(p[i]));
                    }
                }
                if (!ReadSetting(api, session, appProfile, kDrsNgxDlssgMode, Scope::OwnValueOnly, &r->dlssg_mode_app,
                                 error) ||
                    !ReadSetting(api, session, appProfile, kDrsSlDlssOverride, Scope::OwnValueOnly, &r->sl_override_app,
                                 error)) {
                    break;
                }
            } else if (status != NVAPI_EXECUTABLE_NOT_FOUND && status != NVAPI_PROFILE_NOT_FOUND &&
                       status != NVAPI_EXECUTABLE_PATH_IS_AMBIGUOUS) {
                *error = Failed(api, "NvAPI_DRS_FindApplicationByName", status);
                break;
            }
            // Not found or ambiguous: no app profile applies to that name.
        }

        NvDRSProfileHandle base = nullptr;
        status = api.GetBaseProfile(session, &base);
        if (status != NVAPI_OK) {
            *error = Failed(api, "NvAPI_DRS_GetBaseProfile", status);
            break;
        }
        if (!ReadSetting(api, session, base, kDrsNgxDlssgMode, Scope::AnyButDefault, &r->dlssg_mode_base, error) ||
            !ReadSetting(api, session, base, kDrsSlDlssOverride, Scope::AnyButDefault, &r->sl_override_base,
                         error)) {
            break;
        }
        ok = true;
    } while (false);
    return ok;
}

// Balances NvAPI_Initialize's reference count on every exit.
struct UnloadGuard {
    const Nvapi& api;
    ~UnloadGuard() { api.Unload(); }
};

DriverProfileReport ReadDriverProfileImpl(const std::wstring& exePath) {
    DriverProfileReport r;
    wchar_t sysDir[MAX_PATH] = {};
    const UINT len = GetSystemDirectoryW(sysDir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        r.error = "GetSystemDirectoryW failed";
        return r;
    }
    // Absolute System32 path: never a nvapi64.dll from the game folder.
    const std::wstring path = std::wstring(sysDir) + L"\\nvapi64.dll";
    const HMODULE nvapi = LoadLibraryW(path.c_str());
    if (!nvapi) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "System32\\nvapi64.dll not loaded (error %lu); no NVIDIA driver",
                      static_cast<unsigned long>(GetLastError()));
        r.error = buf;
        return r;
    }
    // nvapi64.dll stays loaded: the driver, CSP and Streamline use it too.
    const auto query = reinterpret_cast<QueryInterfaceFn>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (!query) {
        r.error = "nvapi64.dll has no nvapi_QueryInterface export";
        return r;
    }
    Nvapi api;
    Resolve(query, kIdGetErrorMessage, &api.GetErrorMessage);  // optional, for error texts
    const char* missing = nullptr;
    if (!Resolve(query, kIdInitialize, &api.Initialize)) missing = "NvAPI_Initialize";
    else if (!Resolve(query, kIdUnload, &api.Unload)) missing = "NvAPI_Unload";
    else if (!Resolve(query, kIdDrsCreateSession, &api.CreateSession)) missing = "NvAPI_DRS_CreateSession";
    else if (!Resolve(query, kIdDrsDestroySession, &api.DestroySession)) missing = "NvAPI_DRS_DestroySession";
    else if (!Resolve(query, kIdDrsLoadSettings, &api.LoadSettings)) missing = "NvAPI_DRS_LoadSettings";
    else if (!Resolve(query, kIdDrsFindApplicationByName, &api.FindApplicationByName))
        missing = "NvAPI_DRS_FindApplicationByName";
    else if (!Resolve(query, kIdDrsGetProfileInfo, &api.GetProfileInfo)) missing = "NvAPI_DRS_GetProfileInfo";
    else if (!Resolve(query, kIdDrsGetBaseProfile, &api.GetBaseProfile)) missing = "NvAPI_DRS_GetBaseProfile";
    else if (!Resolve(query, kIdDrsGetSetting, &api.GetSetting)) missing = "NvAPI_DRS_GetSetting";
    if (missing) {
        r.error = std::string("nvapi_QueryInterface has no ") + missing;
        return r;
    }

    const NvAPI_Status status = api.Initialize();
    if (status != NVAPI_OK) {
        r.error = Failed(api, "NvAPI_Initialize", status);
        return r;
    }
    std::string error;
    {
        const UnloadGuard unload{api};
        r.nvapi_ok = ReadWithSession(api, exePath, &r, &error);
    }
    if (!r.nvapi_ok) r.error = error;  // values read before the failure stay
    return r;
}

}  // namespace

DriverProfileReport ReadDriverProfile(const std::wstring& exePath) {
    try {
        return ReadDriverProfileImpl(exePath);
    } catch (...) {
        DriverProfileReport r;
        r.error = "unexpected exception while reading the driver profile";
        return r;
    }
}

std::vector<std::string> DriverProfileWarnings(const DriverProfileReport& report) {
    std::vector<std::string> out;
    char buf[256];
    const struct {
        const DrsValue& v;
        const char* where;
    } dlssg[] = {{report.dlssg_mode_app, "app"}, {report.dlssg_mode_base, "global"}},
      over[] = {{report.sl_override_app, "app"}, {report.sl_override_base, "global"}};
    for (const auto& d : dlssg) {
        if (!d.v.found || d.v.value != kNgxDlssgModeOff) continue;
        std::snprintf(buf, sizeof(buf),
                      "NVIDIA App DLSS override: frame generation forced off in the %s profile (setting 0x%08X = %u); "
                      "DLSS-G will never interpolate",
                      d.where, static_cast<unsigned>(kDrsNgxDlssgMode), static_cast<unsigned>(d.v.value));
        out.push_back(buf);
    }
    for (const auto& o : over) {
        if (!o.v.found || o.v.value == 0) continue;
        std::snprintf(buf, sizeof(buf),
                      "NVIDIA App Streamline override is on in the %s profile (setting 0x%08X = %u); Streamline may "
                      "load plugins from the NGX cache instead of ac-dlssg\\sl",
                      o.where, static_cast<unsigned>(kDrsSlDlssOverride), static_cast<unsigned>(o.v.value));
        out.push_back(buf);
    }
    return out;
}

}  // namespace acdb
