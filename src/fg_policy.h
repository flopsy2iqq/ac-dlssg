#pragma once
// The pure decisions of DLSS-G's per-frame flow (spec 6.4, 6.6, 6.8, 6.9,
// 6.10, 6.11, 7 step 5.4, 9). No D3D, no Streamline calls: the presenter and
// the capture coordinator feed them what they observed. Unit-tested in
// tests/test_fg_policy.cpp.
#include <cstdint>
#include <string>
#include <unordered_map>

#include "config.h"

namespace acdb {

// ---------------------------------------------------------------- the gate

// What the presenter knows about one frame when it decides whether the frame
// is presented with DLSS-G (spec 7 step 5.4).
struct FgGateInputs {
    bool userOn = false;       // start_with_fg, then the hotkey
    bool supported = false;    // slIsFeatureSupported and the DLSS-G functions resolved
    std::string stateFailure;  // non-empty: slDLSSGGetState reported this failure status
    bool stalled = false;      // stalled mode (spec 6.4)
    bool windowMinimized = false;  // IsIconic(game window): the NVIDIA DLSS-G guide asks for
                                   // eOff around a minimize (review finding F6)
    std::string aspectRefusal;  // non-empty: RuntimeAspectRefusal refused
    bool captured = false;      // a capture paired with this Present exists
    std::string captureReason;  // why there is none, or why the frame is forced off
    bool forcedOff = false;     // capture slot recreated or DLSS feature re-created this frame
    bool mvScaleMissing = false;  // NGX reported no MV.Scale (0)
    bool cameraOk = false;        // CameraChannel::Read returned Ok
    std::string cameraReason;     // why not
    bool cameraFresh = false;     // CameraLatch said fresh (spec 6.6)
    uint32_t cameraFlags = 0;     // CameraFlags of the snapshot
    bool constantsOk = false;     // BuildFrameConstants returned true
    std::string constantsWhy;     // its why otherwise
    std::string vramRefusal;      // non-empty: the video memory guard keeps DLSS-G off
};

struct FgGateResult {
    bool on = false;
    std::string reason;     // empty when on
    // True for reasons that can change from one frame to the next (logged as
    // a throttled "fg: frame without DLSS-G" WARN); false for states that
    // last (the user's switch, support, a failure status, the aspect test,
    // the video memory guard), which only the mode-change line reports.
    bool perFrame = false;
};

// The first failing condition, in this order:
//  1. !supported       "not supported on this adapter"
//  2. !userOn          "off by the user (start_with_fg or the hotkey)"
//  3. stateFailure     "DLSS-G status <stateFailure>"
//  4. stalled          "D3D12 stall"                            (per frame)
//  5. windowMinimized  "game window minimized"                  (per frame)
//  6. aspectRefusal    aspectRefusal
//  7. !captured        captureReason, or "no DLSS evaluate this frame" (per frame)
//  8. forcedOff        captureReason                            (per frame)
//  9. mvScaleMissing   "MV scale missing"                       (per frame)
// 10. !cameraOk        "camera: <cameraReason>"                 (per frame)
// 11. !cameraFresh     "camera not fresh"                       (per frame)
// 12. kCamPaused       "game paused"                            (per frame)
// 13. kCamMainMenu     "in-game menu open"                      (per frame)
// 14. kCamVR/kCamTriple "VR or triple-screen mode"              (per frame)
// 15. !constantsOk     "frame constants refused: <constantsWhy>" (per frame)
// 16. vramRefusal      vramRefusal
// kCamReplay and kCamJumped never turn DLSS-G off (a jump only sets reset).
FgGateResult DecideFg(const FgGateInputs& in);

// Whether this frame gets depth and motion-vector tags and constants: always
// when DLSS-G is on; with tag_without_fg also while it is off or unsupported,
// when a capture, a fresh camera and valid constants exist.
bool ShouldTag(const FgGateInputs& in, const FgGateResult& gate, bool tagWithoutFg);

// With tag_without_fg, when a lasting reason keeps DLSS-G off (gate.perFrame
// false), the first per-frame condition of DecideFg that fails (4, 5 and 7-15
// above), as if DLSS-G were supported, wanted and allowed: why this frame's
// inputs are incomplete. The presenter logs it as the throttled "fg: frame
// without DLSS-G" WARN, so a run where DLSS-G cannot turn on still names the
// frames that could not be tagged. Empty otherwise: without tag_without_fg,
// while DLSS-G is on, when the gate's own reason is already per frame, or
// when every per-frame condition holds.
std::string TagPathReason(const FgGateInputs& in, const FgGateResult& gate, bool tagWithoutFg);

// ---------------------------------------------------------------- capture slots

enum class SlotAction {
    Copy,      // the slot matches the sources: copy into it
    Recreate,  // release and recreate it for the sources, then copy
    Skip,      // the GPU may still read it: no capture this frame, retry later
};

// Spec 6.4 "When recreated": a slot whose source descriptions changed, or that
// a counted CreateFeature marked (forceRecreate), is recreated only after the
// progress fence has passed lastUse, the value signalled after the last frame
// that tagged it. An empty slot needs no wait. The render thread never waits
// for it: until then the frame has no capture.
SlotAction DecideSlot(bool matches, bool forceRecreate, bool hasTextures, uint64_t progress, uint64_t lastUse);

// ---------------------------------------------------------------- video memory

struct VramCheck {
    bool ok = true;
    std::string reason;  // "video memory: need <x> MiB, free <y> MiB" when refused
};

// Spec 6.11: DLSS-G stays off when budget - usage < estimate + headroom.
// need is rounded up and free rounded down to MiB; usage above the budget
// counts as nothing free.
VramCheck CheckVideoMemory(uint64_t budgetBytes, uint64_t usageBytes, uint64_t estimateBytes, unsigned headroomMib);

// What the presenter learned for one check of the guard.
struct VramInputs {
    bool estimateOk = false;      // slDLSSGGetState(eRequestVRAMEstimate) returned eOk
    uint64_t estimateBytes = 0;   // its estimatedVRAMUsageInBytes
    bool budgetKnown = false;     // IDXGIAdapter3::QueryVideoMemoryInfo succeeded
    uint64_t budgetBytes = 0;
    uint64_t usageBytes = 0;
    unsigned headroomMib = 0;     // fg_vram_headroom_mib
};

// The guard's decision: CheckVideoMemory when the budget is known, else ok.
// A failed estimate query counts as an estimate of 0, so that only the
// headroom is checked instead of keeping DLSS-G off for good (review
// findings SL-6 and F2).
VramCheck DecideVram(const VramInputs& in);

// When the guard runs: before DLSS-G is first enabled, then every 60 frames
// while it refuses; once it passed, never again.
class VramGuard {
public:
    static constexpr uint64_t kRecheckFrames = 60;
    bool CheckDue(uint64_t frame) const;
    // True for the first result and whenever it changes between ok and
    // refused: the presenter logs only those at INFO.
    bool Record(uint64_t frame, const VramCheck& result);
    bool Passed() const { return passed_; }
    const std::string& Refusal() const { return refusal_; }

private:
    bool passed_ = false;
    bool checked_ = false;
    uint64_t last_ = 0;
    std::string refusal_;
};

// ---------------------------------------------------------------- hotkey

// The chord is down when its key is down and exactly the configured
// modifiers are (Ctrl+Shift+F10 is not Ctrl+F10).
bool HotkeyChordDown(const Hotkey& hotkey, bool keyDown, bool ctrlDown, bool shiftDown, bool altDown);

// One press gives one toggle, however long it is held.
class KeyEdge {
public:
    bool Pressed(bool down) {
        const bool edge = down && !was_;
        was_ = down;
        return edge;
    }

private:
    bool was_ = false;
};

// ---------------------------------------------------------------- log throttling

// At most one line per interval per distinct reason text. Bounded: when more
// than 256 reasons are remembered, the oldest window is forgotten.
class ReasonThrottle {
public:
    explicit ReasonThrottle(uint64_t intervalMs = 10000) : interval_(intervalMs) {}
    bool ShouldLog(const std::string& reason, uint64_t nowMs);
    size_t Size() const { return last_.size(); }

private:
    uint64_t interval_;
    std::unordered_map<std::string, uint64_t> last_;
};

// ---------------------------------------------------------------- texts

// Presenter creation failure when Streamline refuses DLSS-G and
// proxy_without_fg=0: "DLSS-G is not supported on this adapter (<why>)", plus
// "; RTX 30 needs dlssg_for_sm86 (version.dll) in the game folder" for an SM86
// GPU without the spoof.
std::string DlssgUnsupportedMessage(const std::string& why, bool sm86, bool spoofLoaded);

// sl::DLSSGStatus bits as names joined by '|' ("eOk" for 0); unknown bits as hex.
std::string DlssgStatusText(uint32_t status);

// Spec 6.10 "Runtime-only switches": with ALLOW_STRETCHING off, a DLSS output
// size (NGX OutWidth/OutHeight) whose aspect differs from the swap chain's by
// more than 0.5% means CSP letterboxes: DLSS-G off. Empty when allowed or
// when a size is unknown.
std::string RuntimeAspectRefusal(bool allowStretching, uint32_t outW, uint32_t outH, uint32_t chainW,
                                 uint32_t chainH);

// ---------------------------------------------------------------- DLSS-G state

// Frames DLSS-G added at one Present, from slDLSSGGetState called right
// after it. sl_dlss_g.h describes numFramesActuallyPresented as the frames
// presented since the last call; the DLSS-G programming guide (13.0) as the
// frames presented per application frame (2 when one frame is generated).
// Read after every Present, both readings give the same count (review
// finding SL-1). 0 and 1 mean none was generated.
uint32_t GeneratedFramesAtPresent(uint32_t numFramesActuallyPresented);

// When the presenter acts on slDLSSGGetState's status (spec 6.8 "Status":
// every 60 frames while DLSS-G is on). Frame() counts every frame, whatever
// the DLSS-G mode, so that DLSS-G going off for single frames cannot put the
// status check off for good (review findings SL-2 and F4); it is due on the
// first frame DLSS-G is on once 60 frames have passed since the last check.
class StatusPollClock {
public:
    static constexpr uint32_t kFrames = 60;
    void Frame();
    bool Due(bool dlssgOn) const;
    void Polled();

private:
    uint32_t since_ = 0;
};

}  // namespace acdb
