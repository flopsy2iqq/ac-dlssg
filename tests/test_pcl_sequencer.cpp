// PclSequencer: the per-frame PCL marker order of spec 7.
#include <sl_pcl.h>

#include <cstdio>
#include <utility>
#include <vector>

#include "pcl_sequencer.h"
#include "test_framework.h"

using namespace acdb;
using M = PclMarker;
using Markers = std::vector<PclMarker>;

// BeforePresent of a frame without an evaluate.
const Markers kAllBeforePresent = {M::SimulationEnd, M::RenderSubmitStart, M::RenderSubmitEnd, M::PresentStart};

TEST(Pcl_NormalFrameEmitsTheFullSequenceOnce) {
    PclSequencer seq;
    CHECK(seq.BeginFrame(1) == Markers({M::SimulationStart}));
    CHECK(seq.InFrame());
    CHECK_EQ(seq.CurrentFrame(), 1u);
    CHECK(seq.BeforePresent() == kAllBeforePresent);
    CHECK(seq.AfterPresent() == Markers({M::PresentEnd}));
    CHECK(!seq.InFrame());
    CHECK_EQ(seq.CurrentFrame(), 1u);
    CHECK_EQ(seq.AbandonedFrames(), 0u);
    CHECK_EQ(seq.OutOfOrderCalls(), 0u);

    // The next frame starts clean.
    CHECK(seq.BeginFrame(2) == Markers({M::SimulationStart}));
    CHECK_EQ(seq.CurrentFrame(), 2u);
    CHECK(seq.BeforePresent() == kAllBeforePresent);
    CHECK(seq.AfterPresent() == Markers({M::PresentEnd}));
    CHECK_EQ(seq.AbandonedFrames(), 0u);
    CHECK_EQ(seq.OutOfOrderCalls(), 0u);
}

TEST(Pcl_EvaluateEmitsSimulationEndAndRenderSubmitStartOnlyOnce) {
    PclSequencer seq;
    seq.BeginFrame(1);
    CHECK(seq.OnEvaluate() == Markers({M::SimulationEnd, M::RenderSubmitStart}));
    // BeforePresent does not repeat what the evaluate emitted.
    CHECK(seq.BeforePresent() == Markers({M::RenderSubmitEnd, M::PresentStart}));
    CHECK(seq.AfterPresent() == Markers({M::PresentEnd}));
    CHECK_EQ(seq.OutOfOrderCalls(), 0u);
}

TEST(Pcl_SecondEvaluateInAFrameEmitsNothing) {
    PclSequencer seq;
    seq.BeginFrame(7);
    CHECK(seq.OnEvaluate() == Markers({M::SimulationEnd, M::RenderSubmitStart}));
    CHECK(seq.OnEvaluate().empty());
    CHECK(seq.BeforePresent() == Markers({M::RenderSubmitEnd, M::PresentStart}));
    CHECK(seq.AfterPresent() == Markers({M::PresentEnd}));
    // Evaluates count per frame: the next frame's first one emits again.
    seq.BeginFrame(8);
    CHECK(seq.OnEvaluate() == Markers({M::SimulationEnd, M::RenderSubmitStart}));
    CHECK_EQ(seq.OutOfOrderCalls(), 0u);
}

TEST(Pcl_EvaluateAfterBeforePresentEmitsNothing) {
    PclSequencer seq;
    seq.BeginFrame(1);
    seq.BeforePresent();
    CHECK(seq.OnEvaluate().empty());
    CHECK(seq.AfterPresent() == Markers({M::PresentEnd}));
}

TEST(Pcl_BeforePresentTwiceIsOutOfOrder) {
    PclSequencer seq;
    seq.BeginFrame(1);
    CHECK(seq.BeforePresent().size() == 4u);
    CHECK(seq.BeforePresent().empty());
    CHECK_EQ(seq.OutOfOrderCalls(), 1u);
    // The frame is still intact and ends normally.
    CHECK(seq.AfterPresent() == Markers({M::PresentEnd}));
    CHECK_EQ(seq.OutOfOrderCalls(), 1u);
}

TEST(Pcl_AfterPresentWithoutBeforePresentIsOutOfOrder) {
    PclSequencer seq;
    seq.BeginFrame(1);
    CHECK(seq.AfterPresent().empty());
    CHECK_EQ(seq.OutOfOrderCalls(), 1u);
    CHECK(seq.InFrame());  // not ended by the refused call
    CHECK(seq.BeforePresent().size() == 4u);
    CHECK(seq.AfterPresent() == Markers({M::PresentEnd}));
    // A second AfterPresent is outside a frame.
    CHECK(seq.AfterPresent().empty());
    CHECK_EQ(seq.OutOfOrderCalls(), 2u);
}

TEST(Pcl_BeginFrameWhileAFrameIsOpenAbandonsIt) {
    PclSequencer seq;
    seq.BeginFrame(1);
    seq.OnEvaluate();
    // Frame 1 never reached AfterPresent: its missing markers are not emitted.
    CHECK(seq.BeginFrame(2) == Markers({M::SimulationStart}));
    CHECK_EQ(seq.AbandonedFrames(), 1u);
    CHECK_EQ(seq.CurrentFrame(), 2u);
    // Frame 2 starts with nothing emitted but SimulationStart.
    CHECK(seq.BeforePresent() == kAllBeforePresent);
    // Abandoned after BeforePresent as well.
    CHECK(seq.BeginFrame(3) == Markers({M::SimulationStart}));
    CHECK_EQ(seq.AbandonedFrames(), 2u);
    CHECK(seq.OnEvaluate() == Markers({M::SimulationEnd, M::RenderSubmitStart}));
    CHECK_EQ(seq.OutOfOrderCalls(), 0u);
}

TEST(Pcl_CallsBeforeAnyBeginFrameEmitNothing) {
    PclSequencer seq;
    CHECK(!seq.InFrame());
    CHECK_EQ(seq.CurrentFrame(), 0u);
    CHECK(seq.OnEvaluate().empty());
    CHECK_EQ(seq.OutOfOrderCalls(), 0u);  // an evaluate outside a frame is not a sequencing error
    CHECK(seq.BeforePresent().empty());
    CHECK_EQ(seq.OutOfOrderCalls(), 1u);
    CHECK(seq.AfterPresent().empty());
    CHECK_EQ(seq.OutOfOrderCalls(), 2u);
    CHECK(!seq.InFrame());
    CHECK_EQ(seq.AbandonedFrames(), 0u);
    // The first real frame is unaffected.
    CHECK(seq.BeginFrame(1) == Markers({M::SimulationStart}));
    CHECK_EQ(seq.AbandonedFrames(), 0u);
}

// Every sequence of 8 calls (BeginFrame with the next index, OnEvaluate,
// BeforePresent, AfterPresent): each frame's markers are always a prefix of the
// full sequence in order, so nothing is duplicated, skipped or reordered; a
// frame that got PresentEnd has all six; the counters match what happened.
TEST(Pcl_EveryCallSequenceKeepsEachFrameAnOrderedPrefix) {
    constexpr int kSteps = 8;
    int total = 1;
    for (int i = 0; i < kSteps; ++i) total *= 4;
    int bad = 0;
    for (int code = 0; code < total && bad < 5; ++code) {
        PclSequencer seq;
        uint32_t next = 1;
        std::vector<Markers> frames(kSteps + 2);  // index = frame number
        std::vector<bool> ended(kSteps + 2, false);
        uint32_t expectAbandoned = 0;
        uint32_t expectOutOfOrder = 0;
        bool open = false;
        bool presentStarted = false;
        int c = code;
        for (int step = 0; step < kSteps; ++step, c /= 4) {
            Markers out;
            switch (c % 4) {
                case 0:
                    if (open) ++expectAbandoned;
                    out = seq.BeginFrame(next++);
                    open = true;
                    presentStarted = false;
                    break;
                case 1:
                    out = seq.OnEvaluate();
                    break;
                case 2:
                    if (!open || presentStarted) ++expectOutOfOrder;
                    out = seq.BeforePresent();
                    if (open) presentStarted = true;
                    break;
                default:
                    if (!open || !presentStarted) ++expectOutOfOrder;
                    out = seq.AfterPresent();
                    if (open && presentStarted) {
                        ended[seq.CurrentFrame()] = true;
                        open = false;
                    }
                    break;
            }
            if (!out.empty()) {
                Markers& f = frames[seq.CurrentFrame()];
                f.insert(f.end(), out.begin(), out.end());
            }
        }
        bool ok = seq.AbandonedFrames() == expectAbandoned && seq.OutOfOrderCalls() == expectOutOfOrder &&
                  seq.InFrame() == open && frames[0].empty();
        for (uint32_t n = 1; n < next; ++n) {
            const Markers& f = frames[n];
            if (f.empty()) ok = false;  // BeginFrame always emits SimulationStart
            for (size_t i = 0; i < f.size(); ++i) {
                if (static_cast<size_t>(f[i]) != i) ok = false;
            }
            if (ended[n] != (f.size() == 6)) ok = false;
        }
        if (!ok) {
            ++bad;
            std::printf("  call sequence %d (base 4, least significant first) breaks the marker rules\n", code);
        }
    }
    CHECK_EQ(bad, 0);
}

TEST(Pcl_MarkerValuesEqualStreamline) {
    const std::pair<PclMarker, sl::PCLMarker> pairs[] = {
        {M::SimulationStart, sl::PCLMarker::eSimulationStart},
        {M::SimulationEnd, sl::PCLMarker::eSimulationEnd},
        {M::RenderSubmitStart, sl::PCLMarker::eRenderSubmitStart},
        {M::RenderSubmitEnd, sl::PCLMarker::eRenderSubmitEnd},
        {M::PresentStart, sl::PCLMarker::ePresentStart},
        {M::PresentEnd, sl::PCLMarker::ePresentEnd},
    };
    for (const auto& [ours, theirs] : pairs) CHECK_EQ(static_cast<uint32_t>(ours), static_cast<uint32_t>(theirs));
}
