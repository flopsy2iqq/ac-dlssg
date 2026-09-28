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

#include "internal_call.h"
#include "log.h"
#include "present_flags.h"

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

}  // namespace

struct D3D12Presenter::Impl {
    // D3D11 side (CSP's device).
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> ctx11;
    ComPtr<ID3D11DeviceContext4> ctx4;
    ID3D11DeviceContext* ctx4_of = nullptr;  // the context ctx4 was queried from
    HWND hwnd = nullptr;

    // D3D12 side.
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGIFactory4> factory;
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

    bool Init(D3D12Presenter& self, const PresenterCreateInfo& info, std::string* err);
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

    // 3. Direct queue.
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    if (FAILED(hr)) {
        *err = HrText("ID3D12Device::CreateCommandQueue", hr);
        return false;
    }

    // 4. Factory of the adapter; tearing support.
    hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        *err = HrText("IDXGIAdapter::GetParent(IDXGIFactory4)", hr);
        return false;
    }
    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory.As(&factory5))) {
        BOOL allow = FALSE;
        tearing = SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))) &&
                  allow;
    }
    chain_flags = tearing ? static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) : 0u;
    // M1 pacing: a game that paces on its waitable object gets one from the
    // D3D12 chain too (see the header; removed in M2 for Streamline's pacer).
    if (gd.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
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
    if (chain_flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
        hr = self.chain_->SetMaximumFrameLatency(latency);
        if (FAILED(hr))
            LOGW("presenter: SetMaximumFrameLatency(%u) failed: 0x%08lX", latency, static_cast<unsigned long>(hr));
        latency_waitable = self.chain_->GetFrameLatencyWaitableObject();
        if (!latency_waitable) {
            *err = "D3D12 chain: GetFrameLatencyWaitableObject returned NULL";
            return false;
        }
    }

    // 6. DXGI must not touch the game window or react to Alt+Enter.
    hr = factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(hr)) LOGW("presenter: MakeWindowAssociation failed: 0x%08lX", static_cast<unsigned long>(hr));

    // 7. Fences, shared back buffer, command recording, timers, watchdog.
    fences = FencePair::Create(device12.Get(), device11.Get(), err);
    if (!fences) return false;
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

    wchar_t env[32] = {};
    const DWORD envLen = GetEnvironmentVariableW(kDebugStallEnv, env, static_cast<DWORD>(std::size(env)));
    if (envLen > 0 && envLen < std::size(env)) {
        const unsigned long ms = std::wcstoul(env, nullptr, 10);
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
    stats_start = GetTickCount64();

    char name[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, static_cast<int>(sizeof(name)) - 1, nullptr, nullptr);
    LOGI("presenter created: %s (LUID %08lX:%08lX), D3D12 chain %ux%u, %u buffers, flags 0x%X, tearing %s, "
         "frame-latency object %s, hwnd %p",
         name, static_cast<unsigned long>(ad.AdapterLuid.HighPart), static_cast<unsigned long>(ad.AdapterLuid.LowPart),
         width, height, kBuffers, chain_flags, tearing ? "yes" : "no", latency_waitable ? "yes" : "no",
         static_cast<void*>(hwnd));
    return true;
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
    const PresentPlan plan = PlanPresent(cspSync, cspFlags, tearing, !fullscreen, false, true);
    LogPresentMode(cspSync, cspFlags, plan, fullscreen);
    const HRESULT presentHr = self.chain_->Present(plan.sync, plan.flags);

    const uint64_t done = fences->Next();
    queue->Signal(fences->Shared12(), done);
    queue->Signal(fences->Progress(), done);
    fences->last_submitted.store(done);
    buffer_value[idx] = done;
    prev_value = done;
    last_present_hr.store(presentHr);

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
    if (!FAILED(presentHr)) WaitForLatency();
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
    // base: CSP frames; presented: frames the D3D12 chain accepted (occluded
    // ones included). M1 has no frame generation, hence fg=off.
    LOGI("stats: base_fps=%.1f presented_fps=%.1f skipped=%u failed=%u occluded=%u uncopied=%u max_frame_ms=%.1f "
         "max_present_ms=%.1f bridge_gpu_ms d3d11=%s d3d12=%s fg=off stalls=%u",
         stats_presents / seconds, stats_delivered / seconds, stats_skipped, stats_failed, stats_occluded,
         stats_uncopied, max_frame_ms, max_present_ms, a11, a12, stalls);
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
        shared12.Detach();
        self.chain_.Detach();
        static_cast<void>(fences.release());
        debug_fence.Detach();
        queue.Detach();
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
    shared12.Reset();
    shared11.Reset();
    fences.reset();
    debug_fence.Reset();
    queue.Reset();
    factory.Reset();
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
