#include "factory_hook.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cwchar>
#include <iterator>
#include <mutex>
#include <string>
#include <type_traits>

#include "bootstrap.h"
#include "compat.h"
#include "internal_call.h"
#include "log.h"
#include "proxy_swapchain.h"
#include "vtable_patch.h"

using Microsoft::WRL::ComPtr;

namespace acdb {
namespace {

using PFN_CreateSwapChain = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                        IDXGISwapChain**);
using PFN_CreateSwapChainForHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
                                                               const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                               IDXGISwapChain1**);
using PFN_CreateSwapChainForCoreWindow = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*,
                                                                     const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*,
                                                                     IDXGISwapChain1**);
using PFN_CreateSwapChainForComposition = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*,
                                                                      const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*,
                                                                      IDXGISwapChain1**);

std::atomic<PFN_CreateSwapChain> g_orig_create{nullptr};
std::atomic<PFN_CreateSwapChainForHwnd> g_orig_hwnd{nullptr};
std::atomic<PFN_CreateSwapChainForCoreWindow> g_orig_core{nullptr};
std::atomic<PFN_CreateSwapChainForComposition> g_orig_comp{nullptr};

std::mutex g_install_mu;
bool g_install_done = false;  // guarded by g_install_mu
std::atomic<bool> g_installed{false};
std::atomic<void**> g_hooked_vtable{nullptr};

// Stores the original before the slot changes, because another thread may
// enter the hook as soon as the slot is patched. Fn comes from the atomic
// only: the hooks are noexcept, a different function type.
template <typename Fn>
bool PatchSlot(void** vtable, size_t index, std::type_identity_t<Fn> hook, std::atomic<Fn>* original) {
    void* const current = vtable[index];
    if (current == reinterpret_cast<void*>(hook)) return true;
    original->store(reinterpret_cast<Fn>(current));
    void* const previous = PatchVtableSlot(vtable, index, reinterpret_cast<void*>(hook));
    if (!previous) {
        original->store(nullptr);
        return false;
    }
    if (previous != current) original->store(reinterpret_cast<Fn>(previous));
    return true;
}

// DXGI semantics: a zero Width or Height takes the window's client size.
void ResolveSize(HWND hwnd, DXGI_SWAP_CHAIN_DESC1* desc) {
    if (desc->Width != 0 && desc->Height != 0) return;
    RECT rc{};
    if (!hwnd || !GetClientRect(hwnd, &rc)) return;
    if (desc->Width == 0) desc->Width = static_cast<UINT>(rc.right - rc.left);
    if (desc->Height == 0) desc->Height = static_cast<UINT>(rc.bottom - rc.top);
}

std::string DecisionReason(const ProxyDecisionInputs& in, const std::string& bootstrapReason,
                           const std::string& compatReason) {
    if (in.internal_call) return "internal call";
    if (!in.is_d3d11_device) return "the device is not a D3D11 device";
    if (!in.is_main_window) return "not the main game window (class acsW)";
    if (!in.bootstrap_possible) return bootstrapReason.empty() ? std::string("bridge not possible") : bootstrapReason;
    if (!in.compat_ok) return compatReason;
    if (in.streamline_shut_down) return "Streamline was already shut down in this process";
    return {};
}

// Decides, and on "proxy" creates the ProxySwapChain. False means the caller
// passes through. Exceptions propagate to the hook, which also passes through.
bool TryProxy(IDXGIFactory2* self, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1& requested,
              const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc, IDXGISwapChain1** out) {
    ProxyDecisionInputs in;
    in.internal_call = IsInternalCall();
    ComPtr<ID3D11Device> device11;
    in.is_d3d11_device =
        device &&
        SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Device), reinterpret_cast<void**>(device11.GetAddressOf()))) &&
        device11;
    in.is_main_window = IsMainGameWindow(hwnd);
    const BootstrapState& bs = BootstrapRunOnce();
    in.bootstrap_possible = bs.possible;

    DXGI_SWAP_CHAIN_DESC1 desc = requested;
    ResolveSize(hwnd, &desc);

    CompatResult compat;
    if (in.is_main_window) {
        // Files and HAGS come from the bootstrap; add-ons may have loaded since.
        CompatInputs ci = bs.compat;
        ci.dlss5_bridge_loaded = GetModuleHandleW(L"dlss5-bridge.addon64") != nullptr;
        ci.renodx_dlss5_loaded = GetModuleHandleW(L"renodx-dlss5.addon64") != nullptr;
        compat = EvaluateCompat(ci, desc.Width, desc.Height);
        in.compat_ok = compat.ok;
    }
    in.streamline_shut_down = false;

    const bool proxy = ShouldProxy(in);
    const std::string reason = proxy ? std::string() : DecisionReason(in, bs.reason, compat.reason);
    const LogLevel level = in.is_main_window ? LogLevel::Info : LogLevel::Debug;
    LogWrite(level,
             "CreateSwapChainForHwnd: hwnd %p%s, %ux%u format %d, %u buffers, swap effect %d, flags 0x%X, %s: %s%s",
             static_cast<void*>(hwnd), in.is_main_window ? " (main window)" : "", desc.Width, desc.Height,
             static_cast<int>(desc.Format), desc.BufferCount, static_cast<int>(desc.SwapEffect), desc.Flags,
             fullscreenDesc ? "fullscreen desc" : "no fullscreen desc", proxy ? "proxy" : "pass-through: ",
             reason.c_str());
    if (!proxy) return false;

    ComPtr<IDXGISwapChain1> created;
    std::string error;
    HRESULT hr = E_FAIL;
    {
        // Everything the proxy creates (D3D12 device, chains) must not re-enter our hooks.
        InternalCallScope internal;
        hr = ProxySwapChain::Create(self, device11.Get(), hwnd, desc, fullscreenDesc, bs.config,
                                    created.GetAddressOf(), &error);
    }
    if (FAILED(hr) || !created) {
        LOGE("proxy swap chain creation failed (0x%08lX): %s; passing through", static_cast<unsigned long>(hr),
             error.empty() ? "no details" : error.c_str());
        return false;
    }
    LOGI("proxy swap chain created %ux%u", desc.Width, desc.Height);
    *out = created.Detach();
    return true;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                              IDXGISwapChain** swapChain) noexcept {
    const auto original = g_orig_create.load();
    if (!original) return DXGI_ERROR_INVALID_CALL;
    if (!IsInternalCall() && desc) {
        LOGD("CreateSwapChain (legacy): hwnd %p, %ux%u, windowed %d: pass-through",
             static_cast<void*>(desc->OutputWindow), desc->BufferDesc.Width, desc->BufferDesc.Height,
             desc->Windowed ? 1 : 0);
    }
    return original(self, device, desc, swapChain);
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device, HWND hwnd,
                                                     const DXGI_SWAP_CHAIN_DESC1* desc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                                                     IDXGIOutput* restrictToOutput,
                                                     IDXGISwapChain1** swapChain) noexcept {
    const auto original = g_orig_hwnd.load();
    if (!original) return DXGI_ERROR_INVALID_CALL;
    // Invalid arguments are left for DXGI to report.
    if (!IsInternalCall() && self && desc && hwnd && swapChain) {
        try {
            IDXGISwapChain1* proxy = nullptr;
            if (TryProxy(self, device, hwnd, *desc, fullscreenDesc, &proxy)) {
                *swapChain = proxy;
                return S_OK;
            }
        } catch (...) {
            LOGE("CreateSwapChainForHwnd hook: unexpected exception; passing through");
        }
    }
    return original(self, device, hwnd, desc, fullscreenDesc, restrictToOutput, swapChain);
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForCoreWindow(IDXGIFactory2* self, IUnknown* device, IUnknown* window,
                                                           const DXGI_SWAP_CHAIN_DESC1* desc,
                                                           IDXGIOutput* restrictToOutput,
                                                           IDXGISwapChain1** swapChain) noexcept {
    const auto original = g_orig_core.load();
    if (!original) return DXGI_ERROR_INVALID_CALL;
    if (!IsInternalCall()) LOGD("CreateSwapChainForCoreWindow: pass-through");
    return original(self, device, window, desc, restrictToOutput, swapChain);
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForComposition(IDXGIFactory2* self, IUnknown* device,
                                                            const DXGI_SWAP_CHAIN_DESC1* desc,
                                                            IDXGIOutput* restrictToOutput,
                                                            IDXGISwapChain1** swapChain) noexcept {
    const auto original = g_orig_comp.load();
    if (!original) return DXGI_ERROR_INVALID_CALL;
    if (!IsInternalCall()) LOGD("CreateSwapChainForComposition: pass-through");
    return original(self, device, desc, restrictToOutput, swapChain);
}

void** VtableOf(IUnknown* object) { return *reinterpret_cast<void***>(object); }

void Install(IUnknown* factory) {
    std::lock_guard<std::mutex> lock(g_install_mu);
    if (g_install_done) return;

    ComPtr<IDXGIFactory2> factory2;
    if (FAILED(factory->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory2.GetAddressOf()))) ||
        !factory2) {
        // Not a factory we can hook; a later factory gets the attempt.
        LOGW("factory hook: the factory does not implement IDXGIFactory2; not hooked");
        return;
    }
    g_install_done = true;

    void** vtable = VtableOf(factory2.Get());
    // The patch assumes one vtable for every factory interface of the class.
    ComPtr<IDXGIFactory> factory0;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory), reinterpret_cast<void**>(factory0.GetAddressOf()))) &&
        factory0 && VtableOf(factory0.Get()) != vtable) {
        LOGW("factory hook: IDXGIFactory and IDXGIFactory2 use different vtables; legacy CreateSwapChain may be missed");
    }

    const bool ok10 = PatchSlot(vtable, kSlotCreateSwapChain, &HookCreateSwapChain, &g_orig_create);
    const bool ok15 = PatchSlot(vtable, kSlotCreateSwapChainForHwnd, &HookCreateSwapChainForHwnd, &g_orig_hwnd);
    const bool ok16 =
        PatchSlot(vtable, kSlotCreateSwapChainForCoreWindow, &HookCreateSwapChainForCoreWindow, &g_orig_core);
    const bool ok24 =
        PatchSlot(vtable, kSlotCreateSwapChainForComposition, &HookCreateSwapChainForComposition, &g_orig_comp);
    if (ok15) g_hooked_vtable.store(vtable);
    g_installed.store(ok15);

    if (ok10 && ok15 && ok16 && ok24) {
        LOGI("factory hook installed on vtable %p (slots 10, 15, 16, 24)", static_cast<void*>(vtable));
    } else {
        LOGE("factory hook on vtable %p incomplete: slot 10 %s, 15 %s, 16 %s, 24 %s", static_cast<void*>(vtable),
             ok10 ? "ok" : "failed", ok15 ? "ok" : "failed", ok16 ? "ok" : "failed", ok24 ? "ok" : "failed");
    }
}

}  // namespace

void FactoryHookInstallOnce(IUnknown* factory) {
    if (!factory) return;
    try {
        Install(factory);
    } catch (...) {
        LOGE("factory hook installation failed with an exception");
    }
}

bool FactoryHookInstalled() { return g_installed.load(); }

bool ShouldProxy(const ProxyDecisionInputs& in) {
    return !in.internal_call && in.is_d3d11_device && in.is_main_window && in.bootstrap_possible && in.compat_ok &&
           !in.streamline_shut_down;
}

bool IsMainGameWindow(HWND hwnd) {
    if (!hwnd) return false;
    // Full-size buffer: a truncated longer name such as "acsWindow" must not match.
    wchar_t name[257] = {};
    const int n = GetClassNameW(hwnd, name, static_cast<int>(std::size(name)));
    return IsMainGameWindowClassName(name, n);
}

bool IsMainGameWindowClassName(const wchar_t* name, int length) {
    return name && length == 4 && std::wmemcmp(name, L"acsW", 4) == 0;
}

HRESULT CallOriginalCreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device, HWND hwnd,
                                           const DXGI_SWAP_CHAIN_DESC1* desc,
                                           const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                                           IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain) {
    if (!self) return DXGI_ERROR_INVALID_CALL;
    // The saved original belongs to the patched class only; a factory of any
    // other class still has its own original in its vtable.
    const auto original = g_orig_hwnd.load();
    if (original && VtableOf(self) == g_hooked_vtable.load())
        return original(self, device, hwnd, desc, fullscreenDesc, restrictToOutput, swapChain);
    return self->CreateSwapChainForHwnd(device, hwnd, desc, fullscreenDesc, restrictToOutput, swapChain);
}

}  // namespace acdb
