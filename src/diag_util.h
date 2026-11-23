#pragma once
// Diagnostics for GitHub issue 10 (NVIDIA driver 621.01: the device of CSP's
// DLSS evaluate is not the device CSP created its swap chain with): one-line
// descriptions of code addresses, COM objects, D3D11 devices and windows for
// bridge.log, the process's modules outside the Windows folder, and the
// devices of the swap chains the factory hook passed through. Logging only:
// nothing here takes part in a decision of the bridge. Only KERNEL32 and
// USER32 functions are used (spec 6.1).
#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <string>
#include <vector>

namespace acdb {

// The module that holds an address: the file name GetModuleFileNameW gives
// for the module GetModuleHandleExW(FROM_ADDRESS) finds, in UTF-8, and the
// address's offset from that module's base. path is "?" and rva 0 when no
// module holds the address (heap, generated code, an unloaded module).
struct ModuleAddress {
    std::string path = "?";
    uintptr_t rva = 0;
};
ModuleAddress ModuleOfAddress(const void* addr);
// "<path>+0x<rva>", or "? (<address>)" when no module holds it.
std::string ModuleAddressText(const void* addr);

// ModuleOfAddress of the object's first vtable entry (QueryInterface), that
// is the module whose code implements the object; "?" for a null object.
ModuleAddress VtableModule(IUnknown* object);
std::string VtableModuleText(IUnknown* object);

// One line about a D3D11 device: "<pointer> (identity <IUnknown pointer>,
// vtable <module+rva>, adapter LUID <high:low>, FL <major>_<minor>, flags
// 0x<creation flags>)"; "none" for a null device. The adapter comes from
// IDXGIDevice::GetAdapter and GetDesc.
std::string DescribeD3D11Device(ID3D11Device* device);

// One line about a window: "<hwnd> class "<GetClassNameW>" title "<at most
// 64 characters>" pid <p> tid <t> visible <0|1> style 0x<s> exstyle 0x<e>
// parent <hwnd|desktop> owner <hwnd> client <w>x<h>"; "<hwnd> (not a
// window)" when it is none. No message is sent to the window, so its thread
// is never waited for.
std::string DescribeWindow(HWND hwnd);

// True when path lies in the Windows folder windowsDir (case-insensitive)
// but not in its DriverStore, where the GPU drivers' modules live.
bool IsWindowsModulePath(const std::wstring& path, const std::wstring& windowsDir);
// "<base> <path>" for every loaded module whose path IsWindowsModulePath
// does not skip, in the loader's order.
std::vector<std::string> NonWindowsModules();

// The devices of the swap chains the factory hook passed through, so that
// the capture coordinator can say whether a DLSS evaluate's device is one of
// them. call is the factory hook's number of the call ("#<call>" in its log
// lines). Only COM identities are kept (no reference): a match means the
// same object unless the device was released and its address reused. At
// most 16 are kept; later ones are dropped.
void NotePassThroughDevice(IUnknown* device, HWND hwnd, int call);
// The call number of the first noted pass-through whose device has the COM
// identity of device, directly or as the object a ReShade proxy wraps; 0
// when none. *hwnd gets its window.
int PassThroughDeviceCall(IUnknown* device, HWND* hwnd);
int PassThroughDeviceCount();

}  // namespace acdb
