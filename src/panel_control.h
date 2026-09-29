#pragma once
// The pure rules of the in-game panel (spec 6.9): when the presenter applies
// a request of the Lua app, what it changes and saves, and the texts the
// status record shows. Unit-tested in tests/test_panel.cpp.
#include <cstdint>
#include <string>

#include "config.h"
#include "ini.h"
#include "panel_status.h"

namespace acdb {

// What the panel can change: the DLSS-G switch the hotkey toggles too, and
// the two camera switches of spec 6.7.
struct PanelSettings {
    bool fgUserOn = false;
    bool flipHandedness = false;  // camera_flip_handedness
    bool negateSide = false;      // camera_negate_side
};

struct ControlRequest {
    uint32_t counter = 0;   // requestCounter
    PanelSettings desired;  // the state of every switch the app wants, not only the one clicked
    bool saveAsDefault = false;
};

// A stable record of the control section as a request (non-zero is on).
ControlRequest ControlRequestFrom(const ControlLayout& layout);

struct ControlDecision {
    bool apply = false;          // a request the bridge has not applied yet
    bool fgChanged = false;      // desired.fgUserOn differs from the current switch
    bool flipChanged = false;
    bool negateChanged = false;
    bool save = false;           // write SavedDefaultKeys(next) into ac-dlssg.ini
    PanelSettings next;          // the settings after the request (current when !apply)
};

// A request is new when its counter differs from the last one applied (any
// difference: the counter wraps from 0x7FFFFFFF to 1, and a restarted app
// might count again). A new request sets every switch to its desired value;
// only the switches that differ count as changed, so a request that repeats
// the bridge's state (the hotkey got there first) changes nothing. An old
// counter changes nothing and saves nothing.
ControlDecision DecideControl(uint32_t lastApplied, const ControlRequest& request, const PanelSettings& current);

// "Save as default": start_with_fg, camera_flip_handedness and
// camera_negate_side of the [bridge] section, as "0" or "1".
IniKeyValues SavedDefaultKeys(const PanelSettings& settings);

// The status record's reason: "on" while DLSS-G is on; else, while the
// user's switch is off, userOffReason (the switch is what the panel changes,
// whatever else would keep DLSS-G off too); else the gate's reason of the
// last frame; else "off".
std::string PanelReason(bool fgOn, bool userOn, const std::string& userOffReason, const std::string& gateReason);

// The hotkey as the panel shows it: "Ctrl+F10", "Ctrl+Shift+G", "Alt+5".
std::string HotkeyText(const Hotkey& hotkey);

}  // namespace acdb
