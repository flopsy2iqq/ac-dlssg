#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <cwctype>
#include <string>
#include <thread>

#include "internal_call.h"
#include "system_dxgi.h"
#include "test_framework.h"
#include "vtable_patch.h"

using namespace acdb;

namespace {

int FnA() { return 1; }
int FnB() { return 2; }
int FnC() { return 3; }
int Replacement() { return 42; }

using Fn = int (*)();

void* AsPtr(Fn f) { return reinterpret_cast<void*>(f); }
int CallSlot(void** table, size_t i) { return reinterpret_cast<Fn>(table[i])(); }

DWORD Protection(const void* p) {
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(p, &mbi, sizeof(mbi));
    return mbi.Protect;
}

// A fake vtable {FnA, FnB, FnC} on its own page with the given protection.
void** MakeTable(DWORD protect) {
    auto** table = static_cast<void**>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!table) return nullptr;
    table[0] = AsPtr(&FnA);
    table[1] = AsPtr(&FnB);
    table[2] = AsPtr(&FnC);
    DWORD old = 0;
    VirtualProtect(table, 4096, protect, &old);
    return table;
}

}  // namespace

TEST(VtablePatch_ReadOnlyPage) {
    void** table = MakeTable(PAGE_READONLY);
    REQUIRE(table != nullptr);
    CHECK_EQ(Protection(table), static_cast<DWORD>(PAGE_READONLY));

    void* prev = PatchVtableSlot(table, 1, AsPtr(&Replacement));
    CHECK(prev == AsPtr(&FnB));
    CHECK(table[1] == AsPtr(&Replacement));
    CHECK_EQ(CallSlot(table, 1), 42);
    CHECK_EQ(CallSlot(table, 0), 1);
    CHECK_EQ(CallSlot(table, 2), 3);
    CHECK_EQ(Protection(table), static_cast<DWORD>(PAGE_READONLY));

    // Patching back returns our replacement and restores the original.
    CHECK(PatchVtableSlot(table, 1, prev) == AsPtr(&Replacement));
    CHECK_EQ(CallSlot(table, 1), 2);
    CHECK_EQ(Protection(table), static_cast<DWORD>(PAGE_READONLY));
    VirtualFree(table, 0, MEM_RELEASE);
}

TEST(VtablePatch_ExecuteReadPageKeepsExecute) {
    void** table = MakeTable(PAGE_EXECUTE_READ);
    REQUIRE(table != nullptr);
    void* prev = PatchVtableSlot(table, 2, AsPtr(&Replacement));
    CHECK(prev == AsPtr(&FnC));
    CHECK_EQ(CallSlot(table, 2), 42);
    CHECK_EQ(Protection(table), static_cast<DWORD>(PAGE_EXECUTE_READ));
    VirtualFree(table, 0, MEM_RELEASE);
}

TEST(VtablePatch_WritablePage) {
    void** table = MakeTable(PAGE_READWRITE);
    REQUIRE(table != nullptr);
    CHECK(PatchVtableSlot(table, 0, AsPtr(&Replacement)) == AsPtr(&FnA));
    CHECK_EQ(CallSlot(table, 0), 42);
    CHECK_EQ(Protection(table), static_cast<DWORD>(PAGE_READWRITE));
    VirtualFree(table, 0, MEM_RELEASE);
}

TEST(VtablePatch_UnwritablePageFails) {
    // Reserved but not committed: VirtualProtect cannot make it writable.
    auto** table = static_cast<void**>(VirtualAlloc(nullptr, 4096, MEM_RESERVE, PAGE_NOACCESS));
    REQUIRE(table != nullptr);
    CHECK(PatchVtableSlot(table, 0, AsPtr(&Replacement)) == nullptr);
    VirtualFree(table, 0, MEM_RELEASE);
}

// ---------------------------------------------------------------- InternalCall

TEST(InternalCall_ScopeSetsAndRestores) {
    CHECK(!IsInternalCall());
    {
        InternalCallScope outer;
        CHECK(IsInternalCall());
        {
            InternalCallScope inner;
            CHECK(IsInternalCall());
        }
        CHECK(IsInternalCall());
    }
    CHECK(!IsInternalCall());
}

TEST(InternalCall_IsPerThread) {
    InternalCallScope scope;
    std::atomic<int> seen{-1};
    std::thread t([&] { seen = IsInternalCall() ? 1 : 0; });
    t.join();
    CHECK_EQ(seen.load(), 0);
    CHECK(IsInternalCall());
}

// ---------------------------------------------------------------- SystemDxgi

TEST(SystemDxgi_LoadsFromSystem32) {
    const SystemDxgi& d = GetSystemDxgi();
    REQUIRE(d.module != nullptr);
    wchar_t path[MAX_PATH] = {};
    REQUIRE(GetModuleFileNameW(d.module, path, MAX_PATH) > 0);
    std::wstring p(path);
    for (auto& c : p) c = static_cast<wchar_t>(std::towlower(c));
    const std::wstring suffix = L"\\system32\\dxgi.dll";
    if (!(p.size() >= suffix.size() && p.compare(p.size() - suffix.size(), suffix.size(), suffix) == 0))
        std::printf("  module path: %ls\n", path);
    CHECK(p.size() >= suffix.size() && p.compare(p.size() - suffix.size(), suffix.size(), suffix) == 0);

    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring expected = std::wstring(sys) + L"\\dxgi.dll";
    for (auto& c : expected) c = static_cast<wchar_t>(std::towlower(c));
    CHECK(p == expected);

    CHECK(&GetSystemDxgi() == &d);  // loaded once
}

TEST(SystemDxgi_ResolvesExports) {
    const SystemDxgi& d = GetSystemDxgi();
    REQUIRE(d.module != nullptr);
    CHECK(d.CreateDXGIFactory != nullptr);
    CHECK(d.CreateDXGIFactory1 != nullptr);
    CHECK(d.CreateDXGIFactory2 != nullptr);
    CHECK(d.DXGIGetDebugInterface1 != nullptr);
    CHECK(d.DXGID3D10CreateDevice != nullptr);
    CHECK(d.CompatValue != nullptr);
    CHECK(reinterpret_cast<FARPROC>(d.CreateDXGIFactory1) == GetProcAddress(d.module, "CreateDXGIFactory1"));
}

TEST(SystemDxgi_CreateFactory1Works) {
    const SystemDxgi& d = GetSystemDxgi();
    REQUIRE(d.CreateDXGIFactory1 != nullptr);
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    REQUIRE(SUCCEEDED(d.CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf()))));
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    CHECK(SUCCEEDED(factory->EnumAdapters1(0, &adapter)));
}
