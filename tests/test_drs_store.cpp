// The NVAPI store of ac-dlssg-drs.exe (drs_store.h) against this machine's
// NVIDIA driver, READ ONLY: these tests never write the driver settings
// (writing them is for the maintainer's own test of the installer, see
// tools\package\install.ps1). Without an NVIDIA driver they only check
// that reading fails cleanly.
#include <windows.h>

#include <cstdio>
#include <string>

#include "driver_profile.h"
#include "drs_store.h"
#include "smooth_motion.h"
#include "test_framework.h"

using namespace acdb;

namespace {

bool SystemHasNvapi() {
    wchar_t dir[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    const DWORD attrs = GetFileAttributesW((std::wstring(dir) + L"\\nvapi64.dll").c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::string Ascii(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
}

std::string Text(const ProfileValue& v) { return v.present ? std::to_string(v.value) : std::string("not set"); }

// The full path that the bridge's own test finds the "Assetto Corsa"
// profile with (test_driver_profile.cpp).
const wchar_t kAcsExe[] = L"C:\\Games\\assettocorsa\\acs.exe";

}  // namespace

// The helper finds the profile the bridge reads for the same exe, by the
// exe and then by the profile's name, with the same values.
TEST(DrsStore_ReadsTheProfileTheBridgeReads) {
    auto store = MakeNvapiStore();
    SmoothMotionProfile p;
    std::string error;
    const DrsStatus st = store->Read(kAcsExe, L"", &p, &error);
    if (!SystemHasNvapi()) {
        std::printf("  no System32\\nvapi64.dll: %s\n", error.c_str());
        CHECK(st == DrsStatus::NoDriver && !error.empty());
        return;
    }
    const DriverProfileReport bridge = ReadDriverProfile(kAcsExe);
    REQUIRE(bridge.nvapi_ok);
    std::printf("  profile '%s' (bridge: '%s'): apis %s, global apis %s, enable %s, global enable %s\n",
                Ascii(p.name).c_str(), Ascii(bridge.app_profile).c_str(), Text(p.apis).c_str(),
                Text(p.globalApis).c_str(), Text(p.enable).c_str(), Text(p.globalEnable).c_str());
    if (bridge.app_profile.empty()) {
        CHECK(st == DrsStatus::NoProfile);
        return;
    }
    REQUIRE(st == DrsStatus::Ok);
    CHECK(p.name == bridge.app_profile);
    // The bridge's listing of the app profile (location 0: its own values)
    // agrees about the setting the helper changes.
    for (const DrsSetting& s : bridge.app_settings) {
        if (s.id != kDrsSmoothMotionApis || s.location != 0) continue;
        CHECK(p.apis.present && p.apis.value == static_cast<uint32_t>(s.number));
    }
    SmoothMotionProfile byName;
    CHECK(store->Read(L"", p.name, &byName, &error) == DrsStatus::Ok);
    CHECK(byName.name == p.name);
    CHECK(byName.apis == p.apis && byName.enable == p.enable);
    CHECK(byName.globalApis == p.globalApis && byName.globalEnable == p.globalEnable);
}

TEST(DrsStore_AProfileNameThatDoesNotExistIsNoProfile) {
    if (!SystemHasNvapi()) return;
    auto store = MakeNvapiStore();
    SmoothMotionProfile p;
    std::string error;
    CHECK(store->Read(L"", L"ac-dlssg test: no profile has this name 7f3a", &p, &error) == DrsStatus::NoProfile);
    CHECK(!error.empty());
}

// Write needs the profile of a successful Read; without one it touches
// nothing and fails.
TEST(DrsStore_AWriteBeforeAnyReadFails) {
    auto store = MakeNvapiStore();
    std::string error;
    CHECK(store->Write(kDrsSmoothMotionApis, 5u, &error) == DrsStatus::Failed);
    CHECK(!error.empty());
    SmoothMotionProfile p;
    if (SystemHasNvapi()) {
        CHECK(store->Read(L"", L"ac-dlssg test: no profile has this name 7f3a", &p, &error) == DrsStatus::NoProfile);
        // A failed Read leaves no profile to write into either.
        CHECK(store->Write(kDrsSmoothMotionApis, std::nullopt, &error) == DrsStatus::Failed);
    }
}
