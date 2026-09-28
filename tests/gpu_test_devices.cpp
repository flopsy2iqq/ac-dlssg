#include "gpu_test_devices.h"

#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace acdb_test {
namespace {

// Creates both devices on one adapter; false (devices left empty) if either fails.
bool CreateOn(IDXGIAdapter1* adapter, GpuTestDevices* out) {
    const D3D_FEATURE_LEVEL fl11 = D3D_FEATURE_LEVEL_11_0;
    ComPtr<ID3D11Device> dev11;
    ComPtr<ID3D11DeviceContext> ctx11;
    // D3D_DRIVER_TYPE_UNKNOWN with an explicit adapter, flags 0, FL 11_0: as CSP does.
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl11, 1, D3D11_SDK_VERSION, &dev11,
                                   nullptr, &ctx11);
    if (FAILED(hr)) {
        std::printf("  D3D11CreateDevice failed: 0x%08lX\n", static_cast<unsigned long>(hr));
        return false;
    }
    ComPtr<ID3D12Device> dev12;
    hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev12));
    if (FAILED(hr)) {
        std::printf("  D3D12CreateDevice failed: 0x%08lX\n", static_cast<unsigned long>(hr));
        return false;
    }
    out->adapter = adapter;
    out->device11 = dev11;
    out->ctx11 = ctx11;
    out->device12 = dev12;
    return true;
}

}  // namespace

bool CreateGpuTestDevices(GpuTestDevices* out) {
    if (!out) return false;
    *out = GpuTestDevices();
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&out->factory));
    if (FAILED(hr)) {
        std::printf("  CreateDXGIFactory2 failed: 0x%08lX\n", static_cast<unsigned long>(hr));
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; out->factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            CreateOn(adapter.Get(), out)) {
            out->warp = false;
            return true;
        }
        adapter.Reset();
    }

    hr = out->factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
    if (FAILED(hr)) {
        std::printf("  EnumWarpAdapter failed: 0x%08lX\n", static_cast<unsigned long>(hr));
        return false;
    }
    if (!CreateOn(adapter.Get(), out)) {
        std::printf("  no adapter creates both a D3D11 and a D3D12 device\n");
        return false;
    }
    out->warp = true;
    return true;
}

}  // namespace acdb_test
