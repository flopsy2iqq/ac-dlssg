#include "proxy_swapchain.h"

#include <cstring>
#include <exception>

#include "log.h"
#include "present_flags.h"

using Microsoft::WRL::ComPtr;

namespace acdb {
namespace {

std::atomic<long> g_live{0};

// COM boundary: nothing may escape into CSP or ReShade.
template <typename Body>
HRESULT Guarded(const char* method, Body&& body) noexcept {
    try {
        return body();
    } catch (...) {
        LOGE("ProxySwapChain::%s: unexpected exception", method);
        return E_FAIL;
    }
}

// DXGI cannot add or remove these through ResizeBuffers; they stay as created.
constexpr UINT kFixedFlags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

BOOL CurrentWindowed(D3D12Presenter* presenter, BOOL fallback) {
    BOOL fullscreen = FALSE;
    if (presenter && SUCCEEDED(presenter->GetFullscreenState(&fullscreen, nullptr))) return !fullscreen;
    return fallback;
}

}  // namespace

HRESULT ProxySwapChain::Create(IDXGIFactory2* factory, ID3D11Device* device11, HWND hwnd,
                               const DXGI_SWAP_CHAIN_DESC1& desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                               const Config& config, IDXGISwapChain1** out, std::string* error) {
    std::string err;
    HRESULT hr = E_FAIL;
    try {
        if (out) *out = nullptr;
        if (!factory || !device11 || !hwnd || !out) {
            hr = E_INVALIDARG;
            err = "ProxySwapChain: null factory, device, window or output pointer";
        } else {
            DXGI_SWAP_CHAIN_DESC1 d = desc;
            if (d.Width == 0 || d.Height == 0) {
                RECT rc{};
                if (GetClientRect(hwnd, &rc)) {
                    if (d.Width == 0) d.Width = static_cast<UINT>(rc.right - rc.left);
                    if (d.Height == 0) d.Height = static_cast<UINT>(rc.bottom - rc.top);
                }
            }

            ComPtr<ProxySwapChain> proxy;
            proxy.Attach(new ProxySwapChain());
            ++g_live;
            proxy->factory_ = factory;
            proxy->device11_ = device11;
            device11->GetImmediateContext(&proxy->ctx11_);
            proxy->hwnd_ = hwnd;
            proxy->desc_ = d;
            if (fullscreenDesc) {
                proxy->fs_desc_ = *fullscreenDesc;
            } else {
                proxy->fs_desc_ = {};
                proxy->fs_desc_.Windowed = TRUE;
            }

            proxy->hidden_ = HiddenChain::Create(factory, device11, d, &err);
            if (proxy->hidden_) {
                PresenterCreateInfo info;
                info.device11 = device11;
                info.hwnd = hwnd;
                info.game_desc = d;
                proxy->presenter_ = D3D12Presenter::Create(info, &err);
            }
            if (proxy->hidden_ && proxy->presenter_) {
                const bool waitable = (d.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0;
                if (proxy->latency_.Init(waitable, config.max_frame_latency)) {
                    proxy->presenter_->SetMaximumFrameLatency(proxy->latency_.CurrentLatency());
                    LOGI("ProxySwapChain created: %ux%u format %d, %u buffers, flags 0x%X, latency %s", d.Width,
                         d.Height, static_cast<int>(d.Format), d.BufferCount, d.Flags,
                         waitable ? "waitable" : "not waitable");
                    *out = proxy.Detach();
                    return S_OK;
                }
                err = "ProxySwapChain: CreateSemaphoreW failed";
            }
            // proxy's final Release tears down whatever was created.
        }
    } catch (const std::exception& e) {
        err = std::string("ProxySwapChain: exception: ") + e.what();
    } catch (...) {
        err = "ProxySwapChain: unknown exception";
    }
    if (err.empty()) err = "ProxySwapChain: creation failed";
    if (error) {
        try {
            *error = err;
        } catch (...) {
        }
    }
    return hr;
}

ProxySwapChain::~ProxySwapChain() {
    // Final Release (spec 6.3) without the Streamline steps: the presenter
    // leaves fullscreen, CPU-signals pending_wait and drains before it
    // releases the D3D12 side; the hidden chain goes after it.
    try {
        presenter_.reset();
    } catch (...) {
    }
    try {
        hidden_.reset();
    } catch (...) {
    }
    try {
        private_ifaces_.clear();
        private_data_.clear();
    } catch (...) {
    }
    --g_live;
    LOGI("ProxySwapChain released");
}

long ProxySwapChain::LiveCount() { return g_live.load(); }

// ---------------------------------------------------------------- IUnknown

HRESULT STDMETHODCALLTYPE ProxySwapChain::QueryInterface(REFIID riid, void** ppv) {
    return Guarded("QueryInterface", [&]() -> HRESULT {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
            riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1) ||
            riid == __uuidof(IDXGISwapChain2) || riid == __uuidof(IDXGISwapChain3) ||
            riid == __uuidof(IDXGISwapChain4)) {
            *ppv = static_cast<IDXGISwapChain4*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    });
}

ULONG STDMETHODCALLTYPE ProxySwapChain::AddRef() { return ++ref_; }

ULONG STDMETHODCALLTYPE ProxySwapChain::Release() {
    ULONG current = ref_.load();
    for (;;) {
        if (current == 0) return 0;  // over-release: never destroy twice
        if (ref_.compare_exchange_weak(current, current - 1)) break;
    }
    if (current == 1) delete this;
    return current - 1;
}

// ---------------------------------------------------------------- IDXGIObject

HRESULT STDMETHODCALLTYPE ProxySwapChain::SetPrivateData(REFGUID name, UINT size, const void* data) {
    return Guarded("SetPrivateData", [&]() -> HRESULT {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        if (!data) {
            if (size != 0) return E_INVALIDARG;
            private_data_.erase(name);
            private_ifaces_.erase(name);
            return S_OK;
        }
        private_ifaces_.erase(name);
        private_data_[name].assign(static_cast<const char*>(data), size);
        return S_OK;
    });
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::SetPrivateDataInterface(REFGUID name, const IUnknown* unknown) {
    return Guarded("SetPrivateDataInterface", [&]() -> HRESULT {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        private_data_.erase(name);
        if (!unknown) {
            private_ifaces_.erase(name);
            return S_OK;
        }
        private_ifaces_[name] = const_cast<IUnknown*>(unknown);
        return S_OK;
    });
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::GetPrivateData(REFGUID name, UINT* size, void* data) {
    return Guarded("GetPrivateData", [&]() -> HRESULT {
        if (!size) return E_INVALIDARG;
        std::lock_guard<std::recursive_mutex> lock(mu_);
        if (const auto it = private_data_.find(name); it != private_data_.end()) {
            const UINT n = static_cast<UINT>(it->second.size());
            if (!data) {
                *size = n;
                return S_OK;
            }
            if (*size < n) {
                *size = n;
                return DXGI_ERROR_MORE_DATA;
            }
            if (n) std::memcpy(data, it->second.data(), n);
            *size = n;
            return S_OK;
        }
        if (const auto it = private_ifaces_.find(name); it != private_ifaces_.end()) {
            constexpr UINT n = sizeof(IUnknown*);
            if (!data) {
                *size = n;
                return S_OK;
            }
            if (*size < n) {
                *size = n;
                return DXGI_ERROR_MORE_DATA;
            }
            // Like DXGI: the caller receives a reference.
            IUnknown* unknown = it->second.Get();
            unknown->AddRef();
            std::memcpy(data, &unknown, n);
            *size = n;
            return S_OK;
        }
        *size = 0;
        return DXGI_ERROR_NOT_FOUND;
    });
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::GetParent(REFIID riid, void** parent) {
    return Guarded("GetParent", [&]() -> HRESULT {
        if (!parent) return E_INVALIDARG;
        *parent = nullptr;
        std::lock_guard<std::recursive_mutex> lock(mu_);
        return factory_ ? factory_->QueryInterface(riid, parent) : E_NOINTERFACE;
    });
}

// ---------------------------------------------------------------- IDXGIDeviceSubObject

HRESULT STDMETHODCALLTYPE ProxySwapChain::GetDevice(REFIID riid, void** device) {
    return Guarded("GetDevice", [&]() -> HRESULT {
        if (!device) return E_INVALIDARG;
        *device = nullptr;
        std::lock_guard<std::recursive_mutex> lock(mu_);
        return device11_ ? device11_->QueryInterface(riid, device) : E_NOINTERFACE;
    });
}

// ---------------------------------------------------------------- IDXGISwapChain

HRESULT ProxySwapChain::PresentCommon(UINT syncInterval, UINT flags) {
    const PresentPlan plan = PlanPresent(syncInterval, flags, false, true, false, true);
    if (plan.is_test) {
        // Not a frame (spec 6.3): no copy, no signal, no semaphore release.
        return Guarded("Present(TEST)", [&]() -> HRESULT {
            std::lock_guard<std::recursive_mutex> lock(mu_);
            return presenter_ ? presenter_->TestPresent() : S_OK;
        });
    }
    HRESULT hr = E_FAIL;
    bool released = false;
    try {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        try {
            ++present_count_;
            hr = presenter_ && hidden_
                     ? presenter_->PresentFrame(ctx11_.Get(), hidden_->Buffer0(), syncInterval, flags)
                     : DXGI_ERROR_INVALID_CALL;
        } catch (...) {
            LOGE("ProxySwapChain::Present: unexpected exception");
            hr = E_FAIL;
        }
        latency_.ReleaseOne();
        released = true;
    } catch (...) {
        LOGE("ProxySwapChain::Present: unexpected exception around the frame");
    }
    // Every non-test Present releases one count, whatever happened above.
    if (!released) {
        try {
            latency_.ReleaseOne();
        } catch (...) {
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::Present(UINT syncInterval, UINT flags) {
    return PresentCommon(syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::GetBuffer(UINT buffer, REFIID riid, void** surface) {
    return Guarded("GetBuffer", [&]() -> HRESULT {
        if (!surface) return E_INVALIDARG;
        *surface = nullptr;
        if (buffer != 0) return DXGI_ERROR_INVALID_CALL;
        std::lock_guard<std::recursive_mutex> lock(mu_);
        ID3D11Texture2D* buffer0 = hidden_ ? hidden_->Buffer0() : nullptr;
        return buffer0 ? buffer0->QueryInterface(riid, surface) : DXGI_ERROR_INVALID_CALL;
    });
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::SetFullscreenState(BOOL fullscreen, IDXGIOutput* target) {
    return Guarded("SetFullscreenState", [&]() -> HRESULT {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        if (!presenter_) return DXGI_ERROR_INVALID_CALL;
        const HRESULT hr = presenter_->SetFullscreenState(fullscreen, target);
        if (SUCCEEDED(hr)) fs_desc_.Windowed = fullscreen ? FALSE : TRUE;
        LOGI("SetFullscreenState(%d): 0x%08lX", fullscreen ? 1 : 0, static_cast<unsigned long>(hr));
        return hr;
    });
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::GetFullscreenState(BOOL* fullscreen, IDXGIOutput** target) {
    return Guarded("GetFullscreenState", [&]() -> HRESULT {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        return presenter_ ? presenter_->GetFullscreenState(fullscreen, target) : DXGI_ERROR_INVALID_CALL;
    });
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC* desc) {
    return Guarded("GetDesc", [&]() -> HRESULT {
        if (!desc) return E_INVALIDARG;
        std::lock_guard<std::recursive_mutex> lock(mu_);
        DXGI_SWAP_CHAIN_DESC d{};
        d.BufferDesc.Width = desc_.Width;
        d.BufferDesc.Height = desc_.Height;
        d.BufferDesc.RefreshRate = fs_desc_.RefreshRate;
        d.BufferDesc.Format = desc_.Format;
        d.BufferDesc.ScanlineOrdering = fs_desc_.ScanlineOrdering;
        d.BufferDesc.Scaling = fs_desc_.Scaling;
        d.SampleDesc = desc_.SampleDesc;
        d.BufferUsage = desc_.BufferUsage;
        d.BufferCount = desc_.BufferCount;
        d.OutputWindow = hwnd_;
        d.Windowed = CurrentWindowed(presenter_.get(), fs_desc_.Windowed);
        d.SwapEffect = desc_.SwapEffect;
        d.Flags = desc_.Flags;
        *desc = d;
        return S_OK;
    });
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format,
                                                        UINT flags) {
    return Guarded("ResizeBuffers", [&]() -> HRESULT {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        if (!presenter_ || !hidden_) return DXGI_ERROR_INVALID_CALL;
        if (count > DXGI_MAX_SWAP_CHAIN_BUFFERS) return DXGI_ERROR_INVALID_CALL;
        const UINT newCount = count ? count : desc_.BufferCount;
        const DXGI_FORMAT newFormat = format != DXGI_FORMAT_UNKNOWN ? format : desc_.Format;
        UINT w = width;
        UINT h = height;
        if (w == 0 || h == 0) {
            RECT rc{};
            if (GetClientRect(hwnd_, &rc)) {
                if (w == 0) w = static_cast<UINT>(rc.right - rc.left);
                if (h == 0) h = static_cast<UINT>(rc.bottom - rc.top);
            }
        }
        // A minimized window has an empty client area; keep the current size.
        if (w == 0) w = desc_.Width;
        if (h == 0) h = desc_.Height;
        const UINT newFlags = (flags & ~kFixedFlags) | (desc_.Flags & kFixedFlags);

        // The hidden chain first: it is the step the game can make fail (a
        // view on buffer 0 still alive gives DXGI_ERROR_INVALID_CALL), and
        // then nothing may have changed, as with a real chain. It needs no
        // D3D12 drain: only the D3D11 immediate context uses it.
        HRESULT hr = hidden_->Resize(w, h, newFormat);
        if (FAILED(hr)) {
            LOGE("ResizeBuffers(%u, %u, %u, %d, 0x%X): hidden chain resize failed: 0x%08lX; the chain keeps %ux%u",
                 count, width, height, static_cast<int>(format), flags, static_cast<unsigned long>(hr), desc_.Width,
                 desc_.Height);
            return hr;
        }
        desc_.Width = w;
        desc_.Height = h;
        desc_.Format = newFormat;
        desc_.BufferCount = newCount;
        desc_.Flags = newFlags;
        // A stall defers this and returns S_OK; any other failure stops the presenter.
        hr = presenter_->Resize(w, h);
        if (FAILED(hr)) {
            LOGE("ResizeBuffers(%u, %u, %u, %d, 0x%X): presenter resize failed: 0x%08lX", count, width, height,
                 static_cast<int>(format), flags, static_cast<unsigned long>(hr));
            return hr;
        }
        LOGI("ResizeBuffers: %ux%u format %d, %u buffers, flags 0x%X", w, h, static_cast<int>(newFormat), newCount,
             newFlags);
        return S_OK;
    });
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::ResizeTarget(const DXGI_MODE_DESC* params) {
    return Guarded("ResizeTarget", [&]() -> HRESULT {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        return presenter_ ? presenter_->ResizeTarget(params) : DXGI_ERROR_INVALID_CALL;
    });
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::GetContainingOutput(IDXGIOutput** output) {
    return Guarded("GetContainingOutput", [&]() -> HRESULT {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        IDXGISwapChain4* chain = presenter_ ? presenter_->Chain() : nullptr;
        return chain ? chain->GetContainingOutput(output) : DXGI_ERROR_INVALID_CALL;
    });
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::GetFrameStatistics(DXGI_FRAME_STATISTICS* stats) {
    return Guarded("GetFrameStatistics", [&]() -> HRESULT {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        IDXGISwapChain4* chain = presenter_ ? presenter_->Chain() : nullptr;
        return chain ? chain->GetFrameStatistics(stats) : DXGI_ERROR_INVALID_CALL;
    });
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::GetLastPresentCount(UINT* count) {
    return Guarded("GetLastPresentCount", [&]() -> HRESULT {
        if (!count) return E_INVALIDARG;
        std::lock_guard<std::recursive_mutex> lock(mu_);
        *count = present_count_;
        return S_OK;
    });
}

// ---------------------------------------------------------------- IDXGISwapChain1

HRESULT STDMETHODCALLTYPE ProxySwapChain::GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) {
    return Guarded("GetDesc1", [&]() -> HRESULT {
        if (!desc) return E_INVALIDARG;
        std::lock_guard<std::recursive_mutex> lock(mu_);
        *desc = desc_;
        return S_OK;
    });
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* desc) {
    return Guarded("GetFullscreenDesc", [&]() -> HRESULT {
        if (!desc) return E_INVALIDARG;
        std::lock_guard<std::recursive_mutex> lock(mu_);
        *desc = fs_desc_;
        desc->Windowed = CurrentWindowed(presenter_.get(), fs_desc_.Windowed);
        return S_OK;
    });
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::GetHwnd(HWND* hwnd) {
    return Guarded("GetHwnd", [&]() -> HRESULT {
        if (!hwnd) return E_INVALIDARG;
        std::lock_guard<std::recursive_mutex> lock(mu_);
        *hwnd = hwnd_;
        return S_OK;
    });
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::GetCoreWindow(REFIID /*riid*/, void** unk) {
    return Guarded("GetCoreWindow", [&]() -> HRESULT {
        if (!unk) return E_INVALIDARG;
        *unk = nullptr;
        return DXGI_ERROR_INVALID_CALL;  // an HWND chain
    });
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::Present1(UINT syncInterval, UINT flags,
                                                   const DXGI_PRESENT_PARAMETERS* /*params*/) {
    // Dirty rectangles and scroll are ignored (spec 6.3).
    return PresentCommon(syncInterval, flags);
}

BOOL STDMETHODCALLTYPE ProxySwapChain::IsTemporaryMonoSupported() {
    try {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        IDXGISwapChain4* chain = presenter_ ? presenter_->Chain() : nullptr;
        return chain ? chain->IsTemporaryMonoSupported() : FALSE;
    } catch (...) {
        return FALSE;
    }
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::GetRestrictToOutput(IDXGIOutput** output) {
    return Guarded("GetRestrictToOutput", [&]() -> HRESULT {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        IDXGISwapChain4* chain = presenter_ ? presenter_->Chain() : nullptr;
        return chain ? chain->GetRestrictToOutput(output) : DXGI_ERROR_INVALID_CALL;
    });
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::SetBackgroundColor(const DXGI_RGBA* color) {
    return Guarded("SetBackgroundColor", [&]() -> HRESULT {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        IDXGISwapChain4* chain = presenter_ ? presenter_->Chain() : nullptr;
        return chain ? chain->SetBackgroundColor(color) : DXGI_ERROR_INVALID_CALL;
    });
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::GetBackgroundColor(DXGI_RGBA* color) {
    return Guarded("GetBackgroundColor", [&]() -> HRESULT {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        IDXGISwapChain4* chain = presenter_ ? presenter_->Chain() : nullptr;
        return chain ? chain->GetBackgroundColor(color) : DXGI_ERROR_INVALID_CALL;
    });
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::SetRotation(DXGI_MODE_ROTATION rotation) {
    return Guarded("SetRotation", [&]() -> HRESULT {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        IDXGISwapChain4* chain = presenter_ ? presenter_->Chain() : nullptr;
        return chain ? chain->SetRotation(rotation) : DXGI_ERROR_INVALID_CALL;
    });
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::GetRotation(DXGI_MODE_ROTATION* rotation) {
    return Guarded("GetRotation", [&]() -> HRESULT {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        IDXGISwapChain4* chain = presenter_ ? presenter_->Chain() : nullptr;
        return chain ? chain->GetRotation(rotation) : DXGI_ERROR_INVALID_CALL;
    });
}

// ---------------------------------------------------------------- IDXGISwapChain2

HRESULT STDMETHODCALLTYPE ProxySwapChain::SetSourceSize(UINT width, UINT height) {
    return Guarded("SetSourceSize", [&]() -> HRESULT {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        IDXGISwapChain4* chain = presenter_ ? presenter_->Chain() : nullptr;
        return chain ? chain->SetSourceSize(width, height) : DXGI_ERROR_INVALID_CALL;
    });
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::GetSourceSize(UINT* width, UINT* height) {
    return Guarded("GetSourceSize", [&]() -> HRESULT {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        IDXGISwapChain4* chain = presenter_ ? presenter_->Chain() : nullptr;
        return chain ? chain->GetSourceSize(width, height) : DXGI_ERROR_INVALID_CALL;
    });
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::SetMaximumFrameLatency(UINT maxLatency) {
    return Guarded("SetMaximumFrameLatency", [&]() -> HRESULT {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        const HRESULT hr = latency_.SetMaximum(maxLatency);
        LOGI("SetMaximumFrameLatency(%u): 0x%08lX, latency now %u", maxLatency, static_cast<unsigned long>(hr),
             latency_.CurrentLatency());
        if (SUCCEEDED(hr) && presenter_) presenter_->SetMaximumFrameLatency(latency_.CurrentLatency());
        return hr;
    });
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::GetMaximumFrameLatency(UINT* maxLatency) {
    return Guarded("GetMaximumFrameLatency", [&]() -> HRESULT {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        return latency_.GetMaximum(maxLatency);
    });
}

HANDLE STDMETHODCALLTYPE ProxySwapChain::GetFrameLatencyWaitableObject() {
    try {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        return latency_.DuplicateForCaller();
    } catch (...) {
        return nullptr;
    }
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::SetMatrixTransform(const DXGI_MATRIX_3X2_F* matrix) {
    return Guarded("SetMatrixTransform", [&]() -> HRESULT {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        IDXGISwapChain4* chain = presenter_ ? presenter_->Chain() : nullptr;
        return chain ? chain->SetMatrixTransform(matrix) : DXGI_ERROR_INVALID_CALL;
    });
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::GetMatrixTransform(DXGI_MATRIX_3X2_F* matrix) {
    return Guarded("GetMatrixTransform", [&]() -> HRESULT {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        IDXGISwapChain4* chain = presenter_ ? presenter_->Chain() : nullptr;
        return chain ? chain->GetMatrixTransform(matrix) : DXGI_ERROR_INVALID_CALL;
    });
}

// ---------------------------------------------------------------- IDXGISwapChain3

UINT STDMETHODCALLTYPE ProxySwapChain::GetCurrentBackBufferIndex() { return 0; }

HRESULT STDMETHODCALLTYPE ProxySwapChain::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE colorSpace, UINT* support) {
    return Guarded("CheckColorSpaceSupport", [&]() -> HRESULT {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        IDXGISwapChain4* chain = presenter_ ? presenter_->Chain() : nullptr;
        return chain ? chain->CheckColorSpaceSupport(colorSpace, support) : DXGI_ERROR_INVALID_CALL;
    });
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::SetColorSpace1(DXGI_COLOR_SPACE_TYPE colorSpace) {
    return Guarded("SetColorSpace1", [&]() -> HRESULT {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        IDXGISwapChain4* chain = presenter_ ? presenter_->Chain() : nullptr;
        return chain ? chain->SetColorSpace1(colorSpace) : DXGI_ERROR_INVALID_CALL;
    });
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::ResizeBuffers1(UINT /*count*/, UINT /*width*/, UINT /*height*/,
                                                         DXGI_FORMAT /*format*/, UINT /*flags*/,
                                                         const UINT* /*nodeMask*/, IUnknown* const* /*queues*/) {
    return DXGI_ERROR_INVALID_CALL;  // D3D12-only API; CSP's chain is D3D11
}

// ---------------------------------------------------------------- IDXGISwapChain4

HRESULT STDMETHODCALLTYPE ProxySwapChain::SetHDRMetaData(DXGI_HDR_METADATA_TYPE type, UINT size, void* metadata) {
    return Guarded("SetHDRMetaData", [&]() -> HRESULT {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        IDXGISwapChain4* chain = presenter_ ? presenter_->Chain() : nullptr;
        return chain ? chain->SetHDRMetaData(type, size, metadata) : DXGI_ERROR_INVALID_CALL;
    });
}

}  // namespace acdb
