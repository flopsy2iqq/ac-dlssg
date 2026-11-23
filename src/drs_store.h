#pragma once
// The NVIDIA driver settings as ac-dlssg-drs.exe reads and writes them
// (NVAPI DRS), behind an interface so that its commands (drs_tool.h) are
// tested against a fake; MakeNvapiStore is the real one. Part of acdb_drs,
// which the bridge DLL never links (smooth_motion.h).
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "smooth_motion.h"

namespace acdb {

// The two Smooth Motion settings of the application profile that holds
// acs.exe, and of the global profile.
struct SmoothMotionProfile {
    std::wstring name;          // the application profile's name, such as "Assetto Corsa"
    ProfileValue apis, enable;  // its own values of kDrsSmoothMotionApis and kDrsSmoothMotionEnable
    // The global (base) profile's values; not present when they are the
    // driver's defaults.
    ProfileValue globalApis, globalEnable;
};

enum class DrsStatus {
    Ok,
    NoDriver,    // NVAPI or the driver settings could not be loaded
    NoProfile,   // no profile holds the exe, or none has the name
    NeedsAdmin,  // NvAPI_DRS_SaveSettings refused: NVAPI_INVALID_USER_PRIVILEGE
    Failed       // any other NVAPI failure
};

class DriverSettingsStore {
public:
    virtual ~DriverSettingsStore() = default;
    // Loads the driver settings afresh and reads the profile: the one that
    // holds exePath (a full path, as NvAPI_DRS_FindApplicationByName takes
    // it) when profileName is empty, else the one with that name. *error
    // says why when the status is not Ok.
    virtual DrsStatus Read(const std::wstring& exePath, const std::wstring& profileName, SmoothMotionProfile* out,
                           std::string* error) = 0;
    // In the profile of the last successful Read: sets the DWORD setting to
    // *value, or deletes the profile's own value when value is empty, then
    // saves the driver settings.
    virtual DrsStatus Write(uint32_t settingId, std::optional<uint32_t> value, std::string* error) = 0;
};

// NVAPI DRS through nvapi_QueryInterface of System32\nvapi64.dll, loaded by
// absolute path (nvapi_drs.h).
std::unique_ptr<DriverSettingsStore> MakeNvapiStore();

}  // namespace acdb
