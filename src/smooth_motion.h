#pragma once
// NVIDIA Smooth Motion in the driver profile (GitHub issue 10): the pure
// decisions of ac-dlssg-drs.exe, the installer's helper that turns Smooth
// Motion off for Assetto Corsa and back on at uninstall. Smooth Motion is
// the driver's own frame generation (NvPresent64.dll, RTX 40 and 50); while
// it takes over the game's window, DLSS-G cannot run (nvpresent.h).
//
// The two settings are not in NVIDIA's public NvApiDriverSettings.h. Their
// names and values are those of NVIDIA Profile Inspector's
// CustomSettingNames.xml, and NvPresent64.dll (driver 621.x) reads them so:
//  - 0xB0CC0875 "Smooth Motion - Enabled APIs", a bit mask: 1 DX12, 2 DX11,
//    4 Vulkan. Absent, every API is allowed (NvPresent reads 0xFFFFFFFF;
//    Profile Inspector shows 7, the value used here).
//  - 0xB0D384C0 "Smooth Motion - Enable", 0 off or 1 on; absent counts as
//    off. The helper only reads it.
// Clearing the DX11 bit was enough to stop Smooth Motion in AC (the i10fixed
// log of issue 10). Only the application profile that holds acs.exe is ever
// changed, never the global one, and the bridge DLL links none of this: it
// never changes driver settings (driver_profile.h).
#include <cstdint>
#include <string>

namespace acdb {

constexpr uint32_t kDrsSmoothMotionApis = 0xB0CC0875;    // "Smooth Motion - Enabled APIs"
constexpr uint32_t kDrsSmoothMotionEnable = 0xB0D384C0;  // "Smooth Motion - Enable"
constexpr uint32_t kSmoothMotionApiDx12 = 1;
constexpr uint32_t kSmoothMotionApiDx11 = 2;
constexpr uint32_t kSmoothMotionApiVulkan = 4;
constexpr uint32_t kSmoothMotionApisDefault = 7;  // absent from both profiles: every API

// A profile's own DWORD value of one setting, as NvAPI_DRS_GetSetting reports
// it (a value the profile only inherits is not its own).
struct ProfileValue {
    bool present = false;
    uint32_t value = 0;
    // NVIDIA's own value in that profile (isCurrentPredefined), not a user's.
    bool predefined = false;
    bool operator==(const ProfileValue&) const = default;
};

enum class SmoothMotionAction { None, Write };

struct SmoothMotionOffPlan {
    SmoothMotionAction action = SmoothMotionAction::None;
    // The mask the driver applies to acs.exe now, and where it comes from:
    // "app", "global" or "default" (neither profile has the setting).
    uint32_t effective = kSmoothMotionApisDefault;
    const char* source = "default";
    uint32_t value = 0;  // Write: the app profile's new value
};

// Pure: the effective mask is the app profile's own value, else the global
// profile's, else kSmoothMotionApisDefault. With its DX11 bit set, Write
// effective & ~kSmoothMotionApiDx11 into the app profile; otherwise None,
// DX11 is already excluded.
SmoothMotionOffPlan PlanSmoothMotionOff(const ProfileValue& app, const ProfileValue& global);

// What the install recorded (the manifest's driverProfile).
struct SmoothMotionRecord {
    ProfileValue before;   // the app profile's own value before the write
    uint32_t written = 0;  // the value the install wrote there
};

enum class RestoreAction {
    Delete,    // NvAPI_DRS_DeleteProfileSetting
    WriteOld,  // NvAPI_DRS_SetSetting(value)
    Leave      // changed or gone since the install: not ours any more
};

struct RestorePlan {
    RestoreAction action = RestoreAction::Leave;
    uint32_t value = 0;  // WriteOld: record.before.value
};

// Pure: only while the app profile's own value is still record.written.
// Delete when the profile had no value of its own before, or had NVIDIA's
// predefined one (deleting the user value brings that back); else WriteOld
// with the old value. Leave otherwise.
RestorePlan PlanRestore(const SmoothMotionRecord& record, const ProfileValue& current);

// The API bits of a mask: "DX12, DX11, Vulkan", "none" for 0, and any other
// bits as ", other bits 0x%08X".
std::string SmoothMotionApisText(uint32_t mask);

// NVIDIA's own profile for the game. It matches only an acs.exe in a folder
// named assettocorsa ("assettocorsa/acs.exe"), as in every Steam install.
constexpr wchar_t kNvidiaAcProfileName[] = L"Assetto Corsa";

// Pure (review finding A-1): the profile that holds acs.exe is one that
// NVIDIA's driver ships (NVDRS_PROFILE.isPredefined) for another game, that
// is, a predefined profile not named kNvidiaAcProfileName (ignoring case).
// One such, "ACS", holds a bare acs.exe and so gets every acs.exe that is
// not in a folder named assettocorsa. A change there would apply to that
// game too, so the helper leaves it alone. A profile a user made, whatever
// its name, is never foreign.
bool IsForeignPredefinedProfile(const std::wstring& name, bool predefined);

}  // namespace acdb
