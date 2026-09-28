#include "pcl_sequencer.h"

#include <sl_pcl.h>

namespace acdb {

namespace {

// The caller casts PclMarker to sl::PCLMarker, so the values must match.
constexpr bool SameValue(PclMarker ours, sl::PCLMarker theirs) {
    return static_cast<uint32_t>(ours) == static_cast<uint32_t>(theirs);
}
static_assert(SameValue(PclMarker::SimulationStart, sl::PCLMarker::eSimulationStart));
static_assert(SameValue(PclMarker::SimulationEnd, sl::PCLMarker::eSimulationEnd));
static_assert(SameValue(PclMarker::RenderSubmitStart, sl::PCLMarker::eRenderSubmitStart));
static_assert(SameValue(PclMarker::RenderSubmitEnd, sl::PCLMarker::eRenderSubmitEnd));
static_assert(SameValue(PclMarker::PresentStart, sl::PCLMarker::ePresentStart));
static_assert(SameValue(PclMarker::PresentEnd, sl::PCLMarker::ePresentEnd));

constexpr uint32_t Bit(PclMarker m) { return 1u << static_cast<uint32_t>(m); }

}  // namespace

std::vector<PclMarker> PclSequencer::BeginFrame(uint32_t frameIndex) {
    if (in_frame_) ++abandoned_;
    frame_ = frameIndex;
    emitted_ = Bit(PclMarker::SimulationStart);
    in_frame_ = true;
    return {PclMarker::SimulationStart};
}

std::vector<PclMarker> PclSequencer::OnEvaluate() {
    // Only the first evaluate before BeforePresent marks the end of simulation.
    const uint32_t done = Bit(PclMarker::SimulationEnd) | Bit(PclMarker::RenderSubmitEnd);
    if (!in_frame_ || (emitted_ & done)) return {};
    emitted_ |= Bit(PclMarker::SimulationEnd) | Bit(PclMarker::RenderSubmitStart);
    return {PclMarker::SimulationEnd, PclMarker::RenderSubmitStart};
}

std::vector<PclMarker> PclSequencer::BeforePresent() {
    if (!in_frame_ || (emitted_ & Bit(PclMarker::PresentStart))) {
        ++out_of_order_;
        return {};
    }
    std::vector<PclMarker> out;
    for (PclMarker m : {PclMarker::SimulationEnd, PclMarker::RenderSubmitStart, PclMarker::RenderSubmitEnd,
                        PclMarker::PresentStart}) {
        if (!(emitted_ & Bit(m))) out.push_back(m);
        emitted_ |= Bit(m);
    }
    return out;
}

std::vector<PclMarker> PclSequencer::AfterPresent() {
    if (!in_frame_ || !(emitted_ & Bit(PclMarker::PresentStart))) {
        ++out_of_order_;
        return {};
    }
    emitted_ |= Bit(PclMarker::PresentEnd);
    in_frame_ = false;
    return {PclMarker::PresentEnd};
}

}  // namespace acdb
