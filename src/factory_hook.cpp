// Temporary stub, written in Task 8 so that HiddenChain links before the real
// factory hook exists. Task 10 replaces this whole file.
#include "factory_hook.h"

namespace acdb {

bool FactoryHookInstalled() { return false; }

HRESULT CallOriginalCreateSwapChainForHwnd(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                           const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                           IDXGISwapChain1**) {
    return E_NOTIMPL;
}

}  // namespace acdb
