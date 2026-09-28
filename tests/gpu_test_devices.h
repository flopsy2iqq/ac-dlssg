#pragma once
// Devices for GPU tests: the first hardware adapter that creates both a D3D11
// (FL 11_0) and a D3D12 (FL 12_0) device, else the WARP adapter for both, so
// the tests also run on CI machines without a GPU. Both devices are on the
// same adapter, which cross-API sharing requires.
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace acdb_test {

struct GpuTestDevices {
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<ID3D11Device> device11;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx11;
    Microsoft::WRL::ComPtr<ID3D12Device> device12;
    bool warp = false;
};

// Returns false (and prints why) when no adapter works; GPU tests then skip.
bool CreateGpuTestDevices(GpuTestDevices* out);

}  // namespace acdb_test
