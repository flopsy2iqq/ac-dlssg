#pragma once
// Hidden D3D11 flip-model swap chain whose buffer 0 is what CSP and ReShade
// render into (spec 6.4 "D3D11-facing back buffer"). It is never presented.
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <memory>
#include <string>
#include <thread>

namespace acdb {

class HiddenChain {
public:
    // Creates a hidden (never shown) top-level window on a thread owned by
    // this object, which pumps its messages until destruction. Then creates a
    // flip-discard chain on it with CallOriginalCreateSwapChainForHwnd when the
    // factory hook is installed, otherwise with factory->CreateSwapChainForHwnd
    // (tests), under InternalCallScope. Chain desc: gameDesc's Width, Height,
    // Format, BufferUsage and SampleDesc; BufferCount 2; SwapEffect
    // FLIP_DISCARD; Scaling STRETCH; AlphaMode IGNORE; Flags 0. Calls
    // MakeWindowAssociation(hwnd, DXGI_MWA_NO_WINDOW_CHANGES).
    static std::unique_ptr<HiddenChain> Create(IDXGIFactory2* factory, ID3D11Device* device,
                                               const DXGI_SWAP_CHAIN_DESC1& gameDesc,
                                               std::string* error);
    ~HiddenChain();  // releases the chain, then closes the window thread

    // Buffer 0, owned by this object (not AddRef'd). Never null after Create.
    ID3D11Texture2D* Buffer0() const { return buffer0_.Get(); }
    HWND Window() const { return hwnd_; }

    // Releases the buffer reference, calls ResizeBuffers(2, w, h, format, 0)
    // and re-fetches buffer 0. The caller guarantees that CSP and ReShade have
    // released their views.
    HRESULT Resize(UINT width, UINT height, DXGI_FORMAT format);

private:
    HiddenChain() = default;
    std::thread thread_;
    DWORD thread_id_ = 0;
    HWND hwnd_ = nullptr;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> chain_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> buffer0_;
};

}  // namespace acdb
