#include "capture_coordinator.h"

#include <cstdio>

#include "capture_slots.h"
#include "d3d12_presenter.h"
#include "fence_pair.h"
#include "log.h"

using Microsoft::WRL::ComPtr;

namespace acdb {

namespace {

constexpr uint32_t kIdleFramesBeforeDrop = 60;  // frames without an evaluate before DropSourceViews

// A ReShade proxy answers IID_ReShadeUnwrappedObject with the object it wraps.
ComPtr<IUnknown> NativeIdentity(ID3D11Device* device) {
    if (!device) return nullptr;
    ComPtr<IUnknown> unwrapped;
    IUnknown* native = device;
    if (SUCCEEDED(device->QueryInterface(IID_ReShadeUnwrappedObject,
                                         reinterpret_cast<void**>(unwrapped.GetAddressOf()))) &&
        unwrapped)
        native = unwrapped.Get();
    ComPtr<IUnknown> identity;
    native->QueryInterface(IID_PPV_ARGS(&identity));
    return identity;
}

bool TextureDesc(ID3D11Resource* resource, D3D11_TEXTURE2D_DESC* desc) {
    if (!resource) return false;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&tex)))) return false;
    tex->GetDesc(desc);
    return true;
}

}  // namespace

const char* CameraReadResultText(CameraChannel::ReadResult r) {
    switch (r) {
        case CameraChannel::ReadResult::Ok: return "ok";
        case CameraChannel::ReadResult::NoSection: return "no camera section";
        case CameraChannel::ReadResult::NotWritten: return "not written (is the AcDlssg Lua app running?)";
        case CameraChannel::ReadResult::Torn: return "torn read";
        case CameraChannel::ReadResult::WriteFailed: return "the Lua app's write failed";
    }
    return "?";
}

CaptureCoordinator::CaptureCoordinator(const Deps& deps) : deps_(deps), throttle_(10000) {
    native_identity_ = NativeIdentity(deps_.device11);
}

CaptureCoordinator::~CaptureCoordinator() = default;

bool CaptureCoordinator::SameDevice(ID3D11DeviceContext* ctx) {
    if (ctx == checked_ctx_) return checked_ok_;
    ComPtr<ID3D11Device> owner;
    ctx->GetDevice(&owner);
    checked_ctx_ = ctx;
    checked_ok_ = owner && native_identity_ && NativeIdentity(owner.Get()).Get() == native_identity_.Get();
    return checked_ok_;
}

void CaptureCoordinator::LogThrottled(const std::string& what) {
    if (throttle_.ShouldLog(what, GetTickCount64())) LOGW("capture: %s (logged at most every 10 s)", what.c_str());
}

void CaptureCoordinator::OnCreateFeature(uint64_t featureKey, const NgxCreateInfo& info) {
    try {
        std::lock_guard<std::mutex> lock(mu_);
        creates_[featureKey] = info;
        for (bool& f : force_) f = true;
        create_off_ = true;
        LOGI("capture: DLSS feature created: render %ux%u, output %ux%u, create flags 0x%X; capture slots are "
             "recreated and DLSS-G is off for this frame",
             info.width, info.height, info.outWidth, info.outHeight, info.createFlags);
    } catch (...) {
    }
}

void CaptureCoordinator::OnEvaluate(const NgxEvaluateInputs& in) {
    try {
        std::lock_guard<std::mutex> lock(mu_);
        if (!in.ctx) return;
        if (in.ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
            if (frame_.evaluates == 0) frame_.reason = "no capture: DLSS evaluate on a deferred context";
            LogThrottled("DLSS evaluate on a deferred context ignored");
            return;
        }
        if (!SameDevice(in.ctx)) {
            if (frame_.evaluates == 0)
                frame_.reason = "no capture: the DLSS evaluate's device is not the game's D3D11 device";
            LogThrottled("DLSS evaluate on another D3D11 device ignored");
            return;
        }
        ++frame_.evaluates;
        LOGT_ONCE_N(12, "trace: capture OnEvaluate: evaluate %u of this frame", frame_.evaluates);
        if (frame_.evaluates == 1) {
            // Spec 7 step 2: the camera is latched once per bridge frame.
            CameraLayout snap{};
            frame_.cameraResult = deps_.camera ? deps_.camera->Read(&snap) : CameraChannel::ReadResult::NoSection;
            if (frame_.cameraResult == CameraChannel::ReadResult::Ok) {
                frame_.camera = snap;
                frame_.cameraFresh = latch_.Latch(snap);
                if (frame_.cameraFresh && !logged_first_fresh_) {
                    logged_first_fresh_ = true;
                    const CameraLayout& c = snap;
                    LOGI("camera: first fresh snapshot: pos (%.3f, %.3f, %.3f) fwd (%.4f, %.4f, %.4f) up (%.4f, "
                         "%.4f, %.4f) fov %.3f near %.4f far %.1f render %.0fx%.0f origin shift (%.3f, %.3f, %.3f)",
                         c.pos[0], c.pos[1], c.pos[2], c.fwd[0], c.fwd[1], c.fwd[2], c.up[0], c.up[1], c.up[2],
                         c.fovVDeg, c.clipNear, c.clipFar, c.renderW, c.renderH, c.originShift[0], c.originShift[1],
                         c.originShift[2]);
                }
            }
            LOGT_ONCE_N(12, "trace: capture OnEvaluate: camera %s, markers next",
                        CameraReadResultText(frame_.cameraResult));
            if (deps_.onFirstEvaluate) deps_.onFirstEvaluate();
            LOGT_ONCE_N(12, "trace: capture OnEvaluate: markers done");
        }
        Capture(in);
    } catch (...) {
    }
}

// Caller holds mu_. The last evaluate of the frame that reaches the slot
// wins. One refused before that (null resources, an unknown render size, a
// slot the GPU may still read) keeps an earlier capture of the frame and its
// parameters (review finding F5); Reset is OR-ed over every counted evaluate.
void CaptureCoordinator::Capture(const NgxEvaluateInputs& in) {
    const uint32_t slot = static_cast<uint32_t>(frame_index_ % CaptureSlots::kSlots);
    frame_.slot = slot;
    const bool reset = frame_.params.ngxReset || in.reset;
    frame_.params.ngxReset = reset;
    // Why this evaluate is not captured; an earlier capture keeps its reason.
    const auto refuse = [this](std::string why) {
        if (!frame_.captured) frame_.reason = std::move(why);
    };

    const auto create = creates_.find(in.featureKey);
    const uint32_t outWidth = create != creates_.end() ? create->second.outWidth : 0;
    const uint32_t outHeight = create != creates_.end() ? create->second.outHeight : 0;
    if (!frame_.captured) {
        frame_.outWidth = outWidth;
        frame_.outHeight = outHeight;
    }

    D3D11_TEXTURE2D_DESC dd{};
    D3D11_TEXTURE2D_DESC md{};
    const bool haveDepth = TextureDesc(in.depth, &dd);
    const bool haveMvec = TextureDesc(in.mvec, &md);
    if (!logged_first_evaluate_) {
        logged_first_evaluate_ = true;
        LOGI("capture: first counted evaluate: depth %d %ux%u, mvec %d %ux%u, subrect %ux%u, create flags 0x%X, mv "
             "scale %.3f,%.3f, jitter %.4f,%.4f",
             haveDepth ? static_cast<int>(dd.Format) : 0, dd.Width, dd.Height,
             haveMvec ? static_cast<int>(md.Format) : 0, md.Width, md.Height, in.subrectW, in.subrectH,
             in.createFlags, in.mvScaleX, in.mvScaleY, in.jitterX, in.jitterY);
    }

    // Phase-1 mismatch list: null resources are no capture, before Matches,
    // so that one bad evaluate neither destroys a slot nor costs a GPU wait.
    if (!in.depth) {
        refuse("no capture: NGX reported no depth");
        return;
    }
    if (!in.mvec) {
        refuse("no capture: NGX reported no motion vectors");
        return;
    }

    // Subrect 0: the create's render size, else the depth texture's size.
    uint32_t w = in.subrectW;
    uint32_t h = in.subrectH;
    if (w == 0 || h == 0) {
        if (create != creates_.end() && create->second.width && create->second.height) {
            w = create->second.width;
            h = create->second.height;
        } else if (haveDepth) {
            w = dd.Width;
            h = dd.Height;
        }
    }
    if (w == 0 || h == 0) {
        refuse("no capture: the render size is unknown");
        return;
    }

    CaptureSlots* slots = deps_.slots;
    FencePair* fences = deps_.fences;
    if (!slots || !fences) {
        refuse("no capture: no capture slots");
        return;
    }
    const bool matches = slots->Matches(slot, in.depth, in.mvec);
    const SlotAction action =
        DecideSlot(matches, force_[slot], slots->HasTextures(slot), fences->ProgressValue(), last_use_[slot]);
    if (action == SlotAction::Skip) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "no capture: capture slot %u is still in use by the GPU", slot);
        refuse(buf);
        return;
    }

    // From here on the slot changes: this evaluate replaces an earlier capture.
    frame_.captured = false;
    frame_.params = CaptureParams();
    frame_.params.jitterX = in.jitterX;
    frame_.params.jitterY = in.jitterY;
    frame_.params.mvScaleX = in.mvScaleX;
    frame_.params.mvScaleY = in.mvScaleY;
    frame_.params.createFlags = in.createFlags;
    frame_.params.ngxReset = reset;
    frame_.params.renderW = w;
    frame_.params.renderH = h;
    frame_.mvScaleMissing = in.mvScaleX == 0.0f || in.mvScaleY == 0.0f;
    frame_.outWidth = outWidth;
    frame_.outHeight = outHeight;
    std::string err;
    if (action == SlotAction::Recreate) {
        force_[slot] = false;
        if (!slots->Recreate(slot, in.depth, in.mvec, &err)) {
            frame_.reason = "no capture: " + err;
            LogThrottled(err);
            return;
        }
        LOGI("capture: slot %u (re)created: depth %d %ux%u, mvec %d %ux%u", slot, static_cast<int>(dd.Format),
             dd.Width, dd.Height, static_cast<int>(md.Format), md.Width, md.Height);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "capture slot %u recreated", slot);
        frame_.forcedOff = true;
        if (!create_off_) frame_.reason = buf;
    }
    if (!slots->Copy(in.ctx, slot, in.depth, in.mvec, &err)) {
        frame_.reason = "no capture: " + err;
        LogThrottled(err);
        return;
    }
    // No fence signal here (review finding F1): a value from the shared
    // fence's counter would lie above the one CSP's next Present waits for,
    // so it would release that wait before the D3D12 queue is done with the
    // shared back buffer, and the queue's later, lower signal would move the
    // fence backwards. The Present's own signal follows this copy on the same
    // immediate context, and the D3D12 queue waits for it before anything
    // reads the slot.
    frame_.captured = true;
    frame_.mvecWidth = md.Width;
    frame_.mvecHeight = md.Height;
    frame_.mvecFormat = slots->MvecFormat(slot);
}

FrameCapture CaptureCoordinator::EndFrame() {
    std::lock_guard<std::mutex> lock(mu_);
    if (frame_.evaluates > 0) idle_frames_ = 0;
    if (frame_.evaluates == 0) {
        // CSP stopped calling DLSS (another AA mode, a menu, a resize while
        // DLSS is off): do not keep its depth texture alive (review F4).
        if (idle_frames_ < kIdleFramesBeforeDrop && ++idle_frames_ == kIdleFramesBeforeDrop && deps_.slots &&
            deps_.slots->CachedSourceViews() > 0) {
            deps_.slots->DropSourceViews();
            LOGI("capture: no DLSS evaluate for %u frames; the cached view of CSP's depth texture was released",
                 kIdleFramesBeforeDrop);
        }
        // Keeps freshness exact: the next frame's latch compares with a frame
        // counter read during this frame, not with an older captured frame.
        CameraLayout snap{};
        if (deps_.camera && deps_.camera->Read(&snap) == CameraChannel::ReadResult::Ok) latch_.Latch(snap);
    }
    if (create_off_) {
        frame_.forcedOff = true;
        frame_.reason = "DLSS feature re-created";
        create_off_ = false;
    }
    FrameCapture out = std::move(frame_);
    frame_ = FrameCapture();
    ++frame_index_;
    return out;
}

void CaptureCoordinator::NoteTagged(uint32_t slot, uint64_t progressValue) {
    std::lock_guard<std::mutex> lock(mu_);
    if (slot < CaptureSlots::kSlots && progressValue > last_use_[slot]) last_use_[slot] = progressValue;
}

void CaptureCoordinator::ResetCamera() {
    std::lock_guard<std::mutex> lock(mu_);
    latch_.Reset();
}


}  // namespace acdb
