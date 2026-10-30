#pragma once
// Reads the NVIDIA driver profile settings that silently change DLSS-G
// (spec 6.8 "Driver profile"). Uses NVAPI DRS through nvapi_QueryInterface of
// System32\nvapi64.dll loaded by absolute path; the bridge never changes
// driver settings.
#include <cstdint>
#include <string>
#include <vector>

namespace acdb {

constexpr uint32_t kDrsNgxDlssgMode = 0x10308298;  // NGX_DLSSG_MODE_ID ("Override DLSSG mode")
constexpr uint32_t kDrsSlDlssOverride = 0x10E41E06;  // SL_DLSS_OVERRIDE_ID ("Enable Streamline override")
constexpr uint32_t kNgxDlssgModeOff = 1;             // NGX_DLSSG_MODE_OFF

struct DrsValue {
    bool found = false;
    uint32_t value = 0;
};

// Diagnostics (GitHub issue 10): one setting of a profile as
// NvAPI_DRS_EnumSettings reports it.
struct DrsSetting {
    uint32_t id = 0;
    uint32_t type = 0;      // NVDRS_SETTING_TYPE: 0 DWORD, 1 binary, 2 string, 3 wide string, 4 QWORD
    uint32_t location = 0;  // NVDRS_SETTING_LOCATION: 0 this profile, 1 global, 2 base, 3 default
    std::wstring name;      // the driver's name for it, may be empty
    uint64_t number = 0;    // a DWORD or QWORD value
    std::wstring text;      // a string value
    uint32_t binary_length = 0;  // a binary value's length in bytes
};

struct DriverProfileReport {
    bool nvapi_ok = false;
    std::string error;                 // why NVAPI or DRS could not be read
    std::wstring app_profile;          // name of the profile that holds the exe, empty if none
    DrsValue dlssg_mode_app, dlssg_mode_base;
    DrsValue sl_override_app, sl_override_base;
    // Diagnostics (GitHub issue 10), from NvAPI_DRS_EnumSettings: every
    // setting of the app profile, and the base profile's settings whose
    // value does not come from the driver's defaults. settings_error says
    // why they could not be listed (NvAPI_DRS_EnumSettings missing or
    // failing); the rest of the report does not depend on them.
    std::vector<DrsSetting> app_settings, base_settings;
    std::string settings_error;
};

// NvAPI_Initialize, DRS CreateSession/LoadSettings, FindApplicationByName(exe
// path as given; the host exe's full path returns the profile the driver
// applies to it, while a bare "acs.exe" can match a different profile) for the
// app profile, GetBaseProfile for the global one, GetSetting for both ids in
// both profiles (app: only values set in that profile itself), DestroySession.
// Never throws; a non-NVIDIA system or a missing nvapi64.dll gives
// nvapi_ok=false.
DriverProfileReport ReadDriverProfile(const std::wstring& exePath);

// Pure: "0x<id> <type> <value> (<origin>)" and, when the setting has a name,
// " \"<name>\"". A DWORD is "dword 0x%08X", a QWORD "qword 0x%016llX", a
// string "string|wstring \"<at most 64 characters>...\"", a binary value
// "binary <n> bytes", anything else "type <n>"; the origin is "this
// profile", "global profile", "base profile" or "default"; the name is cut
// at 48 characters ("..." marks a cut).
std::string DrsSettingText(const DrsSetting& s);

// Pure: one English warning per problem, empty when none.
//  - NGX_DLSSG_MODE == NGX_DLSSG_MODE_OFF in the app or base profile:
//    "NVIDIA App DLSS override: frame generation forced off in the <app|global> profile (setting 0x10308298 = 1); DLSS-G will never interpolate"
//  - SL_DLSS_OVERRIDE != 0 in the app or base profile:
//    "NVIDIA App Streamline override is on in the <app|global> profile (setting 0x10E41E06 = <v>); Streamline may load plugins from the NGX cache instead of ac-dlssg\sl"
std::vector<std::string> DriverProfileWarnings(const DriverProfileReport& report);

}  // namespace acdb
