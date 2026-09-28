// The DXGI exports ReShade resolves from its ProxyLibrary (spec 6.1), which
// are also every dxgi.dll export that d3d11.dll, D3D12Core.dll, Streamline
// and the game bind by name when the bridge itself is the game's dxgi.dll
// (standalone; test_dxgi_export_coverage.cpp). Every export forwards to
// System32\dxgi.dll. The CreateDXGIFactory* exports also
// run the bootstrap and install the factory hook. The internal names carry a
// Bridge_ prefix so that they never clash with the SDK declarations;
// exports.def maps them to the DXGI names.
#include <windows.h>
#include <dxgi1_6.h>

#include <cstdint>

#include "bootstrap.h"
#include "factory_hook.h"
#include "internal_call.h"
#include "log.h"
#include "system_dxgi.h"

namespace acdb {
namespace {

// Returned by a forwarder whose System32 export is missing: E_NOTIMPL for the
// HRESULT exports, 0 (FALSE or a zero size) for the others. ReShade reports a
// missing proxy export the same way.
constexpr uintptr_t kNotImpl = static_cast<uintptr_t>(static_cast<uint32_t>(E_NOTIMPL));
constexpr uintptr_t kZero = 0;

const SystemDxgi* Sys() noexcept {
    try {
        return &GetSystemDxgi();
    } catch (...) {
        return nullptr;
    }
}

// The first real factory call. Runs outside DllMain by construction.
void RunBootstrap() noexcept {
    try {
        BootstrapRunOnce();
    } catch (...) {
        // BootstrapRunOnce reports its own failures; the factory is still created.
    }
}

// Hooks the factory before it is returned: ReShade in vtable mode then saves
// our slot functions as its originals, and in proxy mode it calls the patched
// vtable through its wrapper.
void HookNewFactory(HRESULT hr, void** factory) noexcept {
    if (FAILED(hr) || !factory || !*factory) return;
    try {
        FactoryHookInstallOnce(static_cast<IUnknown*>(*factory));
    } catch (...) {
        LOGE("factory hook installation threw; the factory is returned unhooked");
    }
}

template <typename CreateFn>
HRESULT CreateFactoryCommon(const char* name, CreateFn&& create, void** factory) noexcept {
    // Our own DXGI calls, and Streamline's, re-enter through ReShade, or
    // directly when we are the process's dxgi.dll: pure pass-through.
    if (IsInternalCall()) return create();
    RunBootstrap();
    const HRESULT hr = create();
    HookNewFactory(hr, factory);
    LOGD("%s -> 0x%08lX", name, static_cast<unsigned long>(hr));
    return hr;
}

uintptr_t Forward(PFN_Generic6 fn, uintptr_t missing, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d,
                  uintptr_t e, uintptr_t f) noexcept {
    return fn ? fn(a, b, c, d, e, f) : missing;
}

}  // namespace
}  // namespace acdb

extern "C" {

HRESULT WINAPI Bridge_CreateDXGIFactory(REFIID riid, void** factory) {
    const acdb::SystemDxgi* sys = acdb::Sys();
    const auto fn = sys ? sys->CreateDXGIFactory : nullptr;
    if (!fn) return E_NOTIMPL;
    return acdb::CreateFactoryCommon("CreateDXGIFactory", [&] { return fn(riid, factory); }, factory);
}

HRESULT WINAPI Bridge_CreateDXGIFactory1(REFIID riid, void** factory) {
    const acdb::SystemDxgi* sys = acdb::Sys();
    const auto fn = sys ? sys->CreateDXGIFactory1 : nullptr;
    if (!fn) return E_NOTIMPL;
    return acdb::CreateFactoryCommon("CreateDXGIFactory1", [&] { return fn(riid, factory); }, factory);
}

HRESULT WINAPI Bridge_CreateDXGIFactory2(UINT flags, REFIID riid, void** factory) {
    const acdb::SystemDxgi* sys = acdb::Sys();
    const auto fn = sys ? sys->CreateDXGIFactory2 : nullptr;
    if (!fn) return E_NOTIMPL;
    return acdb::CreateFactoryCommon("CreateDXGIFactory2", [&] { return fn(flags, riid, factory); }, factory);
}

HRESULT WINAPI Bridge_DXGIGetDebugInterface1(UINT flags, REFIID riid, void** debug) {
    const acdb::SystemDxgi* sys = acdb::Sys();
    const auto fn = sys ? sys->DXGIGetDebugInterface1 : nullptr;
    return fn ? fn(flags, riid, debug) : E_NOTIMPL;
}

HRESULT WINAPI Bridge_DXGIDeclareAdapterRemovalSupport() {
    const acdb::SystemDxgi* sys = acdb::Sys();
    const auto fn = sys ? sys->DXGIDeclareAdapterRemovalSupport : nullptr;
    return fn ? fn() : E_NOTIMPL;
}

// Undocumented exports: forwarded with six integer arguments (system_dxgi.h).
#define ACDB_GENERIC_EXPORT(name, missing)                                                                   \
    uintptr_t WINAPI Bridge_##name(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e,          \
                                   uintptr_t f) {                                                            \
        const acdb::SystemDxgi* sys = acdb::Sys();                                                           \
        return acdb::Forward(sys ? sys->name : nullptr, missing, a, b, c, d, e, f);                          \
    }

ACDB_GENERIC_EXPORT(DXGIDisableVBlankVirtualization, acdb::kNotImpl)
ACDB_GENERIC_EXPORT(DXGIReportAdapterConfiguration, acdb::kNotImpl)
ACDB_GENERIC_EXPORT(DXGIDumpJournal, acdb::kNotImpl)
ACDB_GENERIC_EXPORT(CompatValue, acdb::kZero)    // BOOL
ACDB_GENERIC_EXPORT(CompatString, acdb::kZero)   // BOOL
ACDB_GENERIC_EXPORT(DXGID3D10CreateDevice, acdb::kNotImpl)
ACDB_GENERIC_EXPORT(DXGID3D10CreateLayeredDevice, acdb::kNotImpl)
ACDB_GENERIC_EXPORT(DXGID3D10GetLayeredDeviceSize, acdb::kZero)  // SIZE_T
ACDB_GENERIC_EXPORT(DXGID3D10RegisterLayers, acdb::kNotImpl)

#undef ACDB_GENERIC_EXPORT

}  // extern "C"
