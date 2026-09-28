#pragma once
// Class-wide patch of the real DXGI factory vtable (spec 6.2).
#include <windows.h>
#include <dxgi1_6.h>

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
    bool compat_ok = false;             // EvaluateCompat(...).ok
    bool streamline_shut_down = false;  // always false in M1
};
// True only when !internal_call and every other condition holds.
bool ShouldProxy(const ProxyDecisionInputs& in);

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
