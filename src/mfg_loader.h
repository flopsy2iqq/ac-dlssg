#pragma once
// MFG-Unlocker (github.com/dashdogy/MFG-Unlocker, MIT, by dashdogy) lifts
// NVIDIA's RTX 50-only limit of DLSS multi frame generation: on an RTX 40 it
// patches NVIDIA's sl.dlss_g and nvngx_dlssg in memory (no file on disk is
// changed), and Streamline then reports numFramesToGenerateMax above 1.
//
// On a PC whose NVIDIA GPUs are all RTX 40, the installer downloads the
// pinned release and puts its DLL into <game>\ac-dlssg\mfg\version.dll:
// MFG-Unlocker takes its proxy exports from its own file name, and
// version.dll is one of its names. In that folder nothing binds the file by
// name (acs.exe and CSP bind VERSION.dll before the bridge runs), and it
// never takes dlssg_for_sm86's place next to acs.exe. The bootstrap loads it
// by full path, before Streamline's interposer, when DecideMfg says so.
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "gpu_info.h"

namespace acdb {

// A DXGI adapter as the bootstrap enumerates it, in DXGI's order.
struct AdapterId {
    uint32_t vendor = 0;
    uint32_t device = 0;
    bool software = false;  // DXGI_ADAPTER_FLAG_SOFTWARE (WARP)
    std::string name;
};

struct PredictedAdapter {
    bool known = false;
    uint32_t vendor = 0;
    uint32_t device = 0;
    std::string name;
};

// The adapter the game is expected to render on, before CSP made its
// device: the first hardware adapter, which CSP's D3D11CreateDevice(nullptr)
// takes and a GPU preference for acs.exe moves to the front, when it is
// NVIDIA; when it is not, the only NVIDIA hardware adapter (an Optimus
// laptop, where CSP's SELECT_ADAPTER may still pick it; the bridge runs on
// NVIDIA only). Not known without a hardware adapter.
PredictedAdapter PredictRenderAdapter(const std::vector<AdapterId>& adapters);

// The release the installer pins. tools\dev-common.ps1 ($script:AcdbMfgPin)
// holds the same version and DLL hash; tools\test-dev-scripts.ps1 (MP0)
// checks that both agree.
struct MfgPin {
    const char* version;
    const char* dllSha256;  // MFGUnlocker.dll from the release zip, lower-case hex
};
constexpr MfgPin kMfgPin = {"1.5.0", "97c6b90cc443b3a5d2151fe992e57e37717071a11eaea6054f2474e883de75d7"};

// <dataDir>\mfg\version.dll.
std::wstring MfgPath(const std::wstring& dataDir);

struct MfgInputs {
    bool enabled = true;      // ac-dlssg.ini enabled=1
    bool configured = true;   // ac-dlssg.ini mfg_unlocker=1
    bool file_exists = false;  // MfgPath(data_dir) exists
    std::string file_sha256;   // its SHA-256, lower-case hex; empty when unknown
    // The adapter the game is expected to render on (PredictRenderAdapter).
    bool render_adapter_known = false;
    GpuArch render_arch = GpuArch::Unknown;
    std::string render_adapter;  // its name, for the log
    bool spoof_loaded = false;   // dlssg_for_sm86 (or a game-folder version.dll taken for it) is loaded
    // ... and that version.dll is neither the pinned spoof nor MFG-Unlocker
    // (kPanelSpoofUnidentified), so the log does not call it dlssg_for_sm86.
    bool spoof_unidentified = false;
    // A module that exports kMfgQueryExport is loaded already: a copy the
    // user installed (FindLoadedMfgCopy).
    bool other_copy_loaded = false;
    std::wstring other_copy_path;
};

enum class MfgAction {
    NotInstalled,   // no MfgPath(data_dir)
    Skip,           // installed, but not loaded (reason)
    Load,           // load MfgPath(data_dir) now
    AlreadyLoaded,  // the user's own copy is in the process; ours stays out
};

struct MfgDecision {
    MfgAction action = MfgAction::NotInstalled;
    bool warn = false;   // log the reason at WARN
    std::string reason;  // for the log
};

// Pure, in this order: a loaded copy of the user's; no file; the bridge
// disabled; mfg_unlocker=0; the spoof loaded (never both); a render adapter
// that is not Ada or not known; a file that is not kMfgPin (WARN); else Load.
MfgDecision DecideMfg(const MfgInputs& in);

// LoadLibraryExW(path, LOAD_WITH_ALTERED_SEARCH_PATH), timed: *ms is the
// time the load took, MFG-Unlocker's DllMain included. Not from DllMain.
HMODULE LoadMfgModule(const std::wstring& path, double* ms, std::string* error);

// The first loaded module that exports kMfgQueryExport, by full path.
bool FindLoadedMfgCopy(std::wstring* path);

// "loaded <path> (file version <v>) in <ms> ms", for "mfg-unlocker:" in
// bridge.log; a module without kMfgQueryExport is named as one that may
// not be MFG-Unlocker.
std::string MfgLoadedLine(const std::string& path, double ms, const std::string& fileVersion, bool exportsQuery);

// When MFG-Unlocker is loaded and the swap chain's render adapter is not
// an Ada chip: what bridge.log says about it (WARN); empty otherwise.
std::string MfgRenderAdapterNote(bool mfgLoaded, bool renderIsAda, const std::string& renderAdapter);

}  // namespace acdb
