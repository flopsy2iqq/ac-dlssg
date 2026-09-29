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
    bool userOn = false;       // start_with_fg, then the hotkey and the panel
    std::string userSource;    // what set userOn last: "start_with_fg", "hotkey" or "panel" (empty: unknown)
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
//  2. !userOn          UserOffReason(userSource)
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

// "off by the user (<source>)"; "off by the user (start_with_fg, the hotkey
// or the panel)" for an empty source.
std::string UserOffReason(const std::string& source);

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
    uint64_t needMib = 0;  // what had to fit, rounded up (0 when no check was made)
    uint64_t freeMib = 0;  // what was free, rounded down
    uint64_t estimateMib = 0;  // needMib's DLSS-G part (the estimate, or its growth), without the headroom
};

// fg_vram_headroom_mib=auto (spec 6.11): no headroom on a render adapter
// whose video memory budget is below 6144 MiB (a 4 GB card has none to
// spare), else 256 MiB.
unsigned AutoVramHeadroomMib(uint64_t budgetBytes);

// The headroom the guard adds to the estimate: AutoVramHeadroomMib with
// auto, else the configured number.
unsigned VramHeadroomMib(bool autoHeadroom, unsigned configuredMib, uint64_t budgetBytes);

// How much a 2X check may fall short with auto and DLSS-G still turns on,
// marked tight: max(128 MiB, a tenth of estimateMib).
uint64_t VramTightToleranceMib(uint64_t estimateMib);

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
    // The estimate of the resources DLSS-G already holds: eRetainResourcesWhenOff
    // keeps those of its last eOn, and the usage above contains them. 0 before
    // the first eOn.
    uint64_t heldBytes = 0;
};

// The guard's decision: CheckVideoMemory when the budget is known, else ok.
// A failed estimate query counts as an estimate of 0, so that only the
// headroom is checked instead of keeping DLSS-G off for good (review
// findings SL-6 and F2). With resources held, only the growth
// (estimate - heldBytes) plus the headroom has to fit, and no growth always
// fits: a lower or the same multiplier allocates nothing new.
VramCheck DecideVram(const VramInputs& in);

// Multi frame generation (spec 6.11): the check at the wanted multiplier
// decides; when it refuses and the wanted multiplier is above 2X, the 2X
// check (at2x, null when it was not made) may allow DLSS-G at 2X instead.
//
// fg_vram_headroom_mib=auto (autoHeadroom): when the 2X check (atWanted
// for 2X, else at2x) refuses by at most VramTightToleranceMib (needMib -
// freeMib), DLSS-G runs at 2X anyway: check ok, tight, and the note
// "video memory is tight (<free> MiB free for <need> MiB); frame generation
// on anyway (fg_vram_headroom_mib=auto)". A larger shortfall keeps DLSS-G
// off with the actionable reason "not enough video memory: frame generation
// needs <need> MiB, <free> MiB free; lower CSP texture quality, shadows or
// the render resolution", which is also the note. Only 2X is ever tight.
//
// A number (autoHeadroom false) decides as before auto existed: never tight,
// and the refusal keeps "video memory: need <x> MiB, free <y> MiB"; its note
// is the actionable text all the same, for the panel.
struct VramMultiplierDecision {
    VramCheck check;          // ok, or the refusal DLSS-G stays off with (2X's when both were made)
    unsigned multiplier = 2;  // the multiplier the guard allows
    // "video memory: <m>X needs <n> MiB, free <f> MiB; falling back to 2X",
    // empty without a fallback.
    std::string fallback;
    bool tight = false;  // auto: 2X fell short within the tolerance, DLSS-G on anyway
    std::string note;    // the panel's vramNote: the tight or the not-enough text; empty when it fits
};
VramMultiplierDecision DecideVramMultiplier(unsigned wanted, const VramCheck& atWanted, const VramCheck* at2x,
                                            bool autoHeadroom = false);

// When the guard runs: before DLSS-G is first enabled, whenever the wanted
// multiplier is not the one it last checked, and every 60 frames while it
// refuses; once a check for the wanted multiplier passed (a fallback to 2X
// included), not again until the multiplier changes.
class VramGuard {
public:
    static constexpr uint64_t kRecheckFrames = 60;
    bool CheckDue(uint64_t frame, unsigned wanted = 2) const;
    // result is the outcome for `wanted`, granted the multiplier it allows,
    // tight when it passed only as tight (DecideVramMultiplier). True for the
    // first result, whenever it changes between ok and refused, for a new
    // wanted multiplier, for a new granted one and when a pass becomes tight
    // or no longer is: the presenter logs only those at INFO.
    bool Record(uint64_t frame, const VramCheck& result, unsigned wanted = 2, unsigned granted = 2,
                bool tight = false);
    bool Passed() const { return passed_; }
    bool Tight() const { return tight_; }
    unsigned Wanted() const { return wanted_; }
    unsigned Granted() const { return granted_; }
    const std::string& Refusal() const { return refusal_; }

private:
    bool passed_ = false;
    bool checked_ = false;
    bool tight_ = false;
    uint64_t last_ = 0;
    unsigned wanted_ = 2;
    unsigned granted_ = 2;
    std::string refusal_;
};

// ---------------------------------------------------------------- multi frame generation

// fg_multiplier and SetFgMultiplier accept 2, 3 and 4.
bool ValidFgMultiplier(int multiplier);

// DLSSGOptions::numFramesToGenerate for a multiplier: multiplier - 1, and
// never below 1 (sl.dlss_g refuses 0).
uint32_t FramesToGenerate(unsigned multiplier);

struct FgMultiplierChoice {
    unsigned multiplier = 2;           // the multiplier DLSS-G is asked for
    uint32_t numFramesToGenerate = 1;  // FramesToGenerate(multiplier)
    // "<n>X requested, Streamline allows up to <m>X; using <m>X" when the
    // request was lowered, else empty.
    std::string note;
};

// Spec 6.8: the requested multiplier (anything but 2..4 counts as 2X)
// clamped to what Streamline allows, DLSSGState::numFramesToGenerateMax + 1
// (sl.dlss_g 2.14.1 sets it from NGX's DLSSG.MultiFrameCountMax, capped at
// 5, and to 1 when NGX does not report it); a max of 0 or 1 allows 2X only.
FgMultiplierChoice ChooseFgMultiplier(unsigned requested, uint32_t numFramesToGenerateMax);

// The status reason of a lowered multiplier: the clamp note and the video
// memory fallback, joined by "; " when both exist.
std::string FgMultiplierNote(const std::string& clampNote, const std::string& vramFallback);

// Spec 6.8: slDLSSGSetOptions only when something Streamline has would
// change. known: options were sent before, with lastOn and lastFrames
// (numFramesToGenerate). Always when nothing was sent; for a new mode; while
// on (and staying on), for new size hints (sameHints false) or a new
// numFramesToGenerate. While off nothing else is sent: the next eOn carries
// the hints and the count.
bool DlssgOptionsDue(bool known, bool lastOn, uint32_t lastFrames, bool sameHints, bool on, uint32_t frames);

// The multiplier on the presenting thread (spec 6.8): the request
// (fg_multiplier, then SetFgMultiplier) and what Streamline allows. The
// presenter queries slDLSSGGetState before options carry a count while
// QueryDue() holds: before the first options, and again after every new
// request.
class FgMultiplierTracker {
public:
    explicit FgMultiplierTracker(unsigned requested = 2);  // anything but 2..4 counts as 2
    // A new request; false (nothing changes) when it is not 2..4 or is the
    // current one. True makes the max due again and lets the note of this
    // request be logged once more.
    bool Request(unsigned multiplier);
    unsigned Requested() const { return requested_; }
    bool QueryDue() const { return query_due_; }
    // Streamline's DLSSGState::numFramesToGenerateMax (0 when the query
    // failed). Returns the clamp note when it is to be logged: non-empty and
    // not yet logged for this request.
    std::string OnFramesMax(uint32_t numFramesToGenerateMax);
    bool MaxKnown() const { return max_known_; }
    uint32_t FramesMax() const { return max_; }
    // ChooseFgMultiplier(request, last known max); the request itself before
    // Streamline answered once. The video memory guard is asked about it.
    unsigned Wanted() const { return choice_.multiplier; }
    const std::string& ClampNote() const { return choice_.note; }

private:
    void Choose();
    unsigned requested_ = 2;
    bool query_due_ = true;
    bool max_known_ = false;
    uint32_t max_ = 0;
    FgMultiplierChoice choice_;
    std::string logged_;
};

// The multiplier the DLSS-G options carry: the video memory guard's grant
// (the wanted multiplier, or 2X after a fallback) when its last passed check
// was for the wanted multiplier, else the wanted multiplier itself.
unsigned UsedFgMultiplier(unsigned wanted, bool guardPassed, unsigned guardWanted, unsigned guardGranted);

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
