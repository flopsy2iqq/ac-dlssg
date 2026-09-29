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

// How the bridge is loaded. Standalone: it is the process's dxgi.dll (in the
// game folder, without ReShade), bound by the loader for acs.exe, d3d11.dll,
// D3D12 and Streamline alike. Proxy: any other file name, i.e. ReShade's
// [PROXY] ProxyLibrary, or a test loading it by path.
enum class BridgeMode { Proxy, Standalone };

// Standalone when the file name part of modulePath is "dxgi.dll"
// (case-insensitive); Proxy otherwise, including for an empty path.
BridgeMode BridgeModeFromPath(const std::wstring& modulePath);
const char* BridgeModeName(BridgeMode mode);  // "standalone" or "proxy"

// The banner line about the game folder's dxgi.dll. isThisBridge: that file
// is this module (standalone), which is never called ReShade.
std::string GameFolderDxgiLine(bool loaded, bool isThisBridge, const std::string& path, const std::string& version);

struct BootstrapState {
    bool possible = false;
    BridgeMode mode = BridgeMode::Proxy;  // from BridgeModule()'s file name
    std::string reason;         // why not possible; empty when possible
    Config config;
    std::wstring game_dir;      // directory of the host exe
    std::wstring data_dir;      // <game_dir>\ac-dlssg
    std::wstring config_path;   // <data_dir>\ac-dlssg.ini (read here; the panel's "Save as default" writes it)
    std::wstring docs_ac_dir;   // DocumentsAcDir()
    CompatInputs compat;        // files + registry HAGS read at bootstrap; module flags re-read and
                                // HAGS taken from the render adapter per swap chain (FactoryHook)
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
// ACDB_VERSION, host exe path, the bridge module and its mode ("mode:
// standalone: ..." or "mode: proxy: ..."), Windows build, the versions of
// version.dll and of the game folder's dxgi.dll (ReShade, or in standalone
// mode this bridge, GameFolderDxgiLine) and dwrite.dll (CSP), all DXGI
// adapters with vendor/device IDs, LUID, ArchName and driver version, and per
// adapter a "adapter <i> D3DKMT:" line (hybrid role, HAGS supported/enabled,
// number of outputs; logging only) plus a WARN per NvidiaDriverWarnings for
// NVIDIA adapters), reads CompatInputs and logs them. M2 adds, in this order: spoof detection
// (version.dll's path is game_dir), ReadDriverProfile(host exe) with every
// warning logged at WARN before any D3D device exists, and, only when the
// config enables the bridge, CameraChannel::Get().Create (M3; a failure is
// logged and leaves DLSS-G without a camera) and StreamlineRuntime::Get().Init(<data_dir>\sl,
// <data_dir>\logs) (a disabled bridge never loads Streamline: streamline_error
// is then "not loaded: the bridge is disabled"); possible = enabled &&
// streamline_ok, reason "disabled in ac-dlssg.ini" or "Streamline: <error>"
// otherwise. Last, whether or not the bridge is enabled (spec 6.9): the
// panel's status and control sections (PanelStatusChannel::Get().Create and
// PanelControlChannel::Get().Create); when this process owns the status it
// publishes the bootstrap's part of it (mode, the config's switches and
// hotkey, the spoof, the first driver-profile warning) with bridgeState
// kPanelNotLoaded ("waiting for the game's swap chain"), or kPanelPassThrough
// with the reason when the bridge is not possible, and logs "panel: status
// section <name> and control section <name> ready"; a status section of
// another process's bridge is logged at INFO and left alone.
// Must not be called from DllMain. Adapters are enumerated through
// GetSystemDxgi() under InternalCallScope.
const BootstrapState& BootstrapRunOnce();

}  // namespace acdb
