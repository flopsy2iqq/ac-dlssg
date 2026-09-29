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

// What the panel can change: the DLSS-G switch the hotkey toggles too, the
// two camera switches of spec 6.7 and the multiplier (2X/3X/4X buttons).
struct PanelSettings {
    bool fgUserOn = false;
    bool flipHandedness = false;  // camera_flip_handedness
    bool negateSide = false;      // camera_negate_side
    unsigned multiplier = 2;      // the multiplier asked for (FgMultiplier().requested)
};

struct ControlRequest {
    uint32_t counter = 0;   // requestCounter
    PanelSettings desired;  // the state of every switch the app wants, not only the one clicked (not the multiplier)
    uint32_t desiredMultiplier = 0;  // 2..4, or 0 (anything else too): keep the current multiplier
    bool saveAsDefault = false;
};

// A stable record of the control section as a request (non-zero is on).
ControlRequest ControlRequestFrom(const ControlLayout& layout);

struct ControlDecision {
    bool apply = false;          // a request the bridge has not applied yet
    bool fgChanged = false;      // desired.fgUserOn differs from the current switch
    bool flipChanged = false;
    bool negateChanged = false;
    bool multiplierChanged = false;  // a valid desiredMultiplier that differs from the current one
    bool save = false;           // write SavedDefaultKeys(next) into ac-dlssg.ini
    PanelSettings next;          // the settings after the request (current when !apply)
};

// A request is new when its counter differs from the last one applied (any
// difference: the counter wraps from 0x7FFFFFFF to 1, and a restarted app
// might count again). A new request sets every switch to its desired value,
// and the multiplier to desiredMultiplier when that is 2..4; only what
// differs counts as changed, so a request that repeats the bridge's state
// (the hotkey got there first) changes nothing. An old counter changes
// nothing and saves nothing.
ControlDecision DecideControl(uint32_t lastApplied, const ControlRequest& request, const PanelSettings& current);

// "Save as default": start_with_fg, camera_flip_handedness and
// camera_negate_side of the [bridge] section, as "0" or "1", and
// fg_multiplier as "2", "3" or "4".
IniKeyValues SavedDefaultKeys(const PanelSettings& settings);

// StatusLayout::fgMultMax: 0 while Streamline's numFramesToGenerateMax is
// not known (the window keeps every button usable), else the highest
// multiplier it allows (ChooseFgMultiplier): 2 for a max of 0 or 1, 3 for
// 2, 4 from 3 on.
uint32_t PanelMultiplierMax(bool maxKnown, uint32_t numFramesToGenerateMax);

// StatusLayout::restartNote after a request was applied: a successful
// "Save as default" (saved and saveOk) sets "Saved. start_with_fg and the
// other saved switches apply the next time the game starts.", and the note
// stays for the rest of the session; otherwise current is kept. The panel's
// live switches (DLSS-G, the multiplier and the camera switches) apply at
// once and need no restart.
std::string PanelRestartNote(const std::string& current, bool saved, bool saveOk);

// The status record's reason: "on" while DLSS-G is on; else, while the
// user's switch is off, userOffReason (the switch is what the panel changes,
// whatever else would keep DLSS-G off too); else the gate's reason of the
// last frame; else "off".
std::string PanelReason(bool fgOn, bool userOn, const std::string& userOffReason, const std::string& gateReason);

// PanelReason's gateReason: the gate's reason of the last decided frame,
// except during a D3D12 stall (spec 6.4), which switches DLSS-G off outside
// the gate and skips every frame's decision: then "D3D12 stall", unless the
// adapter cannot run DLSS-G at all (supported false keeps that reason).
std::string PanelGateReason(bool supported, bool stalled, const std::string& lastFrameReason);

// StatusLayout::fgPaused for one statistics second: DLSS-G was on for at
// least half of its Presents (so not the second of an enable), slDLSSGGetState
// has answered (stateAnswered), and Streamline generated no frame. Streamline
// pauses interpolation while the game window is not focused, with the mode
// still eOn ("DLSS-G disabled: window not focused" in sl.log).
bool FgPausedInSecond(uint32_t presents, uint32_t fgFrames, uint64_t generated, bool stateAnswered);

// The hotkey as the panel shows it: "Ctrl+F10", "Ctrl+Shift+G", "Alt+5".
std::string HotkeyText(const Hotkey& hotkey);

}  // namespace acdb
