// The pure DLSS-G decisions of M3 (fg_policy.h): the per-frame gate and its
// reason strings, when to tag, the capture-slot recreate rule, the video
// memory guard, hotkey edge detection, log throttling and the texts for an
// unsupported adapter, a failure status and the runtime aspect refusal.
#include <cstdint>
#include <string>

#include "camera_layout.h"
#include "config.h"
#include "fg_policy.h"
#include "test_framework.h"

using namespace acdb;

namespace {

// Every input that allows DLSS-G.
FgGateInputs AllowAll() {
    FgGateInputs in;
    in.userOn = true;
    in.supported = true;
    in.captured = true;
    in.cameraOk = true;
    in.cameraFresh = true;
    in.constantsOk = true;
    return in;
}

bool Contains(const std::string& s, const char* piece) { return s.find(piece) != std::string::npos; }

}  // namespace

// ---------------------------------------------------------------- the gate

TEST(FgGate_OnWhenEverythingHolds) {
    const FgGateResult r = DecideFg(AllowAll());
    CHECK(r.on);
    CHECK(r.reason.empty());
}

TEST(FgGate_EachConditionTurnsItOffWithItsReason) {
    struct Case {
        const char* name;
        void (*apply)(FgGateInputs*);
        const char* piece;  // part of the reason
        bool perFrame;      // logged as a throttled per-frame WARN
    } cases[] = {
        {"user off", [](FgGateInputs* in) { in->userOn = false; }, "off by the user", false},
        {"unsupported", [](FgGateInputs* in) { in->supported = false; }, "not supported", false},
        {"status", [](FgGateInputs* in) { in->stateFailure = "eFailResolutionTooLow"; }, "status eFailResolutionTooLow",
         false},
        {"stall", [](FgGateInputs* in) { in->stalled = true; }, "D3D12 stall", true},
        {"aspect", [](FgGateInputs* in) { in->aspectRefusal = "aspect: x"; }, "aspect: x", false},
        {"no evaluate", [](FgGateInputs* in) { in->captured = false; }, "no DLSS evaluate this frame", true},
        {"no capture",
         [](FgGateInputs* in) {
             in->captured = false;
             in->captureReason = "no capture: null depth";
         },
         "no capture: null depth", true},
        {"forced off",
         [](FgGateInputs* in) {
             in->forcedOff = true;
             in->captureReason = "capture slot 1 recreated";
         },
         "capture slot 1 recreated", true},
        {"mv scale", [](FgGateInputs* in) { in->mvScaleMissing = true; }, "MV scale missing", true},
        {"camera missing",
         [](FgGateInputs* in) {
             in->cameraOk = false;
             in->cameraReason = "not written";
         },
         "camera: not written", true},
        {"camera stale", [](FgGateInputs* in) { in->cameraFresh = false; }, "camera not fresh", true},
        {"paused", [](FgGateInputs* in) { in->cameraFlags = kCamPaused; }, "game paused", true},
        {"menu", [](FgGateInputs* in) { in->cameraFlags = kCamMainMenu; }, "menu open", true},
        {"vr", [](FgGateInputs* in) { in->cameraFlags = kCamVR; }, "VR or triple-screen", true},
        {"triple", [](FgGateInputs* in) { in->cameraFlags = kCamTriple; }, "VR or triple-screen", true},
        {"constants",
         [](FgGateInputs* in) {
             in->constantsOk = false;
             in->constantsWhy = "render size";
         },
         "frame constants refused: render size", true},
        {"vram", [](FgGateInputs* in) { in->vramRefusal = "video memory: need 900 MiB, free 800 MiB"; },
         "video memory: need 900 MiB, free 800 MiB", false},
    };
    for (const auto& c : cases) {
        FgGateInputs in = AllowAll();
        c.apply(&in);
        const FgGateResult r = DecideFg(in);
        if (r.on || !Contains(r.reason, c.piece) || r.perFrame != c.perFrame)
            std::printf("  %s: on %d reason '%s' perFrame %d\n", c.name, r.on, r.reason.c_str(), r.perFrame);
        CHECK(!r.on);
        CHECK(Contains(r.reason, c.piece));
        CHECK_EQ(r.perFrame, c.perFrame);
    }
}

TEST(FgGate_ReplayAndCameraJumpDoNotTurnItOff) {
    FgGateInputs in = AllowAll();
    in.cameraFlags = kCamReplay | kCamJumped;
    CHECK(DecideFg(in).on);
}

TEST(FgGate_FirstFailingConditionIsReported) {
    // Support beats everything (the user's switch cannot fix it); the user's
    // switch beats per-frame reasons; a missing capture beats the camera.
    FgGateInputs in;  // everything false
    CHECK(Contains(DecideFg(in).reason, "not supported"));
    in.supported = true;
    CHECK(Contains(DecideFg(in).reason, "off by the user"));
    in.userOn = true;
    CHECK(Contains(DecideFg(in).reason, "no DLSS evaluate"));
    in.captured = true;
    in.mvScaleMissing = true;
    CHECK(Contains(DecideFg(in).reason, "MV scale missing"));
    in.mvScaleMissing = false;
    CHECK(Contains(DecideFg(in).reason, "camera"));
}

TEST(FgGate_TagWithoutFg) {
    // DLSS-G on always tags.
    FgGateInputs in = AllowAll();
    CHECK(ShouldTag(in, DecideFg(in), false));
    // Off (unsupported here): tags only with tag_without_fg and complete inputs.
    in.supported = false;
    const FgGateResult off = DecideFg(in);
    CHECK(!off.on);
    CHECK(!ShouldTag(in, off, false));
    CHECK(ShouldTag(in, off, true));
    in.cameraFresh = false;
    CHECK(!ShouldTag(in, DecideFg(in), true));
    in.cameraFresh = true;
    in.captured = false;
    CHECK(!ShouldTag(in, DecideFg(in), true));
    in.captured = true;
    in.constantsOk = false;
    CHECK(!ShouldTag(in, DecideFg(in), true));
    // A paused camera still tags with tag_without_fg (only the inputs matter).
    in.constantsOk = true;
    in.cameraFlags = kCamPaused;
    CHECK(ShouldTag(in, DecideFg(in), true));
}

// With tag_without_fg a lasting reason (support, the user's switch, a failure
// status, the aspect test, the video memory guard) keeps DLSS-G off, and the
// per-frame reason behind it is what the throttled WARN names: why this
// frame's inputs are incomplete (the test app's fg-no-camera).
TEST(FgGate_TagPathReasonNamesThePerFrameReasonBehindALastingOne) {
    FgGateInputs in = AllowAll();
    in.supported = false;
    in.cameraOk = false;
    in.cameraReason = "not written";
    const FgGateResult gate = DecideFg(in);
    CHECK(!gate.on);
    CHECK(!gate.perFrame);
    CHECK_EQ(TagPathReason(in, gate, true), std::string("camera: not written"));
    // Without tag_without_fg the lasting reason is all there is.
    CHECK(TagPathReason(in, gate, false).empty());
    // Every lasting reason at once still leaves the per-frame one.
    in.userOn = false;
    in.stateFailure = "eFailResolutionTooLow";
    in.aspectRefusal = "aspect: x";
    in.vramRefusal = "video memory: x";
    CHECK_EQ(TagPathReason(in, DecideFg(in), true), std::string("camera: not written"));
    // A stall is a per-frame reason too, and comes first.
    in.stalled = true;
    CHECK_EQ(TagPathReason(in, DecideFg(in), true), std::string("D3D12 stall"));
    // Complete inputs: nothing to report.
    in.stalled = false;
    in.cameraOk = true;
    CHECK(TagPathReason(in, DecideFg(in), true).empty());
    // When the gate's own reason is per frame, it is already the WARN.
    FgGateInputs pf = AllowAll();
    pf.cameraFresh = false;
    const FgGateResult stale = DecideFg(pf);
    CHECK(stale.perFrame);
    CHECK(TagPathReason(pf, stale, true).empty());
    // DLSS-G on: nothing to report.
    CHECK(TagPathReason(AllowAll(), DecideFg(AllowAll()), true).empty());
}

// ---------------------------------------------------------------- capture slots

TEST(SlotRule_CopyWhenTheSourcesMatch) {
    CHECK(DecideSlot(true, false, true, 0, 100) == SlotAction::Copy);
    CHECK(DecideSlot(true, false, true, 100, 100) == SlotAction::Copy);
}

TEST(SlotRule_RecreateOnlyAfterProgressPassedTheLastTag) {
    // Sources changed: recreate once the GPU is done with the slot.
    CHECK(DecideSlot(false, false, true, 100, 100) == SlotAction::Recreate);
    CHECK(DecideSlot(false, false, true, 101, 100) == SlotAction::Recreate);
    CHECK(DecideSlot(false, false, true, 99, 100) == SlotAction::Skip);
    // A forced recreate (a new DLSS feature) follows the same rule.
    CHECK(DecideSlot(true, true, true, 99, 100) == SlotAction::Skip);
    CHECK(DecideSlot(true, true, true, 100, 100) == SlotAction::Recreate);
    // An empty slot was never tagged: no wait.
    CHECK(DecideSlot(false, false, false, 0, 100) == SlotAction::Recreate);
    CHECK(DecideSlot(false, true, false, 0, 100) == SlotAction::Recreate);
}

// ---------------------------------------------------------------- video memory

TEST(VramGuard_EnoughFreeMemoryPasses) {
    constexpr uint64_t MiB = 1024ull * 1024ull;
    const VramCheck ok = CheckVideoMemory(8000 * MiB, 6000 * MiB, 1000 * MiB, 512);
    CHECK(ok.ok);
    CHECK(ok.reason.empty());
    // Exactly enough passes too.
    CHECK(CheckVideoMemory(8000 * MiB, 6488 * MiB, 1000 * MiB, 512).ok);
}

TEST(VramGuard_TooLittleFreeMemoryRefusesWithNumbers) {
    constexpr uint64_t MiB = 1024ull * 1024ull;
    const VramCheck r = CheckVideoMemory(4000 * MiB, 3500 * MiB, 300 * MiB, 512);
    CHECK(!r.ok);
    CHECK_EQ(r.reason, std::string("video memory: need 812 MiB, free 500 MiB"));
    // Usage above the budget counts as nothing free.
    const VramCheck over = CheckVideoMemory(4000 * MiB, 4100 * MiB, 0, 0);
    CHECK(over.ok);  // needs 0
    const VramCheck over2 = CheckVideoMemory(4000 * MiB, 4100 * MiB, 1, 0);
    CHECK(!over2.ok);
    CHECK_EQ(over2.reason, std::string("video memory: need 1 MiB, free 0 MiB"));
}

TEST(VramGuard_ChecksBeforeFirstEnableAndEvery60FramesWhileBlocked) {
    VramGuard g;
    CHECK(!g.Passed());
    CHECK(g.CheckDue(10));
    g.Record(10, VramCheck{false, "video memory: need 1 MiB, free 0 MiB"});
    CHECK(!g.Passed());
    CHECK_EQ(g.Refusal(), std::string("video memory: need 1 MiB, free 0 MiB"));
    CHECK(!g.CheckDue(11));
    CHECK(!g.CheckDue(69));
    CHECK(g.CheckDue(70));
    g.Record(70, VramCheck{true, ""});
    CHECK(g.Passed());
    CHECK(g.Refusal().empty());
    CHECK(!g.CheckDue(71));
    CHECK(!g.CheckDue(1000));
}

// ---------------------------------------------------------------- hotkey

TEST(Hotkey_ChordNeedsExactlyTheConfiguredModifiers) {
    Hotkey hk;  // ctrl+f10
    CHECK(HotkeyChordDown(hk, true, true, false, false));
    CHECK(!HotkeyChordDown(hk, true, false, false, false));  // no ctrl
    CHECK(!HotkeyChordDown(hk, false, true, false, false));  // no F10
    CHECK(!HotkeyChordDown(hk, true, true, true, false));    // extra shift
    CHECK(!HotkeyChordDown(hk, true, true, false, true));    // extra alt
    Hotkey bare;
    bare.ctrl = false;
    CHECK(HotkeyChordDown(bare, true, false, false, false));
    CHECK(!HotkeyChordDown(bare, true, true, false, false));
}

TEST(Hotkey_EdgeFiresOncePerPress) {
    KeyEdge e;
    CHECK(!e.Pressed(false));
    CHECK(e.Pressed(true));
    CHECK(!e.Pressed(true));  // held
    CHECK(!e.Pressed(true));
    CHECK(!e.Pressed(false));
    CHECK(e.Pressed(true));  // pressed again
}

// ---------------------------------------------------------------- throttling

TEST(ReasonThrottle_OncePerIntervalPerReason) {
    ReasonThrottle t(10000);
    CHECK(t.ShouldLog("a", 1000));
    CHECK(!t.ShouldLog("a", 2000));
    CHECK(t.ShouldLog("b", 2000));  // another reason is independent
    CHECK(!t.ShouldLog("a", 10999));
    CHECK(t.ShouldLog("a", 11000));
    CHECK(!t.ShouldLog("b", 11999));
    CHECK(t.ShouldLog("b", 12000));
}

TEST(ReasonThrottle_ManyDistinctReasonsStayBounded) {
    ReasonThrottle t(10000);
    for (int i = 0; i < 1000; ++i) CHECK(t.ShouldLog("reason " + std::to_string(i), 5));
    CHECK(t.Size() <= 256u);
}

// ---------------------------------------------------------------- texts

TEST(FgText_UnsupportedAdapter) {
    const std::string why = "eErrorNoSupportedAdapterFound: the GPU does not support DLSS Frame Generation";
    CHECK_EQ(DlssgUnsupportedMessage(why, false, false),
             "DLSS-G is not supported on this adapter (" + why + ")");
    CHECK_EQ(DlssgUnsupportedMessage(why, true, false),
             "DLSS-G is not supported on this adapter (" + why +
                 "); RTX 30 needs dlssg_for_sm86 (version.dll) in the game folder");
    // With the spoof loaded the hint would be wrong: something else failed.
    CHECK_EQ(DlssgUnsupportedMessage(why, true, true), "DLSS-G is not supported on this adapter (" + why + ")");
}

TEST(FgText_StatusNames) {
    CHECK_EQ(DlssgStatusText(0), std::string("eOk"));
    CHECK_EQ(DlssgStatusText(1u << 0), std::string("eFailResolutionTooLow"));
    CHECK_EQ(DlssgStatusText((1u << 1) | (1u << 4)),
             std::string("eFailReflexNotDetectedAtRuntime|eFailGetCurrentBackBufferIndexNotCalled"));
    CHECK_EQ(DlssgStatusText((1u << 2) | (1u << 3)),
             std::string("eFailHDRFormatNotSupported|eFailCommonConstantsInvalid"));
    CHECK_EQ(DlssgStatusText(1u << 7), std::string("0x80"));
    CHECK_EQ(DlssgStatusText((1u << 0) | (1u << 9)), std::string("eFailResolutionTooLow|0x200"));
}

TEST(FgText_RuntimeAspectRefusal) {
    // ALLOW_STRETCHING=1: CSP stretches, nothing is letterboxed.
    CHECK(RuntimeAspectRefusal(true, 1920, 1080, 1280, 1024).empty());
    // Same aspect, other size (CSP supersampling): allowed.
    CHECK(RuntimeAspectRefusal(false, 2560, 1440, 1920, 1080).empty());
    // Unknown output size: nothing to compare.
    CHECK(RuntimeAspectRefusal(false, 0, 0, 1920, 1080).empty());
    const std::string r = RuntimeAspectRefusal(false, 1920, 1080, 1280, 1024);
    CHECK_EQ(r, std::string("aspect: the DLSS output 1920x1080 differs from the window 1280x1024 (letterboxed "
                            "output is not supported)"));
}

// ---------------------------------------------------------------- DLSS-G state

TEST(FgState_GeneratedFramesAtOnePresent) {
    // Review finding SL-1: slDLSSGGetState is read right after every Present
    // DLSS-G was on for. numFramesActuallyPresented is then 2 with 2X
    // whether it means "per application frame" (the DLSS-G guide, 13.0) or
    // "since the last call" (sl_dlss_g.h): one generated frame.
    CHECK_EQ(GeneratedFramesAtPresent(2), 1u);
    CHECK_EQ(GeneratedFramesAtPresent(4), 3u);  // 4X
    CHECK_EQ(GeneratedFramesAtPresent(1), 0u);  // the generated frame was dropped
    CHECK_EQ(GeneratedFramesAtPresent(0), 0u);  // nothing presented yet
}

TEST(FgState_StatusIsDueEvery60FramesOnAFrameDlssgWasOnFor) {
    StatusPollClock c;
    for (int i = 0; i < 59; ++i) c.Frame();
    CHECK(!c.Due(true));
    c.Frame();
    CHECK(c.Due(true));
    CHECK(!c.Due(false));  // only a frame DLSS-G was on for reads the status
    c.Polled();
    CHECK(!c.Due(true));
}

TEST(FgState_StatusPollSurvivesDlssgTogglingOffForSingleFrames) {
    // Review findings SL-2 and F4: a per-frame reason turns DLSS-G off for
    // one frame now and then. The frames still count, so the status is read
    // on the first frame DLSS-G is on once 60 frames have passed.
    StatusPollClock c;
    int polls = 0;
    for (int frame = 1; frame <= 300; ++frame) {
        const bool on = frame % 7 != 0;  // off every seventh frame
        c.Frame();
        if (c.Due(on)) {
            ++polls;
            c.Polled();
        }
    }
    CHECK_EQ(polls, 5);
}
