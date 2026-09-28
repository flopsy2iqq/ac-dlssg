#pragma once
// D3D12 side of the bridge: device, queue, flip-model swap chain on the game
// window (Streamline's proxy chain in production, a plain chain without
// Streamline in unit tests), shared back buffer, fences, stall watchdog, GPU
// timing and per-second statistics (spec 6.4, 7, 9).
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <memory>
#include <string>
#include <vector>

#include "fence_pair.h"
#include "gpu_timers.h"
#include "stall_watchdog.h"

namespace acdb {

// ReShade's private IID for unwrapping its proxies (source/com_utils.hpp).
// {7F2C9A11-3B4E-4D6A-812F-5E9CD37A1B42}
extern const GUID IID_ReShadeUnwrappedObject;

class StreamlineRuntime;

struct PresenterCreateInfo {
    ID3D11Device* device11 = nullptr;   // CSP's native device
    HWND hwnd = nullptr;                // the game window
    DXGI_SWAP_CHAIN_DESC1 game_desc{};  // CSP's requested desc
    // M2: the initialised runtime selects the Streamline path; nullptr keeps
    // the plain M1 path (unit tests only; production never proxies without
    // Streamline).
    StreamlineRuntime* streamline = nullptr;
};

// Streamline path (spec 6.4, M2), differences from the plain path:
//  - creation: native D3D12CreateDevice as before, then SetDevice(native)
//    (slSetD3DDevice + feature functions), DlssgSupported(LUID) logged,
//    Upgrade(&device) and the direct queue created from the upgraded device;
//    factory from adapter->GetParent then Upgrade(&factory);
//    CreateSwapChainForHwnd on the upgraded factory returns the Streamline
//    proxy chain, checked with IsProxied (not proxied = creation failure);
//    never slUpgradeInterface on the chain. The chain never gets the
//    frame-latency waitable flag (Reflex paces instead). EnableReflexLowLatency
//    and LogLoadedModules run once; ReflexLowLatencyAvailable is logged.
//    Shared resources, fences, allocators and command lists use the native
//    device; command lists execute on the upgraded queue.
//  - frames: PclSequencer + frame tokens (spec 7): creation performs the first
//    frame start (NewFrameToken(1), ReflexSleep, SimulationStart). In
//    PresentFrame, BeforePresent's markers are emitted before the D3D12
//    Present and AfterPresent's after it; then the next frame starts
//    (NewFrameToken(n+1), ReflexSleep, SimulationStart). Every chain call
//    (GetCurrentBackBufferIndex each frame, GetBuffer, Present,
//    ResizeBuffers, SetFullscreenState) goes through the proxy chain.
//  - shutdown: the destructor drains, CPU-signals pending_wait, and when the
//    owner asked for it (ShutdownStreamlineOnRelease) calls
//    StreamlineRuntime::Shutdown before releasing any D3D12/DXGI object.

class D3D12Presenter {
public:
    // Order (spec 6.4, without the Streamline steps of M2):
    //  1. adapter = IDXGIDevice(device11)->GetAdapter; its LUID.
    //  2. D3D12CreateDevice (resolved at runtime from System32\d3d12.dll) on
    //     that adapter at D3D_FEATURE_LEVEL_12_0. If the result answers
    //     QueryInterface(IID_ReShadeUnwrappedObject), use the unwrapped device.
    //  3. Direct command queue.
    //  4. factory = adapter->GetParent(IDXGIFactory4). Tearing support from
    //     IDXGIFactory5::CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING).
    //  5. CreateSwapChainForHwnd(queue, hwnd, desc) under InternalCallScope,
    //     desc = game size, R8G8B8A8_UNORM, BufferCount 3, FLIP_DISCARD,
    //     STRETCH, ALPHA_MODE_IGNORE, Flags = ALLOW_TEARING if supported, plus
    //     FRAME_LATENCY_WAITABLE_OBJECT when the game's desc has it (M1 only:
    //     it paces the game like its own chain would; M2 drops it, because
    //     Streamline's pacer must not be starved, spec 6.3). Latency 1 until
    //     SetMaximumFrameLatency.
    //  6. MakeWindowAssociation(hwnd, NO_WINDOW_CHANGES | NO_ALT_ENTER).
    //  7. FencePair, shared back buffer (created on device11 with
    //     MISC_SHARED | MISC_SHARED_NTHANDLE, BIND_RENDER_TARGET |
    //     BIND_SHADER_RESOURCE, opened with OpenSharedHandle), per-buffer
    //     command allocators and lists, GpuTimer11/12, StallWatchdog started.
    // Any failure releases everything and returns nullptr with *error set. On
    // the Streamline path, a failure after SetDevice first calls
    // StreamlineRuntime::Shutdown (after the drain, before any release, as in
    // the destructor): Streamline must be shut down before the device it was
    // given is destroyed, and every later chain then passes through.
    static std::unique_ptr<D3D12Presenter> Create(const PresenterCreateInfo& info, std::string* error);
    ~D3D12Presenter();  // CPU-signals pending_wait, drains (500 ms max), stops the watchdog, releases

    // One CSP frame (spec 7 steps 4-7 without Streamline):
    //  - stall handling first (TakeStallPending / stalled mode / 4 s rule);
    //  - ctx4->Wait(shared11, previous frame value) so the shared texture is
    //    free, record pending_wait; CopyResource(shared tex, source) bracketed
    //    by GpuTimer11; ctx4->Signal(shared11, V); ctx->Flush();
    //  - queue->Wait(shared12, V); idx = chain->GetCurrentBackBufferIndex();
    //    wait until progress >= the value recorded for idx (500 ms max, then
    //    stall path); reset allocator/list; barrier back buffer PRESENT ->
    //    COPY_DEST; CopyResource(back buffer, shared tex) bracketed by
    //    GpuTimer12; barrier back; Close; Execute;
    //  - Present with PlanPresent(cspSync, cspFlags, tearing, windowed,
    //    fgOn=false, fgVsyncSupported=true);
    //  - V' = Next(); queue->Signal(shared12, V'); queue->Signal(progress, V');
    //    record V' for idx and as the value the next frame's ctx4->Wait uses;
    //  - waitable chain: CPU-wait (500 ms max) on its frame-latency object, so
    //    that the game starts its next frame when DXGI would let it. A timeout
    //    while the queue is behind takes the stall path.
    // Returns the Present HRESULT (or the device error in the stopped state).
    HRESULT PresentFrame(ID3D11DeviceContext* ctx, ID3D11Texture2D* source, UINT cspSync, UINT cspFlags);

    // The game's frame latency (1..16) for the D3D12 chain's waitable object.
    // No-op when the chain has none.
    void SetMaximumFrameLatency(UINT latency);

    // DXGI_PRESENT_TEST semantics (spec 6.3): the last device error if any;
    // else DXGI_STATUS_OCCLUDED if the last real Present returned it and
    // IsIconic(hwnd); else S_OK. No D3D12 call.
    HRESULT TestPresent() const;

    // Drains (500 ms, then stall path), releases every back-buffer reference,
    // ResizeBuffers(3, w, h, R8G8B8A8_UNORM, chain flags), re-fetches buffers,
    // recreates the shared back buffer when the size changed.
    HRESULT Resize(UINT width, UINT height);

    HRESULT SetFullscreenState(BOOL fullscreen, IDXGIOutput* target);
    HRESULT GetFullscreenState(BOOL* fullscreen, IDXGIOutput** target);
    HRESULT ResizeTarget(const DXGI_MODE_DESC* params);
    IDXGISwapChain4* Chain() const { return chain_.Get(); }
    bool Stopped() const { return stopped_; }
    bool HasLatencyWaitable() const;
    bool UsesStreamline() const;
    // Makes the destructor call StreamlineRuntime::Shutdown after the drain and
    // before any release (set by the final Release of a Streamline proxy).
    void ShutdownStreamlineOnRelease();
    // Frames that reached PresentEnd, and PCL sequencing problems (tests).
    uint32_t FramesWithMarkers() const;
    uint32_t MarkerProblems() const;  // abandoned frames + out-of-order calls

private:
    D3D12Presenter() = default;
    // (Implementation-private members are declared in the .cpp task.)
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Microsoft::WRL::ComPtr<IDXGISwapChain4> chain_;
    bool stopped_ = false;
};

}  // namespace acdb
