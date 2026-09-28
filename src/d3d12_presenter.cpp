#include "d3d12_presenter.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstdlib>
#include <exception>
#include <iterator>
#include <mutex>
#include <thread>

#include <cstring>

#include "camera_channel.h"
#include "capture_coordinator.h"
#include "capture_slots.h"
#include "compat.h"
#include "fg_policy.h"
#include "frame_constants.h"
#include "gpu_info.h"
#include "internal_call.h"
#include "log.h"
#include "ngx_hook.h"
#include "pcl_sequencer.h"
#include "present_flags.h"
#include "streamline_runtime.h"

using Microsoft::WRL::ComPtr;

namespace acdb {

// Declared extern in the header, so this definition has external linkage.
const GUID IID_ReShadeUnwrappedObject = {0x7F2C9A11, 0x3B4E, 0x4D6A, {0x81, 0x2F, 0x5E, 0x9C, 0xD3, 0x7A, 0x1B, 0x42}};

namespace {

constexpr UINT kBuffers = 3;
constexpr DWORD kCpuWaitMs = 500;     // every CPU wait on the render thread (spec 6.4)
constexpr DWORD kGiveUpMs = 4000;     // no progress for this long stops the presenter (spec 9)
constexpr DWORD kPumpSliceMs = 10;
constexpr uint64_t kDebugStallFrame = 30;
constexpr DWORD kDebugStallMaxMs = 60000;
constexpr wchar_t kDebugStallEnv[] = L"ACDLSSG_DEBUG_STALL_MS";

std::string HrText(const char* what, HRESULT hr) {
    char buf[200];
    std::snprintf(buf, sizeof(buf), "%s failed: 0x%08lX", what, static_cast<unsigned long>(hr));
    return buf;
}

bool IsDeviceError(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DEVICE_RESET ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

// What a stopped presenter reports from Present (spec 9).
HRESULT StopErrorFor(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_HUNG ? DXGI_ERROR_DEVICE_HUNG : DXGI_ERROR_DEVICE_REMOVED;
}

// d3d12.dll by absolute path: the core must not import it (spec 6.1). The
// module is never freed; devices created through it outlive any presenter.
PFN_D3D12_CREATE_DEVICE LoadD3D12CreateDevice(std::string* err) {
    static std::once_flag once;
    static PFN_D3D12_CREATE_DEVICE fn = nullptr;
    static std::string load_error;
    std::call_once(once, [] {
        wchar_t dir[MAX_PATH] = {};
        const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            load_error = "GetSystemDirectoryW failed";
            return;
        }
        std::wstring path(dir, n);
        path += L"\\d3d12.dll";
        const HMODULE module = LoadLibraryW(path.c_str());
        if (!module) {
            load_error = HrText("LoadLibraryW(System32\\d3d12.dll)", HRESULT_FROM_WIN32(GetLastError()));
            return;
        }
        fn = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(module, "D3D12CreateDevice"));
        if (!fn) load_error = "d3d12.dll has no D3D12CreateDevice export";
    });
    if (!fn && err) *err = load_error;
    return fn;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

bool IsRgba8Family(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_R8G8B8A8_TYPELESS;
}

void FormatAvg(double sum, int count, char* out, size_t size) {
    if (count > 0)
        std::snprintf(out, size, "%.3f", sum / count);
    else
        std::snprintf(out, size, "n/a");
}

int64_t QpcNow() {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double QpcMs(int64_t ticks) {
    static const double freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? static_cast<double>(f.QuadPart) : 1.0;
    }();
    return static_cast<double>(ticks) * 1000.0 / freq;
}

constexpr unsigned kMaxOcclusionLogs = 100;  // per presenter; the stats line keeps counting

// Local video memory of the render adapter for this process, in bytes. False
// (with *err set when err is given) when the adapter cannot tell.
bool QueryVramBytes(IDXGIAdapter3* adapter, uint64_t* usage, uint64_t* budget, std::string* err) {
    if (!adapter) {
        if (err) *err = "the adapter has no IDXGIAdapter3";
        return false;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    const HRESULT hr = adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
    if (FAILED(hr)) {
        if (err) *err = HrText("IDXGIAdapter3::QueryVideoMemoryInfo", hr);
        return false;
    }
    *usage = info.CurrentUsage;
    *budget = info.Budget;
    return true;
}

// The same in MiB.
bool QueryVramMiB(IDXGIAdapter3* adapter, uint64_t* usage, uint64_t* budget, std::string* err) {
    if (!QueryVramBytes(adapter, usage, budget, err)) return false;
    *usage /= 1024 * 1024;
    *budget /= 1024 * 1024;
    return true;
}

constexpr unsigned kStateAnswersLogged = 3;  // the first slDLSSGGetState answers at INFO
constexpr uint32_t kViewport = 0;

// NgxHook is installed once per process, by the first presenter that asks.
std::atomic<bool> g_ngx_installed{false};

bool SameHints(const DlssgSizeHints& a, const DlssgSizeHints& b) {
    return a.numBackBuffers == b.numBackBuffers && a.mvecDepthWidth == b.mvecDepthWidth &&
           a.mvecDepthHeight == b.mvecDepthHeight && a.colorWidth == b.colorWidth && a.colorHeight == b.colorHeight &&
           a.colorBufferFormat == b.colorBufferFormat && a.mvecBufferFormat == b.mvecBufferFormat &&
           a.depthBufferFormat == b.depthBufferFormat;
}

// The file names of the loaded modules NgxHook hooks: every module that
// exports the NGX D3D11 entry points and passes NgxClassifyModule.
std::string NgxModuleNames() {
    using EnumFn = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD);
    const HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    const auto enumModules = k32 ? reinterpret_cast<EnumFn>(GetProcAddress(k32, "K32EnumProcessModules")) : nullptr;
    if (!enumModules) return "(module list unavailable)";
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!enumModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return "(module list unavailable)";
    const DWORD count = (needed < sizeof(mods) ? needed : static_cast<DWORD>(sizeof(mods))) / sizeof(HMODULE);
    const HMODULE host = GetModuleHandleW(nullptr);
    std::string names;
    for (DWORD i = 0; i < count; ++i) {
        void* create = reinterpret_cast<void*>(GetProcAddress(mods[i], "NVSDK_NGX_D3D11_CreateFeature"));
        void* eval = reinterpret_cast<void*>(GetProcAddress(mods[i], "NVSDK_NGX_D3D11_EvaluateFeature"));
        void* evalC = reinterpret_cast<void*>(GetProcAddress(mods[i], "NVSDK_NGX_D3D11_EvaluateFeature_C"));
        if (!create || (!eval && !evalC)) continue;
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(mods[i], path, MAX_PATH);
        if (NgxClassifyModule(path, mods[i] == host, create, eval, evalC) != NgxSkip::None) continue;
        const wchar_t* file = std::wcsrchr(path, L'\\');
        char narrow[MAX_PATH] = {};
        WideCharToMultiByte(CP_UTF8, 0, file ? file + 1 : path, -1, narrow, MAX_PATH - 1, nullptr, nullptr);
        if (!names.empty()) names += ", ";
        names += narrow;
    }
    return names.empty() ? std::string("(none yet)") : names;
}

}  // namespace

struct D3D12Presenter::Impl {
    // D3D11 side (CSP's device).
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> ctx11;
    ComPtr<ID3D11DeviceContext4> ctx4;
    ID3D11DeviceContext* ctx4_of = nullptr;  // the context ctx4 was queried from
    HWND hwnd = nullptr;

    // D3D12 side. device12 is always the native device (fences, shared
    // resources, allocators, lists); on the Streamline path the queue comes
    // from device_sl and factory is Streamline's factory proxy.
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12Device> device_sl;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIFactory4> factory_native;
    bool tearing = false;
    UINT chain_flags = 0;
    UINT width = 0;
    UINT height = 0;
    std::unique_ptr<FencePair> fences;
    ComPtr<ID3D11Texture2D> shared11;
    ComPtr<ID3D12Resource> shared12;
    ComPtr<ID3D12Resource> back[kBuffers];
    ComPtr<ID3D12CommandAllocator> alloc[kBuffers];
    ComPtr<ID3D12GraphicsCommandList> list[kBuffers];
    uint64_t buffer_value[kBuffers] = {};  // progress value that retires each buffer's last use
    bool timer12_pending[kBuffers] = {};
    GpuTimer11 timer11;
    std::unique_ptr<GpuTimer12> timer12;
    std::unique_ptr<StallWatchdog> watchdog;

    // Frame state.
    uint64_t prev_value = 0;  // V' of the last delivered frame: the next D3D11 wait
    uint64_t last11 = 0;      // highest value D3D11 was asked to signal
    uint64_t frames = 0;      // frames that reached the D3D12 side
    bool stalled = false;
    ULONGLONG stall_start = 0;
    bool resize_pending = false;
    UINT pending_w = 0;
    UINT pending_h = 0;
    HRESULT stop_error = S_OK;
    std::atomic<HRESULT> last_device_error{S_OK};
    std::atomic<HRESULT> last_present_hr{S_OK};
    bool warned_source = false;
    bool warned_present = false;

    // Streamline path (M2); nullptr is the plain path.
    StreamlineRuntime* sl = nullptr;
    bool reflex_on = false;
    bool shutdown_sl = false;  // ShutdownStreamlineOnRelease
    PclSequencer pcl;
    uint32_t frame_index = 0;          // the frame token counter, first frame 1
    sl::FrameToken* token = nullptr;   // the current frame's token
    uint32_t frames_with_markers = 0;  // frames that reached PresentEnd

    // Frame latency of a waitable chain (M1 pacing, see the header).
    HANDLE latency_waitable = nullptr;
    UINT latency = 1;
    bool latency_wait_off = false;  // after a timeout with an idle queue, until it signals again

    // Last logged present mode and occlusion state.
    UINT mode_csp_sync = UINT_MAX;
    UINT mode_csp_flags = UINT_MAX;
    UINT mode_sync = UINT_MAX;
    UINT mode_flags = UINT_MAX;
    BOOL mode_fullscreen = -1;
    bool occluded = false;
    unsigned occlusion_logs = 0;

    // Per-second statistics.
    ComPtr<IDXGIAdapter3> adapter3;  // the render adapter, for vram_mib
    ULONGLONG stats_start = 0;
    unsigned stats_presents = 0;   // CSP frames (base rate)
    unsigned stats_delivered = 0;  // D3D12 Presents that succeeded
    unsigned stats_skipped = 0;    // frames not delivered: stalled mode, back-buffer wait timeout
    unsigned stats_failed = 0;     // D3D12 Presents that failed
    unsigned stats_occluded = 0;   // D3D12 Presents that returned DXGI_STATUS_OCCLUDED
    unsigned stats_uncopied = 0;   // frames whose source did not match the shared back buffer
    double max_frame_ms = 0;       // longest interval between two CSP frames
    double max_present_ms = 0;     // longest time inside one frame's delivery
    int64_t last_frame_qpc = 0;
    double sum11 = 0;
    int n11 = 0;
    double sum12 = 0;
    int n12 = 0;
    unsigned stalls = 0;  // since creation

    // ACDLSSG_DEBUG_STALL_MS test hook.
    DWORD debug_stall_ms = 0;
    ComPtr<ID3D12Fence> debug_fence;
    HANDLE debug_cancel = nullptr;
    std::thread debug_thread;

    // M3: DLSS-G.
    Config config;
    PresenterEnvironment env;
    bool fg_supported = false;  // slIsFeatureSupported and the DLSS-G functions
    std::unique_ptr<CaptureSlots> slots;
    std::unique_ptr<CaptureCoordinator> coordinator;
    bool ngx_attached = false;
    uint32_t ngx_modules_logged = UINT32_MAX;
    // pcl and token: the render thread's evaluate emits markers too.
    std::mutex marker_mu;
    bool fg_user_on = true;  // start_with_fg, then the hotkey
    KeyEdge hotkey_edge;
    bool mode_known = false;     // SetDlssgOptions succeeded once
    bool mode_on = false;        // the DLSS-G mode Streamline has
    DlssgSizeHints mode_hints;
    bool mode_logged = false;  // the first mode line was written
    bool logged_on = false;    // the last mode line said on
    std::string logged_off_reason;
    ReasonThrottle frame_off_throttle{10000};
    bool prev_had_inputs = false;  // frame N-1 was presented with DLSS-G on its tags and constants
    bool prev_tagged = false;
    bool have_prev_camera = false;  // frame N-1's snapshot, for BuildFrameConstants
    CameraLayout prev_camera{};
    std::string state_failure;  // until the next resize or toggle (spec 9)
    VramGuard vram_guard;
    bool logged_no_estimate = false;
    bool vsync_available = false;  // bIsVsyncSupportAvailable == eTrue
    bool polled_state = false;     // slDLSSGGetState answered at least once while on
    StatusPollClock status_clock;  // when the answer's status is acted on
    unsigned state_answers_logged = 0;
    uint32_t last_mvec_w = 0;
    uint32_t last_mvec_h = 0;
    DXGI_FORMAT last_mvec_format = DXGI_FORMAT_UNKNOWN;
    bool have_csp_present = false;
    UINT last_csp_sync = 0;
    UINT last_csp_flags = 0;
    bool logged_vsync_fallback = false;
    bool logged_constants = false;
    bool logged_first_tags = false;
    bool logged_active = false;
    bool logged_aspect = false;
    D3D12Presenter::FgTotals totals;
    unsigned stats_captures = 0;
    unsigned stats_camera_fresh = 0;
    unsigned stats_tagged = 0;
    unsigned stats_fg_frames = 0;
    unsigned stats_double = 0;
    uint64_t stats_generated = 0;

    struct FrameDecision {
        bool fg = false;   // the gate allows DLSS-G for this Present
        bool tag = false;  // tags and constants are set
        std::string reason;
        bool perFrame = false;
        std::string tagPathReason;  // tag_without_fg: why the inputs are incomplete (TagPathReason)
        sl::Constants constants;
    };

    bool Init(D3D12Presenter& self, const PresenterCreateInfo& info, std::string* err);
    void StartFrame();
    void EmitMarkers(const std::vector<PclMarker>& markers);  // caller holds marker_mu
    void OnFirstEvaluate();
    void AttachNgx();
    void ProcessNgx();
    void PollHotkey();
    DlssgSizeHints Hints() const;
    FrameDecision Decide(const FrameCapture& cap);
    void RunVramCheck();  // records the result in vram_guard
    bool SetMode(bool on);
    void LogMode(bool on, const std::string& reason, bool perFrame, const std::string& tagPathReason = {});
    void PollState(bool presentedOn);
    void CountFrame(const FrameCapture& cap, bool tagged, bool fg);
    void PresentAgain(D3D12Presenter& self);
    void BeforeChainChange(D3D12Presenter& self);
    bool CreateSharedTexture(UINT w, UINT h, std::string* err);
    bool FetchBuffers(D3D12Presenter& self, std::string* err);
    ID3D11DeviceContext4* Ctx4For(ID3D11DeviceContext* ctx);

    HRESULT PresentFrame(D3D12Presenter& self, ID3D11DeviceContext* ctx, ID3D11Texture2D* source, UINT cspSync,
                         UINT cspFlags);
    HRESULT Deliver(D3D12Presenter& self, ID3D11DeviceContext* ctx, ID3D11Texture2D* source, UINT cspSync,
                    UINT cspFlags);
    bool SourceMatches(ID3D11Texture2D* source);
    void LogPresentMode(UINT cspSync, UINT cspFlags, const PresentPlan& plan, BOOL fullscreen);
    void NoteOcclusion(HRESULT presentHr);
    void WaitForLatency();
    void MaybeLogStats(ID3D11DeviceContext* ctx);

    void EnterStall(const char* reason);
    bool TryLeaveStall(D3D12Presenter& self);
    void Stop(D3D12Presenter& self, HRESULT error, const char* reason);
    bool Drain(D3D12Presenter& self, const char* why);
    HRESULT ApplyResize(D3D12Presenter& self, UINT w, UINT h);
    void StartDebugStall();
    void Shutdown(D3D12Presenter& self);
};

// ---------------------------------------------------------------- creation

bool D3D12Presenter::Impl::Init(D3D12Presenter& self, const PresenterCreateInfo& info, std::string* err) {
    InternalCallScope internal;
    const DXGI_SWAP_CHAIN_DESC1& gd = info.game_desc;
    if (!info.device11 || !info.hwnd) {
        *err = "presenter: null device or window";
        return false;
    }
    if (gd.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "presenter: game format %d is not R8G8B8A8_UNORM", static_cast<int>(gd.Format));
        *err = buf;
        return false;
    }
    if (gd.Width == 0 || gd.Height == 0) {
        *err = "presenter: zero swap chain size";
        return false;
    }
    device11 = info.device11;
    hwnd = info.hwnd;
    width = gd.Width;
    height = gd.Height;
    config = info.config;
    env = info.env;
    fg_user_on = config.start_with_fg;
    device11->GetImmediateContext(&ctx11);
    HRESULT hr = ctx11 ? ctx11.As(&ctx4) : E_NOINTERFACE;
    if (FAILED(hr)) {
        *err = HrText("QueryInterface(ID3D11DeviceContext4)", hr);
        return false;
    }
    ctx4_of = ctx11.Get();

    // 1. The adapter of CSP's device.
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    hr = device11.As(&dxgiDevice);
    if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) {
        *err = HrText("IDXGIDevice::GetAdapter", hr);
        return false;
    }
    DXGI_ADAPTER_DESC ad{};
    adapter->GetDesc(&ad);
    adapter.As(&adapter3);  // Windows 10 always has it; without it vram_mib is n/a

    // 2. D3D12 device on that adapter.
    const PFN_D3D12_CREATE_DEVICE create = LoadD3D12CreateDevice(err);
    if (!create) return false;
    ComPtr<ID3D12Device> device;
    hr = create(adapter.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device),
                reinterpret_cast<void**>(device.GetAddressOf()));
    if (FAILED(hr) || !device) {
        *err = HrText("D3D12CreateDevice", FAILED(hr) ? hr : E_POINTER);
        return false;
    }
    ComPtr<ID3D12Device> unwrapped;
    if (SUCCEEDED(device->QueryInterface(IID_ReShadeUnwrappedObject,
                                         reinterpret_cast<void**>(unwrapped.GetAddressOf()))) &&
        unwrapped) {
        LOGW("presenter: the D3D12 device came back wrapped by ReShade; using the unwrapped device");
        device = unwrapped;
    }
    device12 = device;

    // Streamline path (spec 6.4 steps 2-4): device, DLSS-G support, device proxy.
    sl = info.streamline;
    if (sl) {
        std::string slErr;
        if (!sl->SetDevice(device12.Get(), &slErr)) {
            *err = "Streamline: " + slErr;
            return false;
        }
        // Streamline now holds this device: a failure from here on must shut
        // it down before the device is released (Streamline's guide), which
        // also makes every later chain pass through. Cleared on success.
        shutdown_sl = true;
        std::string why;
        const bool fg = sl->DlssgSupported(ad.AdapterLuid, &why);
        LOGI("Streamline: DLSS-G %s on this adapter (%s)", fg ? "supported" : "not supported", why.c_str());
        fg_supported = fg && sl->DlssgFunctionsResolved();
        if (fg && !fg_supported) why += "; slDLSSGSetOptions or slDLSSGGetState could not be resolved";
        // Spec criterion 5: without DLSS-G the chain passes through, unless
        // proxy_without_fg keeps the M2 behaviour.
        if (!fg_supported) {
            if (!config.proxy_without_fg) {
                *err = DlssgUnsupportedMessage(why, IsAmpereSm86(ad.VendorId, ad.DeviceId), env.spoof_loaded);
                return false;
            }
            LOGI("presenter: DLSS-G is not supported; the chain is proxied anyway (proxy_without_fg=1)");
        }
        void* p = device12.Get();
        if (!sl->Upgrade(&p) || !p || p == device12.Get()) {
            *err = "Streamline: slUpgradeInterface(ID3D12Device) failed";
            return false;
        }
        device_sl.Attach(static_cast<ID3D12Device*>(p));
    }

    // 3. Direct queue (from the device proxy on the Streamline path).
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12Device* queueDevice = sl ? device_sl.Get() : device12.Get();
    hr = queueDevice->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    if (FAILED(hr)) {
        *err = HrText("ID3D12Device::CreateCommandQueue", hr);
        return false;
    }

    // 4. Factory of the adapter (Streamline's proxy of it on the Streamline
    // path); tearing support from the native factory.
    hr = adapter->GetParent(IID_PPV_ARGS(&factory_native));
    if (FAILED(hr)) {
        *err = HrText("IDXGIAdapter::GetParent(IDXGIFactory4)", hr);
        return false;
    }
    factory = factory_native;
    if (sl) {
        void* p = factory_native.Get();
        if (!sl->Upgrade(&p) || !p || p == factory_native.Get()) {
            *err = "Streamline: slUpgradeInterface(IDXGIFactory) failed";
            return false;
        }
        factory.Reset();
        factory.Attach(static_cast<IDXGIFactory4*>(p));
    }
    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory_native.As(&factory5))) {
        BOOL allow = FALSE;
        tearing = SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))) &&
                  allow;
    }
    chain_flags = tearing ? static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) : 0u;
    // M1 pacing: a game that paces on its waitable object gets one from the
    // D3D12 chain too. Never on the Streamline chain: its pacer must not be
    // starved; Reflex paces instead (spec 6.3).
    if (!sl && (gd.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
        chain_flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    // 5. The D3D12 chain on the game window.
    DXGI_SWAP_CHAIN_DESC1 cd{};
    cd.Width = width;
    cd.Height = height;
    cd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    cd.SampleDesc.Count = 1;
    cd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    cd.BufferCount = kBuffers;
    cd.Scaling = DXGI_SCALING_STRETCH;
    cd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    cd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    cd.Flags = chain_flags;
    ComPtr<IDXGISwapChain1> chain1;
    hr = factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &cd, nullptr, nullptr, &chain1);
    if (SUCCEEDED(hr)) hr = chain1 ? chain1.As(&self.chain_) : E_POINTER;
    if (FAILED(hr)) {
        *err = HrText("D3D12 CreateSwapChainForHwnd", hr);
        return false;
    }
    // Spec 6.4 step 8: the chain is already Streamline's proxy; never
    // slUpgradeInterface on it.
    if (sl && !sl->IsProxied(self.chain_.Get())) {
        *err = "Streamline: the D3D12 swap chain is not a Streamline proxy";
        return false;
    }
    if (chain_flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
        hr = self.chain_->SetMaximumFrameLatency(latency);
        if (FAILED(hr))
            LOGW("presenter: SetMaximumFrameLatency(%u) failed: 0x%08lX", latency, static_cast<unsigned long>(hr));
        latency_waitable = self.chain_->GetFrameLatencyWaitableObject();
        if (!latency_waitable) {
            *err = "D3D12 chain: GetFrameLatencyWaitableObject returned NULL";
            return false;
        }
        // DXGI creates the object with a count of L, which pays for the game's
        // wait before its first frame. The game's waits are served by the
        // bridge's own semaphore, so take that count here; otherwise every later
        // post-Present wait lets one frame more through than the game's own
        // chain would (one refresh of extra latency with VSync).
        WaitForSingleObject(latency_waitable, 0);
    }

    // 6. DXGI must not touch the game window or react to Alt+Enter.
    hr = factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(hr)) LOGW("presenter: MakeWindowAssociation failed: 0x%08lX", static_cast<unsigned long>(hr));

    // 7. Fences, shared back buffer, command recording, timers, watchdog.
    fences = FencePair::Create(device12.Get(), device11.Get(), err);
    if (!fences) return false;
    // M3: capture slots on the native device pair, and the coordinator that
    // NgxHook reports CSP's evaluates to (spec 6.4, 6.5).
    slots = CaptureSlots::Create(device11.Get(), device12.Get(), err);
    if (!slots) return false;
    {
        CaptureCoordinator::Deps deps;
        deps.device11 = device11.Get();
        deps.slots = slots.get();
        deps.fences = fences.get();
        deps.camera = env.camera ? env.camera : &CameraChannel::Get();
        deps.onFirstEvaluate = [this] { OnFirstEvaluate(); };
        coordinator = std::make_unique<CaptureCoordinator>(deps);
    }
    if (!CreateSharedTexture(width, height, err)) return false;
    if (!FetchBuffers(self, err)) return false;
    for (UINT i = 0; i < kBuffers; ++i) {
        hr = device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc[i]));
        if (SUCCEEDED(hr))
            hr = device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[i].Get(), nullptr,
                                             IID_PPV_ARGS(&list[i]));
        if (SUCCEEDED(hr)) hr = list[i]->Close();
        if (FAILED(hr)) {
            *err = HrText("command allocator/list creation", hr);
            return false;
        }
    }
    if (!timer11.Init(device11.Get())) LOGW("presenter: D3D11 GPU timing unavailable");
    timer12 = std::make_unique<GpuTimer12>();
    if (!timer12->Init(device12.Get(), queue.Get(), kBuffers)) LOGW("presenter: D3D12 GPU timing unavailable");

    wchar_t envText[32] = {};
    const DWORD envLen = GetEnvironmentVariableW(kDebugStallEnv, envText, static_cast<DWORD>(std::size(envText)));
    if (envLen > 0 && envLen < std::size(envText)) {
        const unsigned long ms = std::wcstoul(envText, nullptr, 10);
        if (ms > 0) {
            debug_stall_ms = static_cast<DWORD>(std::min<unsigned long>(ms, kDebugStallMaxMs));
            debug_cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!debug_cancel || FAILED(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&debug_fence)))) {
                LOGW("presenter: ACDLSSG_DEBUG_STALL_MS set but the debug fence could not be created");
                debug_stall_ms = 0;
            } else {
                LOGW("presenter: ACDLSSG_DEBUG_STALL_MS=%lu: the D3D12 queue will stall on frame %llu",
                     static_cast<unsigned long>(debug_stall_ms), static_cast<unsigned long long>(kDebugStallFrame));
            }
        }
    }

    watchdog = std::make_unique<StallWatchdog>(fences.get(), device12.Get());
    watchdog->Start();

    if (sl) {
        reflex_on = sl->EnableReflexLowLatency();
        LOGI("Streamline: Reflex low latency %s (lowLatencyAvailable %s)", reflex_on ? "on" : "off",
             sl->ReflexLowLatencyAvailable() ? "yes" : "no");
        sl->LogLoadedModules();
    }
    stats_start = GetTickCount64();

    char name[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, static_cast<int>(sizeof(name)) - 1, nullptr, nullptr);
    LOGI("presenter created: %s (LUID %08lX:%08lX), D3D12 chain %ux%u, %u buffers, flags 0x%X, tearing %s, "
         "frame-latency object %s, hwnd %p, %s",
         name, static_cast<unsigned long>(ad.AdapterLuid.HighPart), static_cast<unsigned long>(ad.AdapterLuid.LowPart),
         width, height, kBuffers, chain_flags, tearing ? "yes" : "no", latency_waitable ? "yes" : "no",
         static_cast<void*>(hwnd), sl ? "Streamline proxy chain" : "plain chain (no Streamline)");
    uint64_t vramUsage = 0;
    uint64_t vramBudget = 0;
    std::string vramErr;
    if (QueryVramMiB(adapter3.Get(), &vramUsage, &vramBudget, &vramErr)) {
        LOGI("presenter: VRAM (local) budget %llu MiB, usage %llu MiB", static_cast<unsigned long long>(vramBudget),
             static_cast<unsigned long long>(vramUsage));
    } else {
        LOGI("presenter: VRAM unknown: %s", vramErr.c_str());
    }
    // Spec 7 step 1 for the first frame: it starts at the end of creation.
    if (sl) StartFrame();
    // Last: from here on CSP's evaluates reach this presenter.
    AttachNgx();
    shutdown_sl = false;  // from now on the owner decides (ShutdownStreamlineOnRelease)
    return true;
}

// Spec 7 step 1: frame token, Reflex sleep, SimulationStart.
void D3D12Presenter::Impl::StartFrame() {
    ++frame_index;
    sl::FrameToken* next = sl->NewFrameToken(frame_index);
    if (next) sl->ReflexSleep(*next);  // blocks for pacing: outside the lock
    std::lock_guard<std::mutex> lock(marker_mu);
    token = next;
    EmitMarkers(pcl.BeginFrame(frame_index));
}

void D3D12Presenter::Impl::EmitMarkers(const std::vector<PclMarker>& markers) {
    if (!token) return;  // the runtime logs the token failure
    for (PclMarker m : markers) sl->Marker(m, *token);
}

// Spec 7 step 2, from the render thread: the first qualifying evaluate of the
// frame ends the simulation and starts the render submission.
void D3D12Presenter::Impl::OnFirstEvaluate() {
    if (!sl) return;
    std::lock_guard<std::mutex> lock(marker_mu);
    EmitMarkers(pcl.OnEvaluate());
}

// NgxHook is installed once per process; every presenter attaches its
// coordinator as the last step of its creation.
void D3D12Presenter::Impl::AttachNgx() {
    if (!env.install_ngx_hook || !coordinator) return;
    if (!g_ngx_installed.exchange(true)) {
        std::string e;
        if (!NgxHook::Get().Install(coordinator.get(), &e))
            LOGW("NGX hook: installation failed: %s; DLSS-G gets no captures", e.c_str());
    }
    NgxHook::Get().SetSink(coordinator.get());
    ngx_attached = true;
    ProcessNgx();
}

// Start of every PresentFrame: finish a rescan a DLL load deferred, and log
// the hooked modules whenever their number changes.
void D3D12Presenter::Impl::ProcessNgx() {
    NgxHook::Get().ProcessPendingRescan();
    const uint32_t n = NgxHook::Get().HookedModules();
    if (n == ngx_modules_logged) return;
    ngx_modules_logged = n;
    LOGI("NGX hook: installed on %u module(s): %s", n, NgxModuleNames().c_str());
}

// Spec 6.9: the hotkey toggles DLSS-G on the present thread, only while the
// game window has the focus.
void D3D12Presenter::Impl::PollHotkey() {
    const HWND foreground = GetForegroundWindow();
    const bool focused = foreground && (foreground == hwnd || GetAncestor(foreground, GA_ROOT) == GetAncestor(hwnd, GA_ROOT));
    bool chord = false;
    if (focused) {
        const auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        const Hotkey& hk = config.hotkey;
        chord = HotkeyChordDown(hk, down(static_cast<int>(hk.vk)), down(VK_CONTROL), down(VK_SHIFT), down(VK_MENU));
    }
    if (!hotkey_edge.Pressed(chord)) return;
    fg_user_on = !fg_user_on;
    prev_had_inputs = false;  // spec 8: the next DLSS-G frame has reset
    state_failure.clear();   // spec 9: a failure status is retried after a toggle
    LOGI("fg: hotkey -> %s", fg_user_on ? "on" : "off");
}

// The size and format hints for slDLSSGSetOptions and the estimate: the
// Streamline chain's back buffers, the depth slot (R32_FLOAT) and the last
// captured motion vectors.
DlssgSizeHints D3D12Presenter::Impl::Hints() const {
    DlssgSizeHints h;
    h.numBackBuffers = kBuffers;
    h.colorWidth = width;
    h.colorHeight = height;
    h.colorBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    h.depthBufferFormat = DXGI_FORMAT_R32_FLOAT;
    h.mvecDepthWidth = last_mvec_w;
    h.mvecDepthHeight = last_mvec_h;
    h.mvecBufferFormat = last_mvec_format;
    return h;
}

// Spec 7 step 5.4 plus the video memory guard (6.11).
D3D12Presenter::Impl::FrameDecision D3D12Presenter::Impl::Decide(const FrameCapture& cap) {
    FrameDecision d;
    FgGateInputs g;
    g.userOn = fg_user_on;
    g.supported = fg_supported;
    g.stateFailure = state_failure;
    g.stalled = stalled;
    g.windowMinimized = IsIconic(hwnd) != FALSE;
    g.aspectRefusal = RuntimeAspectRefusal(env.allow_stretching, cap.outWidth, cap.outHeight, width, height);
    g.captured = cap.captured;
    g.captureReason = cap.reason;
    g.forcedOff = cap.forcedOff;
    g.mvScaleMissing = cap.mvScaleMissing;
    g.cameraOk = cap.cameraResult == CameraChannel::ReadResult::Ok;
    g.cameraReason = CameraReadResultText(cap.cameraResult);
    g.cameraFresh = cap.cameraFresh;
    g.cameraFlags = g.cameraOk ? cap.camera.flags : 0;
    if (cap.captured && g.cameraOk) {
        CameraLayout cur = cap.camera;
        CameraLayout prev = prev_camera;
        const uint32_t rw = cap.params.renderW;
        const uint32_t rh = cap.params.renderH;
        // The projection's aspect (the camera's render size) and the MV
        // scale's reference (the subrect) must agree; the subrect wins.
        if (AspectMismatch(cur.renderW, cur.renderH, rw, rh)) {
            if (!logged_aspect) {
                logged_aspect = true;
                LOGW("camera: render size %.0fx%.0f and the DLSS subrect %ux%u differ in aspect by more than 0.5%%; "
                     "the subrect is used for both (logged once)",
                     cur.renderW, cur.renderH, rw, rh);
            }
            cur.renderW = prev.renderW = static_cast<float>(rw);
            cur.renderH = prev.renderH = static_cast<float>(rh);
        }
        // An origin re-base between N-1 and N must not look like motion.
        const bool originChanged =
            have_prev_camera && std::memcmp(prev_camera.originShift, cur.originShift, sizeof(cur.originShift)) != 0;
        if (originChanged)
            LOGD("camera: origin shift changed (%.3f, %.3f, %.3f) -> (%.3f, %.3f, %.3f); reset", prev.originShift[0],
                 prev.originShift[1], prev.originShift[2], cur.originShift[0], cur.originShift[1], cur.originShift[2]);
        ConstantsInput in;
        in.cur = &cur;
        in.prev = have_prev_camera ? &prev : nullptr;
        in.capture = cap.params;
        in.prevFrameHadInputs = prev_had_inputs && !originChanged;
        in.options.flipHandedness = config.camera_flip_handedness;
        in.options.negateSide = config.camera_negate_side;
        g.constantsOk = BuildFrameConstants(in, &d.constants, &g.constantsWhy);
        if (g.constantsOk && !logged_constants) {
            logged_constants = true;
            LOGI("constants: %s", FormatConstantsForLog(d.constants).c_str());
        }
    }
    FgGateResult gate = DecideFg(g);
    if (gate.on && !vram_guard.Passed()) {
        if (vram_guard.CheckDue(frame_index)) RunVramCheck();
        if (!vram_guard.Passed()) {
            g.vramRefusal = vram_guard.Refusal();
            gate = DecideFg(g);
        }
    }
    if (!token) gate = FgGateResult{false, "no Streamline frame token", true};
    d.fg = gate.on;
    d.tag = token && ShouldTag(g, gate, config.tag_without_fg);
    d.reason = gate.reason;
    d.perFrame = gate.perFrame;
    d.tagPathReason = TagPathReason(g, gate, config.tag_without_fg);
    return d;
}

// Spec 6.11: the estimate for the current sizes against the free budget.
// A failed estimate checks the headroom only (DecideVram, review findings
// SL-6 and F2). The check is logged at INFO when its outcome changes and
// at DEBUG otherwise, so a lasting refusal is not logged every 60 frames.
void D3D12Presenter::Impl::RunVramCheck() {
    VramInputs in;
    in.headroomMib = config.fg_vram_headroom_mib;
    sl::DLSSGState st;
    const sl::Result r = sl->GetDlssgState(true, Hints(), &st);
    in.estimateOk = r == sl::Result::eOk;
    if (in.estimateOk) {
        in.estimateBytes = st.estimatedVRAMUsageInBytes;
        vsync_available = st.bIsVsyncSupportAvailable == sl::Boolean::eTrue;
    } else if (!logged_no_estimate) {
        logged_no_estimate = true;
        LOGW("fg: no DLSS-G video memory estimate (%s); the video memory check uses the headroom only (logged once)",
             SlResultName(r));
    }
    std::string why;
    in.budgetKnown = QueryVramBytes(adapter3.Get(), &in.usageBytes, &in.budgetBytes, &why);
    const VramCheck c = DecideVram(in);
    const bool changed = vram_guard.Record(frame_index, c);
    char estimate[32] = "n/a";
    if (in.estimateOk)
        std::snprintf(estimate, sizeof(estimate), "%llu MiB",
                      static_cast<unsigned long long>(in.estimateBytes / (1024 * 1024)));
    const LogLevel level = changed ? LogLevel::Info : LogLevel::Debug;
    if (!in.budgetKnown) {
        LogWrite(level, "fg: video memory check skipped (%s); DLSS-G estimate %s", why.c_str(), estimate);
        return;
    }
    LogWrite(level,
             "fg: video memory check: DLSS-G estimate %s + headroom %u MiB, budget %llu MiB, usage %llu MiB: %s; "
             "VSync with DLSS-G %s",
             estimate, in.headroomMib, static_cast<unsigned long long>(in.budgetBytes / (1024 * 1024)),
             static_cast<unsigned long long>(in.usageBytes / (1024 * 1024)), c.ok ? "ok" : c.reason.c_str(),
             vsync_available ? "available" : "not available");
}

// slDLSSGSetOptions only when the mode, or while on the size hints, change
// (spec 6.8). Returns the mode Streamline has afterwards.
bool D3D12Presenter::Impl::SetMode(bool on) {
    if (!sl || !fg_supported) return false;
    const DlssgSizeHints h = Hints();
    if (mode_known && on == mode_on && (!on || SameHints(h, mode_hints))) return mode_on;
    if (sl->SetDlssgOptions(on, h) == sl::Result::eOk) {
        mode_known = true;
        mode_on = on;
        mode_hints = h;
    }
    return mode_on;
}

// "fg: DLSS-G on" / "fg: DLSS-G off (<reason>)" on every mode change, and
// when a lasting off reason changes; per-frame reasons are throttled WARNs,
// and so is tagPathReason, the per-frame reason behind a lasting one that
// tag_without_fg reports (TagPathReason).
void D3D12Presenter::Impl::LogMode(bool on, const std::string& reason, bool perFrame,
                                   const std::string& tagPathReason) {
    if (on) {
        if (!mode_logged || !logged_on) LOGI("fg: DLSS-G on");
        mode_logged = true;
        logged_on = true;
        logged_off_reason.clear();
        return;
    }
    if (!mode_logged || logged_on || (!perFrame && reason != logged_off_reason)) {
        LOGI("fg: DLSS-G off (%s)", reason.c_str());
        logged_off_reason = reason;
    }
    mode_logged = true;
    logged_on = false;
    const std::string& warn = perFrame ? reason : tagPathReason;
    if (!warn.empty() && frame_off_throttle.ShouldLog(warn, GetTickCount64()))
        LOGW("fg: frame without DLSS-G: %s", warn.c_str());
}

// After every Present: presentedOn is whether DLSS-G was on at it. After a
// DLSS-G Present, slDLSSGGetState without options (only the estimate is
// expensive, DLSS-G guide 13.0) gives that Present's generated frames
// (review finding SL-1). Every 60 frames (StatusPollClock) the same answer's
// status, or a failed call, keeps DLSS-G off until the next resize or toggle
// (spec 6.8, 9).
void D3D12Presenter::Impl::PollState(bool presentedOn) {
    if (!sl || !fg_supported) return;
    status_clock.Frame();
    if (!presentedOn) return;
    const bool statusDue = status_clock.Due(true);
    if (statusDue) status_clock.Polled();
    sl::DLSSGState st;
    const sl::Result r = sl->GetDlssgState(false, Hints(), &st);
    if (r != sl::Result::eOk) {
        if (!statusDue) return;  // the runtime logs the failure, throttled
        state_failure = std::string("query failed: ") + SlResultName(r);
        LOGW("fg: slDLSSGGetState failed (%s); DLSS-G stays off until the next resize or toggle", SlResultName(r));
        return;
    }
    vsync_available = st.bIsVsyncSupportAvailable == sl::Boolean::eTrue;
    polled_state = true;
    const uint32_t generated = GeneratedFramesAtPresent(st.numFramesActuallyPresented);
    stats_generated += generated;
    if (state_answers_logged < kStateAnswersLogged) {
        // The raw value settles in game which reading of it holds.
        ++state_answers_logged;
        LOGI("fg: slDLSSGGetState after a DLSS-G Present (frame %u): numFramesActuallyPresented %u, status %s",
             frame_index, st.numFramesActuallyPresented, DlssgStatusText(static_cast<uint32_t>(st.status)).c_str());
    }
    if (generated > 0 && !logged_active) {
        logged_active = true;
        LOGI("fg: DLSS-G active");
        sl->LogLoadedModules();  // spec 6.8: again when DLSS-G first reports active
    }
    if (!statusDue) return;
    const uint32_t status = static_cast<uint32_t>(st.status);
    if (status != 0) {
        state_failure = DlssgStatusText(status);
        LOGW("fg: DLSS-G status %s; DLSS-G stays off until the next resize or toggle", state_failure.c_str());
    }
}

void D3D12Presenter::Impl::CountFrame(const FrameCapture& cap, bool tagged, bool fg) {
    if (cap.captured) {
        ++stats_captures;
        ++totals.captures;
    }
    if (cap.captured && cap.cameraFresh) {
        ++stats_camera_fresh;
        ++totals.camera_fresh;
    }
    if (tagged) {
        ++stats_tagged;
        ++totals.tagged;
    }
    if (fg) {
        ++stats_fg_frames;
        ++totals.fg_frames;
    }
}

// Spec 6.3 "Resizing" step 1: the last frame once more, D3D12 side only
// (spec 7 steps 5-7), with its own frame token and the full marker sequence
// and DLSS-G already set to eOff. Not a CSP frame: no statistics, no latency
// release.
void D3D12Presenter::Impl::PresentAgain(D3D12Presenter& self) {
    if (self.stopped_ || stalled || !have_csp_present || !fences) return;
    const UINT idx = self.chain_->GetCurrentBackBufferIndex();
    if (idx >= kBuffers) return;
    if (!fences->CpuWaitProgress(buffer_value[idx], kCpuWaitMs)) {
        EnterStall("a D3D12 back buffer was still in use after 500 ms");
        return;
    }
    if (timer12_pending[idx]) {
        double ms = 0;
        if (timer12->Read(idx, &ms)) {
            sum12 += ms;
            ++n12;
        }
        timer12_pending[idx] = false;
    }
    ID3D12GraphicsCommandList* cl = list[idx].Get();
    HRESULT hr = alloc[idx]->Reset();
    if (SUCCEEDED(hr)) hr = cl->Reset(alloc[idx].Get(), nullptr);
    if (FAILED(hr)) return;
    const D3D12_RESOURCE_BARRIER toCopy =
        Transition(back[idx].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->ResourceBarrier(1, &toCopy);
    cl->CopyResource(back[idx].Get(), shared12.Get());
    const D3D12_RESOURCE_BARRIER toPresent =
        Transition(back[idx].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    cl->ResourceBarrier(1, &toPresent);
    if (FAILED(cl->Close())) return;
    ID3D12CommandList* lists[] = {cl};
    queue->ExecuteCommandLists(1, lists);

    BOOL fullscreen = FALSE;
    if (FAILED(self.chain_->GetFullscreenState(&fullscreen, nullptr))) fullscreen = FALSE;
    const PresentPlan plan = PlanPresent(last_csp_sync, last_csp_flags, tearing, !fullscreen, false, vsync_available);
    {
        std::lock_guard<std::mutex> lock(marker_mu);
        EmitMarkers(pcl.BeforePresent());
    }
    const HRESULT presentHr = self.chain_->Present(plan.sync, plan.flags);
    {
        std::lock_guard<std::mutex> lock(marker_mu);
        const std::vector<PclMarker> after = pcl.AfterPresent();
        if (token && !after.empty()) ++frames_with_markers;
        EmitMarkers(after);
    }
    const uint64_t done = fences->Next();
    queue->Signal(fences->Shared12(), done);
    queue->Signal(fences->Progress(), done);
    fences->last_submitted.store(done);
    buffer_value[idx] = done;
    prev_value = done;
    LOGI("fg: the last frame was presented once more with DLSS-G off before the swap chain change (0x%08lX)",
         static_cast<unsigned long>(presentHr));
    if (IsDeviceError(presentHr)) {
        last_device_error.store(presentHr);
        Stop(self, StopErrorFor(presentHr), "Present returned a device error");
        return;
    }
    StartFrame();
}

// Spec 6.3 "Resizing" step 1 and spec 8: runs before ResizeBuffers,
// SetFullscreenState and ResizeTarget. The camera latch restarts; the capture
// slots are recreated lazily when the sources change.
void D3D12Presenter::Impl::BeforeChainChange(D3D12Presenter& self) {
    if (coordinator) coordinator->ResetCamera();
    have_prev_camera = false;
    prev_had_inputs = false;
    if (!sl) return;
    state_failure.clear();  // spec 9: retried after the next resize
    const bool wasOn = mode_on;
    if (wasOn) {
        SetMode(false);
        LogMode(false, "swap chain change", false);
    }
    if (prev_tagged && token) sl->SetNullTags(*token, kViewport);
    prev_tagged = false;
    if (wasOn) PresentAgain(self);
}

bool D3D12Presenter::Impl::CreateSharedTexture(UINT w, UINT h, std::string* err) {
    shared12.Reset();
    shared11.Reset();
    // Created on D3D11 and opened on D3D12: the other direction failed with
    // E_INVALIDARG on the reference machine (spec 6.4).
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = device11->CreateTexture2D(&td, nullptr, &tex);
    if (FAILED(hr)) {
        *err = HrText("shared back buffer CreateTexture2D", hr);
        return false;
    }
    ComPtr<IDXGIResource1> resource;
    HANDLE handle = nullptr;
    hr = tex.As(&resource);
    if (SUCCEEDED(hr))
        hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                          &handle);
    if (FAILED(hr)) {
        *err = HrText("shared back buffer CreateSharedHandle", hr);
        return false;
    }
    ComPtr<ID3D12Resource> opened;
    hr = device12->OpenSharedHandle(handle, IID_PPV_ARGS(&opened));
    CloseHandle(handle);
    if (FAILED(hr)) {
        *err = HrText("shared back buffer OpenSharedHandle", hr);
        return false;
    }
    shared11 = tex;
    shared12 = opened;
    return true;
}

bool D3D12Presenter::Impl::FetchBuffers(D3D12Presenter& self, std::string* err) {
    for (UINT i = 0; i < kBuffers; ++i) {
        back[i].Reset();
        const HRESULT hr = self.chain_->GetBuffer(i, IID_PPV_ARGS(&back[i]));
        if (FAILED(hr)) {
            *err = HrText("D3D12 chain GetBuffer", hr);
            return false;
        }
    }
    return true;
}

ID3D11DeviceContext4* D3D12Presenter::Impl::Ctx4For(ID3D11DeviceContext* ctx) {
    if (ctx != ctx4_of) {
        ComPtr<ID3D11DeviceContext4> c4;
        if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&c4)))) return nullptr;
        ctx4 = c4;
        ctx4_of = ctx;
    }
    return ctx4.Get();
}

std::unique_ptr<D3D12Presenter> D3D12Presenter::Create(const PresenterCreateInfo& info, std::string* error) {
    std::string err;
    try {
        std::unique_ptr<D3D12Presenter> presenter(new D3D12Presenter());
        presenter->impl_ = std::make_unique<Impl>();
        if (presenter->impl_->Init(*presenter, info, &err)) return presenter;
        // The destructor releases whatever Init created.
    } catch (const std::exception& e) {
        err = std::string("presenter: exception: ") + e.what();
    } catch (...) {
        err = "presenter: unknown exception";
    }
    if (err.empty()) err = "presenter: creation failed";
    LOGE("%s", err.c_str());
    if (error) {
        try {
            *error = err;
        } catch (...) {
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------- frames

HRESULT D3D12Presenter::PresentFrame(ID3D11DeviceContext* ctx, ID3D11Texture2D* source, UINT cspSync, UINT cspFlags) {
    try {
        if (!impl_) return E_FAIL;
        return impl_->PresentFrame(*this, ctx, source, cspSync, cspFlags);
    } catch (...) {
        LOGE("presenter: unexpected exception in PresentFrame");
        return E_FAIL;
    }
}

HRESULT D3D12Presenter::Impl::PresentFrame(D3D12Presenter& self, ID3D11DeviceContext* ctx, ID3D11Texture2D* source,
                                           UINT cspSync, UINT cspFlags) {
    if (ngx_attached) ProcessNgx();
    if (self.stopped_) return stop_error;
    if (!ctx) return E_INVALIDARG;
    ++stats_presents;
    const int64_t start = QpcNow();
    if (last_frame_qpc) max_frame_ms = std::max(max_frame_ms, QpcMs(start - last_frame_qpc));
    last_frame_qpc = start;
    const HRESULT hr = Deliver(self, ctx, source, cspSync, cspFlags);
    max_present_ms = std::max(max_present_ms, QpcMs(QpcNow() - start));
    MaybeLogStats(ctx);
    return hr;
}

HRESULT D3D12Presenter::Impl::Deliver(D3D12Presenter& self, ID3D11DeviceContext* ctx, ID3D11Texture2D* source,
                                      UINT cspSync, UINT cspFlags) {
    // M3: this Present ends the bridge frame; its capture pairs with it.
    FrameCapture cap;
    if (coordinator) cap = coordinator->EndFrame();
    if (cap.evaluates > 1) {
        ++stats_double;
        ++totals.double_evaluates;
    }
    if (cap.captured) {
        last_mvec_w = cap.mvecWidth;
        last_mvec_h = cap.mvecHeight;
        last_mvec_format = cap.mvecFormat;
    }
    if (sl) PollHotkey();
    // A frame that is not delivered breaks the history (spec 6.7 reset).
    struct NotDelivered {
        Impl& impl;
        bool delivered = false;
        ~NotDelivered() {
            if (delivered) return;
            impl.prev_had_inputs = false;
            impl.have_prev_camera = false;
        }
    } history{*this};

    // Stall handling first (spec 6.4 "Stalled mode").
    if (watchdog->DeviceRemoved() || device12->GetDeviceRemovedReason() != S_OK) {
        Stop(self, DXGI_ERROR_DEVICE_REMOVED, "the D3D12 device was removed");
        return stop_error;
    }
    if (watchdog->TakeStallPending() && !stalled) EnterStall("the watchdog saw no D3D12 progress for 500 ms");
    if (stalled && !TryLeaveStall(self)) {
        ++stats_skipped;
        return self.stopped_ ? stop_error : S_OK;
    }
    if (resize_pending) {
        const HRESULT hr = ApplyResize(self, pending_w, pending_h);
        if (FAILED(hr)) return self.stopped_ ? stop_error : hr;
    }

    ID3D11DeviceContext4* c4 = Ctx4For(ctx);
    if (!c4) return E_NOINTERFACE;

    // D3D11 side: the previous D3D12 copy must be done with the shared texture.
    if (prev_value) {
        c4->Wait(fences->Shared11(), prev_value);
        uint64_t pending = fences->pending_wait.load();
        while (pending < prev_value && !fences->pending_wait.compare_exchange_weak(pending, prev_value)) {
        }
    }
    if (SourceMatches(source)) {
        timer11.Begin(ctx);
        ctx->CopyResource(shared11.Get(), source);
        timer11.End(ctx);
    } else {
        ++stats_uncopied;
    }
    const uint64_t v = fences->Next();
    c4->Signal(fences->Shared11(), v);
    last11 = v;
    ctx->Flush();  // the D3D12 queue waits for v, so it must reach the GPU now

    // D3D12 side.
    ++frames;
    if (debug_stall_ms && frames == kDebugStallFrame) StartDebugStall();
    // v also covers this frame's capture: its copy into the slot ran earlier
    // on the same immediate context, so nothing on the queue reads the slot
    // before that copy is done (review finding F1).
    HRESULT hr = queue->Wait(fences->Shared12(), v);
    if (FAILED(hr)) LOGW("presenter: queue Wait failed: 0x%08lX", static_cast<unsigned long>(hr));
    const UINT idx = self.chain_->GetCurrentBackBufferIndex();
    if (idx >= kBuffers) {
        LOGE("presenter: unexpected back buffer index %u", idx);
        return E_FAIL;
    }
    if (!fences->CpuWaitProgress(buffer_value[idx], kCpuWaitMs)) {
        EnterStall("a D3D12 back buffer was still in use after 500 ms");
        ++stats_skipped;
        return S_OK;
    }
    if (timer12_pending[idx]) {
        double ms = 0;
        if (timer12->Read(idx, &ms)) {
            sum12 += ms;
            ++n12;
        }
        timer12_pending[idx] = false;
    }

    ID3D12GraphicsCommandList* cl = list[idx].Get();
    hr = alloc[idx]->Reset();
    if (SUCCEEDED(hr)) hr = cl->Reset(alloc[idx].Get(), nullptr);
    if (FAILED(hr)) {
        LOGE("presenter: command list reset failed: 0x%08lX", static_cast<unsigned long>(hr));
        return hr;
    }
    // The shared texture is promoted from COMMON to COPY_SOURCE implicitly and
    // decays back to COMMON after the execute, which cross-API sharing needs.
    const D3D12_RESOURCE_BARRIER toCopy =
        Transition(back[idx].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->ResourceBarrier(1, &toCopy);
    timer12->Begin(cl, idx);
    cl->CopyResource(back[idx].Get(), shared12.Get());
    timer12->End(cl, idx);
    const D3D12_RESOURCE_BARRIER toPresent =
        Transition(back[idx].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    cl->ResourceBarrier(1, &toPresent);

    // M3, spec 7 step 5.4: DLSS-G for this Present; tags and constants for
    // this frame's token, with the frame's open command list.
    bool tagged = false;
    bool fgThisFrame = false;
    if (sl) {
        const FrameDecision dec = Decide(cap);
        bool tagsSet = false;
        if (dec.tag) {
            const sl::Extent extent{0, 0, cap.params.renderW, cap.params.renderH};
            const sl::Result tagResult =
                sl->SetTagsForFrame(*token, kViewport, slots->Depth12(cap.slot), slots->Mvec12(cap.slot), extent, cl);
            tagsSet = tagResult == sl::Result::eOk;
            tagged = tagsSet && sl->SetConstants(dec.constants, *token, kViewport) == sl::Result::eOk;
            if (tagged && !logged_first_tags) {
                logged_first_tags = true;
                LOGI("fg: first tags and constants set (frame %u)", frame_index);
            }
        }
        // Streamline drops its references to the slots with null tags.
        if (!tagged && (prev_tagged || tagsSet) && token) sl->SetNullTags(*token, kViewport);
        std::string reason = dec.reason;
        bool perFrame = dec.perFrame;
        if (dec.fg && !tagged) {
            reason = "Streamline refused the tags or constants";
            perFrame = true;
        }
        fgThisFrame = SetMode(dec.fg && tagged);
        if (dec.fg && tagged && !fgThisFrame) {
            reason = "slDLSSGSetOptions(eOn) failed";
            perFrame = false;
        }
        LogMode(fgThisFrame, reason, perFrame, dec.tagPathReason);
    }

    hr = cl->Close();
    if (FAILED(hr)) {
        LOGE("presenter: command list close failed: 0x%08lX", static_cast<unsigned long>(hr));
        return hr;
    }
    ID3D12CommandList* lists[] = {cl};
    queue->ExecuteCommandLists(1, lists);
    timer12_pending[idx] = true;

    BOOL fullscreen = FALSE;
    if (FAILED(self.chain_->GetFullscreenState(&fullscreen, nullptr))) fullscreen = FALSE;
    // Spec 7 step 6, with the VSync rule for DLSS-G.
    const PresentPlan plan = PlanPresent(cspSync, cspFlags, tearing, !fullscreen, fgThisFrame, vsync_available);
    if (plan.vsync_unavailable_with_fg && !logged_vsync_fallback) {
        logged_vsync_fallback = true;
        LOGW("fg: VSync is not available with DLSS-G here (bIsVsyncSupportAvailable is not eTrue); presenting with "
             "sync interval 0 without tearing, DWM keeps the borderless window tear-free (logged once)");
    }
    LogPresentMode(cspSync, cspFlags, plan, fullscreen);
    have_csp_present = true;
    last_csp_sync = cspSync;
    last_csp_flags = cspFlags;
    if (sl) {
        std::lock_guard<std::mutex> lock(marker_mu);
        EmitMarkers(pcl.BeforePresent());
    }
    const HRESULT presentHr = self.chain_->Present(plan.sync, plan.flags);
    if (sl) {
        std::lock_guard<std::mutex> lock(marker_mu);
        const std::vector<PclMarker> after = pcl.AfterPresent();
        if (token && !after.empty()) ++frames_with_markers;
        EmitMarkers(after);
    }

    const uint64_t done = fences->Next();
    queue->Signal(fences->Shared12(), done);
    queue->Signal(fences->Progress(), done);
    fences->last_submitted.store(done);
    buffer_value[idx] = done;
    prev_value = done;
    last_present_hr.store(presentHr);

    // M3: the D3D12 side is done with the slot once progress passes done;
    // this frame is frame N-1 of the next one.
    if (tagged && coordinator) coordinator->NoteTagged(cap.slot, done);
    history.delivered = true;
    prev_tagged = tagged;
    // Spec 6.7 reset: only a frame DLSS-G used gives the next one history;
    // tags without DLSS-G (tag_without_fg), a toggle, a stall or a resize
    // leave the next DLSS-G frame with reset.
    prev_had_inputs = tagged && fgThisFrame;
    have_prev_camera = cap.cameraResult == CameraChannel::ReadResult::Ok;
    if (have_prev_camera) prev_camera = cap.camera;
    CountFrame(cap, tagged, fgThisFrame && SUCCEEDED(presentHr));
    if (sl) PollState(fgThisFrame && SUCCEEDED(presentHr));

    if (FAILED(presentHr)) {
        ++stats_failed;
    } else {
        ++stats_delivered;
        NoteOcclusion(presentHr);
    }
    if (IsDeviceError(presentHr)) {
        last_device_error.store(presentHr);
        char reason[64];
        std::snprintf(reason, sizeof(reason), "Present returned 0x%08lX", static_cast<unsigned long>(presentHr));
        Stop(self, StopErrorFor(presentHr), reason);
    } else if (FAILED(presentHr) && !warned_present) {
        warned_present = true;
        LOGW("presenter: D3D12 Present(%u, 0x%X) failed: 0x%08lX (logged once; the stats line counts them)",
             plan.sync, plan.flags, static_cast<unsigned long>(presentHr));
    }
    if (sl) {
        if (!self.stopped_) StartFrame();  // spec 7 step 7: frame start for N+1
    } else if (!FAILED(presentHr)) {
        WaitForLatency();
    }
    return presentHr;
}

void D3D12Presenter::Impl::LogPresentMode(UINT cspSync, UINT cspFlags, const PresentPlan& plan, BOOL fullscreen) {
    if (cspSync == mode_csp_sync && cspFlags == mode_csp_flags && plan.sync == mode_sync && plan.flags == mode_flags &&
        fullscreen == mode_fullscreen)
        return;
    mode_csp_sync = cspSync;
    mode_csp_flags = cspFlags;
    mode_sync = plan.sync;
    mode_flags = plan.flags;
    mode_fullscreen = fullscreen;
    const bool askedTearing = (cspFlags & DXGI_PRESENT_ALLOW_TEARING) != 0;
    const bool keptTearing = (plan.flags & DXGI_PRESENT_ALLOW_TEARING) != 0;
    LOGI("present mode: CSP Present(%u, 0x%X) -> D3D12 Present(%u, 0x%X), %s, chain tearing %s%s", cspSync, cspFlags,
         plan.sync, plan.flags, fullscreen ? "fullscreen" : "windowed", tearing ? "supported" : "not supported",
         askedTearing && !keptTearing ? ", ALLOW_TEARING stripped" : "");
}

void D3D12Presenter::Impl::NoteOcclusion(HRESULT presentHr) {
    const bool now = presentHr == DXGI_STATUS_OCCLUDED;
    if (now) ++stats_occluded;
    if (now == occluded) return;
    occluded = now;
    if (occlusion_logs < kMaxOcclusionLogs) {
        ++occlusion_logs;
        LOGI("%s", now ? "window occluded (D3D12 Present returned DXGI_STATUS_OCCLUDED)" : "window visible again");
        if (occlusion_logs == kMaxOcclusionLogs) LOGI("further occlusion changes are counted in the stats line only");
    }
}

// The game's next frame starts when its own waitable chain would let it:
// DXGI signals the object once a queued frame leaves the present queue.
void D3D12Presenter::Impl::WaitForLatency() {
    if (!latency_waitable) return;
    if (latency_wait_off) {
        if (WaitForSingleObject(latency_waitable, 0) != WAIT_OBJECT_0) return;
        latency_wait_off = false;
        LOGI("presenter: the D3D12 frame-latency object is signalled again; pacing on it resumes");
        return;
    }
    if (WaitForSingleObject(latency_waitable, kCpuWaitMs) == WAIT_OBJECT_0) return;
    if (fences->ProgressValue() < fences->last_submitted.load()) {
        EnterStall("the D3D12 frame-latency object stayed unsignalled for 500 ms");
        return;
    }
    // The queue is idle, so DXGI is holding the frames back, not the GPU.
    latency_wait_off = true;
    LOGW("presenter: the D3D12 frame-latency object stayed unsignalled for 500 ms with an idle queue; frames are not "
         "paced on it until it signals again");
}

void D3D12Presenter::SetMaximumFrameLatency(UINT latency) {
    try {
        if (!impl_) return;
        const UINT n = std::clamp<UINT>(latency, 1, 16);
        if (n == impl_->latency) return;
        impl_->latency = n;
        if (!impl_->latency_waitable || !chain_) return;
        const HRESULT hr = chain_->SetMaximumFrameLatency(n);
        if (FAILED(hr))
            LOGW("presenter: SetMaximumFrameLatency(%u) failed: 0x%08lX", n, static_cast<unsigned long>(hr));
        else
            LOGI("presenter: D3D12 chain frame latency %u", n);
    } catch (...) {
    }
}

bool D3D12Presenter::HasLatencyWaitable() const { return impl_ && impl_->latency_waitable; }

bool D3D12Presenter::UsesStreamline() const { return impl_ && impl_->sl; }

void D3D12Presenter::ShutdownStreamlineOnRelease() {
    if (impl_) impl_->shutdown_sl = true;
}

uint32_t D3D12Presenter::FramesWithMarkers() const { return impl_ ? impl_->frames_with_markers : 0; }

uint32_t D3D12Presenter::MarkerProblems() const {
    if (!impl_) return 0;
    std::lock_guard<std::mutex> lock(impl_->marker_mu);
    return impl_->pcl.AbandonedFrames() + impl_->pcl.OutOfOrderCalls();
}

NgxEvaluateSink* D3D12Presenter::CaptureSink() const { return impl_ ? impl_->coordinator.get() : nullptr; }

bool D3D12Presenter::DlssgSupported() const { return impl_ && impl_->fg_supported; }

D3D12Presenter::FgTotals D3D12Presenter::Totals() const { return impl_ ? impl_->totals : FgTotals(); }

bool D3D12Presenter::Impl::SourceMatches(ID3D11Texture2D* source) {
    if (!source || !shared11) return false;
    D3D11_TEXTURE2D_DESC sd{};
    source->GetDesc(&sd);
    // CopyResource needs identical sizes and one typeless family; anything
    // else would be dropped by the runtime, so the frame keeps stale content.
    const bool ok = sd.Width == width && sd.Height == height && sd.SampleDesc.Count == 1 && sd.MipLevels == 1 &&
                    sd.ArraySize == 1 && IsRgba8Family(sd.Format);
    if (!ok && !warned_source) {
        warned_source = true;
        LOGW("presenter: source %ux%u format %d does not match the shared back buffer %ux%u; frames are not copied "
             "(logged once)",
             sd.Width, sd.Height, static_cast<int>(sd.Format), width, height);
    }
    return ok;
}

void D3D12Presenter::Impl::MaybeLogStats(ID3D11DeviceContext* ctx) {
    double s = 0;
    n11 += timer11.Collect(ctx, &s);
    sum11 += s;
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG elapsed = now - stats_start;
    if (elapsed < 1000) return;
    char a11[32];
    char a12[32];
    FormatAvg(sum11, n11, a11, sizeof(a11));
    FormatAvg(sum12, n12, a12, sizeof(a12));
    const double seconds = static_cast<double>(elapsed) / 1000.0;
    char vram[48] = "n/a";
    uint64_t vramUsage = 0;
    uint64_t vramBudget = 0;
    if (QueryVramMiB(adapter3.Get(), &vramUsage, &vramBudget, nullptr))
        std::snprintf(vram, sizeof(vram), "%llu/%llu", static_cast<unsigned long long>(vramUsage),
                      static_cast<unsigned long long>(vramBudget));
    // generated: frames DLSS-G added in this second, the sum of
    // numFramesActuallyPresented - 1 over its Presents (n/a until
    // slDLSSGGetState answered once).
    char generated[24] = "n/a";
    if (polled_state)
        std::snprintf(generated, sizeof(generated), "%llu", static_cast<unsigned long long>(stats_generated));
    uint32_t pclProblems = 0;
    {
        std::lock_guard<std::mutex> lock(marker_mu);
        pclProblems = pcl.AbandonedFrames() + pcl.OutOfOrderCalls();
    }
    // base: CSP frames; presented: frames the D3D12 chain accepted (occluded
    // ones included). fg: the DLSS-G mode Streamline has; its off reason is
    // logged when it changes. The M3 fields sit before vram_mib; vram_mib:
    // local video memory usage/budget of the render adapter, last so that
    // parsers of the fields before it keep working.
    LOGI("stats: base_fps=%.1f presented_fps=%.1f skipped=%u failed=%u occluded=%u uncopied=%u max_frame_ms=%.1f "
         "max_present_ms=%.1f bridge_gpu_ms d3d11=%s d3d12=%s fg=%s stalls=%u streamline=%s reflex=%s "
         "pcl_problems=%u captures=%u camera_fresh=%u tagged=%u fg_frames=%u generated=%s double_evaluates=%u "
         "vram_mib=%s",
         stats_presents / seconds, stats_delivered / seconds, stats_skipped, stats_failed, stats_occluded,
         stats_uncopied, max_frame_ms, max_present_ms, a11, a12, mode_on ? "on" : "off", stalls, sl ? "on" : "off",
         reflex_on ? "on" : "off", pclProblems, stats_captures, stats_camera_fresh, stats_tagged, stats_fg_frames,
         generated, stats_double, vram);
    stats_captures = stats_camera_fresh = stats_tagged = stats_fg_frames = stats_double = 0;
    stats_generated = 0;
    stats_start = now;
    stats_presents = stats_delivered = stats_skipped = stats_failed = stats_occluded = stats_uncopied = 0;
    max_frame_ms = max_present_ms = 0;
    sum11 = sum12 = 0;
    n11 = n12 = 0;
}

// ---------------------------------------------------------------- stalls

void D3D12Presenter::Impl::EnterStall(const char* reason) {
    stalled = true;
    stall_start = GetTickCount64();
    ++stalls;
    // Spec 6.4 "Stalled mode": DLSS-G off, on this (the presenting) thread.
    prev_had_inputs = false;
    if (mode_on) {
        SetMode(false);
        LogMode(false, "D3D12 stall", false);
    }
    const uint64_t pending = fences->pending_wait.load();
    fences->CpuSignalShared(pending);  // releases CSP's D3D11 queue
    LOGW("D3D12 stall: %s (progress %llu, last submitted %llu, pending wait %llu); D3D11 released, nothing is "
         "delivered until the queue catches up",
         reason, static_cast<unsigned long long>(fences->ProgressValue()),
         static_cast<unsigned long long>(fences->last_submitted.load()), static_cast<unsigned long long>(pending));
}

bool D3D12Presenter::Impl::TryLeaveStall(D3D12Presenter& self) {
    const uint64_t progress = fences->ProgressValue();
    const uint64_t submitted = fences->last_submitted.load();
    if (progress >= submitted) {
        stalled = false;
        LOGI("D3D12 stall over after %llu ms: progress reached %llu",
             static_cast<unsigned long long>(GetTickCount64() - stall_start),
             static_cast<unsigned long long>(progress));
        return true;
    }
    if (watchdog->MsSinceProgress() >= kGiveUpMs) {
        Stop(self, DXGI_ERROR_DEVICE_HUNG, "no D3D12 progress for 4 s");
        return false;
    }
    // After the release, D3D11 signals values above the ones still queued on
    // D3D12. Where a late, lower queue signal lowers the shared fence (not seen
    // on the reference machine, but fence values are not required to be
    // monotonic), the queue's next wait would stay pending; re-raise the fence.
    fences->CpuSignalShared(std::max(fences->pending_wait.load(), last11));
    return false;
}

void D3D12Presenter::Impl::Stop(D3D12Presenter& self, HRESULT error, const char* reason) {
    if (self.stopped_) return;
    if (fences) fences->CpuSignalShared(fences->pending_wait.load());
    self.stopped_ = true;
    stop_error = error;
    last_device_error.store(error);
    const HRESULT removed = device12 ? device12->GetDeviceRemovedReason() : S_OK;
    LOGE("presenter stopped: %s; Present now returns 0x%08lX (GetDeviceRemovedReason 0x%08lX)", reason,
         static_cast<unsigned long>(error), static_cast<unsigned long>(removed));
}

bool D3D12Presenter::Impl::Drain(D3D12Presenter& self, const char* why) {
    if (!fences) return false;
    if (ctx11) ctx11->Flush();
    if (fences->CpuWaitProgress(fences->last_submitted.load(), kCpuWaitMs)) {
        if (stalled) TryLeaveStall(self);
        return true;
    }
    if (!stalled) {
        char reason[128];
        std::snprintf(reason, sizeof(reason), "%s could not drain the D3D12 queue in 500 ms", why);
        EnterStall(reason);
    }
    return false;
}

HRESULT D3D12Presenter::Impl::ApplyResize(D3D12Presenter& self, UINT w, UINT h) {
    InternalCallScope internal;
    resize_pending = false;
    for (auto& b : back) b.Reset();
    HRESULT hr = self.chain_->ResizeBuffers(kBuffers, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, chain_flags);
    if (FAILED(hr)) {
        char reason[96];
        std::snprintf(reason, sizeof(reason), "D3D12 ResizeBuffers(%u, %u) failed: 0x%08lX", w, h,
                      static_cast<unsigned long>(hr));
        Stop(self, StopErrorFor(hr), reason);
        return hr;
    }
    std::string err;
    if (!FetchBuffers(self, &err) || ((w != width || h != height) && !CreateSharedTexture(w, h, &err))) {
        Stop(self, DXGI_ERROR_DEVICE_REMOVED, err.c_str());
        return E_FAIL;
    }
    for (UINT i = 0; i < kBuffers; ++i) {
        buffer_value[i] = 0;
        timer12_pending[i] = false;
    }
    // Spec 6.3 "Resizing" step 3: Streamline tracks the index through this call.
    if (sl) self.chain_->GetCurrentBackBufferIndex();
    width = w;
    height = h;
    warned_source = false;
    LOGI("presenter resized to %ux%u", w, h);
    return S_OK;
}

HRESULT D3D12Presenter::Resize(UINT width, UINT height) {
    try {
        if (!impl_ || !chain_) return DXGI_ERROR_INVALID_CALL;
        if (stopped_) return impl_->stop_error;
        if (width == 0 || height == 0) return DXGI_ERROR_INVALID_CALL;
        impl_->BeforeChainChange(*this);
        if (!impl_->Drain(*this, "ResizeBuffers")) {
            // Stall path: the resize runs once the queue has caught up.
            impl_->resize_pending = true;
            impl_->pending_w = width;
            impl_->pending_h = height;
            LOGW("presenter: resize to %ux%u deferred by the D3D12 stall; it runs once the queue catches up", width,
                 height);
            return S_OK;
        }
        return impl_->ApplyResize(*this, width, height);
    } catch (...) {
        LOGE("presenter: unexpected exception in Resize");
        return E_FAIL;
    }
}

// ---------------------------------------------------------------- queries

HRESULT D3D12Presenter::TestPresent() const {
    if (!impl_) return S_OK;
    const HRESULT device = impl_->last_device_error.load();
    if (FAILED(device)) return device;
    if (impl_->last_present_hr.load() == DXGI_STATUS_OCCLUDED && IsIconic(impl_->hwnd)) return DXGI_STATUS_OCCLUDED;
    return S_OK;
}

HRESULT D3D12Presenter::SetFullscreenState(BOOL fullscreen, IDXGIOutput* target) {
    try {
        if (!impl_ || !chain_) return DXGI_ERROR_INVALID_CALL;
        if (!stopped_) impl_->BeforeChainChange(*this);
        if (!stopped_ && !impl_->Drain(*this, "SetFullscreenState")) return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
        return chain_->SetFullscreenState(fullscreen, target);
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT D3D12Presenter::GetFullscreenState(BOOL* fullscreen, IDXGIOutput** target) {
    try {
        if (!chain_) return DXGI_ERROR_INVALID_CALL;
        return chain_->GetFullscreenState(fullscreen, target);
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT D3D12Presenter::ResizeTarget(const DXGI_MODE_DESC* params) {
    try {
        if (!impl_ || !chain_) return DXGI_ERROR_INVALID_CALL;
        if (!stopped_) impl_->BeforeChainChange(*this);
        if (!stopped_ && !impl_->Drain(*this, "ResizeTarget")) return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
        return chain_->ResizeTarget(params);
    } catch (...) {
        return E_FAIL;
    }
}

// ---------------------------------------------------------------- debug hook

void D3D12Presenter::Impl::StartDebugStall() {
    if (!debug_fence || debug_thread.joinable()) return;
    queue->Wait(debug_fence.Get(), 1);
    LOGW("presenter: debug stall: the D3D12 queue now waits %lu ms (ACDLSSG_DEBUG_STALL_MS)",
         static_cast<unsigned long>(debug_stall_ms));
    ID3D12Fence* fence = debug_fence.Get();
    const HANDLE cancel = debug_cancel;
    const DWORD ms = debug_stall_ms;
    try {
        debug_thread = std::thread([fence, cancel, ms] {
            WaitForSingleObject(cancel, ms);
            fence->Signal(1);
        });
    } catch (...) {
        fence->Signal(1);  // never leave the queue blocked for good
    }
}

// ---------------------------------------------------------------- shutdown

void D3D12Presenter::Impl::Shutdown(D3D12Presenter& self) {
    // M3: no evaluate reaches the coordinator from here on.
    if (ngx_attached) {
        NgxHook::Get().SetSink(nullptr);
        ngx_attached = false;
    }
    // Spec 6.3 "Final Release" step 1: DLSS-G off and null tags.
    if (sl && !self.stopped_) {
        if (mode_on) {
            SetMode(false);
            LogMode(false, "the swap chain is released", false);
        }
        if (prev_tagged && token) sl->SetNullTags(*token, kViewport);
        prev_tagged = false;
    }
    // Release the debug wait first so that the drain below can finish.
    if (debug_thread.joinable()) {
        SetEvent(debug_cancel);
        debug_thread.join();
    }
    if (debug_fence) debug_fence->Signal(1);
    if (debug_cancel) {
        CloseHandle(debug_cancel);
        debug_cancel = nullptr;
    }
    if (latency_waitable) {
        CloseHandle(latency_waitable);
        latency_waitable = nullptr;
    }

    if (self.chain_ && !self.stopped_) {
        BOOL fullscreen = FALSE;
        if (SUCCEEDED(self.chain_->GetFullscreenState(&fullscreen, nullptr)) && fullscreen)
            self.chain_->SetFullscreenState(FALSE, nullptr);
    }

    bool drained = true;
    if (fences && queue) {
        fences->CpuSignalShared(fences->pending_wait.load());
        if (ctx11) ctx11->Flush();
        const uint64_t final = fences->Next();
        if (SUCCEEDED(queue->Signal(fences->Progress(), final))) {
            drained = false;
            const ULONGLONG start = GetTickCount64();
            for (;;) {
                if (fences->ProgressValue() >= final) {
                    drained = true;
                    break;
                }
                if (GetTickCount64() - start >= kCpuWaitMs) break;
                // Same late-signal problem as in TryLeaveStall.
                fences->CpuSignalShared(std::max(fences->pending_wait.load(), last11));
                fences->CpuWaitProgress(final, kPumpSliceMs);
            }
        }
    }
    if (watchdog) watchdog->Stop();
    watchdog.reset();

    // Spec 6.3 "Final Release" step 4: slShutdown after the drain and before
    // any D3D12/DXGI object is released.
    if (sl && shutdown_sl) sl->Shutdown();

    if (!drained) {
        // Releasing objects the GPU still uses can crash the driver later;
        // leaking them is the lesser harm at this point.
        LOGE("presenter: the D3D12 queue did not drain in 500 ms; its objects are leaked");
        for (UINT i = 0; i < kBuffers; ++i) {
            list[i].Detach();
            alloc[i].Detach();
            back[i].Detach();
        }
        static_cast<void>(timer12.release());
        static_cast<void>(slots.release());
        coordinator.reset();  // holds no GPU object
        shared12.Detach();
        self.chain_.Detach();
        static_cast<void>(fences.release());
        debug_fence.Detach();
        queue.Detach();
        device_sl.Detach();
        device12.Detach();
        return;
    }
    for (UINT i = 0; i < kBuffers; ++i) {
        list[i].Reset();
        alloc[i].Reset();
        back[i].Reset();
    }
    self.chain_.Reset();
    timer12.reset();
    coordinator.reset();
    slots.reset();
    shared12.Reset();
    shared11.Reset();
    fences.reset();
    debug_fence.Reset();
    queue.Reset();
    factory.Reset();
    factory_native.Reset();
    device_sl.Reset();
    device12.Reset();
    LOGI("presenter released");
}

D3D12Presenter::~D3D12Presenter() {
    try {
        if (impl_) impl_->Shutdown(*this);
    } catch (...) {
        LOGE("presenter: unexpected exception during shutdown");
    }
}

}  // namespace acdb
