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
    std::vector<std::string> warnings;    // human-readable parse problems
};

// Accepts "ctrl+f10", "Ctrl+Shift+G", "alt+5", "f9". Key names: F1..F24,
// A..Z, 0..9. Modifiers: ctrl, shift, alt. Case-insensitive, no spaces.
bool ParseHotkey(const std::string& text, Hotkey* out);

Config ParseConfig(const IniFile& ini);
// Defaults when the file is missing.
Config LoadConfig(const std::wstring& path);

}  // namespace acdb
