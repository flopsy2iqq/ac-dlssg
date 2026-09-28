#include "hidden_chain.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>

#include "factory_hook.h"
#include "internal_call.h"
#include "log.h"

using Microsoft::WRL::ComPtr;

namespace acdb {
namespace {

constexpr wchar_t kClassName[] = L"acdb_hidden";
constexpr wchar_t kWindowTitle[] = L"ac-dlssg hidden";
constexpr auto kWindowTimeout = std::chrono::seconds(5);

LRESULT CALLBACK HiddenWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // The chain keeps this HWND for its whole life; nothing may destroy it
    // before the owning thread does.
    if (msg == WM_CLOSE) return 0;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HINSTANCE ThisModule() {
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&HiddenWndProc), &mod);
    return mod;
}

// The class is registered while at least one hidden window exists and
// unregistered with the last one: Windows never unregisters a DLL's classes
// on unload, and a stale class would point at an unmapped window procedure.
std::mutex g_class_mu;
int g_class_users = 0;  // guarded by g_class_mu

bool AcquireWindowClass(HINSTANCE inst, DWORD* err) {
    std::lock_guard<std::mutex> lock(g_class_mu);
    if (g_class_users == 0) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &HiddenWndProc;
        wc.hInstance = inst;
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) {
            const DWORD e = GetLastError();
            if (e != ERROR_CLASS_ALREADY_EXISTS) {
                *err = e;
                return false;
            }
        }
    }
    ++g_class_users;
    return true;
}

void ReleaseWindowClass(HINSTANCE inst) {
    std::lock_guard<std::mutex> lock(g_class_mu);
    if (--g_class_users == 0) UnregisterClassW(kClassName, inst);
}

struct WindowStartup {
    std::mutex mu;
    std::condition_variable cv;
    bool finished = false;   // the thread created the window or gave up
    bool abandoned = false;  // Create timed out and no longer waits
    HWND hwnd = nullptr;
    DWORD thread_id = 0;
    DWORD error = 0;
};

void WindowThread(std::shared_ptr<WindowStartup> st, int width, int height) {
    const HINSTANCE inst = ThisModule();
    HWND hwnd = nullptr;
    bool have_class = false;
    try {
        DWORD err = 0;
        have_class = AcquireWindowClass(inst, &err);
        if (have_class) {
            // WS_POPUP without WS_VISIBLE, and never shown. The tool-window and
            // no-activate styles keep it out of the taskbar and Alt+Tab even
            // if something showed it.
            hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName, kWindowTitle, WS_POPUP, 0, 0,
                                   width, height, nullptr, nullptr, inst, nullptr);
            if (!hwnd) err = GetLastError();
        }

        bool abandoned = false;
        {
            std::lock_guard<std::mutex> lock(st->mu);
            abandoned = st->abandoned;
            st->hwnd = abandoned ? nullptr : hwnd;
            st->thread_id = GetCurrentThreadId();
            st->error = err;
            st->finished = true;
        }
        st->cv.notify_all();

        // DXGI sends messages to the chain's window from other threads, so
        // this thread pumps until the destructor posts WM_QUIT.
        if (hwnd && !abandoned) {
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    } catch (...) {
        LOGE("hidden chain: window thread failed with an exception");
    }
    if (hwnd) DestroyWindow(hwnd);
    if (have_class) {
        try {
            ReleaseWindowClass(inst);
        } catch (...) {
        }
    }
}

bool StartWindowThread(int width, int height, std::thread* thread, DWORD* thread_id, HWND* hwnd,
                       std::string* error) {
    auto st = std::make_shared<WindowStartup>();
    std::thread t(WindowThread, st, width, height);

    std::unique_lock<std::mutex> lock(st->mu);
    if (!st->cv.wait_for(lock, kWindowTimeout, [&] { return st->finished; })) {
        // The thread destroys its window and exits by itself once it sees this.
        st->abandoned = true;
        lock.unlock();
        t.detach();
        *error = "timed out waiting for the hidden window";
        return false;
    }
    if (!st->hwnd) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "hidden window creation failed: error %lu",
                      static_cast<unsigned long>(st->error));
        lock.unlock();
        t.join();
        *error = buf;
        return false;
    }
    *thread_id = st->thread_id;
    *hwnd = st->hwnd;
    lock.unlock();
    *thread = std::move(t);
    return true;
}

std::string HrError(const char* what, HRESULT hr) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s failed: 0x%08lX", what, static_cast<unsigned long>(hr));
    return buf;
}

}  // namespace

std::unique_ptr<HiddenChain> HiddenChain::Create(IDXGIFactory2* factory, ID3D11Device* device,
                                                 const DXGI_SWAP_CHAIN_DESC1& gameDesc, std::string* error) {
    std::string err;
    try {
        if (!factory || !device) {
            err = "null factory or device";
        } else {
            std::unique_ptr<HiddenChain> self(new HiddenChain());
            // Sized like the chain only for tidiness; the chain's own size is explicit.
            const int w = static_cast<int>(std::clamp<UINT>(gameDesc.Width, 1, 16384));
            const int h = static_cast<int>(std::clamp<UINT>(gameDesc.Height, 1, 16384));
            if (StartWindowThread(w, h, &self->thread_, &self->thread_id_, &self->hwnd_, &err)) {
                DXGI_SWAP_CHAIN_DESC1 desc{};
                desc.Width = gameDesc.Width;
                desc.Height = gameDesc.Height;
                desc.Format = gameDesc.Format;
                desc.Stereo = FALSE;
                desc.SampleDesc = gameDesc.SampleDesc;
                desc.BufferUsage = gameDesc.BufferUsage;
                desc.BufferCount = 2;
                desc.Scaling = DXGI_SCALING_STRETCH;
                desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
                desc.Flags = 0;

                HRESULT hr;
                {
                    InternalCallScope internal;
                    if (FactoryHookInstalled()) {
                        hr = CallOriginalCreateSwapChainForHwnd(factory, device, self->hwnd_, &desc, nullptr,
                                                                nullptr, &self->chain_);
                    } else {
                        hr = factory->CreateSwapChainForHwnd(device, self->hwnd_, &desc, nullptr, nullptr,
                                                             &self->chain_);
                    }
                    if (SUCCEEDED(hr) && self->chain_) {
                        // MakeWindowAssociation belongs on the factory that owns the chain.
                        ComPtr<IDXGIFactory> owner;
                        if (FAILED(self->chain_->GetParent(IID_PPV_ARGS(&owner)))) owner = factory;
                        const HRESULT mwa = owner->MakeWindowAssociation(self->hwnd_, DXGI_MWA_NO_WINDOW_CHANGES);
                        if (FAILED(mwa))
                            LOGW("hidden chain: MakeWindowAssociation failed: 0x%08lX", static_cast<unsigned long>(mwa));
                    }
                }

                if (SUCCEEDED(hr) && !self->chain_) hr = E_POINTER;
                if (FAILED(hr)) {
                    err = HrError("hidden CreateSwapChainForHwnd", hr);
                } else {
                    hr = self->chain_->GetBuffer(0, IID_PPV_ARGS(&self->buffer0_));
                    if (SUCCEEDED(hr) && !self->buffer0_) hr = E_POINTER;
                    if (FAILED(hr)) err = HrError("hidden GetBuffer(0)", hr);
                }
                if (SUCCEEDED(hr)) {
                    LOGI("hidden chain created: %ux%u format %d usage 0x%X hwnd %p", desc.Width, desc.Height,
                         static_cast<int>(desc.Format), static_cast<unsigned>(desc.BufferUsage),
                         static_cast<void*>(self->hwnd_));
                    return self;
                }
            }
            // self's destructor releases the partial chain and joins the window thread.
        }
    } catch (const std::exception& e) {
        err = std::string("hidden chain: exception: ") + e.what();
    } catch (...) {
        err = "hidden chain: unknown exception";
    }
    LOGE("hidden chain: %s", err.c_str());
    if (error) {
        try {
            *error = err;
        } catch (...) {
        }
    }
    return nullptr;
}

HiddenChain::~HiddenChain() {
    buffer0_.Reset();
    // The chain's final release may still message the window, which the
    // thread keeps pumping until WM_QUIT below.
    chain_.Reset();
    if (!thread_.joinable()) return;
    try {
        bool posted = false;
        for (int attempt = 0; attempt < 50 && !posted; ++attempt) {
            posted = PostThreadMessageW(thread_id_, WM_QUIT, 0, 0) != FALSE;
            if (!posted) Sleep(10);
        }
        if (posted) {
            thread_.join();
        } else {
            // Joining now would hang forever; leak the idle thread instead.
            LOGE("hidden chain: PostThreadMessage(WM_QUIT) failed: %lu", GetLastError());
            thread_.detach();
        }
    } catch (...) {
        LOGE("hidden chain: joining the window thread failed");
    }
}

HRESULT HiddenChain::Resize(UINT width, UINT height, DXGI_FORMAT format) {
    try {
        if (!chain_) return DXGI_ERROR_INVALID_CALL;
        buffer0_.Reset();
        HRESULT hr = chain_->ResizeBuffers(2, width, height, format, 0);
        if (FAILED(hr))
            LOGE("hidden chain: ResizeBuffers(%u, %u, %d) failed: 0x%08lX", width, height, static_cast<int>(format),
                 static_cast<unsigned long>(hr));
        // Also after a failure: the old buffers stay valid then, and Buffer0
        // must never be null.
        const HRESULT get = chain_->GetBuffer(0, IID_PPV_ARGS(&buffer0_));
        if (FAILED(get)) {
            LOGE("hidden chain: GetBuffer(0) after resize failed: 0x%08lX", static_cast<unsigned long>(get));
            if (SUCCEEDED(hr)) hr = get;
        }
        if (SUCCEEDED(hr)) LOGD("hidden chain resized: %ux%u format %d", width, height, static_cast<int>(format));
        return hr;
    } catch (...) {
        return E_UNEXPECTED;
    }
}

}  // namespace acdb
