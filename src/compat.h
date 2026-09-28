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
    // System state.
    // ReadCompatInputs: the registry value (HwSchMode == 2). Per swap chain
    // FactoryHook replaces it with the render adapter's D3DKMT state and uses
    // the registry value only when D3DKMT cannot answer (EvaluateChainCompat).
    bool hags_on = false;
    bool dlss5_bridge_loaded = false;   // GetModuleHandleW(L"dlss5-bridge.addon64")
    bool renodx_dlss5_loaded = false;   // GetModuleHandleW(L"renodx-dlss5.addon64")
};

struct CompatResult {
    bool ok = true;
    std::string reason;  // empty when ok; otherwise the first failing rule, in English
};

// Rules in this order; the first that fails is reported:
//  1. old_swapchain == 1               -> "OLD_SWAPCHAIN=1 is not supported"
//  2. exclusive_fullscreen == 1        -> "EXCLUSIVE_FULLSCREEN=1 is not supported"
//  3. hdr_enabled == 1                 -> "HDR output is not supported"
//  4. fsr_active != 1 or fsr_old_implementation != 3 (missing counts as 0)
//                                      -> "CSP upscaler is not DLSS (need [FSR] ACTIVE=1, OLD_IMPLEMENTATION=3)"
//  5. aasamples > 1                    -> "MSAA (AASAMPLES>1) is not supported"
//  6. camera_mode present and not DEFAULT (case-insensitive)
//                                      -> "camera mode <MODE> (VR or triple screen) is not supported"
//  7. allow_stretching != 1 (missing counts as 0) and both video sizes known
//     and AspectMismatch(...)          -> "video.ini aspect ratio differs from the window; letterboxed output is not supported in v1"
//  8. dlss5_bridge_loaded              -> "dlss5-bridge.addon64 is loaded"
//  9. renodx_dlss5_loaded              -> "renodx-dlss5.addon64 is loaded"
// 10. !hags_on                         -> "hardware-accelerated GPU scheduling is off"
CompatResult EvaluateCompat(const CompatInputs& in, unsigned swapWidth, unsigned swapHeight);

// True when |(videoW/videoH) / (swapW/swapH) - 1| > 0.005. False when any
// argument is zero.
bool AspectMismatch(double videoW, double videoH, double swapW, double swapH);

// Reads the four files, the HAGS registry value (the fallback) and the module flags.
CompatInputs ReadCompatInputs(const std::wstring& gameDir, const std::wstring& docsAcDir);

// HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers HwSchMode == 2. The
// value can be absent while HAGS is on for the GPU (hybrid laptops), so it is
// only the fallback for the per-adapter state (adapter_caps.h).
bool ReadHagsEnabled();

// "<Documents>\Assetto Corsa". The environment variable ACDLSSG_DOCS_DIR, when
// set, replaces the whole result (used by the test app).
std::wstring DocumentsAcDir();

}  // namespace acdb
