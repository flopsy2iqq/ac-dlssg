#include "driver_profile.h"

#include <windows.h>

// NvApiDriverSettings.h needs nvapi.h's types.
#include <nvapi.h>
#include <NvApiDriverSettings.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

namespace acdb {

static_assert(kDrsNgxDlssgMode == static_cast<uint32_t>(NGX_DLSSG_MODE_ID));
static_assert(kDrsSlDlssOverride == static_cast<uint32_t>(SL_DLSS_OVERRIDE_ID));
static_assert(kDrsNgxDlssFgOverride == static_cast<uint32_t>(NGX_DLSS_FG_OVERRIDE_ID));
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
constexpr unsigned kIdDrsEnumSettings = 0xae3039da;  // diagnostics (GitHub issue 10)

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
    decltype(&NvAPI_DRS_EnumSettings) EnumSettings = nullptr;  // optional (diagnostics)
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

// ---- Diagnostics (GitHub issue 10): the profiles' settings, listed only.

// DrsSetting keeps NVAPI's numbers for the type and the location.
static_assert(NVDRS_DWORD_TYPE == 0 && NVDRS_BINARY_TYPE == 1 && NVDRS_STRING_TYPE == 2 && NVDRS_WSTRING_TYPE == 3 &&
              NVDRS_QWORD_TYPE == 4);
static_assert(NVDRS_CURRENT_PROFILE_LOCATION == 0 && NVDRS_GLOBAL_PROFILE_LOCATION == 1 &&
              NVDRS_BASE_PROFILE_LOCATION == 2 && NVDRS_DEFAULT_PROFILE_LOCATION == 3);

// UTF-8 of at most max characters of w; "..." marks a cut.
std::string Shortened(const std::wstring& w, size_t max) {
    const bool cut = w.size() > max;
    const int n = static_cast<int>(cut ? max : w.size());
    std::string s;
    if (n > 0) {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, w.data(), n, nullptr, 0, nullptr, nullptr);
        if (bytes > 0) {
            s.assign(static_cast<size_t>(bytes), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.data(), n, s.data(), bytes, nullptr, nullptr);
        }
    }
    return cut ? s + "..." : s;
}

const char* OriginName(uint32_t location) {
    switch (location) {
        case NVDRS_CURRENT_PROFILE_LOCATION: return "this profile";
        case NVDRS_GLOBAL_PROFILE_LOCATION: return "global profile";
        case NVDRS_BASE_PROFILE_LOCATION: return "base profile";
        case NVDRS_DEFAULT_PROFILE_LOCATION: return "default";
    }
    return "?";
}

std::wstring WideOf(const NvU16* s) {
    std::wstring w;
    for (size_t i = 0; i < NVAPI_UNICODE_STRING_MAX && s[i]; ++i) w.push_back(static_cast<wchar_t>(s[i]));
    return w;
}

DrsSetting ToDrsSetting(const NVDRS_SETTING& s) {
    DrsSetting d;
    d.id = s.settingId;
    d.type = static_cast<uint32_t>(s.settingType);
    d.location = static_cast<uint32_t>(s.settingLocation);
    d.name = WideOf(s.settingName);
    switch (s.settingType) {
        case NVDRS_DWORD_TYPE: d.number = s.u32CurrentValue; break;
        case NVDRS_QWORD_TYPE: d.number = s.u64CurrentValue; break;
        case NVDRS_STRING_TYPE:
        case NVDRS_WSTRING_TYPE: d.text = WideOf(s.wszCurrentValue); break;
        case NVDRS_BINARY_TYPE: d.binary_length = s.binaryCurrentValue.valueLength; break;
        default: break;
    }
    return d;
}

// Every setting of the profile, or with skipDefault those whose value does
// not come from the driver's defaults; false with *error when it fails.
// Driver 621.01 starts a later page a few settings before startIndex, so a
// setting already listed is skipped; the pages are large, so that usually
// one call returns the whole profile.
bool EnumProfileSettings(const Nvapi& api, NvDRSSessionHandle session, NvDRSProfileHandle profile, bool skipDefault,
                         std::vector<DrsSetting>* out, std::string* error) {
    constexpr NvU32 kBatch = 128;         // NVDRS_SETTING is about 12 KB
    constexpr NvU32 kMaxSettings = 4096;  // a bound for a misbehaving driver
    std::vector<NVDRS_SETTING> batch(kBatch);
    std::vector<NvU32> seen;
    for (NvU32 start = 0; start < kMaxSettings;) {
        for (NVDRS_SETTING& s : batch) {
            std::memset(&s, 0, sizeof(s));
            s.version = NVDRS_SETTING_VER;
        }
        NvU32 count = kBatch;
        const NvAPI_Status status = api.EnumSettings(session, profile, start, &count, batch.data());
        if (status == NVAPI_END_ENUMERATION) return true;
        if (status != NVAPI_OK) {
            *error = Failed(api, "NvAPI_DRS_EnumSettings", status);
            return false;
        }
        if (count > kBatch) count = kBatch;
        for (NvU32 i = 0; i < count; ++i) {
            const NVDRS_SETTING& s = batch[i];
            if (std::find(seen.begin(), seen.end(), s.settingId) != seen.end()) continue;
            seen.push_back(s.settingId);
            if (skipDefault && s.settingLocation == NVDRS_DEFAULT_PROFILE_LOCATION) continue;
            out->push_back(ToDrsSetting(s));
        }
        if (count < kBatch) return true;
        start += count;
    }
    return true;
}

// Lists the settings into *out; a failure only fills r->settings_error (the
// first one is kept), so the rest of the report never depends on it.
void ListSettings(const Nvapi& api, NvDRSSessionHandle session, NvDRSProfileHandle profile, bool skipDefault,
                  std::vector<DrsSetting>* out, DriverProfileReport* r) {
    if (!api.EnumSettings) return;
    std::string err;
    try {
        if (EnumProfileSettings(api, session, profile, skipDefault, out, &err)) return;
    } catch (...) {
        err = "unexpected exception while listing the settings";
    }
    try {
        if (r->settings_error.empty()) r->settings_error = err;
    } catch (...) {
    }
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
                                 error) ||
                    !ReadSetting(api, session, appProfile, kDrsNgxDlssFgOverride, Scope::OwnValueOnly,
                                 &r->fg_override_app, error)) {
                    break;
                }
                ListSettings(api, session, appProfile, false, &r->app_settings, r);  // diagnostics
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
                         error) ||
            !ReadSetting(api, session, base, kDrsNgxDlssFgOverride, Scope::AnyButDefault, &r->fg_override_base,
                         error)) {
            break;
        }
        ListSettings(api, session, base, true, &r->base_settings, r);  // diagnostics
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
    // Optional: without it only the diagnostic list of settings is missing.
    if (!Resolve(query, kIdDrsEnumSettings, &api.EnumSettings))
        r.settings_error = "nvapi_QueryInterface has no NvAPI_DRS_EnumSettings";

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

std::string DrsSettingText(const DrsSetting& s) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "0x%08X ", static_cast<unsigned>(s.id));
    std::string text = buf;
    switch (s.type) {
        case NVDRS_DWORD_TYPE:
            std::snprintf(buf, sizeof(buf), "dword 0x%08X", static_cast<unsigned>(s.number));
            text += buf;
            break;
        case NVDRS_QWORD_TYPE:
            std::snprintf(buf, sizeof(buf), "qword 0x%016llX", static_cast<unsigned long long>(s.number));
            text += buf;
            break;
        case NVDRS_STRING_TYPE:
        case NVDRS_WSTRING_TYPE:
            text += (s.type == NVDRS_STRING_TYPE ? "string \"" : "wstring \"") + Shortened(s.text, 64) + "\"";
            break;
        case NVDRS_BINARY_TYPE:
            std::snprintf(buf, sizeof(buf), "binary %u bytes", static_cast<unsigned>(s.binary_length));
            text += buf;
            break;
        default:
            std::snprintf(buf, sizeof(buf), "type %u", static_cast<unsigned>(s.type));
            text += buf;
            break;
    }
    text += std::string(" (") + OriginName(s.location) + ")";
    if (!s.name.empty()) text += " \"" + Shortened(s.name, 48) + "\"";
    return text;
}

namespace {

// A setting as the driver applies it: the app profile's own value, else the
// global one (Streamline's readOTAOverrideDRS reads them in this order).
struct EffectiveValue {
    DrsValue v;
    const char* where = "";  // "app" or "global"
};

EffectiveValue Effective(const DrsValue& app, const DrsValue& global) {
    if (app.found) return {app, "app"};
    return {global, "global"};
}

DriverProfileNote Finding(DriverNoteKind kind, const char* text) {
    DriverProfileNote n;
    n.kind = kind;
    n.text = text;
    return n;
}

}  // namespace

std::vector<DriverProfileNote> DriverProfileNotes(const DriverProfileReport& report,
                                                  const DriverProfileContext& context) {
    std::vector<DriverProfileNote> warnings;
    char buf[256];
    const struct {
        const DrsValue& v;
        const char* where;
    } dlssg[] = {{report.dlssg_mode_app, "app"}, {report.dlssg_mode_base, "global"}};
    for (const auto& d : dlssg) {
        if (!d.v.found || d.v.value != kNgxDlssgModeOff) continue;
        std::snprintf(buf, sizeof(buf),
                      "NVIDIA App DLSS override: frame generation forced off in the %s profile (setting 0x%08X = %u); "
                      "DLSS-G will never interpolate",
                      d.where, static_cast<unsigned>(kDrsNgxDlssgMode), static_cast<unsigned>(d.v.value));
        warnings.push_back(Finding(DriverNoteKind::Warning, buf));
    }

    bool note = false;
    const EffectiveValue sl = Effective(report.sl_override_app, report.sl_override_base);
    if (sl.v.found && sl.v.value != 0) {
        std::snprintf(buf, sizeof(buf),
                      "NVIDIA App Streamline override is on in the %s profile (setting 0x%08X = %u); Streamline may "
                      "load plugins from the NGX cache instead of ac-dlssg\\sl",
                      sl.where, static_cast<unsigned>(kDrsSlDlssOverride), static_cast<unsigned>(sl.v.value));
        warnings.push_back(Finding(DriverNoteKind::Warning, buf));
    } else if (report.sl_override_base.found && report.sl_override_base.value != 0) {
        // The app profile's 0 wins: Streamline keeps ac-dlssg\sl's plugins.
        note = true;
    }

    bool fgWarning = false;
    const EffectiveValue fg = Effective(report.fg_override_app, report.fg_override_base);
    if (fg.v.found && fg.v.value != 0) {
        if (!context.pinned_dlssg_user.empty()) {
            std::snprintf(buf, sizeof(buf),
                          "NVIDIA App DLSS-FG override is on in the %s profile (setting 0x%08X = %u); NGX may load a "
                          "newer DLSS-G than %s supports",
                          fg.where, static_cast<unsigned>(kDrsNgxDlssFgOverride), static_cast<unsigned>(fg.v.value),
                          context.pinned_dlssg_user.c_str());
            warnings.push_back(Finding(DriverNoteKind::Warning, buf));
            fgWarning = true;
        } else {
            note = true;
        }
    }
    if (note && !fgWarning) warnings.push_back(Finding(DriverNoteKind::Note, kDriverOverrideNote));
    return warnings;
}

uint32_t DriverNotesPanelCode(const std::vector<DriverProfileNote>& notes) {
    if (notes.empty()) return 0;
    for (const DriverProfileNote& n : notes)
        if (n.kind == DriverNoteKind::Warning) return static_cast<uint32_t>(DriverNoteKind::Warning);
    return static_cast<uint32_t>(DriverNoteKind::Note);
}

std::string DriverNotesPanelText(const std::vector<DriverProfileNote>& notes) {
    for (const DriverProfileNote& n : notes)
        if (n.kind == DriverNoteKind::Warning) return n.text;
    return notes.empty() ? std::string() : notes.front().text;
}

}  // namespace acdb
