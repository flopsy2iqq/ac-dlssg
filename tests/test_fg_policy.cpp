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
        // Review finding F6: DLSS-G off while the game window is minimized.
        {"minimized", [](FgGateInputs* in) { in->windowMinimized = true; }, "game window minimized", true},
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

TEST(VramGuard_ReportsOnlyAChangedOutcome) {
    // Review finding F2 (in game): a refusal that lasts is logged once, not
    // at every recheck.
    VramGuard g;
    CHECK(g.Record(10, VramCheck{false, "video memory: need 900 MiB, free 800 MiB"}));
    CHECK(!g.Record(70, VramCheck{false, "video memory: need 900 MiB, free 790 MiB"}));
    CHECK(!g.Record(130, VramCheck{false, "video memory: need 900 MiB, free 810 MiB"}));
    CHECK(g.Record(190, VramCheck{true, ""}));
}

TEST(VramGuard_AFailedEstimateChecksTheHeadroomOnly) {
    // Review findings SL-6 and F2 (in game): a failed estimate query must not
    // keep DLSS-G off for good; the budget check runs with an estimate of 0.
    constexpr uint64_t MiB = 1024ull * 1024ull;
    VramInputs in;
    in.estimateOk = false;
    in.budgetKnown = true;
    in.budgetBytes = 8000 * MiB;
    in.usageBytes = 6000 * MiB;
    in.headroomMib = 512;
    CHECK(DecideVram(in).ok);
    in.usageBytes = 7600 * MiB;
    const VramCheck refused = DecideVram(in);
    CHECK(!refused.ok);
    CHECK_EQ(refused.reason, std::string("video memory: need 512 MiB, free 400 MiB"));
    // With an estimate it counts; without a budget nothing is refused.
    in.estimateOk = true;
    in.estimateBytes = 300 * MiB;
    in.usageBytes = 7200 * MiB;
    CHECK(!DecideVram(in).ok);
    in.budgetKnown = false;
    CHECK(DecideVram(in).ok);
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

// ---------------------------------------------------------------- multi frame generation

TEST(FgMult_ValidMultipliersAre2To4) {
    CHECK(!ValidFgMultiplier(0));
    CHECK(!ValidFgMultiplier(1));
    CHECK(ValidFgMultiplier(2));
    CHECK(ValidFgMultiplier(3));
    CHECK(ValidFgMultiplier(4));
    CHECK(!ValidFgMultiplier(5));
    CHECK(!ValidFgMultiplier(-3));
}

TEST(FgMult_FramesToGenerateIsTheMultiplierMinusOne) {
    CHECK_EQ(FramesToGenerate(2), 1u);
    CHECK_EQ(FramesToGenerate(3), 2u);
    CHECK_EQ(FramesToGenerate(4), 3u);
    // Never 0: sl.dlss_g 2.14.1 refuses numFramesToGenerate 0 in slDLSSGSetOptions.
    CHECK_EQ(FramesToGenerate(1), 1u);
    CHECK_EQ(FramesToGenerate(0), 1u);
}

TEST(FgMult_ARequestWithinStreamlinesMaxIsUsed) {
    // numFramesToGenerateMax 3: up to 4X (RTX 50, or dlssg_for_sm86's Blackwell report).
    for (unsigned m : {2u, 3u, 4u}) {
        const FgMultiplierChoice c = ChooseFgMultiplier(m, 3);
        CHECK_EQ(c.multiplier, m);
        CHECK_EQ(c.numFramesToGenerate, m - 1);
        CHECK(c.note.empty());
    }
    // sl.dlss_g caps its max at 5 (6X); a higher max changes nothing here.
    CHECK_EQ(ChooseFgMultiplier(4, 5).multiplier, 4u);
    CHECK(ChooseFgMultiplier(4, 5).note.empty());
    CHECK_EQ(ChooseFgMultiplier(4, 0xFFFFFFFFu).multiplier, 4u);  // no overflow
}

TEST(FgMult_ARequestAboveTheMaxUsesTheMaxAndSaysSo) {
    FgMultiplierChoice c = ChooseFgMultiplier(4, 2);
    CHECK_EQ(c.multiplier, 3u);
    CHECK_EQ(c.numFramesToGenerate, 2u);
    CHECK_EQ(c.note, std::string("4X requested, Streamline allows up to 3X; using 3X"));
    c = ChooseFgMultiplier(3, 2);
    CHECK_EQ(c.multiplier, 3u);
    CHECK(c.note.empty());
}

TEST(FgMult_AMaxOf0Or1Means2X) {
    // 1 is what sl.dlss_g reports without NGX's DLSSG.MultiFrameCountMax
    // (RTX 40); 0 is a state that does not report it.
    for (uint32_t max : {0u, 1u}) {
        for (unsigned m : {2u, 3u, 4u}) {
            const FgMultiplierChoice c = ChooseFgMultiplier(m, max);
            CHECK_EQ(c.multiplier, 2u);
            CHECK_EQ(c.numFramesToGenerate, 1u);
            CHECK_EQ(c.note.empty(), m == 2);
        }
    }
    CHECK_EQ(ChooseFgMultiplier(3, 0).note, std::string("3X requested, Streamline allows up to 2X; using 2X"));
    CHECK_EQ(ChooseFgMultiplier(4, 1).note, std::string("4X requested, Streamline allows up to 2X; using 2X"));
}

TEST(FgMult_AnInvalidRequestCountsAs2X) {
    for (unsigned m : {0u, 1u, 5u, 7u}) {
        const FgMultiplierChoice c = ChooseFgMultiplier(m, 3);
        CHECK_EQ(c.multiplier, 2u);
        CHECK_EQ(c.numFramesToGenerate, 1u);
        CHECK(c.note.empty());
    }
}

TEST(FgMult_TheStatusNoteJoinsTheClampAndTheFallback) {
    CHECK(FgMultiplierNote("", "").empty());
    CHECK_EQ(FgMultiplierNote("4X requested, Streamline allows up to 3X; using 3X", ""),
             std::string("4X requested, Streamline allows up to 3X; using 3X"));
    CHECK_EQ(FgMultiplierNote("", "video memory: 4X needs 700 MiB, free 500 MiB; falling back to 2X"),
             std::string("video memory: 4X needs 700 MiB, free 500 MiB; falling back to 2X"));
    CHECK_EQ(FgMultiplierNote("4X requested, Streamline allows up to 3X; using 3X",
                              "video memory: 3X needs 500 MiB, free 400 MiB; falling back to 2X"),
             std::string("4X requested, Streamline allows up to 3X; using 3X; video memory: 3X needs 500 MiB, free "
                         "400 MiB; falling back to 2X"));
}

TEST(FgOptions_SentOnlyWhenStreamlineWouldSeeAChange) {
    // Nothing sent yet: always.
    CHECK(DlssgOptionsDue(false, false, 0, true, false, 1));
    CHECK(DlssgOptionsDue(false, false, 0, true, true, 3));
    // The same mode and count: never.
    CHECK(!DlssgOptionsDue(true, true, 1, true, true, 1));
    CHECK(!DlssgOptionsDue(true, false, 1, true, false, 1));
    // The mode.
    CHECK(DlssgOptionsDue(true, false, 1, true, true, 1));
    CHECK(DlssgOptionsDue(true, true, 1, true, false, 1));
    // While on, new size hints; while off they wait for the next eOn.
    CHECK(DlssgOptionsDue(true, true, 1, false, true, 1));
    CHECK(!DlssgOptionsDue(true, false, 1, false, false, 1));
    // A new multiplier while on is sent at once; while off, the next eOn
    // carries it (no eOff with a new count).
    CHECK(DlssgOptionsDue(true, true, 1, true, true, 3));
    CHECK(DlssgOptionsDue(true, true, 3, true, true, 1));
    CHECK(!DlssgOptionsDue(true, false, 1, true, false, 3));
}

TEST(FgState_GeneratedFramesAt3XAnd4X) {
    // numFramesActuallyPresented - 1 per real frame, whatever the multiplier.
    CHECK_EQ(GeneratedFramesAtPresent(3), 2u);
    CHECK_EQ(GeneratedFramesAtPresent(4), 3u);
}

// ---------------------------------------------------------------- video memory with MFG

TEST(VramGuard_CheckGivesTheNumbersInMiB) {
    constexpr uint64_t MiB = 1024ull * 1024ull;
    const VramCheck r = CheckVideoMemory(4000 * MiB, 3500 * MiB, 300 * MiB + 1, 512);
    CHECK(!r.ok);
    CHECK_EQ(r.needMib, 813u);  // rounded up
    CHECK_EQ(r.freeMib, 500u);
    const VramCheck ok = CheckVideoMemory(4000 * MiB, 1000 * MiB + 1, 300 * MiB, 0);
    CHECK(ok.ok);
    CHECK_EQ(ok.needMib, 300u);
    CHECK_EQ(ok.freeMib, 2999u);  // rounded down
}

TEST(VramGuard_OnlyTheGrowthOverWhatDlssgHoldsMustFit) {
    // eRetainResourcesWhenOff keeps the resources of DLSS-G's last eOn, and
    // the process's usage already contains them: the 4 GB laptop at 2X
    // (283 MiB held) with 60 MiB free asks for 3X.
    constexpr uint64_t MiB = 1024ull * 1024ull;
    VramInputs in;
    in.estimateOk = true;
    in.estimateBytes = 350 * MiB;
    in.budgetKnown = true;
    in.budgetBytes = 4000 * MiB;
    in.usageBytes = 3940 * MiB;
    in.headroomMib = 0;
    in.heldBytes = 283 * MiB;
    VramCheck c = DecideVram(in);
    CHECK(!c.ok);
    CHECK_EQ(c.reason, std::string("video memory: need 67 MiB, free 60 MiB"));
    in.estimateBytes = 330 * MiB;
    CHECK(DecideVram(in).ok);  // 47 MiB more
    in.headroomMib = 16;
    CHECK(!DecideVram(in).ok);  // 47 + 16 > 60
    // Without resources held the whole estimate counts.
    in.headroomMib = 0;
    in.heldBytes = 0;
    c = DecideVram(in);
    CHECK(!c.ok);
    CHECK_EQ(c.reason, std::string("video memory: need 330 MiB, free 60 MiB"));
}

TEST(VramGuard_NoGrowthAlwaysFits) {
    // 4X back to 2X: nothing new is allocated, even when the headroom no
    // longer fits.
    constexpr uint64_t MiB = 1024ull * 1024ull;
    VramInputs in;
    in.estimateOk = true;
    in.estimateBytes = 283 * MiB;
    in.budgetKnown = true;
    in.budgetBytes = 4000 * MiB;
    in.usageBytes = 3990 * MiB;
    in.headroomMib = 512;
    in.heldBytes = 400 * MiB;
    CHECK(DecideVram(in).ok);
    in.estimateBytes = 400 * MiB;  // the same multiplier again
    CHECK(DecideVram(in).ok);
    // A failed estimate still checks the headroom only (review findings SL-6 and F2).
    in.estimateOk = false;
    const VramCheck c = DecideVram(in);
    CHECK(!c.ok);
    CHECK_EQ(c.reason, std::string("video memory: need 512 MiB, free 10 MiB"));
}

TEST(VramFallback_AHigherMultiplierThatDoesNotFitFallsBackTo2X) {
    constexpr uint64_t MiB = 1024ull * 1024ull;
    const VramCheck at4 = CheckVideoMemory(4000 * MiB, 3500 * MiB, 700 * MiB, 0);
    const VramCheck at2 = CheckVideoMemory(4000 * MiB, 3500 * MiB, 300 * MiB, 0);
    REQUIRE(!at4.ok);
    REQUIRE(at2.ok);
    const VramMultiplierDecision d = DecideVramMultiplier(4, at4, &at2);
    CHECK(d.check.ok);
    CHECK_EQ(d.multiplier, 2u);
    CHECK_EQ(d.fallback, std::string("video memory: 4X needs 700 MiB, free 500 MiB; falling back to 2X"));
}

TEST(VramFallback_AMultiplierThatFitsIsKept) {
    const VramMultiplierDecision d = DecideVramMultiplier(3, VramCheck{}, nullptr);
    CHECK(d.check.ok);
    CHECK_EQ(d.multiplier, 3u);
    CHECK(d.fallback.empty());
}

TEST(VramFallback_WhenNeither2XNorTheHigherOneFitsDlssgStaysOff) {
    constexpr uint64_t MiB = 1024ull * 1024ull;
    const VramCheck at4 = CheckVideoMemory(4000 * MiB, 3900 * MiB, 700 * MiB, 0);
    const VramCheck at2 = CheckVideoMemory(4000 * MiB, 3900 * MiB, 300 * MiB, 0);
    const VramMultiplierDecision d = DecideVramMultiplier(4, at4, &at2);
    CHECK(!d.check.ok);
    // The smallest need is the one to report.
    CHECK_EQ(d.check.reason, std::string("video memory: need 300 MiB, free 100 MiB"));
    CHECK(d.fallback.empty());
    // 2X has nothing to fall back to; nor has a higher multiplier whose 2X
    // check was not made.
    const VramMultiplierDecision two = DecideVramMultiplier(2, at2, nullptr);
    CHECK(!two.check.ok);
    CHECK_EQ(two.multiplier, 2u);
    CHECK_EQ(two.check.reason, at2.reason);
    const VramMultiplierDecision three = DecideVramMultiplier(3, at4, nullptr);
    CHECK(!three.check.ok);
    CHECK_EQ(three.check.reason, at4.reason);
}

TEST(VramGuard_RechecksWhenTheWantedMultiplierChanges) {
    VramGuard g;
    CHECK(g.CheckDue(10, 2));
    g.Record(10, VramCheck{}, 2, 2);
    CHECK(g.Passed());
    CHECK_EQ(g.Wanted(), 2u);
    CHECK_EQ(g.Granted(), 2u);
    CHECK(!g.CheckDue(11, 2));
    CHECK(!g.CheckDue(5000, 2));
    // A new multiplier is checked at once, not 60 frames later.
    CHECK(g.CheckDue(12, 4));
    g.Record(12, VramCheck{}, 4, 2);  // 4X did not fit, 2X did
    CHECK(g.Passed());
    CHECK_EQ(g.Wanted(), 4u);
    CHECK_EQ(g.Granted(), 2u);
    // Not checked again while 4X stays wanted (a fallback is not retried).
    CHECK(!g.CheckDue(500, 4));
    CHECK(g.CheckDue(501, 2));
    // A refusal is rechecked every 60 frames, and a new multiplier at once.
    g.Record(501, VramCheck{false, "video memory: need 1 MiB, free 0 MiB"}, 3, 2);
    CHECK(!g.Passed());
    CHECK_EQ(g.Refusal(), std::string("video memory: need 1 MiB, free 0 MiB"));
    CHECK(!g.CheckDue(502, 3));
    CHECK(g.CheckDue(561, 3));
    CHECK(g.CheckDue(502, 2));
}

TEST(VramGuard_ANewOrFallenBackMultiplierIsAChange) {
    VramGuard g;
    CHECK(g.Record(10, VramCheck{}, 2, 2));
    CHECK(g.Record(20, VramCheck{}, 3, 3));   // a new multiplier
    CHECK(!g.Record(30, VramCheck{}, 3, 3));  // the same outcome
    CHECK(g.Record(40, VramCheck{}, 3, 2));   // now a fallback
}

// ---------------------------------------------------------------- fg_vram_headroom_mib=auto

TEST(VramAuto_HeadroomFollowsTheBudget) {
    constexpr uint64_t MiB = 1024ull * 1024ull;
    // The 4 GB RTX 3050 Ti laptop reports a budget of about 3.5 GiB.
    CHECK_EQ(AutoVramHeadroomMib(3500 * MiB), 0u);
    CHECK_EQ(AutoVramHeadroomMib(6144 * MiB - 1), 0u);
    CHECK_EQ(AutoVramHeadroomMib(6144 * MiB), 256u);
    CHECK_EQ(AutoVramHeadroomMib(9800 * MiB), 256u);  // the RTX 3080
    CHECK_EQ(AutoVramHeadroomMib(0), 0u);
    // A number is used as it is, whatever the budget.
    CHECK_EQ(VramHeadroomMib(false, 512, 3500 * MiB), 512u);
    CHECK_EQ(VramHeadroomMib(false, 0, 9800 * MiB), 0u);
    CHECK_EQ(VramHeadroomMib(true, 512, 3500 * MiB), 0u);
    CHECK_EQ(VramHeadroomMib(true, 0, 9800 * MiB), 256u);
}

TEST(VramAuto_TheCheckKnowsTheEstimateItHadToFit) {
    constexpr uint64_t MiB = 1024ull * 1024ull;
    const VramCheck r = CheckVideoMemory(4000 * MiB, 3500 * MiB, 300 * MiB + 1, 512);
    CHECK_EQ(r.estimateMib, 301u);  // rounded up, without the headroom
    CHECK_EQ(r.needMib, 813u);
    CHECK_EQ(CheckVideoMemory(4000 * MiB, 3500 * MiB, 0, 512).estimateMib, 0u);
}

TEST(VramAuto_ToleranceIs128MiBOrATenthOfTheEstimate) {
    CHECK_EQ(VramTightToleranceMib(0, 256), 128u);  // no estimate: the 256 MiB headroom only
    CHECK_EQ(VramTightToleranceMib(283, 283), 128u);
    CHECK_EQ(VramTightToleranceMib(1280, 1536), 128u);
    CHECK_EQ(VramTightToleranceMib(2000, 2000), 200u);
    // Never more than half of the need.
    CHECK_EQ(VramTightToleranceMib(120, 120), 60u);
    CHECK_EQ(VramTightToleranceMib(121, 121), 60u);
    CHECK_EQ(VramTightToleranceMib(0, 0), 0u);
}

// The laptop: 346 MiB free for a 380 MiB estimate at 2X. With auto, DLSS-G
// runs anyway and the video memory is marked tight; a number keeps today's
// refusal, and the panel's note says that auto would run it.
TEST(VramAuto_ASmallShortfallAt2XIsTight) {
    constexpr uint64_t MiB = 1024ull * 1024ull;
    const VramCheck at2 = CheckVideoMemory(3500 * MiB, 3154 * MiB, 380 * MiB, 0);
    REQUIRE(!at2.ok);
    VramMultiplierDecision d = DecideVramMultiplier(2, at2, nullptr, true);
    CHECK(d.check.ok);
    CHECK(d.check.reason.empty());
    CHECK(d.tight);
    CHECK_EQ(d.multiplier, 2u);
    CHECK(d.fallback.empty());
    CHECK_EQ(d.note,
             std::string("video memory is tight (346 MiB free for 380 MiB); frame generation on anyway "
                         "(fg_vram_headroom_mib=auto)"));
    // A number: exactly the refusal of before, with the note for the panel.
    d = DecideVramMultiplier(2, at2, nullptr, false);
    CHECK(!d.check.ok);
    CHECK(!d.tight);
    CHECK_EQ(d.check.reason, std::string("video memory: need 380 MiB, free 346 MiB"));
    CHECK_EQ(d.note, std::string("fg_vram_headroom_mib=0 keeps frame generation off (380 MiB needed, 346 MiB free): "
                                 "set it to auto in ac-dlssg.ini and restart the game"));
}

TEST(VramAuto_ALargerShortfallKeepsDlssgOffWithAnActionableReason) {
    constexpr uint64_t MiB = 1024ull * 1024ull;
    const VramCheck at2 = CheckVideoMemory(3500 * MiB, 3400 * MiB, 380 * MiB, 0);
    const VramMultiplierDecision d = DecideVramMultiplier(2, at2, nullptr, true);
    CHECK(!d.check.ok);
    CHECK(!d.tight);
    const std::string text =
        "not enough video memory: frame generation needs 380 MiB, 100 MiB free; lower CSP texture quality, shadows "
        "or the render resolution";
    CHECK_EQ(d.check.reason, text);
    CHECK_EQ(d.note, text);
    CHECK(text.size() < 160u);  // fits the status record's reason
}

TEST(VramAuto_TheToleranceBoundaries) {
    constexpr uint64_t MiB = 1024ull * 1024ull;
    // 128 MiB short is tight, 129 MiB is not (estimate below 1280 MiB).
    CHECK(DecideVramMultiplier(2, CheckVideoMemory(3500 * MiB, 3328 * MiB, 300 * MiB, 0), nullptr, true).tight);
    CHECK(!DecideVramMultiplier(2, CheckVideoMemory(3500 * MiB, 3329 * MiB, 300 * MiB, 0), nullptr, true).tight);
    // A 2000 MiB estimate tolerates 200 MiB.
    CHECK(DecideVramMultiplier(2, CheckVideoMemory(8000 * MiB, 6200 * MiB, 2000 * MiB, 0), nullptr, true).tight);
    CHECK(!DecideVramMultiplier(2, CheckVideoMemory(8000 * MiB, 6201 * MiB, 2000 * MiB, 0), nullptr, true).tight);
    // The headroom counts: 256 MiB on an 8 GB card, 492 MiB free for 283 + 256.
    const VramMultiplierDecision d =
        DecideVramMultiplier(2, CheckVideoMemory(8192 * MiB, 7700 * MiB, 283 * MiB, 256), nullptr, true);
    CHECK(d.tight);
    CHECK_EQ(d.note, std::string("video memory is tight (492 MiB free for 539 MiB); frame generation on anyway "
                                 "(fg_vram_headroom_mib=auto)"));
    // Enough memory: nothing to note.
    const VramMultiplierDecision fits = DecideVramMultiplier(2, VramCheck{}, nullptr, true);
    CHECK(fits.check.ok);
    CHECK(!fits.tight);
    CHECK(fits.note.empty());
}

// A number that keeps DLSS-G off where auto would run it (the first
// installs' 512 on the 4 GB laptop: 346 MiB free for a 283 MiB estimate):
// the panel's note names the number and says to set auto and restart the
// game, rather than to lower the textures. The guard's reason stays.
TEST(VramNumber_AHeadroomThatKeepsDlssgOffWhereAutoWouldRunItSaysSo) {
    constexpr uint64_t MiB = 1024ull * 1024ull;
    VramMultiplierDecision d =
        DecideVramMultiplier(2, CheckVideoMemory(3500 * MiB, 3154 * MiB, 283 * MiB, 512), nullptr, false);
    CHECK(!d.check.ok);
    CHECK(!d.tight);
    CHECK_EQ(d.check.reason, std::string("video memory: need 795 MiB, free 346 MiB"));
    const std::string hint = "fg_vram_headroom_mib=512 keeps frame generation off (795 MiB needed, 346 MiB free): set "
                             "it to auto in ac-dlssg.ini and restart the game";
    CHECK_EQ(d.note, hint);
    CHECK(hint.size() < 160u);  // fits the status record's vramNote
    // The longest one fits too.
    d = DecideVramMultiplier(2, CheckVideoMemory(99999 * MiB, 0, 65535 * MiB, 65536), nullptr, false);
    CHECK(!d.check.ok);
    CHECK(d.note.size() < 160u);
    // An 8 GB card with 1024: auto's 256 MiB would fit (692 MiB free for 539).
    d = DecideVramMultiplier(2, CheckVideoMemory(8192 * MiB, 7500 * MiB, 283 * MiB, 1024), nullptr, false);
    CHECK(d.note.find("fg_vram_headroom_mib=1024 keeps frame generation off (1307 MiB needed, 692 MiB free)") == 0);
    // 3X falling back: the 2X check decides.
    const VramCheck at3 = CheckVideoMemory(3500 * MiB, 3154 * MiB, 500 * MiB, 512);
    const VramCheck at2 = CheckVideoMemory(3500 * MiB, 3154 * MiB, 283 * MiB, 512);
    d = DecideVramMultiplier(3, at3, &at2, false);
    CHECK(!d.check.ok);
    CHECK_EQ(d.note, hint);
    // 0, where auto would run it as tight.
    d = DecideVramMultiplier(2, CheckVideoMemory(3500 * MiB, 3154 * MiB, 380 * MiB, 0), nullptr, false);
    CHECK(d.note.find("fg_vram_headroom_mib=0 keeps frame generation off (380 MiB needed, 346 MiB free)") == 0);
    // Where auto would not run it either, the note says what to lower.
    d = DecideVramMultiplier(2, CheckVideoMemory(3500 * MiB, 3400 * MiB, 283 * MiB, 512), nullptr, false);
    CHECK_EQ(d.note, std::string("not enough video memory: frame generation needs 795 MiB, 100 MiB free; lower CSP "
                                 "texture quality, shadows or the render resolution"));
}

// A small output (a 1280x720 window) needs little, less than the 128 MiB
// tolerance. With nothing, or almost nothing, free that is not a little
// short but clearly not enough: at least half of the need must be free.
TEST(VramAuto_NothingFreeIsNeverTight) {
    constexpr uint64_t MiB = 1024ull * 1024ull;
    // Usage above the budget counts as nothing free.
    const VramCheck none = CheckVideoMemory(3500 * MiB, 3600 * MiB, 120 * MiB, 0);
    REQUIRE(!none.ok);
    const VramMultiplierDecision d = DecideVramMultiplier(2, none, nullptr, true);
    CHECK(!d.check.ok);
    CHECK(!d.tight);
    CHECK_EQ(d.note, std::string("not enough video memory: frame generation needs 120 MiB, 0 MiB free; lower CSP "
                                 "texture quality, shadows or the render resolution"));
    // 59 MiB free for 120 MiB is not enough; 60 MiB, half of it, is tight.
    CHECK(!DecideVramMultiplier(2, CheckVideoMemory(3500 * MiB, 3441 * MiB, 120 * MiB, 0), nullptr, true).tight);
    CHECK(DecideVramMultiplier(2, CheckVideoMemory(3500 * MiB, 3440 * MiB, 120 * MiB, 0), nullptr, true).tight);
    // The same for 3X falling back to a 2X that has nothing free.
    const VramCheck at3 = CheckVideoMemory(3500 * MiB, 3600 * MiB, 180 * MiB, 0);
    const VramMultiplierDecision d3 = DecideVramMultiplier(3, at3, &none, true);
    CHECK(!d3.check.ok);
    CHECK(!d3.tight);
}

// The multiplier fallback stays: a higher multiplier that does not fit falls
// back to 2X, which may itself be tight. Only 2X is ever tight.
TEST(VramAuto_AHigherMultiplierFallsBackTo2XWhichMayBeTight) {
    constexpr uint64_t MiB = 1024ull * 1024ull;
    const VramCheck at4 = CheckVideoMemory(3500 * MiB, 3154 * MiB, 700 * MiB, 0);
    const VramCheck at2 = CheckVideoMemory(3500 * MiB, 3154 * MiB, 380 * MiB, 0);
    VramMultiplierDecision d = DecideVramMultiplier(4, at4, &at2, true);
    CHECK(d.check.ok);
    CHECK(d.tight);
    CHECK_EQ(d.multiplier, 2u);
    CHECK_EQ(d.fallback, std::string("video memory: 4X needs 700 MiB, free 346 MiB; falling back to 2X"));
    CHECK(d.note.find("video memory is tight (346 MiB free for 380 MiB)") == 0);
    // 3X a little short, 2X fits: a plain fallback, not tight.
    const VramCheck at3 = CheckVideoMemory(3500 * MiB, 3154 * MiB, 400 * MiB, 0);
    const VramCheck at2fits = CheckVideoMemory(3500 * MiB, 3154 * MiB, 300 * MiB, 0);
    d = DecideVramMultiplier(3, at3, &at2fits, true);
    CHECK(d.check.ok);
    CHECK(!d.tight);
    CHECK_EQ(d.multiplier, 2u);
    CHECK(d.note.empty());
    // 3X a little short and no 2X check made: refused, never tight at 3X.
    d = DecideVramMultiplier(3, at3, nullptr, true);
    CHECK(!d.check.ok);
    CHECK(!d.tight);
    CHECK_EQ(d.multiplier, 3u);
}

TEST(VramAuto_TheGuardReportsATightChangeOnce) {
    VramGuard g;
    CHECK(g.Record(10, VramCheck{}, 2, 2, true));
    CHECK(g.Passed());
    CHECK(g.Tight());
    CHECK(!g.Record(20, VramCheck{}, 2, 2, true));  // the same outcome
    CHECK(g.Record(30, VramCheck{}, 2, 2, false));  // no longer tight
    CHECK(!g.Tight());
    CHECK(g.Record(40, VramCheck{false, "x"}, 2, 2, false));
    CHECK(!g.Tight());
}

// The presenter's loop: a check whenever CheckDue, decided and recorded. A
// tight pass is a pass, so once DLSS-G is on (and its resources make the
// budget tighter still) it is not checked again every 60 frames and cannot
// go off and on with the budget; only a new wanted multiplier checks again.
// A refusal is still checked every 60 frames.
TEST(VramAuto_ATightPassIsNotCheckedAgainEvery60Frames) {
    constexpr uint64_t MiB = 1024ull * 1024ull;
    const auto run = [&](uint64_t usageMib, uint64_t frames, int* checks) {
        VramGuard g;
        *checks = 0;
        for (uint64_t f = 0; f < frames; ++f) {
            if (!g.CheckDue(f, 2)) continue;
            ++*checks;
            const VramCheck at2 = CheckVideoMemory(3500 * MiB, usageMib * MiB, 380 * MiB, AutoVramHeadroomMib(3500 * MiB));
            const VramMultiplierDecision d = DecideVramMultiplier(2, at2, nullptr, true);
            g.Record(f, d.check, 2, d.multiplier, d.tight);
            // DLSS-G on: its resources are in the usage from now on.
            if (g.Passed()) usageMib += 380;
        }
        return g;
    };
    int checks = 0;
    VramGuard tight = run(3154, 600, &checks);  // 346 MiB free for 380 MiB: tight
    CHECK(tight.Passed());
    CHECK(tight.Tight());
    CHECK_EQ(checks, 1);
    CHECK(tight.CheckDue(10000, 3));  // a new multiplier is checked again
    VramGuard refused = run(3400, 600, &checks);  // 100 MiB free: not enough
    CHECK(!refused.Passed());
    CHECK_EQ(checks, 10);
}

// ---------------------------------------------------------------- a number that blocks: switched to auto

namespace {

constexpr uint64_t kMiB = 1024ull * 1024ull;

// The presenter's two decisions for one check of the guard (2X, or a higher
// wanted multiplier with its 2X check): with the configured number, and with
// auto's headroom for the same estimates, budget and usage.
struct HeadroomCase {
    VramMultiplierDecision withNumber;
    VramMultiplierDecision withAuto;
};

HeadroomCase DecideBoth(unsigned wanted, uint64_t budgetMib, uint64_t usageMib, uint64_t estimateWantedMib,
                        uint64_t estimate2xMib, unsigned number) {
    const unsigned autoMib = AutoVramHeadroomMib(budgetMib * kMiB);
    const auto check = [&](uint64_t estimateMib, unsigned headroom) {
        return CheckVideoMemory(budgetMib * kMiB, usageMib * kMiB, estimateMib * kMiB, headroom);
    };
    HeadroomCase c;
    const VramCheck numberWanted = check(estimateWantedMib, number);
    const VramCheck number2x = check(estimate2xMib, number);
    c.withNumber = DecideVramMultiplier(wanted, numberWanted, wanted > 2 && !numberWanted.ok ? &number2x : nullptr,
                                        false);
    const VramCheck autoWanted = check(estimateWantedMib, autoMib);
    const VramCheck auto2x = check(estimate2xMib, autoMib);
    c.withAuto = DecideVramMultiplier(wanted, autoWanted, wanted > 2 && !autoWanted.ok ? &auto2x : nullptr, true);
    return c;
}

VramHeadroomAction Decide(const HeadroomCase& c, bool autoHeadroom = false) {
    return DecideVramHeadroom(autoHeadroom, c.withNumber, c.withAuto);
}

}  // namespace

// The friend's laptop: a stale 512 (an old default) on the 4 GB RTX 3050 Ti,
// 346 MiB free for a 283 MiB estimate. The number keeps DLSS-G off, auto (no
// headroom below a 6144 MiB budget) runs it: switch to auto.
TEST(VramHeadroomFix_ANumberThatBlocksWhereAutoFitsIsSwitchedToAuto) {
    const HeadroomCase c = DecideBoth(2, 3500, 3154, 283, 283, 512);
    REQUIRE(!c.withNumber.check.ok);
    REQUIRE(c.withAuto.check.ok);
    CHECK(!c.withAuto.tight);
    CHECK(Decide(c) == VramHeadroomAction::SwitchToAuto);
    // An 8 GB card with 1024: auto's 256 MiB fits (692 MiB free for 539).
    CHECK(Decide(DecideBoth(2, 8192, 7500, 283, 283, 1024)) == VramHeadroomAction::SwitchToAuto);
}

// Auto runs DLSS-G as tight where the number (0 here) refuses: that counts as
// auto helping too.
TEST(VramHeadroomFix_ANumberThatBlocksWhereAutoIsTightIsSwitchedToAuto) {
    const HeadroomCase c = DecideBoth(2, 3500, 3154, 380, 380, 0);
    REQUIRE(!c.withNumber.check.ok);
    REQUIRE(c.withAuto.check.ok);
    CHECK(c.withAuto.tight);
    CHECK(Decide(c) == VramHeadroomAction::SwitchToAuto);
}

// Where auto would keep DLSS-G off too, the number is not touched: its
// "not enough video memory ..." reason stays.
TEST(VramHeadroomFix_ANumberIsKeptWhenAutoWouldNotRunDlssgEither) {
    const HeadroomCase c = DecideBoth(2, 3500, 3400, 283, 283, 512);  // 100 MiB free
    REQUIRE(!c.withNumber.check.ok);
    REQUIRE(!c.withAuto.check.ok);
    CHECK(Decide(c) == VramHeadroomAction::Keep);
    CHECK_EQ(c.withNumber.check.reason, std::string("video memory: need 795 MiB, free 100 MiB"));
}

// A number that lets DLSS-G run (at the wanted multiplier, or at 2X after a
// fallback) is the user's choice and stays; auto is never switched.
TEST(VramHeadroomFix_ANumberThatLetsDlssgRunAndAutoAreKept) {
    CHECK(Decide(DecideBoth(2, 9800, 5000, 283, 283, 512)) == VramHeadroomAction::Keep);
    // 4X does not fit with 512, 2X does: DLSS-G runs at 2X (a fallback, not off).
    const HeadroomCase fallback = DecideBoth(4, 9800, 8800, 700, 283, 512);
    REQUIRE(fallback.withNumber.check.ok);
    REQUIRE(fallback.withNumber.multiplier == 2u);
    CHECK(Decide(fallback) == VramHeadroomAction::Keep);
    // Already auto (the session was switched, or the ini says auto).
    const HeadroomCase c = DecideBoth(2, 3500, 3154, 283, 283, 512);
    CHECK(Decide(c, true) == VramHeadroomAction::Keep);
}

// A higher wanted multiplier: the number refuses it and its 2X; auto runs
// DLSS-G, at 3X itself or at 2X after a fallback.
TEST(VramHeadroomFix_AHigherMultiplierThatAutoRunsIsSwitchedToo) {
    HeadroomCase c = DecideBoth(3, 3500, 3154, 330, 283, 512);
    REQUIRE(!c.withNumber.check.ok);
    REQUIRE(c.withAuto.check.ok);
    CHECK_EQ(c.withAuto.multiplier, 3u);
    CHECK(Decide(c) == VramHeadroomAction::SwitchToAuto);
    c = DecideBoth(3, 3500, 3154, 500, 283, 512);
    REQUIRE(c.withAuto.check.ok);
    CHECK_EQ(c.withAuto.multiplier, 2u);
    CHECK(Decide(c) == VramHeadroomAction::SwitchToAuto);
}

// The log line and the panel's note of the switch, saved or not.
TEST(VramHeadroomFix_TheLogLineAndTheNoteSayWhatChanged) {
    CHECK_EQ(VramHeadroomSwitchLog(512, true, ""),
             std::string("fg_vram_headroom_mib=512 kept frame generation off; switched to auto and saved it to "
                         "ac-dlssg.ini"));
    CHECK_EQ(VramHeadroomSwitchNote(512, true, ""),
             std::string("Video memory setting fixed: fg_vram_headroom_mib was 512, now auto (saved). Frame generation "
                         "is on."));
    const std::string why = "creating C:\\Games\\assettocorsa\\ac-dlssg\\ac-dlssg.ini.new failed: error 5";
    CHECK_EQ(VramHeadroomSwitchLog(0, false, why),
             "fg_vram_headroom_mib=0 kept frame generation off; switched to auto until the game is closed; could not "
             "save ac-dlssg.ini: " +
                 why);
    CHECK_EQ(VramHeadroomSwitchNote(0, false, why),
             "Video memory setting fixed: fg_vram_headroom_mib was 0, now auto. Frame generation is on; this applies "
             "until the game is closed; could not save ac-dlssg.ini: " +
                 why);
}

// ---------------------------------------------------------------- the multiplier on the present thread

TEST(FgMultTracker_StartsWithTheConfiguredRequestAndNoMax) {
    const FgMultiplierTracker t(3);
    CHECK_EQ(t.Requested(), 3u);
    CHECK(t.QueryDue());
    CHECK(!t.MaxKnown());
    CHECK_EQ(t.FramesMax(), 0u);
    // Nothing lowers the request before Streamline answered.
    CHECK_EQ(t.Wanted(), 3u);
    CHECK(t.ClampNote().empty());
    // An invalid configuration counts as 2X.
    CHECK_EQ(FgMultiplierTracker(7).Requested(), 2u);
    CHECK_EQ(FgMultiplierTracker(1).Wanted(), 2u);
}

TEST(FgMultTracker_TheMaxClampsTheRequestAndTheNoteIsLoggedOnce) {
    FgMultiplierTracker t(4);
    CHECK_EQ(t.OnFramesMax(2), std::string("4X requested, Streamline allows up to 3X; using 3X"));
    CHECK(!t.QueryDue());
    CHECK(t.MaxKnown());
    CHECK_EQ(t.FramesMax(), 2u);
    CHECK_EQ(t.Wanted(), 3u);
    CHECK_EQ(t.ClampNote(), std::string("4X requested, Streamline allows up to 3X; using 3X"));
    // The same answer again: nothing new to log, the status keeps the note.
    CHECK(t.OnFramesMax(2).empty());
    CHECK_EQ(t.ClampNote(), std::string("4X requested, Streamline allows up to 3X; using 3X"));
    // A request within the max: no note.
    FgMultiplierTracker ok(3);
    CHECK(ok.OnFramesMax(3).empty());
    CHECK_EQ(ok.Wanted(), 3u);
    CHECK(ok.ClampNote().empty());
}

TEST(FgMultTracker_AFailedQueryMeans2X) {
    FgMultiplierTracker t(3);
    CHECK_EQ(t.OnFramesMax(0), std::string("3X requested, Streamline allows up to 2X; using 2X"));
    CHECK_EQ(t.Wanted(), 2u);
    CHECK(!t.QueryDue());  // asked again at the next request only
}

TEST(FgMultTracker_ANewRequestIsQueriedAgainBeforeTheNextOptions) {
    FgMultiplierTracker t(2);
    CHECK(t.OnFramesMax(1).empty());
    // Unchanged or invalid requests change nothing.
    CHECK(!t.Request(2));
    CHECK(!t.Request(5));
    CHECK(!t.Request(0));
    CHECK(!t.QueryDue());
    CHECK(t.Request(4));
    CHECK_EQ(t.Requested(), 4u);
    CHECK(t.QueryDue());
    // Until the answer, the last known max already clamps it (the status is
    // right while DLSS-G is off).
    CHECK_EQ(t.Wanted(), 2u);
    CHECK_EQ(t.ClampNote(), std::string("4X requested, Streamline allows up to 2X; using 2X"));
    // The answer for the new request logs its note once more.
    CHECK_EQ(t.OnFramesMax(1), std::string("4X requested, Streamline allows up to 2X; using 2X"));
    CHECK(t.OnFramesMax(1).empty());
    // Back and forth: each request that is lowered is logged again.
    CHECK(t.Request(2));
    CHECK(t.OnFramesMax(1).empty());
    CHECK(t.ClampNote().empty());
    CHECK(t.Request(4));
    CHECK_EQ(t.OnFramesMax(1), std::string("4X requested, Streamline allows up to 2X; using 2X"));
}

TEST(FgMultTracker_AHigherMaxAllowsTheRequest) {
    FgMultiplierTracker t(4);
    CHECK_EQ(t.OnFramesMax(3), std::string());
    CHECK_EQ(t.Wanted(), 4u);
    CHECK(t.Request(3));
    CHECK_EQ(t.Wanted(), 3u);
    CHECK(t.OnFramesMax(3).empty());
    CHECK_EQ(t.Wanted(), 3u);
}

TEST(FgMult_TheOptionsCarryTheGuardsGrantForTheWantedMultiplier) {
    // No check yet, or a check for another multiplier: the wanted one.
    CHECK_EQ(UsedFgMultiplier(4, false, 2, 2), 4u);
    CHECK_EQ(UsedFgMultiplier(4, true, 3, 3), 4u);
    // The guard passed the wanted multiplier as it is, or fell back to 2X.
    CHECK_EQ(UsedFgMultiplier(4, true, 4, 4), 4u);
    CHECK_EQ(UsedFgMultiplier(4, true, 4, 2), 2u);
    // A refusal keeps DLSS-G off; the options would carry the wanted count.
    CHECK_EQ(UsedFgMultiplier(3, false, 3, 2), 3u);
}
