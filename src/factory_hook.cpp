#include "factory_hook.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <mutex>
#include <string>
#include <type_traits>

#include "adapter_caps.h"
#include "bootstrap.h"
#include "compat.h"
#include "internal_call.h"
#include "log.h"
#include "proxy_swapchain.h"
#include "streamline_runtime.h"
#include "vtable_patch.h"

using Microsoft::WRL::ComPtr;

namespace acdb {
namespace {

constexpr uint32_t kNvidiaVendorId = 0x10DE;

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

std::string ToUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    s.resize(static_cast<size_t>(n) - 1);
    return s;
}

RenderAdapter AdapterOf(ID3D11Device* device) {
    RenderAdapter a;
    if (!device) {
        a.error = "the device is not a D3D11 device";
        return a;
    }
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    HRESULT hr = device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgiDevice.GetAddressOf()));
    if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC d{};
    if (SUCCEEDED(hr)) hr = adapter->GetDesc(&d);
    if (FAILED(hr)) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "IDXGIDevice::GetAdapter/GetDesc failed: 0x%08lX", static_cast<unsigned long>(hr));
        a.error = buf;
        return a;
    }
    a.known = true;
    a.luid = d.AdapterLuid;
    a.vendor_id = d.VendorId;
    a.device_id = d.DeviceId;
    a.description = ToUtf8(d.Description);
    return a;
}

// ACDLSSG_DEBUG_HAGS (see the header).
void ApplyDebugHags(const LUID& luid, AdapterHags* kmt) {
    wchar_t value[32] = {};
    const DWORD n = GetEnvironmentVariableW(kDebugHagsEnv, value, static_cast<DWORD>(std::size(value)));
    if (n == 0) return;
    const std::string text = n < std::size(value) ? ToUtf8(value) : std::string("(too long)");
    if (text == "off") {
        *kmt = AdapterHags();
        kmt->state = HagsState::Off;
        LOGW("ACDLSSG_DEBUG_HAGS=off: HAGS of LUID %s is taken as off instead of D3DKMT's answer (test hook)",
             LuidText(luid).c_str());
    } else if (text == "fail") {
        *kmt = AdapterHags();
        kmt->reason = "ACDLSSG_DEBUG_HAGS=fail";
        LOGW("ACDLSSG_DEBUG_HAGS=fail: the D3DKMT query for LUID %s counts as failed (test hook)",
             LuidText(luid).c_str());
    } else {
        LOGW("ACDLSSG_DEBUG_HAGS=%s is not \"off\" or \"fail\"; ignored", text.c_str());
    }
}

// HAGS of the render adapter, logged with the adapter for every main-window
// swap chain.
HagsDecision RenderAdapterHags(const RenderAdapter& adapter, bool registryOn) {
    AdapterHags kmt;
    if (adapter.known) {
        kmt = QueryAdapterKmt(adapter.luid).hags;
        ApplyDebugHags(adapter.luid, &kmt);
    } else {
        kmt.reason = "the adapter of CSP's device is unknown (" + adapter.error + ")";
    }
    const HagsDecision hags = DecideHags(adapter.luid, kmt, registryOn);
    if (adapter.known) {
        LOGI("render adapter: %s, vendor 0x%04X device 0x%04X, LUID %s; HAGS %s (%s)", adapter.description.c_str(),
             adapter.vendor_id, adapter.device_id, LuidText(adapter.luid).c_str(), hags.on ? "on" : "off",
             hags.source.c_str());
    } else {
        LOGI("render adapter: unknown (%s); HAGS %s (%s)", adapter.error.c_str(), hags.on ? "on" : "off",
             hags.source.c_str());
    }
    return hags;
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
    std::string adapterReason;
    if (in.is_main_window) {
        // CSP's own adapter must be NVIDIA; its HAGS state decides rule 10,
        // with the registry value read at bootstrap only as the fallback when
        // D3DKMT cannot answer.
        const RenderAdapter adapter = AdapterOf(device11.Get());
        adapterReason = RenderAdapterRefusal(adapter);
        in.nvidia_adapter = adapterReason.empty();
        const HagsDecision hags = RenderAdapterHags(adapter, bs.compat.hags_on);
        // Files come from the bootstrap; add-ons may have loaded since.
        CompatInputs ci = bs.compat;
        ci.dlss5_bridge_loaded = GetModuleHandleW(L"dlss5-bridge.addon64") != nullptr;
        ci.renodx_dlss5_loaded = GetModuleHandleW(L"renodx-dlss5.addon64") != nullptr;
        compat = EvaluateChainCompat(ci, hags, desc.Width, desc.Height);
        in.compat_ok = compat.ok;
    }
    in.streamline_shut_down = StreamlineRuntime::Get().IsShutDown();

    const bool proxy = ShouldProxy(in);
    const std::string reason = proxy ? std::string() : PassThroughReason(in, bs.reason, adapterReason, compat.reason);
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
        // bootstrap_possible implies streamline_ok, so production always takes the Streamline path.
        StreamlineRuntime* streamline = bs.streamline_ok ? &StreamlineRuntime::Get() : nullptr;
        hr = ProxySwapChain::Create(self, device11.Get(), hwnd, desc, fullscreenDesc, bs.config, streamline,
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
           !in.streamline_shut_down && in.nvidia_adapter;
}

HagsDecision DecideHags(const LUID& luid, const AdapterHags& kmt, bool registryOn) {
    HagsDecision d;
    const std::string kmtSource = "D3DKMT for LUID " + LuidText(luid);
    if (kmt.state != HagsState::Unknown) {
        d.on = kmt.state == HagsState::On;
        d.source = kmtSource;
        return d;
    }
    d.on = registryOn;
    d.source = "registry fallback (" + kmtSource + ": " + (kmt.reason.empty() ? "no answer" : kmt.reason) + ")";
    return d;
}

CompatResult EvaluateChainCompat(CompatInputs inputs, const HagsDecision& hags, unsigned width, unsigned height) {
    inputs.hags_on = hags.on;
    CompatResult r = EvaluateCompat(inputs, width, height);
    if (r.ok || hags.on) return r;
    // Rule 10 is the last rule: the refusal is HAGS exactly when HAGS on passes.
    inputs.hags_on = true;
    if (EvaluateCompat(inputs, width, height).ok) r.reason += " (" + hags.source + ")";
    return r;
}

std::string RenderAdapterRefusal(const RenderAdapter& adapter) {
    if (!adapter.known) return "the adapter of CSP's device is unknown (" + adapter.error + ")";
    if (adapter.vendor_id == kNvidiaVendorId) return {};
    char vendor[16];
    std::snprintf(vendor, sizeof(vendor), "0x%04X", adapter.vendor_id);
    return "CSP renders on " + adapter.description + " (vendor " + vendor +
           "), not an NVIDIA GPU: set acs.exe to High performance in Windows Settings > System > Display > Graphics";
}

std::string PassThroughReason(const ProxyDecisionInputs& in, const std::string& bootstrapReason,
                              const std::string& adapterReason, const std::string& compatReason) {
    if (in.internal_call) return "internal call";
    if (!in.is_d3d11_device) return "the device is not a D3D11 device";
    if (!in.is_main_window) return "not the main game window (class acsW)";
    if (!in.bootstrap_possible) return bootstrapReason.empty() ? std::string("bridge not possible") : bootstrapReason;
    // Spec 8: after slShutdown the bridge stays off for the rest of the process.
    if (in.streamline_shut_down) return "Streamline already shut down";
    if (!in.nvidia_adapter) return adapterReason.empty() ? std::string("not an NVIDIA adapter") : adapterReason;
    if (!in.compat_ok) return compatReason;
    return {};
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
