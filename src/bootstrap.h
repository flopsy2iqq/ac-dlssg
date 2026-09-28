#pragma once
// One-time start-up on the first CreateDXGIFactory* call (spec 6.10). In M2
// "possible" means: enabled in the config and Streamline initialised from
// <data_dir>\sl. Whether DLSS-G itself is supported is only logged per swap
// chain until M3 uses it. Compatibility is evaluated per swap chain in
// FactoryHook, because it needs the swap chain size and the module state at
// that moment.
#include <windows.h>

#include <string>
#include <vector>

#include "compat.h"
#include "config.h"
#include "driver_profile.h"

namespace acdb {

struct BootstrapState {
    bool possible = false;
    std::string reason;         // why not possible; empty when possible
    Config config;
    std::wstring game_dir;      // directory of the host exe
    std::wstring data_dir;      // <game_dir>\ac-dlssg
    std::wstring docs_ac_dir;   // DocumentsAcDir()
    CompatInputs compat;        // files + HAGS read at bootstrap; module flags re-read per swap chain
    bool streamline_ok = false;       // StreamlineRuntime::Init succeeded
    std::string streamline_error;     // why not, for the log and the decision line
    bool spoof_loaded = false;        // version.dll loaded from game_dir (dlssg_for_sm86)
    DriverProfileReport driver_profile;
    std::vector<std::string> driver_warnings;  // DriverProfileWarnings(driver_profile)
};

void SetBridgeModule(HMODULE module);
HMODULE BridgeModule();

// Runs once (std::call_once): loads <data_dir>\ac-dlssg.ini, moves the
// previous run's <data_dir>\logs\bridge.log to bridge.prev.log, opens
// bridge.log at the configured level, logs a banner (bridge version
// ACDB_VERSION, host exe path, Windows build, the versions of version.dll and
// of the game folder's dxgi.dll (ReShade) and dwrite.dll (CSP), all DXGI
// adapters with vendor/device IDs, LUID, ArchName and driver version), reads
// CompatInputs and logs them. M2 adds, in this order: spoof detection
// (version.dll's path is game_dir), ReadDriverProfile(host exe) with every
// warning logged at WARN before any D3D device exists, and, only when the
// config enables the bridge, StreamlineRuntime::Get().Init(<data_dir>\sl,
// <data_dir>\logs) (a disabled bridge never loads Streamline: streamline_error
// is then "not loaded: the bridge is disabled"); possible = enabled &&
// streamline_ok, reason "disabled in ac-dlssg.ini" or "Streamline: <error>"
// otherwise.
// Must not be called from DllMain. Adapters are enumerated through
// GetSystemDxgi() under InternalCallScope.
const BootstrapState& BootstrapRunOnce();

}  // namespace acdb
