// The diagnostics helpers of GitHub issue 10 (src/diag_util.h): which module
// holds an address or implements a COM object, the one-line descriptions of a
// D3D11 device and of a window that bridge.log carries, and which loaded
// modules count as Windows' own.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>
#include <vector>

#include "diag_util.h"
#include "gpu_test_devices.h"
#include "test_framework.h"

using Microsoft::WRL::ComPtr;
using namespace acdb;

namespace {

bool EndsWithNoCase(const std::string& s, const char* suffix) {
    const size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && _stricmp(s.c_str() + (s.size() - n), suffix) == 0;
}

bool Has(const std::string& s, const std::string& piece) {
    const bool ok = s.find(piece) != std::string::npos;
    if (!ok) std::printf("  '%s' does not contain '%s'\n", s.c_str(), piece.c_str());
    return ok;
}

// Code of this executable, for the address lookup.
__declspec(noinline) int CodeInTheTestExecutable(int x) { return x * 3 + 1; }

// A hidden top-level window of a class registered here, so that its
// description cannot just repeat the class the bridge looks for.
class DiagWindow {
public:
    DiagWindow(const wchar_t* className, const wchar_t* title) : class_(className) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = className;
        registered_ = RegisterClassExW(&wc) != 0;
        hwnd_ = CreateWindowExW(0, className, title, WS_OVERLAPPEDWINDOW, 0, 0, 300, 200, nullptr, nullptr,
                                wc.hInstance, nullptr);
    }
    ~DiagWindow() {
        if (hwnd_) DestroyWindow(hwnd_);
        if (registered_) UnregisterClassW(class_.c_str(), GetModuleHandleW(nullptr));
    }
    DiagWindow(const DiagWindow&) = delete;
    DiagWindow& operator=(const DiagWindow&) = delete;

    HWND Get() const { return hwnd_; }

private:
    std::wstring class_;
    bool registered_ = false;
    HWND hwnd_ = nullptr;
};

}  // namespace

TEST(Diag_ModuleOfAddressNamesTheExecutableAndTheOffsetIntoIt) {
    CHECK_EQ(CodeInTheTestExecutable(2), 7);
    const void* fn = reinterpret_cast<const void*>(&CodeInTheTestExecutable);
    const ModuleAddress m = ModuleOfAddress(fn);
    std::printf("  %s\n", ModuleAddressText(fn).c_str());
    CHECK(EndsWithNoCase(m.path, "\\acdb_tests.exe"));
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    CHECK_EQ(m.rva, reinterpret_cast<uintptr_t>(fn) - base);
    // Inside the image, below its SizeOfImage.
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    CHECK(m.rva > 0 && m.rva < nt->OptionalHeader.SizeOfImage);
}

TEST(Diag_ModuleOfAddressOutsideEveryModuleIsUnknown) {
    void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    REQUIRE(page != nullptr);
    const ModuleAddress m = ModuleOfAddress(page);
    VirtualFree(page, 0, MEM_RELEASE);
    CHECK(m.path == "?");
    CHECK_EQ(m.rva, static_cast<uintptr_t>(0));
    CHECK(ModuleOfAddress(nullptr).path == "?");
}

// The D3D11 runtime implements the device object, whatever driver is below it.
TEST(Diag_VtableModuleOfAD3D11DeviceIsTheD3D11Runtime) {
    acdb_test::GpuTestDevices d;
    if (!acdb_test::CreateGpuTestDevices(&d)) return;
    const ModuleAddress m = VtableModule(d.device11.Get());
    std::printf("  %s\n", VtableModuleText(d.device11.Get()).c_str());
    CHECK(EndsWithNoCase(m.path, "\\d3d11.dll"));
    CHECK(m.rva > 0);
    CHECK(VtableModule(nullptr).path == "?");
}

TEST(Diag_DeviceDescriptionNamesItsAdapterLuidAndFeatureLevel) {
    acdb_test::GpuTestDevices d;
    if (!acdb_test::CreateGpuTestDevices(&d)) return;
    DXGI_ADAPTER_DESC1 ad{};
    REQUIRE(SUCCEEDED(d.adapter->GetDesc1(&ad)));
    char luid[24];
    std::snprintf(luid, sizeof(luid), "%08lX:%08lX", static_cast<unsigned long>(ad.AdapterLuid.HighPart),
                  static_cast<unsigned long>(ad.AdapterLuid.LowPart));
    const std::string text = DescribeD3D11Device(d.device11.Get());
    std::printf("  %s\n", text.c_str());
    CHECK(Has(text, std::string("adapter LUID ") + luid));
    CHECK(Has(text, "FL 11_0"));  // CreateGpuTestDevices asks for exactly 11_0
    CHECK(Has(text, "d3d11.dll+0x"));
    CHECK(DescribeD3D11Device(nullptr) == "none");
}

TEST(Diag_WindowDescriptionNamesTheRealClassAndTitle) {
    DiagWindow window(L"acdbDiagWindow", L"diag test window");
    REQUIRE(window.Get() != nullptr);
    const std::string text = DescribeWindow(window.Get());
    std::printf("  %s\n", text.c_str());
    CHECK(Has(text, "class \"acdbDiagWindow\""));
    CHECK(Has(text, "title \"diag test window\""));
    CHECK(Has(text, "pid " + std::to_string(GetCurrentProcessId())));
    CHECK(Has(text, "tid " + std::to_string(GetCurrentThreadId())));
    CHECK(Has(text, "visible 0"));
    CHECK(Has(text, "parent desktop"));
    RECT rc{};
    REQUIRE(GetClientRect(window.Get(), &rc));
    CHECK(Has(text, "client " + std::to_string(rc.right - rc.left) + "x" + std::to_string(rc.bottom - rc.top)));
}

TEST(Diag_ModulesInTheWindowsFolderAreSkippedExceptTheDriverStore) {
    const std::wstring win = L"C:\\WINDOWS";
    CHECK(IsWindowsModulePath(L"C:\\WINDOWS\\SYSTEM32\\ntdll.dll", win));
    CHECK(IsWindowsModulePath(L"c:\\windows\\system32\\KERNEL32.DLL", win));
    CHECK(IsWindowsModulePath(L"C:\\Windows\\WinSxS\\amd64_microsoft.windows.common-controls\\comctl32.dll", win));
    CHECK(!IsWindowsModulePath(
        L"C:\\WINDOWS\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_20f49c6eb5c9b39b\\nvwgf2umx.dll", win));
    CHECK(!IsWindowsModulePath(L"C:\\WindowsApps\\something.dll", win));
    CHECK(!IsWindowsModulePath(L"D:\\Steam\\steamapps\\common\\assettocorsa\\DWrite.dll", win));
    CHECK(!IsWindowsModulePath(L"", win));
}

TEST(Diag_NonWindowsModulesListTheExecutableButNotKernel32) {
    const std::vector<std::string> modules = NonWindowsModules();
    bool exe = false;
    bool kernel32 = false;
    for (const std::string& m : modules) {
        exe = exe || EndsWithNoCase(m, "\\acdb_tests.exe");
        kernel32 = kernel32 || EndsWithNoCase(m, "\\kernel32.dll");
    }
    CHECK(exe);
    CHECK(!kernel32);
    if (!exe) {
        for (const std::string& m : modules) std::printf("  %s\n", m.c_str());
    }
}
