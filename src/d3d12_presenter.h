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

#include "config.h"
#include "fence_pair.h"
#include "gpu_timers.h"
#include "stall_watchdog.h"

namespace acdb {

// ReShade's private IID for unwrapping its proxies (source/com_utils.hpp).
// {7F2C9A11-3B4E-4D6A-812F-5E9CD37A1B42}
extern const GUID IID_ReShadeUnwrappedObject;

class StreamlineRuntime;
class CameraChannel;
class NgxEvaluateSink;
class PanelStatusChannel;
class PanelControlChannel;

// What FactoryHook knows from the bootstrap (M3). The defaults are what the
// unit tests need: no NGX hook, the process-wide camera channel.
struct PresenterEnvironment {
    // Install NgxHook once per process (first presenter) and attach this
    // presenter's capture coordinator with NgxHook::SetSink.
    bool install_ngx_hook = false;
    bool spoof_loaded = false;      // dlssg_for_sm86's version.dll from the game folder
    bool allow_stretching = false;  // dxgi_tweaks.ini ALLOW_STRETCHING=1 (runtime aspect test)
    const CameraChannel* camera = nullptr;  // nullptr: CameraChannel::Get()
    // Spec 6.9, the in-game panel: the status section the presenter publishes
    // into and the control section it reads the Lua app's requests from, and
    // ac-dlssg.ini for "Save as default". nullptr (the default, and the unit
    // tests') publishes nothing and reads no request.
    PanelStatusChannel* status = nullptr;
    const PanelControlChannel* control = nullptr;
    std::wstring config_path;
};

struct PresenterCreateInfo {
    ID3D11Device* device11 = nullptr;   // CSP's native device
    HWND hwnd = nullptr;                // the game window
    DXGI_SWAP_CHAIN_DESC1 game_desc{};  // CSP's requested desc
    // M2: the initialised runtime selects the Streamline path; nullptr keeps
    // the plain M1 path (unit tests only; production never proxies without
    // Streamline).
    StreamlineRuntime* streamline = nullptr;
    Config config;              // M3: start_with_fg, hotkey, camera switches, DLSS-G policy
    PresenterEnvironment env;   // M3
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
//
// M3 (DLSS-G), Streamline path:
//  - creation: when DlssgSupported (slIsFeatureSupported and the DLSS-G
//    functions) is false and config.proxy_without_fg is 0, creation fails
//    with DlssgUnsupportedMessage (after SetDevice, so Streamline is shut down
//    before the device is released, and the chain passes through). Both
//    paths create CaptureSlots on the native device pair and a
//    CaptureCoordinator; with env.install_ngx_hook the first presenter of the
//    process installs NgxHook, and every presenter attaches its coordinator
//    (SetSink) as the last step of creation and detaches it first thing in
//    the destructor. "NGX hook: installed on <n> module(s): <names>" is
//    logged then and whenever the number of hooked modules changes.
//  - PresentFrame: NgxHook::ProcessPendingRescan first; the hotkey (Ctrl+F10
//    by default, only while the game window is in the foreground) toggles
//    DLSS-G; the coordinator's frame ends (EndFrame); the D3D12 queue's wait
//    for the Present's value V also covers the capture, whose copy precedes
//    V's signal on the immediate context (the capture signals nothing,
//    review finding F1); the gate
//    (DecideFg) with BuildFrameConstants (frame N-1's snapshot as prev, the
//    subrect as the render size when the camera's aspect differs by more
//    than 0.5%, reset when originShift changed or frame N-1 had no DLSS-G
//    inputs) and the video memory guard decides DLSS-G for this Present;
//    tags (the open command list, extent {0,0,subrect}) and constants are
//    set when DLSS-G is on, or with tag_without_fg when the inputs exist;
//    null tags when the previous frame was tagged and this one is not;
//    SetDlssgOptions only when the mode (or, while on, the size hints)
//    changes; PlanPresent with the mode and bIsVsyncSupportAvailable;
//    slDLSSGGetState (no options) after every Present DLSS-G was on for:
//    numFramesActuallyPresented - 1 frames were generated ("fg: DLSS-G
//    active" at the first; the first three raw answers are logged); its
//    status, or a failed call, is acted on every 60 frames, counted whatever
//    the mode (StatusPollClock), and keeps DLSS-G off until the next resize
//    or toggle.
//  - Resize, SetFullscreenState, ResizeTarget: with DLSS-G on, eOff and null
//    tags, then the last frame is presented once more with its own token and
//    the full marker sequence (not a CSP frame); the camera latch restarts.
//  - stalls: entering stalled mode sets eOff.
//  - log lines and stats fields: see the M3 log contract (tests).
//
// The in-game panel (spec 6.9), both paths, with env.status and env.control:
//  - creation: the control record's current requestCounter is the baseline
//    (a request from before this presenter is never applied), and the
//    status is published: bridgeState kPanelFgAvailable when Streamline
//    supports DLSS-G, else kPanelProxyNoFg with the reason as stateReason.
//  - PresentFrame, first thing after the NGX rescan: one compare of the
//    control record's seq with the last one seen; when it changed, a seqlock
//    read (a torn one is retried at the next frame) and DecideControl. A new
//    request sets the DLSS-G switch exactly as the hotkey does ("fg: panel ->
//    on|off": the next DLSS-G frame has reset, a failure status is retried),
//    sets the camera switches for this frame on ("panel: camera_flip_handedness
//    0 -> 1"; the next DLSS-G frame has reset), and with saveAsDefault writes
//    start_with_fg and both camera switches into env.config_path with
//    WriteIniKeys ("panel: saved ..." or the WARN "panel: Save as default
//    failed: ..."); the status is published at once.
//  - the status is published again after every statistics line (fps, bridge
//    GPU ms, video memory, per-second counts) and at the end of every frame
//    whose DLSS-G mode or user switch changed; the final release publishes
//    kPanelPassThrough, "the game's swap chain was released".

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
    //  8. Logs the render adapter's local video memory (IDXGIAdapter3::
    //     QueryVideoMemoryInfo(0, LOCAL)): "presenter: VRAM (local) budget
    //     <b> MiB, usage <u> MiB". The per-second stats line ends with
    //     "vram_mib=<usage>/<budget>" (n/a when the adapter cannot tell).
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

    // M3. The capture coordinator NgxHook reports to (tests feed it directly).
    NgxEvaluateSink* CaptureSink() const;
    // Streamline accepted DLSS-G on this adapter and its functions resolved.
    bool DlssgSupported() const;
    // Counts since creation, the per-second stats fields' totals (tests).
    struct FgTotals {
        uint32_t captures = 0;          // Presents paired with a capture
        uint32_t camera_fresh = 0;      // ... whose camera snapshot was fresh
        uint32_t tagged = 0;            // Presents whose tags and constants Streamline accepted
        uint32_t fg_frames = 0;         // Presents with DLSS-G on
        uint32_t double_evaluates = 0;  // frames with more than one counted evaluate
    };
    FgTotals Totals() const;

private:
    D3D12Presenter() = default;
    // (Implementation-private members are declared in the .cpp task.)
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Microsoft::WRL::ComPtr<IDXGISwapChain4> chain_;
    bool stopped_ = false;
};

}  // namespace acdb
