#include "panel_control.h"

#include <windows.h>

#include <cstdio>

namespace acdb {

ControlRequest ControlRequestFrom(const ControlLayout& layout) {
    ControlRequest r;
    r.counter = layout.requestCounter;
    r.desired.fgUserOn = layout.fgEnabled != 0;
    r.desired.flipHandedness = layout.cameraFlipHandedness != 0;
    r.desired.negateSide = layout.cameraNegateSide != 0;
    r.saveAsDefault = layout.saveAsDefault != 0;
    return r;
}

ControlDecision DecideControl(uint32_t lastApplied, const ControlRequest& request, const PanelSettings& current) {
    ControlDecision d;
    d.next = current;
    if (request.counter == lastApplied) return d;
    d.apply = true;
    d.next = request.desired;
    d.fgChanged = request.desired.fgUserOn != current.fgUserOn;
    d.flipChanged = request.desired.flipHandedness != current.flipHandedness;
    d.negateChanged = request.desired.negateSide != current.negateSide;
    d.save = request.saveAsDefault;
    return d;
}

IniKeyValues SavedDefaultKeys(const PanelSettings& settings) {
    return {
        {"start_with_fg", settings.fgUserOn ? "1" : "0"},
        {"camera_flip_handedness", settings.flipHandedness ? "1" : "0"},
        {"camera_negate_side", settings.negateSide ? "1" : "0"},
    };
}

std::string PanelReason(bool fgOn, bool userOn, const std::string& userOffReason, const std::string& gateReason) {
    if (fgOn) return "on";
    if (!userOn) return userOffReason;
    if (!gateReason.empty()) return gateReason;
    return "off";
}

bool FgPausedInSecond(uint32_t presents, uint32_t fgFrames, uint64_t generated, bool stateAnswered) {
    return stateAnswered && fgFrames > 0 && uint64_t{fgFrames} * 2 >= presents && generated == 0;
}

std::string HotkeyText(const Hotkey& hotkey) {
    std::string text;
    if (hotkey.ctrl) text += "Ctrl+";
    if (hotkey.shift) text += "Shift+";
    if (hotkey.alt) text += "Alt+";
    const unsigned vk = hotkey.vk;
    if (vk >= VK_F1 && vk <= VK_F24) {
        text += "F" + std::to_string(vk - VK_F1 + 1);
    } else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        text += static_cast<char>(vk);
    } else {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "VK 0x%02X", vk);
        text += buf;
    }
    return text;
}

}  // namespace acdb
