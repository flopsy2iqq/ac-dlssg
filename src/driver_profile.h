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
constexpr uint32_t kDrsNgxDlssFgOverride = 0x10E41E03;  // NGX_DLSS_FG_OVERRIDE_ID ("Enable DLSS-FG override")
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
    bool app_profile_predefined = false;  // ... and NVIDIA's driver ships it (NVDRS_PROFILE.isPredefined)
    DrsValue dlssg_mode_app, dlssg_mode_base;
    // "Smooth Motion - Enabled APIs" (kDrsSmoothMotionApis, smooth_motion.h):
    // the app profile's own value and the global one, for the Smooth Motion
    // reason (review finding B-10).
    DrsValue smooth_motion_apis_app, smooth_motion_apis_base;
    DrsValue sl_override_app, sl_override_base;
    DrsValue fg_override_app, fg_override_base;
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
// app profile, GetBaseProfile for the global one, GetSetting for the three
// ids in both profiles (app: only values set in that profile itself),
// DestroySession.
// Never throws; a non-NVIDIA system or a missing nvapi64.dll gives
// nvapi_ok=false.
DriverProfileReport ReadDriverProfile(const std::wstring& exePath);

// Pure (review finding B-10): an app profile holds the exe, and "Smooth
// Motion - Enabled APIs" in effect for it (its own value, else the global
// profile's, else every API, as PlanSmoothMotionOff reads them) lacks DX11.
// False when the settings could not be read.
bool SmoothMotionDx11Excluded(const DriverProfileReport& report);

// Pure: "0x<id> <type> <value> (<origin>)" and, when the setting has a name,
// " \"<name>\"". A DWORD is "dword 0x%08X", a QWORD "qword 0x%016llX", a
// string "string|wstring \"<at most 64 characters>...\"", a binary value
// "binary <n> bytes", anything else "type <n>"; the origin is "this
// profile", "global profile", "base profile" or "default"; the name is cut
// at 48 characters ("..." marks a cut).
std::string DrsSettingText(const DrsSetting& s);

// How a finding is shown: StatusLayout::driverWarning (kPanelDriverWarning,
// kPanelDriverNote) and bridge.log (WARN or INFO).
enum class DriverNoteKind : uint32_t { Warning = 1, Note = 2 };

struct DriverProfileNote {
    DriverNoteKind kind = DriverNoteKind::Warning;
    std::string text;
    bool operator==(const DriverProfileNote&) const = default;
};

// What DriverProfileNotes needs besides the profile: the add-on that works
// only with the DLSS-G of ac-dlssg\sl, "dlssg_for_sm86" (RTX 20/30, tied to
// its DLSS-G 310.9.1) or "MFG-Unlocker" (RTX 40, it patches only the DLSS-G
// versions it knows); empty when neither is in use.
struct DriverProfileContext {
    std::string pinned_dlssg_user;
};

// The note for the NVIDIA App's DLSS override when it does no harm.
constexpr char kDriverOverrideNote[] =
    "Note: the NVIDIA App's DLSS override is on, so DLSS-G may use the App's newer model. This is normal, not an "
    "error.";

// Pure: the findings, warnings first, empty when none.
//  - NGX_DLSSG_MODE == NGX_DLSSG_MODE_OFF in the app or base profile, a
//    warning for each:
//    "NVIDIA App DLSS override: frame generation forced off in the <app|global> profile (setting 0x10308298 = 1); DLSS-G will never interpolate"
//  - SL_DLSS_OVERRIDE: Streamline reads the app profile's value and the
//    global one only when the app profile has none. A value other than 0
//    is a warning:
//    "NVIDIA App Streamline override is on in the <app|global> profile (setting 0x10E41E06 = <v>); Streamline may load plugins from the NGX cache instead of ac-dlssg\sl"
//    The app profile's 0 under a global value other than 0 is the note.
//  - NGX_DLSS_FG_OVERRIDE, the app profile's value, else the global one,
//    other than 0 (NGX loads the NVIDIA App's DLSS-G from its cache): with
//    context.pinned_dlssg_user a warning
//    "NVIDIA App DLSS-FG override is on in the <app|global> profile (setting 0x10E41E03 = <v>); NGX may load a newer DLSS-G than <user> supports",
//    else the note.
// The note is kDriverOverrideNote, at most once, and never next to the
// DLSS-FG warning.
std::vector<DriverProfileNote> DriverProfileNotes(const DriverProfileReport& report,
                                                  const DriverProfileContext& context);

// Pure, for the panel: StatusLayout::driverWarning (0 none, 1 when there is
// a warning, 2 when there are only notes), and the text it shows, the first
// warning, else the first note.
uint32_t DriverNotesPanelCode(const std::vector<DriverProfileNote>& notes);
std::string DriverNotesPanelText(const std::vector<DriverProfileNote>& notes);

}  // namespace acdb
