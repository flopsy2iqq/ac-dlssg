#include "nvpresent.h"

#include <psapi.h>

#include <cwchar>
#include <iterator>

namespace acdb {
namespace {

constexpr wchar_t kNvPresentWindowClass[] = L"InvisibleWindowClassNvPresent";

std::string ToUtf8(const wchar_t* w, int n) {
    if (!w || n <= 0) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string s(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, n, s.data(), bytes, nullptr, nullptr);
    return s;
}

}  // namespace

bool StackHasModule(const void* const* frames, size_t n, const ModuleRange& module) {
    if (!frames || module.size == 0) return false;
    for (size_t i = 0; i < n; ++i) {
        const auto a = reinterpret_cast<uintptr_t>(frames[i]);
        if (a >= module.base && a - module.base < module.size) return true;
    }
    return false;
}

ModuleRange LoadedModuleRange(const wchar_t* name) {
    ModuleRange r;
    const HMODULE m = name ? GetModuleHandleW(name) : nullptr;
    MODULEINFO info{};
    if (!m || !K32GetModuleInformation(GetCurrentProcess(), m, &info, sizeof(info))) return r;
    r.base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
    r.size = info.SizeOfImage;
    return r;
}

bool NvPresentInCallStack() {
    const ModuleRange nvPresent = LoadedModuleRange(kNvPresentModule);
    if (nvPresent.size == 0) return false;
    void* frames[kNvPresentStackFrames] = {};
    // Skips this function's own frame: frames[0] is in the caller.
    const USHORT n = RtlCaptureStackBackTrace(1, kNvPresentStackFrames, frames, nullptr);
    return StackHasModule(frames, n, nvPresent);
}

bool IsNvPresentWindow(HWND hwnd) {
    if (!hwnd) return false;
    // A buffer longer than the class name, so a longer name cannot match.
    wchar_t name[64] = {};
    const int n = GetClassNameW(hwnd, name, static_cast<int>(std::size(name)));
    return IsNvPresentWindowClassName(name, n);
}

bool IsNvPresentWindowClassName(const wchar_t* name, int length) {
    constexpr int kLength = static_cast<int>(std::size(kNvPresentWindowClass)) - 1;
    return name && length == kLength && std::wmemcmp(name, kNvPresentWindowClass, kLength) == 0;
}

std::string NvPresentLoadedText() {
    const HMODULE m = GetModuleHandleW(kNvPresentModule);
    if (!m) return "NvPresent64.dll (NVIDIA Smooth Motion) is not loaded";
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(m, path, static_cast<DWORD>(std::size(path)));
    return "NvPresent64.dll (NVIDIA Smooth Motion) is loaded from " +
           (n > 0 && n < std::size(path) ? ToUtf8(path, static_cast<int>(n)) : std::string("an unknown path"));
}

}  // namespace acdb
