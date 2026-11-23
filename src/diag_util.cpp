#include "diag_util.h"

#include <dxgi.h>
#include <psapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <mutex>

#include "adapter_caps.h"     // LuidText
#include "d3d12_presenter.h"  // IID_ReShadeUnwrappedObject

using Microsoft::WRL::ComPtr;

namespace acdb {
namespace {

constexpr int kMaxPassThroughDevices = 16;
constexpr int kTitleChars = 64;

std::string ToUtf8(const wchar_t* w, int n) {
    if (!w || n <= 0) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string s(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, n, s.data(), bytes, nullptr, nullptr);
    return s;
}

std::string PointerText(const void* p) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%p", p);
    return buf;
}

// The first vtable entry of a COM object; nullptr when it cannot be read. No
// C++ object lives here, so the SEH frame needs no unwinding.
const void* FirstVtableEntry(IUnknown* object) {
    if (!object) return nullptr;
    const void* entry = nullptr;
    __try {
        void* const* vtable = *reinterpret_cast<void* const* const*>(object);
        entry = vtable ? vtable[0] : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        entry = nullptr;
    }
    return entry;
}

// The COM identity (IUnknown) of an object as a number, 0 when it has none.
uintptr_t IdentityOf(IUnknown* object) {
    if (!object) return 0;
    ComPtr<IUnknown> identity;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&identity))) || !identity) return 0;
    return reinterpret_cast<uintptr_t>(identity.Get());
}

// The identity of the object a ReShade proxy wraps; 0 for any other object.
uintptr_t UnwrappedIdentityOf(IUnknown* object) {
    if (!object) return 0;
    ComPtr<IUnknown> unwrapped;
    if (FAILED(object->QueryInterface(IID_ReShadeUnwrappedObject, reinterpret_cast<void**>(unwrapped.GetAddressOf()))) ||
        !unwrapped)
        return 0;
    return IdentityOf(unwrapped.Get());
}

struct PassThroughDevice {
    uintptr_t identity = 0;
    uintptr_t unwrapped = 0;
    HWND hwnd = nullptr;
    int call = 0;
};
std::mutex g_pass_mu;
PassThroughDevice g_pass[kMaxPassThroughDevices];  // guarded by g_pass_mu
int g_pass_count = 0;                              // guarded by g_pass_mu

}  // namespace

ModuleAddress ModuleOfAddress(const void* addr) {
    ModuleAddress m;
    if (!addr) return m;
    try {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                static_cast<LPCWSTR>(addr), &module) ||
            !module)
            return m;
        wchar_t path[1024];
        const DWORD n = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
        if (n == 0) return m;  // unloaded meanwhile
        m.path = ToUtf8(path, static_cast<int>(std::min<DWORD>(n, static_cast<DWORD>(std::size(path)))));
        m.rva = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(module);
    } catch (...) {
        m = ModuleAddress();
    }
    return m;
}

std::string ModuleAddressText(const void* addr) {
    try {
        const ModuleAddress m = ModuleOfAddress(addr);
        char buf[40];
        if (m.path == "?") {
            std::snprintf(buf, sizeof(buf), "? (%p)", addr);
            return buf;
        }
        std::snprintf(buf, sizeof(buf), "+0x%llX", static_cast<unsigned long long>(m.rva));
        return m.path + buf;
    } catch (...) {
        return "?";
    }
}

ModuleAddress VtableModule(IUnknown* object) { return ModuleOfAddress(FirstVtableEntry(object)); }

std::string VtableModuleText(IUnknown* object) { return ModuleAddressText(FirstVtableEntry(object)); }

std::string DescribeD3D11Device(ID3D11Device* device) {
    if (!device) return "none";
    try {
        std::string adapter;
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> dxgiAdapter;
        DXGI_ADAPTER_DESC desc{};
        HRESULT hr =
            device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgiDevice.GetAddressOf()));
        if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(&dxgiAdapter);
        if (SUCCEEDED(hr)) hr = dxgiAdapter->GetDesc(&desc);
        if (SUCCEEDED(hr)) {
            adapter = "adapter LUID " + LuidText(desc.AdapterLuid);
        } else {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "adapter unknown (0x%08lX)", static_cast<unsigned long>(hr));
            adapter = buf;
        }
        const unsigned fl = static_cast<unsigned>(device->GetFeatureLevel());
        char tail[48];
        std::snprintf(tail, sizeof(tail), ", FL %u_%u, flags 0x%X)", (fl >> 12) & 0xFu, (fl >> 8) & 0xFu,
                      device->GetCreationFlags());
        return PointerText(device) + " (identity " + PointerText(reinterpret_cast<void*>(IdentityOf(device))) +
               ", vtable " + VtableModuleText(device) + ", " + adapter + tail;
    } catch (...) {
        return PointerText(device) + " (?)";
    }
}

std::string DescribeWindow(HWND hwnd) {
    try {
        const std::string head = PointerText(hwnd);
        if (!hwnd || !IsWindow(hwnd)) return head + " (not a window)";
        wchar_t cls[257] = {};
        const int clsLen = GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)));
        // InternalGetWindowText reads the text without sending WM_GETTEXT, so
        // it cannot wait for the window's thread as GetWindowTextW would.
        wchar_t title[kTitleChars + 1] = {};
        const int titleLen = InternalGetWindowText(hwnd, title, static_cast<int>(std::size(title)));
        DWORD pid = 0;
        const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const HWND parent = GetAncestor(hwnd, GA_PARENT);
        const std::string parentText = parent && parent == GetDesktopWindow() ? std::string("desktop") : PointerText(parent);
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      " pid %lu tid %lu visible %d style 0x%08lX exstyle 0x%08lX parent %s owner %p client %ldx%ld",
                      static_cast<unsigned long>(pid), static_cast<unsigned long>(tid), IsWindowVisible(hwnd) ? 1 : 0,
                      static_cast<unsigned long>(GetWindowLongPtrW(hwnd, GWL_STYLE)),
                      static_cast<unsigned long>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), parentText.c_str(),
                      static_cast<void*>(GetWindow(hwnd, GW_OWNER)), static_cast<long>(rc.right - rc.left),
                      static_cast<long>(rc.bottom - rc.top));
        return head + " class \"" + ToUtf8(cls, clsLen) + "\" title \"" + ToUtf8(title, titleLen) +
               (titleLen >= kTitleChars ? "..." : "") + "\"" + buf;
    } catch (...) {
        return "?";
    }
}

bool IsWindowsModulePath(const std::wstring& path, const std::wstring& windowsDir) {
    std::wstring dir = windowsDir;
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    if (dir.empty() || path.size() <= dir.size() + 1) return false;
    // The folder itself, not just a name that starts like it ("C:\WindowsApps").
    if (_wcsnicmp(path.c_str(), dir.c_str(), dir.size()) != 0 || (path[dir.size()] != L'\\' && path[dir.size()] != L'/'))
        return false;
    // The GPU drivers' user-mode modules live in System32\DriverStore.
    std::wstring lower = path;
    for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));
    return lower.find(L"\\driverstore\\") == std::wstring::npos;
}

std::vector<std::string> NonWindowsModules() {
    std::vector<std::string> out;
    try {
        wchar_t winDir[MAX_PATH] = {};
        const UINT wn = GetWindowsDirectoryW(winDir, MAX_PATH);
        const std::wstring windows(winDir, wn < MAX_PATH ? wn : 0);
        std::vector<HMODULE> mods(512);
        DWORD needed = 0;
        for (int attempt = 0; attempt < 2; ++attempt) {
            const DWORD bytes = static_cast<DWORD>(mods.size() * sizeof(HMODULE));
            if (!K32EnumProcessModulesEx(GetCurrentProcess(), mods.data(), bytes, &needed, LIST_MODULES_ALL)) return out;
            if (needed <= bytes) break;
            mods.resize(needed / sizeof(HMODULE) + 32);
        }
        const size_t count = std::min<size_t>(needed / sizeof(HMODULE), mods.size());
        for (size_t i = 0; i < count; ++i) {
            wchar_t path[1024];
            const DWORD n = GetModuleFileNameW(mods[i], path, static_cast<DWORD>(std::size(path)));
            if (n == 0) continue;  // unloaded meanwhile
            const std::wstring p(path, std::min<DWORD>(n, static_cast<DWORD>(std::size(path))));
            if (IsWindowsModulePath(p, windows)) continue;
            out.push_back(PointerText(mods[i]) + " " + ToUtf8(p.c_str(), static_cast<int>(p.size())));
        }
    } catch (...) {
    }
    return out;
}

void NotePassThroughDevice(IUnknown* device, HWND hwnd, int call) {
    if (!device) return;
    try {
        PassThroughDevice d;
        d.identity = IdentityOf(device);
        d.unwrapped = UnwrappedIdentityOf(device);
        d.hwnd = hwnd;
        d.call = call;
        std::lock_guard<std::mutex> lock(g_pass_mu);
        if (g_pass_count < kMaxPassThroughDevices) g_pass[g_pass_count++] = d;
    } catch (...) {
    }
}

int PassThroughDeviceCall(IUnknown* device, HWND* hwnd) {
    if (hwnd) *hwnd = nullptr;
    if (!device) return 0;
    try {
        const uintptr_t identity = IdentityOf(device);
        const uintptr_t unwrapped = UnwrappedIdentityOf(device);
        const auto same = [](uintptr_t a, const PassThroughDevice& p) { return a && (a == p.identity || a == p.unwrapped); };
        std::lock_guard<std::mutex> lock(g_pass_mu);
        for (int i = 0; i < g_pass_count; ++i) {
            if (same(identity, g_pass[i]) || same(unwrapped, g_pass[i])) {
                if (hwnd) *hwnd = g_pass[i].hwnd;
                return g_pass[i].call;
            }
        }
    } catch (...) {
    }
    return 0;
}

int PassThroughDeviceCount() {
    try {
        std::lock_guard<std::mutex> lock(g_pass_mu);
        return g_pass_count;
    } catch (...) {
        return 0;
    }
}

}  // namespace acdb
