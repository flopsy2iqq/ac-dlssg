#include "config.h"

#include <windows.h>

namespace acdb {
namespace {

std::string Lower(std::string s) {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// VK code for "f1".."f24", "a".."z", "0".."9" (lower case); 0 if unknown.
unsigned KeyCode(const std::string& name) {
    if (name.size() == 1) {
        const char c = name[0];
        if (c >= 'a' && c <= 'z') return static_cast<unsigned>('A' + (c - 'a'));
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c);
        return 0;
    }
    if (name.size() >= 2 && name.size() <= 3 && name[0] == 'f') {
        unsigned n = 0;
        for (size_t i = 1; i < name.size(); ++i) {
            if (name[i] < '0' || name[i] > '9') return 0;
            n = n * 10 + static_cast<unsigned>(name[i] - '0');
        }
        if (name[1] == '0' || n < 1 || n > 24) return 0;
        return VK_F1 + (n - 1);
    }
    return 0;
}

void ParseBool(const std::optional<std::string>& raw, const char* key, bool* out, std::vector<std::string>* warnings) {
    if (!raw) return;
    const auto v = ToInt(raw);
    if (v && (*v == 0 || *v == 1)) {
        *out = *v == 1;
        return;
    }
    warnings->push_back(std::string(key) + ": invalid value '" + *raw + "' (expected 0 or 1); using default");
}

}  // namespace

bool ParseHotkey(const std::string& text, Hotkey* out) {
    if (!out) return false;
    Hotkey result;
    result.vk = 0;
    result.ctrl = result.shift = result.alt = false;

    const std::string s = Lower(text);
    size_t start = 0;
    while (true) {
        const size_t plus = s.find('+', start);
        const std::string token = s.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        if (plus == std::string::npos) {
            result.vk = KeyCode(token);
            if (result.vk == 0) return false;
            break;
        }
        if (token == "ctrl") {
            result.ctrl = true;
        } else if (token == "shift") {
            result.shift = true;
        } else if (token == "alt") {
            result.alt = true;
        } else {
            return false;
        }
        start = plus + 1;
    }
    *out = result;
    return true;
}

Config ParseConfig(const IniFile& ini) {
    Config c;
    const char* kSection = "bridge";

    ParseBool(ini.Get(kSection, "enabled"), "enabled", &c.enabled, &c.warnings);
    ParseBool(ini.Get(kSection, "start_with_fg"), "start_with_fg", &c.start_with_fg, &c.warnings);

    if (const auto hk = ini.Get(kSection, "hotkey")) {
        if (!ParseHotkey(*hk, &c.hotkey))
            c.warnings.push_back("hotkey: invalid value '" + *hk + "' (expected e.g. ctrl+f10); using default");
    }

    if (const auto raw = ini.Get(kSection, "max_frame_latency"); raw && !raw->empty()) {
        const auto v = ToInt(raw);
        if (v && *v >= 0 && *v <= 16) {
            c.max_frame_latency = static_cast<unsigned>(*v);
        } else {
            c.warnings.push_back("max_frame_latency: invalid value '" + *raw +
                                 "' (expected 1..16, or 0 for unset); using default");
        }
    }

    ParseBool(ini.Get(kSection, "camera_flip_handedness"), "camera_flip_handedness", &c.camera_flip_handedness,
              &c.warnings);
    ParseBool(ini.Get(kSection, "camera_negate_side"), "camera_negate_side", &c.camera_negate_side, &c.warnings);
    ParseBool(ini.Get(kSection, "proxy_without_fg"), "proxy_without_fg", &c.proxy_without_fg, &c.warnings);
    ParseBool(ini.Get(kSection, "tag_without_fg"), "tag_without_fg", &c.tag_without_fg, &c.warnings);
    ParseBool(ini.Get(kSection, "spoof_load_any"), "spoof_load_any", &c.spoof_load_any, &c.warnings);
    if (const auto raw = ini.Get(kSection, "fg_vram_headroom_mib"); raw && !raw->empty()) {
        const auto v = ToInt(raw);
        if (v && *v >= 0 && *v <= 65536) {
            c.fg_vram_headroom_mib = static_cast<unsigned>(*v);
        } else {
            c.warnings.push_back("fg_vram_headroom_mib: invalid value '" + *raw +
                                 "' (expected 0..65536 MiB); using default");
        }
    }

    if (const auto raw = ini.Get(kSection, "log_level")) {
        const std::string v = Lower(*raw);
        if (v == "error") {
            c.log_level = LogLevel::Error;
        } else if (v == "warn") {
            c.log_level = LogLevel::Warn;
        } else if (v == "info") {
            c.log_level = LogLevel::Info;
        } else if (v == "debug") {
            c.log_level = LogLevel::Debug;
        } else {
            c.warnings.push_back("log_level: invalid value '" + *raw +
                                 "' (expected error, warn, info or debug); using default");
        }
    }
    return c;
}

Config LoadConfig(const std::wstring& path) {
    const auto ini = IniFile::Load(path);
    if (!ini) return Config();
    return ParseConfig(*ini);
}

}  // namespace acdb
