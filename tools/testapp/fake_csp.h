#pragma once
// Stand-ins for the parts of CSP that feed DLSS-G, for the test app's
// --fake-camera and --fake-ngx modes (spec 6.5, 6.6, 11). Header-only, so
// that tests/test_testapp_fakes.cpp checks them against the bridge's own
// CameraChannel and NgxHook; the test app itself still links nothing of the
// bridge and only includes these declarations.
//
//  - CameraWriter writes the camera record into the shared section exactly
//    like apps/lua/AcDlssg/AcDlssg.lua: CSP's ac.writeMemoryMappedFile creates
//    or opens Local\<name> with PAGE_READWRITE and maps it for read and write;
//    the writer continues the frame counter it finds, and every write is a
//    seqlock with forced parity (seq odd, fence, fields, fence, seq even).
//  - PathPose is a camera driving along a circle at 25 m/s, with CSP's basis
//    (side = cross(look, up)).
//  - HaltonJitter is the 8-phase Halton(2,3) sequence centred on the pixel,
//    in render pixels, as NGX's Jitter.Offset expects.
//  - FakeNgx makes CSP's DLSS calls against tests/fake_nvngx: one parameter
//    block from NVSDK_NGX_D3D11_AllocateParameters, a SuperSampling
//    CreateFeature with MVLowRes (create flags 2), and per frame an
//    EvaluateFeature on the immediate context with a real R32_TYPELESS depth
//    texture and two ping-ponged R16G16_FLOAT motion-vector textures at the
//    render size, cleared with a moving pattern, MV.Scale = -(render size),
//    the subrect = render size and Reset on the first frame of a feature.
#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "camera_layout.h"
#include "fake_nvngx/fake_nvngx.h"
#include "ngx_params.h"

namespace testapp {

// ------------------------------------------------------------------ jitter

// Element `index` (from 1) of the Halton sequence of `base`, in [0, 1).
inline double Halton(uint32_t index, uint32_t base) {
    double f = 1.0, r = 0.0;
    while (index > 0) {
        f /= base;
        r += f * (index % base);
        index /= base;
    }
    return r;
}

struct Jitter {
    float x = 0, y = 0;
};

constexpr uint32_t kJitterPhases = 8;

// The jitter of `frame`: Halton(2,3) at index (frame mod 8) + 1, minus 0.5.
inline Jitter HaltonJitter(uint32_t frame) {
    const uint32_t i = frame % kJitterPhases + 1;
    return Jitter{static_cast<float>(Halton(i, 2) - 0.5), static_cast<float>(Halton(i, 3) - 0.5)};
}

// ------------------------------------------------------------------ camera

struct CameraPose {
    float pos[3] = {}, fwd[3] = {}, up[3] = {}, side[3] = {};
};

// The camera at `seconds` into the drive: a circle of 200 m radius at 25 m/s
// with a small vertical bob, looking along the path, world up, and side =
// cross(look, up) as CSP builds its side vectors.
inline CameraPose PathPose(double seconds) {
    constexpr double kRadius = 200.0;
    constexpr double kSpeed = 25.0;
    const double a = seconds * kSpeed / kRadius;
    CameraPose p;
    p.pos[0] = static_cast<float>(kRadius * std::cos(a));
    p.pos[1] = static_cast<float>(1.5 + 0.2 * std::sin(seconds * 0.5));
    p.pos[2] = static_cast<float>(kRadius * std::sin(a));
    p.fwd[0] = static_cast<float>(-std::sin(a));
    p.fwd[1] = 0.0f;
    p.fwd[2] = static_cast<float>(std::cos(a));
    p.up[0] = 0.0f;
    p.up[1] = 1.0f;
    p.up[2] = 0.0f;
    p.side[0] = p.fwd[1] * p.up[2] - p.fwd[2] * p.up[1];
    p.side[1] = p.fwd[2] * p.up[0] - p.fwd[0] * p.up[2];
    p.side[2] = p.fwd[0] * p.up[1] - p.fwd[1] * p.up[0];
    return p;
}

// The section the Lua app opens: ac.writeMemoryMappedFile('AcDlssg.Camera.v1')
// under CSP's Local\ prefix. It is shared by every process of the logon
// session, a running game included.
constexpr wchar_t kCameraSectionName[] = L"Local\\AcDlssg.Camera.v1";

// The camera constants of the fake drive.
constexpr float kFovVDeg = 56.0f;
constexpr float kClipNear = 0.1f;
constexpr float kClipFar = 20000.0f;

class CameraWriter {
public:
    CameraWriter() = default;
    ~CameraWriter() { Close(); }
    CameraWriter(const CameraWriter&) = delete;
    CameraWriter& operator=(const CameraWriter&) = delete;

    // As ac.writeMemoryMappedFile: an existing section (the bridge creates it
    // at bootstrap, 4096 bytes) is opened with its size, else one of the
    // layout's size is created. The counter continues from the section's.
    bool Open(const wchar_t* name, std::string* error) {
        Close();
        section_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      static_cast<DWORD>(sizeof(acdb::CameraLayout)), name);
        if (!section_) return Failed("CreateFileMappingW", error);
        view_ = static_cast<acdb::CameraLayout*>(
            MapViewOfFile(section_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(acdb::CameraLayout)));
        if (!view_) return Failed("MapViewOfFile", error);
        frame_ = view_->frame;
        havePrev_ = false;
        return true;
    }

    void Close() {
        if (view_) UnmapViewOfFile(view_);
        view_ = nullptr;
        if (section_) CloseHandle(section_);
        section_ = nullptr;
    }

    bool IsOpen() const { return view_ != nullptr; }

    // One record, as the Lua app's publish(): the first write after Open has
    // no previous camera and is flagged as a jump.
    void Write(const CameraPose& pose, float renderW, float renderH, float dt, double simTimeMs) {
        if (!view_) return;
        volatile uint32_t* seq = &view_->seq;
        const uint32_t s = (*seq | 1u) & 0x7FFFFFFFu;
        *seq = s;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        ++frame_;
        view_->magic = acdb::kCameraMagic;
        view_->version = acdb::kCameraVersion;
        view_->frame = frame_;
        std::memcpy(view_->pos, pose.pos, sizeof(pose.pos));
        std::memcpy(view_->fwd, pose.fwd, sizeof(pose.fwd));
        std::memcpy(view_->up, pose.up, sizeof(pose.up));
        std::memcpy(view_->side, pose.side, sizeof(pose.side));
        view_->fovVDeg = kFovVDeg;
        view_->clipNear = kClipNear;
        view_->clipFar = kClipFar;
        view_->originShift[0] = view_->originShift[1] = view_->originShift[2] = 0.0f;
        view_->renderW = renderW;
        view_->renderH = renderH;
        view_->flags = havePrev_ ? 0u : static_cast<uint32_t>(acdb::kCamJumped);
        view_->dt = dt;
        view_->simTimeMs = simTimeMs;
        havePrev_ = true;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        *seq = s + 1;
    }

    // The frame counter of the last write.
    uint32_t Frame() const { return frame_; }

    // Test hook: leaves seq as a writer that died mid-write would.
    void SetSeqForTest(uint32_t seq) {
        if (view_) *static_cast<volatile uint32_t*>(&view_->seq) = seq;
    }

private:
    bool Failed(const char* what, std::string* error) {
        const DWORD code = GetLastError();
        Close();
        if (error) *error = std::string(what) + " failed: error " + std::to_string(code);
        return false;
    }

    HANDLE section_ = nullptr;
    acdb::CameraLayout* view_ = nullptr;
    uint32_t frame_ = 0;
    bool havePrev_ = false;
};

// ------------------------------------------------------------------ NGX

class FakeNgx {
public:
    FakeNgx() = default;
    ~FakeNgx() { Release(); }
    FakeNgx(const FakeNgx&) = delete;
    FakeNgx& operator=(const FakeNgx&) = delete;

    // Resolves the fake module's entry points and allocates the one parameter
    // block that the create and every evaluate use, as NVIDIA's helpers do.
    bool Attach(HMODULE module, std::string* error) {
        Release();
        allocate_ = reinterpret_cast<AllocateFn>(GetProcAddress(module, "NVSDK_NGX_D3D11_AllocateParameters"));
        destroy_ = reinterpret_cast<DestroyFn>(GetProcAddress(module, "NVSDK_NGX_D3D11_DestroyParameters"));
        create_ = reinterpret_cast<acdb::PfnNgxCreateFeature>(GetProcAddress(module, "NVSDK_NGX_D3D11_CreateFeature"));
        eval_ = reinterpret_cast<acdb::PfnNgxEvaluateFeature>(
            GetProcAddress(module, "NVSDK_NGX_D3D11_EvaluateFeature"));
        getState_ = reinterpret_cast<StateFn>(GetProcAddress(module, "FakeNgxGetState"));
        if (!allocate_ || !destroy_ || !create_ || !eval_ || !getState_) {
            if (error) *error = "the module does not export the fake NGX entry points";
            return false;
        }
        acdb::NgxParameter* p = nullptr;
        const int r = allocate_(&p);
        if (r != acdb::kNgxSuccess || !p) {
            if (error) *error = "NVSDK_NGX_D3D11_AllocateParameters failed: " + Hex(r);
            return false;
        }
        params_ = p;
        return true;
    }

    // (Re)creates the DLSS feature and its render-size inputs, as CSP does at
    // start and after a resize. The fake hands back a new handle each time,
    // as NGX allocates one per feature.
    bool CreateFeature(ID3D11Device* device, ID3D11DeviceContext* ctx, UINT renderW, UINT renderH, UINT outW,
                       UINT outH, std::string* error) {
        if (!params_) return Fail("not attached", error);
        if (!CreateInputs(device, renderW, renderH, error)) return false;
        getState_()->nextHandle = reinterpret_cast<void*>(kHandleBase + kHandleStep * static_cast<uintptr_t>(creates_));
        params_->Set(acdb::ngxkey::kWidth, static_cast<unsigned int>(renderW));
        params_->Set(acdb::ngxkey::kHeight, static_cast<unsigned int>(renderH));
        params_->Set(acdb::ngxkey::kOutWidth, static_cast<unsigned int>(outW));
        params_->Set(acdb::ngxkey::kOutHeight, static_cast<unsigned int>(outH));
        params_->Set(acdb::ngxkey::kCreateFlags, static_cast<int>(acdb::kNgxDlssFlagMVLowRes));
        params_->Set("PerfQualityValue", 1);  // NVSDK_NGX_PerfQuality_Value_MaxQuality, as CSP sends
        acdb::NgxHandle* handle = nullptr;
        const int r = create_(ctx, acdb::kNgxFeatureSuperSampling, params_, &handle);
        if (r != acdb::kNgxSuccess || !handle) return Fail("NVSDK_NGX_D3D11_CreateFeature failed: " + Hex(r), error);
        handle_ = handle;
        renderW_ = renderW;
        renderH_ = renderH;
        ++creates_;
        resetPending_ = true;
        return true;
    }

    // Renders this frame's depth and motion vectors and evaluates, on the
    // immediate context, after the scene and before Present.
    bool Evaluate(ID3D11DeviceContext* ctx, uint32_t frame, std::string* error) {
        if (!handle_) return Fail("no feature", error);
        const uint32_t pp = evaluates_ % 2;
        RenderInputs(ctx, frame, pp);
        const Jitter j = HaltonJitter(frame);
        params_->Set(acdb::ngxkey::kDepth, static_cast<ID3D11Resource*>(depth_.Get()));
        params_->Set(acdb::ngxkey::kMotionVectors, static_cast<ID3D11Resource*>(mvec_[pp].Get()));
        params_->Set(acdb::ngxkey::kJitterOffsetX, j.x);
        params_->Set(acdb::ngxkey::kJitterOffsetY, j.y);
        params_->Set(acdb::ngxkey::kMvScaleX, -static_cast<float>(renderW_));
        params_->Set(acdb::ngxkey::kMvScaleY, -static_cast<float>(renderH_));
        params_->Set(acdb::ngxkey::kSubrectWidth, static_cast<unsigned int>(renderW_));
        params_->Set(acdb::ngxkey::kSubrectHeight, static_cast<unsigned int>(renderH_));
        params_->Set(acdb::ngxkey::kReset, resetPending_ ? 1 : 0);
        const int r = eval_(ctx, handle_, params_, nullptr);
        resetPending_ = false;
        ++evaluates_;
        if (r != acdb::kNgxSuccess) return Fail("NVSDK_NGX_D3D11_EvaluateFeature failed: " + Hex(r), error);
        return true;
    }

    // Gives the parameter block back and drops the inputs. The module stays
    // loaded, as NGX does in CSP.
    void Release() {
        if (params_ && destroy_) destroy_(params_);
        params_ = nullptr;
        handle_ = nullptr;
        ReleaseInputs();
    }

    UINT RenderWidth() const { return renderW_; }
    UINT RenderHeight() const { return renderH_; }
    uint32_t Creates() const { return creates_; }
    uint32_t Evaluates() const { return evaluates_; }
    // The fake module's call counters (FakeNgxState); null before Attach.
    const FakeNgxState* State() const { return getState_ ? getState_() : nullptr; }

private:
    using AllocateFn = int(__cdecl*)(acdb::NgxParameter**);
    using DestroyFn = int(__cdecl*)(acdb::NgxParameter*);
    using StateFn = FakeNgxState* (*)();
    static constexpr uintptr_t kHandleBase = 0x7A0000;
    static constexpr uintptr_t kHandleStep = 0x10000;

    static std::string Hex(long v) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(v));
        return buf;
    }
    static bool Fail(const std::string& what, std::string* error) {
        if (error) *error = what;
        return false;
    }

    bool CreateInputs(ID3D11Device* device, UINT w, UINT h, std::string* error) {
        ReleaseInputs();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = 1;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.Format = DXGI_FORMAT_R32_TYPELESS;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device->CreateTexture2D(&td, nullptr, &depth_);
        if (FAILED(hr)) return Fail("depth texture creation failed: " + Hex(hr), error);
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
        dd.Format = DXGI_FORMAT_D32_FLOAT;
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        hr = device->CreateDepthStencilView(depth_.Get(), &dd, &dsv_);
        if (FAILED(hr)) return Fail("depth view creation failed: " + Hex(hr), error);
        td.Format = DXGI_FORMAT_R16G16_FLOAT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (int i = 0; i < 2; ++i) {
            hr = device->CreateTexture2D(&td, nullptr, &mvec_[i]);
            if (SUCCEEDED(hr)) hr = device->CreateRenderTargetView(mvec_[i].Get(), nullptr, &mvecRtv_[i]);
            if (FAILED(hr)) return Fail("motion-vector texture creation failed: " + Hex(hr), error);
        }
        return true;
    }

    void ReleaseInputs() {
        dsv_.Reset();
        depth_.Reset();
        for (int i = 0; i < 2; ++i) {
            mvecRtv_[i].Reset();
            mvec_[i].Reset();
        }
    }

    // Depth sweeps through [0.2, 0.8); the motion vectors are a small camera
    // motion with a rectangle moving across it (in UV units, which MV.Scale
    // turns into pixels).
    void RenderInputs(ID3D11DeviceContext* ctx, uint32_t frame, uint32_t pp) {
        ctx->ClearDepthStencilView(dsv_.Get(), D3D11_CLEAR_DEPTH, 0.2f + 0.6f * static_cast<float>(frame % 64) / 64.0f,
                                   0);
        const float background[4] = {0.0015f, 0.0f, 0.0f, 0.0f};
        Microsoft::WRL::ComPtr<ID3D11DeviceContext1> ctx1;
        if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))) {
            const float moving[4] = {-0.004f, 0.001f, 0.0f, 0.0f};
            const UINT w = renderW_ / 4 > 0 ? renderW_ / 4 : 1;
            const UINT span = renderW_ > w ? renderW_ - w : 1;
            const LONG x = static_cast<LONG>(frame * 4u % span);
            const LONG y = static_cast<LONG>(renderH_ / 3);
            const D3D11_RECT rect{x, y, x + static_cast<LONG>(w), y + static_cast<LONG>(renderH_ / 4 + 1)};
            ctx1->ClearView(mvecRtv_[pp].Get(), background, nullptr, 0);
            ctx1->ClearView(mvecRtv_[pp].Get(), moving, &rect, 1);
        } else {
            ctx->ClearRenderTargetView(mvecRtv_[pp].Get(), background);
        }
    }

    AllocateFn allocate_ = nullptr;
    DestroyFn destroy_ = nullptr;
    acdb::PfnNgxCreateFeature create_ = nullptr;
    acdb::PfnNgxEvaluateFeature eval_ = nullptr;
    StateFn getState_ = nullptr;
    acdb::NgxParameter* params_ = nullptr;
    acdb::NgxHandle* handle_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depth_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> mvec_[2];
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> mvecRtv_[2];
    UINT renderW_ = 0, renderH_ = 0;
    uint32_t creates_ = 0;
    uint32_t evaluates_ = 0;
    bool resetPending_ = false;
};

}  // namespace testapp
