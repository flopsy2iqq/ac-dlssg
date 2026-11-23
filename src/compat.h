#pragma once
// Compatibility refusals (spec 6.10). Pure rules in EvaluateCompat; I/O in
// ReadCompatInputs.
#include <optional>
#include <string>

namespace acdb {

struct CompatInputs {
    // CSP keys, each read from <game>\extension\config\<file> and overridden by
    // <docs>\cfg\extension\<file> when the key is present there.
    std::optional<long long> old_swapchain;          // dxgi_tweaks.ini [COMPATIBILITY] OLD_SWAPCHAIN
    std::optional<long long> exclusive_fullscreen;   // dxgi_tweaks.ini [COMPATIBILITY] EXCLUSIVE_FULLSCREEN
    std::optional<long long> allow_stretching;       // dxgi_tweaks.ini [COMPATIBILITY] ALLOW_STRETCHING
    std::optional<long long> hdr_enabled;            // dxgi_tweaks.ini [HDR] ENABLED
    std::optional<long long> fsr_active;             // graphics_adjustments.ini [FSR] ACTIVE
    std::optional<long long> fsr_old_implementation; // graphics_adjustments.ini [FSR] OLD_IMPLEMENTATION
    // AC keys, from <docs>\cfg\video.ini.
    std::optional<long long> aasamples;              // [VIDEO] AASAMPLES
    std::optional<long long> video_width;            // [VIDEO] WIDTH
    std::optional<long long> video_height;           // [VIDEO] HEIGHT
    std::optional<std::string> camera_mode;          // [CAMERA] MODE
    std::string video_ini_path;  // that video.ini's full path (UTF-8), for the MSAA reason
    // System state.
    // ReadCompatInputs: the registry value (HwSchMode == 2). Per swap chain
    // FactoryHook replaces it with the render adapter's D3DKMT state and uses
    // the registry value only when D3DKMT cannot answer (EvaluateChainCompat).
    bool hags_on = false;
    bool dlss5_bridge_loaded = false;   // LoadedDlss5BridgeFile() is not empty
    std::string dlss5_bridge_file;      // ... and which file it is, for the reason
    bool renodx_dlss5_loaded = false;   // GetModuleHandleW(L"renodx-dlss5.addon64")
    // NVIDIA Smooth Motion creates the game's swap chain: NvPresentInCallStack()
    // at its CreateSwapChainForHwnd (FactoryHook sets it per swap chain).
    bool nvpresent_caller = false;
    // The driver profile that holds acs.exe, for rule 0's reason (review
    // finding B-10); the bootstrap sets them from ReadDriverProfile.
    // sm_profile: its name (UTF-8), empty when none holds acs.exe or the
    // settings could not be read; sm_profile_foreign: it is another game's
    // predefined profile (IsForeignPredefinedProfile); sm_dx11_excluded:
    // "Smooth Motion - Enabled APIs" in effect for it already lacks DX11
    // (SmoothMotionDx11Excluded).
    std::string sm_profile;
    bool sm_profile_foreign = false;
    bool sm_dx11_excluded = false;
    // ac-dlssg.ini [debug] triple_diag=1 (the bootstrap sets it from the
    // config): triple-screen mode is let through for the diagnostics of
    // triple_diag.h.
    bool triple_diag = false;
};

struct CompatResult {
    bool ok = true;
    std::string reason;  // empty when ok; otherwise the first failing rule, in English
};

// Rules in this order; the first that fails is reported:
//  0. nvpresent_caller                 -> SmoothMotionRefusal(in)
//  1. old_swapchain == 1               -> "OLD_SWAPCHAIN=1 is not supported"
//  2. exclusive_fullscreen == 1        -> "EXCLUSIVE_FULLSCREEN=1 is not supported"
//  3. hdr_enabled == 1                 -> "HDR output is not supported"
//  4. fsr_active != 1 or fsr_old_implementation != 3 (missing counts as 0)
//                                      -> "CSP upscaler is not DLSS (need [FSR] ACTIVE=1, OLD_IMPLEMENTATION=3)"
//  5. aasamples > 1                    -> "MSAA is on: AASAMPLES=<n> in <video_ini_path, else video.ini> (read at
//                                         game start); set Anti-aliasing to Off in Content Manager > Settings >
//                                         Assetto Corsa > Video" (in every DLSS mode, not only DLAA)
//  6. camera_mode present and not DEFAULT (case-insensitive), and not TRIPLE
//     with triple_diag (the diagnostics of triple_diag.h)
//                                      -> VrRefusal for a VR mode; "camera mode TRIPLE (triple screen) is not
//                                         supported"; "custom rendering mode <MODE> (360, fisheye or split
//                                         screen) is not supported" for CSP's __EXT_ modes; else "camera mode
//                                         <MODE> is not supported"
//  7. allow_stretching != 1 (missing counts as 0) and both video sizes known
//     and AspectMismatch(...)          -> "video.ini aspect ratio differs from the window; letterboxed output is not supported in v1"
//  8. dlss5_bridge_loaded              -> "DLSS 5 Bridge (<dlss5_bridge_file, else dlss5-bridge.addon64>) cannot
//                                         run together with ac-dlssg yet. Use CSP's built-in Neural Rendering
//                                         instead, which works with ac-dlssg, or remove the add-on for frame
//                                         generation."
//  9. renodx_dlss5_loaded              -> the same for "RenoDX DLSS5 (renodx-dlss5.addon64)"
// 10. !hags_on                         -> "hardware-accelerated GPU scheduling is off"
CompatResult EvaluateCompat(const CompatInputs& in, unsigned swapWidth, unsigned swapHeight);

// Rule 0's reason, by the driver profile that holds acs.exe (review finding
// B-10), each one shorter than the panel's 256 characters:
//  - sm_dx11_excluded with a profile (install.bat would find nothing to do):
//    "NVIDIA Smooth Motion is still on for AC, though the profile "<name>"
//    keeps DX11 out; DLSS-G cannot run with it. In NVIDIA Profile Inspector
//    set Smooth Motion - Enabled APIs to 5 in the global profile ("Base
//    Profile") too, then restart AC."
//  - sm_profile_foreign (install.bat leaves that profile alone): "NVIDIA
//    Smooth Motion is on for AC; DLSS-G cannot run with it. acs.exe gets
//    another game's NVIDIA profile "<name>", which install.bat leaves alone:
//    in NVIDIA Profile Inspector set its Smooth Motion - Enabled APIs to 5,
//    then restart AC."
//  - else: "NVIDIA Smooth Motion (the driver's own frame generation) is
//    active for Assetto Corsa, even if the NVIDIA App shows it off. It cannot
//    run with DLSS-G. Run install.bat again to turn it off for AC (or see the
//    README), then restart the game."
// A name longer than 20 bytes is cut at a whole UTF-8 character, with "...".
std::string SmoothMotionRefusal(const CompatInputs& in);

// Which of the three SmoothMotionRefusal gives: install.bat, the global
// profile (sm_dx11_excluded with a profile) or another game's profile
// (sm_profile_foreign with a profile).
enum class SmoothMotionFix { InstallBat, GlobalProfile, ForeignProfile };
SmoothMotionFix SmoothMotionFixOf(const CompatInputs& in);

// VR, by video.ini's [CAMERA] MODE (case-insensitive): AC's OPENVR and
// OCULUS, and CSP's OpenXR mode __EXT_OPENXR. DLSS-G presents through the
// monitor's swap chain, which in VR only mirrors the headset, so the bridge
// stays off: "VR (MODE=<MODE>): DLSS Frame Generation works only on a
// monitor, not in a headset; ac-dlssg stays off in VR". Empty for any other
// mode or none.
std::string VrRefusal(const std::optional<std::string>& cameraMode);

// True when |(videoW/videoH) / (swapW/swapH) - 1| > 0.005. False when any
// argument is zero.
bool AspectMismatch(double videoW, double videoH, double swapW, double swapH);

// Reads the four files, the HAGS registry value (the fallback) and the module
// flags; video_ini_path is set whether or not video.ini exists.
CompatInputs ReadCompatInputs(const std::wstring& gameDir, const std::wstring& docsAcDir);

// HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers HwSchMode == 2. The
// value can be absent while HAGS is on for the GPU (hybrid laptops), so it is
// only the fallback for the per-adapter state (adapter_caps.h).
bool ReadHagsEnabled();

// The DLSS 5 Bridge ReShade add-on in the process, by its file name:
// "dlss5-bridge.addon64", or "dlss5-dx11-bridge.addon64" (its name up to
// 1.1.0, which ReShade still loads); empty when neither is loaded.
std::string LoadedDlss5BridgeFile();

// "<Documents>\Assetto Corsa". The environment variable ACDLSSG_DOCS_DIR, when
// set, replaces the whole result (used by the test app).
std::wstring DocumentsAcDir();

}  // namespace acdb
