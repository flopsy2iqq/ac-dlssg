#pragma once
// NVIDIA Smooth Motion in the process (GitHub issue 10). Smooth Motion is the
// driver's own frame generation for DX11 and DX12 games on RTX 40 and 50,
// implemented in NvPresent64.dll. On driver 621.01 it created the game
// window's swap chain (the CreateSwapChainForHwnd call came from
// NvPresent64.dll) even while the NVIDIA App showed it off, and then a
// second swap chain on its own window of class
// "InvisibleWindowClassNvPresent"; DLSS-G cannot run on top of it. Only
// KERNEL32 and USER32 functions are used (spec 6.1); nothing here changes a
// driver setting.
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace acdb {

constexpr wchar_t kNvPresentModule[] = L"NvPresent64.dll";
// How many return addresses NvPresentInCallStack looks at, from its caller
// on. The direct caller is not enough: in ReShade mode it is ReShade's
// dxgi.dll, which sits between NvPresent64.dll and the bridge's hook.
constexpr unsigned kNvPresentStackFrames = 8;

// A module's address range [base, base + size); size 0 when not loaded.
struct ModuleRange {
    uintptr_t base = 0;
    size_t size = 0;
};

// Pure: true when one of frames[0, n) lies inside the module's range.
bool StackHasModule(const void* const* frames, size_t n, const ModuleRange& module);

// The range of the loaded module of that name (its SizeOfImage), or an
// empty one.
ModuleRange LoadedModuleRange(const wchar_t* name);

// True when NvPresent64.dll is loaded and one of the calling thread's
// kNvPresentStackFrames return addresses above this function lies in it:
// NVIDIA Smooth Motion is making the call. RtlCaptureStackBackTrace, so the
// result is as reliable as the callers' unwind data, which every x64
// module has.
bool NvPresentInCallStack();

// True when hwnd's class is exactly "InvisibleWindowClassNvPresent", the
// window Smooth Motion creates its own swap chain on.
bool IsNvPresentWindow(HWND hwnd);
// The name test of IsNvPresentWindow: name[0, length) is exactly that class.
bool IsNvPresentWindowClassName(const wchar_t* name, int length);

// For bridge.log: "NvPresent64.dll (NVIDIA Smooth Motion) is loaded from
// <path>" or "NvPresent64.dll (NVIDIA Smooth Motion) is not loaded".
std::string NvPresentLoadedText();

}  // namespace acdb
