#include "system_dxgi.h"

#include <string>

namespace acdb {
namespace {

template <typename T>
void Resolve(HMODULE module, const char* name, T* out) {
    *out = reinterpret_cast<T>(GetProcAddress(module, name));
}

SystemDxgi Load() {
    SystemDxgi d;
    wchar_t sys[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return d;
    const std::wstring path = std::wstring(sys, n) + L"\\dxgi.dll";
    // Absolute path: loading by name would find ReShade's dxgi.dll, which
    // is already in the process under that name.
    d.module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!d.module) return d;

    Resolve(d.module, "CreateDXGIFactory", &d.CreateDXGIFactory);
    Resolve(d.module, "CreateDXGIFactory1", &d.CreateDXGIFactory1);
    Resolve(d.module, "CreateDXGIFactory2", &d.CreateDXGIFactory2);
    Resolve(d.module, "DXGIGetDebugInterface1", &d.DXGIGetDebugInterface1);
    Resolve(d.module, "DXGIDeclareAdapterRemovalSupport", &d.DXGIDeclareAdapterRemovalSupport);
    Resolve(d.module, "DXGIDisableVBlankVirtualization", &d.DXGIDisableVBlankVirtualization);
    Resolve(d.module, "DXGIReportAdapterConfiguration", &d.DXGIReportAdapterConfiguration);
    Resolve(d.module, "DXGIDumpJournal", &d.DXGIDumpJournal);
    Resolve(d.module, "CompatValue", &d.CompatValue);
    Resolve(d.module, "CompatString", &d.CompatString);
    Resolve(d.module, "DXGID3D10CreateDevice", &d.DXGID3D10CreateDevice);
    Resolve(d.module, "DXGID3D10CreateLayeredDevice", &d.DXGID3D10CreateLayeredDevice);
    Resolve(d.module, "DXGID3D10GetLayeredDeviceSize", &d.DXGID3D10GetLayeredDeviceSize);
    Resolve(d.module, "DXGID3D10RegisterLayers", &d.DXGID3D10RegisterLayers);
    return d;
}

}  // namespace

const SystemDxgi& GetSystemDxgi() {
    static const SystemDxgi instance = Load();  // thread-safe one-time init
    return instance;
}

}  // namespace acdb
