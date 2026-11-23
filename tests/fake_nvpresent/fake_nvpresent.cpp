// A stand-in for NVIDIA Smooth Motion's NvPresent64.dll (never the real one),
// built as NvPresent64.dll into a subfolder so that nothing finds it by name.
// Its exports call back through it, so the caller sits in a module of that
// name, as the game's swap chain does when Smooth Motion creates it (GitHub
// issue 10): the bridge's NvPresentInCallStack test and the test app's
// --fake-nvpresent scenario use it.
#include <windows.h>
#include <dxgi1_2.h>

// Without optimisation the calls below stay real calls: a tail jump would
// leave this module out of the caller's stack.
#pragma optimize("", off)

extern "C" __declspec(dllexport) int __cdecl FakeNvPresentCall(int(__cdecl* fn)(void*), void* ctx) {
    const int r = fn(ctx);
    return r;
}

// IDXGIFactory2::CreateSwapChainForHwnd from inside this module.
extern "C" __declspec(dllexport) HRESULT __cdecl FakeNvPresentCreateSwapChainForHwnd(
    IDXGIFactory2* factory, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
    IDXGISwapChain1** swapChain) {
    const HRESULT hr = factory->CreateSwapChainForHwnd(device, hwnd, desc, nullptr, nullptr, swapChain);
    return hr;
}

#pragma optimize("", on)
