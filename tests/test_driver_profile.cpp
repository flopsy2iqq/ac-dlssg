// DriverProfileWarnings (pure) and ReadDriverProfile against this machine's
// NVIDIA driver.
#include <windows.h>

#include <algorithm>
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

// Diagnostics (GitHub issue 10): the lines that list a profile's settings
// give the id, the type, the value (a DWORD in hex) and where the value comes
// from, then the setting's name when the driver has one.
TEST(DriverProfile_SettingTextShowsIdTypeValueAndOrigin) {
    DrsSetting d;
    d.id = 0x10E41E06;
    d.type = 0;
    d.location = 0;
    d.name = L"Enable Streamline override";
    d.number = 1;
    CHECK_EQ(DrsSettingText(d), std::string("0x10E41E06 dword 0x00000001 (this profile) \"Enable Streamline override\""));
    DrsSetting q;
    q.id = 0x00ABCDEF;
    q.type = 4;
    q.location = 2;
    q.number = 0x123456789ULL;
    CHECK_EQ(DrsSettingText(q), std::string("0x00ABCDEF qword 0x0000000123456789 (base profile)"));
    DrsSetting b;
    b.id = 1;
    b.type = 1;
    b.location = 1;
    b.binary_length = 120;
    CHECK_EQ(DrsSettingText(b), std::string("0x00000001 binary 120 bytes (global profile)"));
    DrsSetting u;
    u.id = 2;
    u.type = 9;
    u.location = 3;
    CHECK_EQ(DrsSettingText(u), std::string("0x00000002 type 9 (default)"));
}

TEST(DriverProfile_SettingTextShortensLongStrings) {
    DrsSetting s;
    s.id = 0x2;
    s.type = 3;
    s.location = 0;
    s.text = std::wstring(100, L'a');
    s.name = std::wstring(60, L'n');
    CHECK_EQ(DrsSettingText(s), "0x00000002 wstring \"" + std::string(64, 'a') + "...\" (this profile) \"" +
                                    std::string(48, 'n') + "...\"");
    s.type = 2;
    s.text = L"short";
    s.name.clear();
    CHECK_EQ(DrsSettingText(s), std::string("0x00000002 string \"short\" (this profile)"));
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
    // Diagnostics (GitHub issue 10): the settings of acs.exe's profile and the
    // global profile's non-default ones are listed; the test exe has no app
    // profile, so none of an app profile.
    std::printf("  acs.exe: %zu app profile settings, %zu non-default global ones\n", acsPath.app_settings.size(),
                acsPath.base_settings.size());
    for (const DrsSetting& s : acsPath.app_settings) std::printf("    app %s\n", DrsSettingText(s).c_str());
    for (const DrsSetting& s : acsPath.base_settings) std::printf("    global %s\n", DrsSettingText(s).c_str());
    CHECK_EQ(acsPath.settings_error, std::string());
    CHECK(!acsPath.app_settings.empty());
    CHECK(self.app_settings.empty());
    // Each setting once, although driver 621.01's later pages overlap.
    std::vector<uint32_t> ids;
    for (const DrsSetting& s : acsPath.app_settings) ids.push_back(s.id);
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
    bool onlyNonDefault = true;
    for (const DrsSetting& s : acsPath.base_settings) onlyNonDefault = onlyNonDefault && s.location != 3;
    CHECK(onlyNonDefault);
}
