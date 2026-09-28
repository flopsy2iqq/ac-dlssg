#pragma once
// Class-wide patch of the real DXGI factory vtable (spec 6.2).
#include <windows.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <string>

#include "adapter_caps.h"
#include "compat.h"

namespace acdb {

// Slots in the IDXGIFactory2 vtable.
constexpr size_t kSlotCreateSwapChain = 10;
constexpr size_t kSlotCreateSwapChainForHwnd = 15;
constexpr size_t kSlotCreateSwapChainForCoreWindow = 16;
constexpr size_t kSlotCreateSwapChainForComposition = 24;

// Patches the four slots of the factory's IDXGIFactory2 vtable once per
// process (later calls do nothing). Must be called before the factory is
// handed back to ReShade, so that ReShade saves our functions as originals.
// Slot 15 may proxy; slots 10, 16 and 24 pass through and log at Debug.
void FactoryHookInstallOnce(IUnknown* factory);
bool FactoryHookInstalled();

struct ProxyDecisionInputs {
    bool internal_call = false;         // IsInternalCall()
    bool is_d3d11_device = false;       // pDevice QIs to ID3D11Device
    bool is_main_window = false;        // IsMainGameWindow(hWnd)
    bool bootstrap_possible = false;    // BootstrapRunOnce().possible
    bool compat_ok = false;             // EvaluateChainCompat(...).ok
    bool streamline_shut_down = false;  // StreamlineRuntime::Get().IsShutDown()
    bool nvidia_adapter = false;        // RenderAdapterRefusal(adapter of pDevice) is empty
};
// True only when !internal_call and every other condition holds.
bool ShouldProxy(const ProxyDecisionInputs& in);

// HAGS for the compatibility decision of one main-window swap chain (rule
// 10): the state of the adapter of CSP's D3D11 device, identified by its LUID.
struct HagsDecision {
    bool on = false;
    // "D3DKMT for LUID <HHHHHHHH:LLLLLLLL>", or, when D3DKMT has no answer,
    // "registry fallback (D3DKMT for LUID <luid>: <why not>)".
    std::string source;
};
// Pure: kmt's state when it is On or Off; otherwise registryOn (HwSchMode ==
// 2, read at bootstrap).
HagsDecision DecideHags(const LUID& luid, const AdapterHags& kmt, bool registryOn);

// Pure: EvaluateCompat on the bootstrap's inputs with hags_on = hags.on. When
// rule 10 refuses, its reason keeps the rule's text and names the source:
// "hardware-accelerated GPU scheduling is off (<hags.source>)".
CompatResult EvaluateChainCompat(CompatInputs inputs, const HagsDecision& hags, unsigned width, unsigned height);

// Test hook, read for every main-window swap chain: ACDLSSG_DEBUG_HAGS=off
// replaces D3DKMT's HAGS answer with off, ACDLSSG_DEBUG_HAGS=fail makes the
// query count as failed (so the registry decides). Either logs a WARN; any
// other value is ignored with a WARN.
constexpr wchar_t kDebugHagsEnv[] = L"ACDLSSG_DEBUG_HAGS";

// The adapter of CSP's D3D11 device (IDXGIDevice::GetAdapter). Its LUID is
// the identity every per-adapter decision uses; never EnumAdapters(0) or the
// outputs of a GPU, which on a hybrid laptop belong to the integrated GPU.
struct RenderAdapter {
    bool known = false;  // IDXGIDevice::GetAdapter and GetDesc succeeded
    LUID luid{};
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    std::string description;  // UTF-8
    std::string error;        // why not known
};

// Pure: empty when the adapter is NVIDIA (vendor 0x10DE); otherwise the
// pass-through reason:
//   "CSP renders on <description> (vendor 0xVVVV), not an NVIDIA GPU: set
//    acs.exe to High performance in Windows Settings > System > Display > Graphics"
// or, for an unknown adapter, "the adapter of CSP's device is unknown (<error>)".
std::string RenderAdapterRefusal(const RenderAdapter& adapter);

// Pure: why a swap chain passes through, empty when ShouldProxy(in). In
// order: internal call, not a D3D11 device, not the main window, bootstrap
// not possible (bootstrapReason), Streamline already shut down, not an NVIDIA
// adapter (adapterReason), compatibility (compatReason).
std::string PassThroughReason(const ProxyDecisionInputs& in, const std::string& bootstrapReason,
                              const std::string& adapterReason, const std::string& compatReason);

// Window class name is exactly "acsW" (case-sensitive).
bool IsMainGameWindow(HWND hwnd);
// The name test of IsMainGameWindow: name[0..length) is exactly "acsW".
// Separate because GetClassNameW reports the spelling of the session-wide
// class atom, which another process may have registered first.
bool IsMainGameWindowClassName(const wchar_t* name, int length);

// The saved original slot 15, for pass-through and for creating the hidden
// chain. Valid after FactoryHookInstallOnce.
HRESULT CallOriginalCreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device, HWND hwnd,
                                           const DXGI_SWAP_CHAIN_DESC1* desc,
                                           const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                                           IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain);

}  // namespace acdb
