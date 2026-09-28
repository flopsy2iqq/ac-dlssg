#pragma once
// System32\dxgi.dll loaded by absolute path (never by name, which would
// resolve to the game folder's dxgi.dll: ReShade, or in standalone mode the
// bridge itself).
#include <windows.h>
#include <dxgi1_6.h>

#include <cstdint>

namespace acdb {

// Generic forwarder signature for undocumented exports: up to six integer or
// pointer arguments, integer return. None of these exports takes floats.
using PFN_Generic6 = uintptr_t(WINAPI*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

struct SystemDxgi {
    HMODULE module = nullptr;
    HRESULT(WINAPI* CreateDXGIFactory)(REFIID, void**) = nullptr;
    HRESULT(WINAPI* CreateDXGIFactory1)(REFIID, void**) = nullptr;
    HRESULT(WINAPI* CreateDXGIFactory2)(UINT, REFIID, void**) = nullptr;
    HRESULT(WINAPI* DXGIGetDebugInterface1)(UINT, REFIID, void**) = nullptr;
    HRESULT(WINAPI* DXGIDeclareAdapterRemovalSupport)() = nullptr;
    PFN_Generic6 DXGIDisableVBlankVirtualization = nullptr;
    PFN_Generic6 DXGIReportAdapterConfiguration = nullptr;
    PFN_Generic6 DXGIDumpJournal = nullptr;
    PFN_Generic6 CompatValue = nullptr;
    PFN_Generic6 CompatString = nullptr;
    PFN_Generic6 DXGID3D10CreateDevice = nullptr;
    PFN_Generic6 DXGID3D10CreateLayeredDevice = nullptr;
    PFN_Generic6 DXGID3D10GetLayeredDeviceSize = nullptr;
    PFN_Generic6 DXGID3D10RegisterLayers = nullptr;
};

// Loads once (thread-safe). module is null if System32\dxgi.dll cannot be
// loaded; individual pointers are null for exports that do not exist.
const SystemDxgi& GetSystemDxgi();

}  // namespace acdb
