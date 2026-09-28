#pragma once
// The D3D11-facing swap chain CSP and ReShade see (spec 6.3).
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "config.h"
#include "d3d12_presenter.h"
#include "hidden_chain.h"
#include "latency_semaphore.h"

namespace acdb {

class ProxySwapChain final : public IDXGISwapChain4 {
public:
    // Builds HiddenChain(factory, device11, desc) and D3D12Presenter on hwnd,
    // initialises the latency semaphore from desc.Flags and
    // config.max_frame_latency, and passes that latency (and every later
    // SetMaximumFrameLatency result) on to the presenter. On failure returns
    // the HRESULT and *error, and creates nothing (the caller then passes
    // through). ResizeBuffers resizes the hidden chain first: when that fails
    // (a view on buffer 0 is alive), nothing has changed.
    // streamline: the process's initialised StreamlineRuntime in production;
    // nullptr gives the plain D3D12 path (unit tests, which create many
    // proxies in one process while Streamline allows one lifetime). The
    // final Release of a Streamline-backed proxy calls
    // StreamlineRuntime::Shutdown (spec 6.3 "Final Release"), after which
    // FactoryHook passes every later chain through.
    static HRESULT Create(IDXGIFactory2* factory, ID3D11Device* device11, HWND hwnd,
                          const DXGI_SWAP_CHAIN_DESC1& desc,
                          const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                          const Config& config, StreamlineRuntime* streamline, IDXGISwapChain1** out,
                          std::string* error);

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID name, UINT size, const void* data) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID name, const IUnknown* unknown) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID name, UINT* size, void* data) override;
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** parent) override;
    // IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** device) override;
    // IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT syncInterval, UINT flags) override;
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT buffer, REFIID riid, void** surface) override;
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen, IDXGIOutput* target) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fullscreen, IDXGIOutput** target) override;
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* desc) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) override;
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* params) override;
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** output) override;
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* stats) override;
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* count) override;
    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* desc) override;
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* hwnd) override;
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID riid, void** unk) override;
    HRESULT STDMETHODCALLTYPE Present1(UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS* params) override;
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override;
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** output) override;
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* color) override;
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* color) override;
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION rotation) override;
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* rotation) override;
    // IDXGISwapChain2
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT width, UINT height) override;
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* width, UINT* height) override;
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT maxLatency) override;
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* maxLatency) override;
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override;
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* matrix) override;
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* matrix) override;
    // IDXGISwapChain3
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override;
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE colorSpace, UINT* support) override;
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE colorSpace) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags,
                                             const UINT* nodeMask, IUnknown* const* queues) override;
    // IDXGISwapChain4
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE type, UINT size, void* metadata) override;

    // Number of live proxies (tests).
    static long LiveCount();

private:
    ProxySwapChain() = default;
    ~ProxySwapChain();
    HRESULT PresentCommon(UINT syncInterval, UINT flags);

    std::atomic<ULONG> ref_{1};
    // Recursive: SetFullscreenState sends window messages synchronously, and a
    // game may call ResizeBuffers from its WM_SIZE handler on the same thread.
    std::recursive_mutex mu_;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory_;
    Microsoft::WRL::ComPtr<ID3D11Device> device11_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx11_;
    HWND hwnd_ = nullptr;
    DXGI_SWAP_CHAIN_DESC1 desc_{};                 // the game's view, updated by ResizeBuffers
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs_desc_{};
    std::unique_ptr<HiddenChain> hidden_;
    std::unique_ptr<D3D12Presenter> presenter_;
    LatencySemaphore latency_;
    std::map<GUID, std::string, bool (*)(const GUID&, const GUID&)> private_data_{
        [](const GUID& a, const GUID& b) { return memcmp(&a, &b, sizeof(GUID)) < 0; }};
    std::map<GUID, Microsoft::WRL::ComPtr<IUnknown>, bool (*)(const GUID&, const GUID&)> private_ifaces_{
        [](const GUID& a, const GUID& b) { return memcmp(&a, &b, sizeof(GUID)) < 0; }};
    UINT present_count_ = 0;
};

}  // namespace acdb
