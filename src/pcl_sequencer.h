#pragma once
// Per-frame PCL marker order (spec 7): every presented frame carries exactly
// one ordered SimulationStart, SimulationEnd, RenderSubmitStart,
// RenderSubmitEnd, PresentStart, PresentEnd sequence with one frame token.
// Pure state machine; the caller turns the returned markers into
// slPCLSetMarker calls with the frame's token.
#include <cstdint>
#include <vector>

namespace acdb {

// Values equal sl::PCLMarker so a cast is enough.
enum class PclMarker : uint32_t {
    SimulationStart = 0,
    SimulationEnd = 1,
    RenderSubmitStart = 2,
    RenderSubmitEnd = 3,
    PresentStart = 4,
    PresentEnd = 5,
};

class PclSequencer {
public:
    // Starts frame frameIndex (the caller's monotonic counter, first 1) and
    // returns {SimulationStart}. If the previous frame never reached
    // AfterPresent, it is abandoned without emitting its missing markers and
    // AbandonedFrames() grows.
    std::vector<PclMarker> BeginFrame(uint32_t frameIndex);

    // The first qualifying NGX evaluate of the frame (M3): returns
    // {SimulationEnd, RenderSubmitStart}; later calls in the same frame, or
    // calls outside a frame, return {}.
    std::vector<PclMarker> OnEvaluate();

    // Right before the D3D12 Present: the markers not yet emitted among
    // SimulationEnd and RenderSubmitStart, then RenderSubmitEnd and
    // PresentStart. Outside a frame, or when called twice, returns {} and
    // OutOfOrderCalls() grows.
    std::vector<PclMarker> BeforePresent();

    // Right after the D3D12 Present: {PresentEnd}, which ends the frame.
    // Without a preceding BeforePresent, returns {} and OutOfOrderCalls() grows.
    std::vector<PclMarker> AfterPresent();

    uint32_t CurrentFrame() const { return frame_; }
    bool InFrame() const { return in_frame_; }
    uint32_t AbandonedFrames() const { return abandoned_; }
    uint32_t OutOfOrderCalls() const { return out_of_order_; }

private:
    uint32_t frame_ = 0;
    uint32_t emitted_ = 0;  // bit i set: PclMarker value i emitted for frame_
    bool in_frame_ = false;
    uint32_t abandoned_ = 0;
    uint32_t out_of_order_ = 0;
};

}  // namespace acdb
