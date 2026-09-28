// DriverProfileWarnings (pure) and ReadDriverProfile against this machine's
// NVIDIA driver.
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "driver_profile.h"
#include "test_framework.h"

using namespace acdb;

namespace {

const char kDlssgApp[] =
    "NVIDIA App DLSS override: frame generation forced off in the app profile (setting 0x10308298 = 1); "
    "DLSS-G will never interpolate";
const char kDlssgGlobal[] =
    "NVIDIA App DLSS override: frame generation forced off in the global profile (setting 0x10308298 = 1); "
    "DLSS-G will never interpolate";

std::string SlWarning(const char* where, uint32_t v) {
    return std::string("NVIDIA App Streamline override is on in the ") + where + " profile (setting 0x10E41E06 = " +
           std::to_string(v) + "); Streamline may load plugins from the NGX cache instead of ac-dlssg\\sl";
}

DrsValue Set(uint32_t v) {
    DrsValue d;
    d.found = true;
    d.value = v;
    return d;
}

std::string Utf8(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
}

void Print(const char* what, const DriverProfileReport& r) {
    auto v = [](const DrsValue& d) { return d.found ? std::to_string(d.value) : std::string("not set"); };
    std::printf("  %s: nvapi_ok=%d error='%s' app_profile='%s'\n", what, r.nvapi_ok ? 1 : 0, r.error.c_str(),
                Utf8(r.app_profile).c_str());
    std::printf("    NGX_DLSSG_MODE app=%s global=%s; SL_DLSS_OVERRIDE app=%s global=%s\n", v(r.dlssg_mode_app).c_str(),
                v(r.dlssg_mode_base).c_str(), v(r.sl_override_app).c_str(), v(r.sl_override_base).c_str());
    for (const auto& w : DriverProfileWarnings(r)) std::printf("    warning: %s\n", w.c_str());
}

bool SystemHasNvapi() {
    wchar_t dir[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    const DWORD attrs = GetFileAttributesW((std::wstring(dir) + L"\\nvapi64.dll").c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace

TEST(DriverProfile_NoWarningsForAnEmptyReport) {
    CHECK(DriverProfileWarnings(DriverProfileReport()).empty());
    DriverProfileReport failed;
    failed.error = "nvapi64.dll not found";
    CHECK(DriverProfileWarnings(failed).empty());
}

// Every combination of the four triggers, in the order dlssg app, dlssg
// global, override app, override global.
TEST(DriverProfile_WarningsForEveryCombination) {
    for (unsigned mask = 0; mask < 16; ++mask) {
        DriverProfileReport r;
        r.nvapi_ok = true;
        // Set but harmless values where a trigger is off.
        r.dlssg_mode_app = Set((mask & 1) ? kNgxDlssgModeOff : 2u);
        r.dlssg_mode_base = Set((mask & 2) ? kNgxDlssgModeOff : 0u);
        r.sl_override_app = Set((mask & 4) ? 1u : 0u);
        r.sl_override_base = Set((mask & 8) ? 3u : 0u);

        std::vector<std::string> expected;
        if (mask & 1) expected.push_back(kDlssgApp);
        if (mask & 2) expected.push_back(kDlssgGlobal);
        if (mask & 4) expected.push_back(SlWarning("app", 1));
        if (mask & 8) expected.push_back(SlWarning("global", 3));

        const auto got = DriverProfileWarnings(r);
        if (got != expected) {
            std::printf("  mask %u: got %zu warnings, expected %zu\n", mask, got.size(), expected.size());
            for (const auto& w : got) std::printf("    got: %s\n", w.c_str());
        }
        CHECK(got == expected);
    }
}

TEST(DriverProfile_ValuesThatAreNotFoundNeverWarn) {
    DriverProfileReport r;
    r.nvapi_ok = true;
    r.dlssg_mode_app.value = kNgxDlssgModeOff;
    r.dlssg_mode_base.value = kNgxDlssgModeOff;
    r.sl_override_app.value = 1;
    r.sl_override_base.value = 1;
    CHECK(DriverProfileWarnings(r).empty());
}

TEST(DriverProfile_OnlyModeOffForcesFrameGenerationOff) {
    for (uint32_t v : {0u, 2u, 3u, 4u, 0xFFFFFFFFu}) {
        DriverProfileReport r;
        r.dlssg_mode_app = Set(v);
        r.dlssg_mode_base = Set(v);
        CHECK(DriverProfileWarnings(r).empty());
    }
}

TEST(DriverProfile_ReadsThisMachinesDriver) {
    wchar_t exe[MAX_PATH] = {};
    REQUIRE(GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0);
    const DriverProfileReport self = ReadDriverProfile(exe);
    const DriverProfileReport acs = ReadDriverProfile(L"acs.exe");
    // Production passes the host exe's full path; the file need not exist.
    const DriverProfileReport acsPath = ReadDriverProfile(L"C:\\Games\\assettocorsa\\acs.exe");
    Print("test exe", self);
    Print("acs.exe", acs);
    Print("C:\\Games\\assettocorsa\\acs.exe", acsPath);
    if (!SystemHasNvapi()) {
        std::printf("  no System32\\nvapi64.dll: expecting nvapi_ok=false\n");
        CHECK(!self.nvapi_ok && !self.error.empty());
        CHECK(!acs.nvapi_ok && !acs.error.empty());
        return;
    }
    CHECK(self.nvapi_ok);
    CHECK(self.error.empty());
    CHECK(acs.nvapi_ok);
    CHECK(acs.error.empty());
    CHECK(acsPath.nvapi_ok);
    // Both FindApplicationByName outcomes are exercised: the test exe has no
    // profile, and NVIDIA's drivers ship one for Assetto Corsa's acs.exe.
    CHECK(self.app_profile.empty());
    CHECK(!acsPath.app_profile.empty());
}
