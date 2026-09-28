#pragma once
// CaptureCoordinator: the NGX evaluate sink the presenter owns (spec 6.4
// "Capture slots", 6.5 "Copying into slot N mod 3", 6.6 "Freshness", 7 step
// 2). It turns CSP's counted DLSS evaluates into one capture per bridge frame:
//
// Render thread (OnEvaluate, after NGX's original returned, on CSP's
// immediate context):
//  - an evaluate on a deferred context, or on a context whose device is not
//    the game's D3D11 device (native identities compared, so a ReShade proxy
//    counts as the device it wraps), is not CSP's: no capture, no markers;
//  - the first qualifying evaluate of the frame reads the camera
//    (CameraChannel::Read) and, when Ok, latches it (CameraLatch::Latch) once
//    for the frame, and calls onFirstEvaluate (the presenter emits PCL
//    SimulationEnd and RenderSubmitStart); later evaluates reuse that result;
//  - null depth or motion vectors: no capture this frame (the slot is kept);
//  - slot = frame index mod 3, and DecideSlot (fg_policy.h) with the progress
//    fence and the value that last tagged the slot: Copy, Recreate (the frame
//    is then forced off), or Skip (no capture until the GPU is done);
//    CaptureSlots refusing the sources (format, MSAA, ...) is no capture;
//  - CaptureSlots::Copy, then ID3D11DeviceContext4::Signal(shared fence, V)
//    on the given context with V from the presenter's single counter;
//  - the last evaluate of the frame wins for the resources and parameters;
//    Reset is OR-ed over the frame's evaluates;
//  - subrect 0 falls back to the feature's create Width/Height, else the
//    depth texture's size; an MV scale of 0 is flagged (DLSS-G off).
// OnCreateFeature records the create parameters per feature, forces every
// slot to be recreated at its next turn (after progress passed its last tag)
// and forces the current frame off.
//
// Present thread: EndFrame hands the frame's capture to the Present that ends
// it and starts the next frame. A frame without a qualifying evaluate latches
// the camera there, so that freshness always means "written during this
// frame". NoteTagged records the progress value after which the D3D12 side is
// done with a slot; ResetCamera resets the latch (resize, target change).
//
// All methods are serialised by one mutex; nothing waits on the GPU.
#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include "camera_channel.h"
#include "fg_policy.h"
#include "frame_constants.h"
#include "ngx_hook.h"

namespace acdb {

class CaptureSlots;
class FencePair;

// A CameraChannel::ReadResult in words, for the frame's DLSS-G-off reason.
const char* CameraReadResultText(CameraChannel::ReadResult r);

struct FrameCapture {
    uint32_t evaluates = 0;   // qualifying evaluates since the previous Present
    bool captured = false;    // copied into slot and the shared fence signalled
    uint32_t slot = 0;        // this frame's slot (frame index mod 3)
    uint64_t fenceValue = 0;  // shared-fence value signalled after the copy
    // renderW/H: the tag extent and the MV scale's reference size (the
    // subrect after its fallback).
    CaptureParams params;
    bool mvScaleMissing = false;
    bool forcedOff = false;  // a slot was recreated or the DLSS feature re-created
    std::string reason;      // why nothing was captured, or why the frame is forced off
    // The camera as latched for this frame (read once at its first qualifying
    // evaluate); camera is valid only when cameraResult is Ok.
    CameraChannel::ReadResult cameraResult = CameraChannel::ReadResult::NoSection;
    bool cameraFresh = false;
    CameraLayout camera{};
    // The captured textures, for DLSS-G's size and format hints.
    uint32_t mvecWidth = 0, mvecHeight = 0;
    DXGI_FORMAT mvecFormat = DXGI_FORMAT_UNKNOWN;  // the slot's typed format
    // NGX OutWidth/OutHeight of the evaluated feature (0 when its create was
    // not observed), for the runtime aspect test of spec 6.10.
    uint32_t outWidth = 0, outHeight = 0;
};

class CaptureCoordinator final : public NgxEvaluateSink {
public:
    struct Deps {
        ID3D11Device* device11 = nullptr;  // the game's (native) D3D11 device
        CaptureSlots* slots = nullptr;
        FencePair* fences = nullptr;
        const CameraChannel* camera = nullptr;
        std::function<void()> onFirstEvaluate;  // called under the coordinator's lock
    };
    explicit CaptureCoordinator(const Deps& deps);
    ~CaptureCoordinator() override;
    CaptureCoordinator(const CaptureCoordinator&) = delete;
    CaptureCoordinator& operator=(const CaptureCoordinator&) = delete;

    void OnCreateFeature(uint64_t featureKey, const NgxCreateInfo& info) override;
    void OnEvaluate(const NgxEvaluateInputs& in) override;

    FrameCapture EndFrame();
    void NoteTagged(uint32_t slot, uint64_t progressValue);
    void ResetCamera();

private:
    bool SameDevice(ID3D11DeviceContext* ctx);
    ID3D11DeviceContext4* Ctx4For(ID3D11DeviceContext* ctx);
    void Capture(const NgxEvaluateInputs& in);
    void LogThrottled(const std::string& what);

    Deps deps_;
    std::mutex mu_;
    Microsoft::WRL::ComPtr<IUnknown> native_identity_;
    ID3D11DeviceContext* checked_ctx_ = nullptr;
    bool checked_ok_ = false;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> ctx4_;
    ID3D11DeviceContext* ctx4_of_ = nullptr;
    CameraLatch latch_;
    uint64_t frame_index_ = 1;  // the bridge frame in progress
    FrameCapture frame_;
    bool create_off_ = false;  // a counted CreateFeature happened during this frame
    bool force_[3] = {};
    uint64_t last_use_[3] = {};
    std::unordered_map<uint64_t, NgxCreateInfo> creates_;
    ReasonThrottle throttle_;
    bool logged_first_evaluate_ = false;
    bool logged_first_fresh_ = false;
};

}  // namespace acdb
