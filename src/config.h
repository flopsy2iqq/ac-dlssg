#pragma once
// ac-dlssg.ini, section [bridge] (spec 6.10). Missing keys keep their
// defaults; an invalid value keeps the default and is reported in warnings.
#include <string>
#include <vector>

#include "ini.h"
#include "log.h"

namespace acdb {

struct Hotkey {
    unsigned vk = 0x79;  // VK_F10
    bool ctrl = true;
    bool shift = false;
    bool alt = false;
};

struct Config {
    bool enabled = true;           // enabled=1
    bool start_with_fg = true;     // start_with_fg=1
    Hotkey hotkey;                 // hotkey=ctrl+f10
    unsigned max_frame_latency = 0;  // 0 = unset; 1..16 overrides CSP's value
    LogLevel log_level = LogLevel::Info;  // log_level=error|warn|info|debug
    // M3. Camera A/B switches of spec 6.7 (FrameConstantsOptions).
    bool camera_flip_handedness = false;  // camera_flip_handedness=0|1 (flipHandedness)
    bool camera_negate_side = false;      // camera_negate_side=0|1 (negateSide)
    // When Streamline says DLSS-G is unsupported: 0 fails the presenter, so the
    // chain passes through (spec criterion 5); 1 keeps the M2 behaviour of
    // proxying without DLSS-G.
    bool proxy_without_fg = false;  // proxy_without_fg=0|1
    // 1 sets tags and constants every frame with a capture and a fresh camera
    // even while DLSS-G is unsupported or off (test app).
    bool tag_without_fg = false;  // tag_without_fg=0|1
    // Video memory guard (spec 6.11): free budget must cover the DLSS-G
    // estimate plus this much. 0..65536. 0 since 2026-09-29: on the 4 GB
    // RTX 3050 Ti laptop 346 MiB were free for a 283 MiB estimate, and 512
    // kept DLSS-G off.
    unsigned fg_vram_headroom_mib = 0;  // fg_vram_headroom_mib=<MiB>
    // When the process bound VERSION.dll to System32 (Windows 11 25H2), the
    // bootstrap loads the game folder's version.dll itself only if it is the
    // pinned dlssg_for_sm86 release; 1 loads any version.dll there.
    bool spoof_load_any = false;  // spoof_load_any=0|1
    // Multi frame generation (spec 3, 6.8): the multiplier asked of DLSS-G,
    // numFramesToGenerate = fg_multiplier - 1. Streamline's
    // numFramesToGenerateMax and the video memory guard may lower it
    // (ChooseFgMultiplier, DecideVramMultiplier); SetFgMultiplier changes it
    // at runtime.
    unsigned fg_multiplier = 2;  // fg_multiplier=2|3|4
    std::vector<std::string> warnings;    // human-readable parse problems
};

// Accepts "ctrl+f10", "Ctrl+Shift+G", "alt+5", "f9". Key names: F1..F24,
// A..Z, 0..9. Modifiers: ctrl, shift, alt. Case-insensitive, no spaces.
bool ParseHotkey(const std::string& text, Hotkey* out);

Config ParseConfig(const IniFile& ini);
// Defaults when the file is missing.
Config LoadConfig(const std::wstring& path);

}  // namespace acdb
