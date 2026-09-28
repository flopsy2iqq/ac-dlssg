// ReadModuleVersion against this exe's own version resource (test_version.rc)
// and against VERSION.dll's reading of a system module.
#include <windows.h>
#include <winver.h>

#include <cstdio>
#include <string>
#include <vector>

#include "module_version.h"
#include "test_framework.h"

using namespace acdb;

TEST(ModuleVersion_ReadsTheTestExeResource) {
    const ModuleVersion v = ReadModuleVersion(GetModuleHandleW(nullptr));
    REQUIRE(v.found);
    CHECK_EQ(v.file[0], 1u);
    CHECK_EQ(v.file[1], 2u);
    CHECK_EQ(v.file[2], 3u);
    CHECK_EQ(v.file[3], 4u);
    CHECK(v.product == "1.2.3-test");
    CHECK(ModuleVersionText(GetModuleHandleW(nullptr)) == "1.2.3.4 (product 1.2.3-test)");
}

TEST(ModuleVersion_NoModuleOrNoResource) {
    CHECK(!ReadModuleVersion(nullptr).found);
    CHECK(ModuleVersionText(nullptr) == "no version resource");
    // The bridge DLL has no version resource; mapped as a resource image only.
    std::wstring path = L"" ACDB_DLL_PATH;
    for (auto& c : path) {
        if (c == L'/') c = L'\\';
    }
    const HMODULE dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
    REQUIRE(dll != nullptr);
    CHECK(!ReadModuleVersion(dll).found);
    FreeLibrary(dll);
}

// A Windows file: VERSION.dll applies the OS version lie to it (major.minor
// 6.2 for an exe without a compatibility manifest); the resource is not lied about.
TEST(ModuleVersion_ReadsAWindowsModule) {
    const HMODULE mod = GetModuleHandleW(L"d3d11.dll");
    REQUIRE(mod != nullptr);
    const ModuleVersion v = ReadModuleVersion(mod);
    REQUIRE(v.found);

    wchar_t path[MAX_PATH] = {};
    REQUIRE(GetModuleFileNameW(mod, path, MAX_PATH) > 0);
    // The language-neutral resource, which is what the loaded module carries.
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL, path, &ignored);
    REQUIRE(size > 0);
    std::vector<BYTE> data(size);
    REQUIRE(GetFileVersionInfoExW(FILE_VER_GET_NEUTRAL, path, 0, size, data.data()));
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT len = 0;
    REQUIRE(VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fixed), &len) && fixed);
    std::printf("  d3d11.dll: %s; VERSION.dll reports %u.%u.%u.%u\n", ModuleVersionText(mod).c_str(),
                HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS), HIWORD(fixed->dwFileVersionLS),
                LOWORD(fixed->dwFileVersionLS));
    // Build and revision are exact; major.minor is the real 10.0, which
    // VERSION.dll may report as 6.2 (see above).
    CHECK_EQ(v.file[2], static_cast<unsigned>(HIWORD(fixed->dwFileVersionLS)));
    CHECK_EQ(v.file[3], static_cast<unsigned>(LOWORD(fixed->dwFileVersionLS)));
    CHECK(v.file[0] >= 10u);
    CHECK(!v.product.empty());
}

// The same reading as VERSION.dll for a file the version lie does not touch.
TEST(ModuleVersion_MatchesVersionDllForTheTestExe) {
    const ModuleVersion v = ReadModuleVersion(GetModuleHandleW(nullptr));
    REQUIRE(v.found);
    wchar_t path[MAX_PATH] = {};
    REQUIRE(GetModuleFileNameW(nullptr, path, MAX_PATH) > 0);
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &ignored);
    REQUIRE(size > 0);
    std::vector<BYTE> data(size);
    REQUIRE(GetFileVersionInfoW(path, 0, size, data.data()));
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT len = 0;
    REQUIRE(VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fixed), &len) && fixed);
    CHECK_EQ(v.file[0], static_cast<unsigned>(HIWORD(fixed->dwFileVersionMS)));
    CHECK_EQ(v.file[1], static_cast<unsigned>(LOWORD(fixed->dwFileVersionMS)));
    CHECK_EQ(v.file[2], static_cast<unsigned>(HIWORD(fixed->dwFileVersionLS)));
    CHECK_EQ(v.file[3], static_cast<unsigned>(LOWORD(fixed->dwFileVersionLS)));
    wchar_t* product = nullptr;
    CHECK(VerQueryValueW(data.data(), L"\\StringFileInfo\\040904b0\\ProductVersion", reinterpret_cast<void**>(&product),
                         &len) &&
          product && std::wstring(product) == L"1.2.3-test");
}
