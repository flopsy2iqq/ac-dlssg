// DriverProfileNotes and the "DLSS-G provider" line (pure), and
// ReadDriverProfile against this machine's NVIDIA driver.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "driver_profile.h"
#include "fg_policy.h"
#include "ngx_hook.h"
#include "panel_status.h"
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

std::string FgWarning(const char* where, uint32_t v, const char* user) {
    return std::string("NVIDIA App DLSS-FG override is on in the ") + where + " profile (setting 0x10E41E03 = " +
           std::to_string(v) + "); NGX may load a newer DLSS-G than " + user + " supports";
}

const char kOverrideNote[] =
    "Note: the NVIDIA App's DLSS override is on, so DLSS-G may use the App's newer model. This is normal, not an error.";

DriverProfileNote Warning(const std::string& text) { return {DriverNoteKind::Warning, text}; }
DriverProfileNote Note(const std::string& text) { return {DriverNoteKind::Note, text}; }

void Print(const char* what, const std::vector<DriverProfileNote>& notes) {
    std::printf("  %s: %zu notes\n", what, notes.size());
    for (const auto& n : notes)
        std::printf("    %s: %s\n", n.kind == DriverNoteKind::Warning ? "warning" : "note", n.text.c_str());
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
    Print("notes", DriverProfileNotes(r, DriverProfileContext()));
}

bool SystemHasNvapi() {
    wchar_t dir[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    const DWORD attrs = GetFileAttributesW((std::wstring(dir) + L"\\nvapi64.dll").c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace

TEST(DriverProfile_NoNotesForAnEmptyReport) {
    CHECK(DriverProfileNotes(DriverProfileReport(), DriverProfileContext()).empty());
    DriverProfileReport failed;
    failed.error = "nvapi64.dll not found";
    CHECK(DriverProfileNotes(failed, DriverProfileContext()).empty());
    CHECK_EQ(DriverNotesPanelCode({}), 0u);
    CHECK_EQ(DriverNotesPanelText({}), std::string());
}

// NGX_DLSSG_MODE = NGX_DLSSG_MODE_OFF stays a warning, in either profile.
TEST(DriverProfile_FrameGenerationForcedOffIsAWarningInEitherProfile) {
    for (unsigned mask = 0; mask < 4; ++mask) {
        DriverProfileReport r;
        r.nvapi_ok = true;
        // Set but harmless values where a trigger is off.
        r.dlssg_mode_app = Set((mask & 1) ? kNgxDlssgModeOff : 2u);
        r.dlssg_mode_base = Set((mask & 2) ? kNgxDlssgModeOff : 0u);
        std::vector<DriverProfileNote> expected;
        if (mask & 1) expected.push_back(Warning(kDlssgApp));
        if (mask & 2) expected.push_back(Warning(kDlssgGlobal));
        const auto got = DriverProfileNotes(r, DriverProfileContext());
        if (got != expected) Print("mask", got);
        CHECK(got == expected);
    }
}

// Streamline reads SL_DLSS_OVERRIDE from the app profile first and takes the
// global value only when the app profile has none (sl.ota readOTAOverrideDRS):
// with the app profile's 0 the global override does not touch ac-dlssg\sl,
// which is a note, not a warning (GitHub issue 10, i10fixed).
TEST(DriverProfile_TheAppProfilesStreamlineOverrideWins) {
    struct Case {
        DrsValue app, global;
        std::vector<DriverProfileNote> expected;
    };
    const Case cases[] = {
        {DrsValue(), DrsValue(), {}},
        {DrsValue(), Set(0), {}},
        {DrsValue(), Set(1), {Warning(SlWarning("global", 1))}},
        {Set(0), Set(1), {Note(kOverrideNote)}},
        {Set(0), DrsValue(), {}},
        {Set(2), Set(0), {Warning(SlWarning("app", 2))}},
        {Set(1), Set(3), {Warning(SlWarning("app", 1))}},
    };
    for (const Case& c : cases) {
        DriverProfileReport r;
        r.nvapi_ok = true;
        r.sl_override_app = c.app;
        r.sl_override_base = c.global;
        const auto got = DriverProfileNotes(r, DriverProfileContext());
        if (got != c.expected) Print("Streamline override", got);
        CHECK(got == c.expected);
    }
}

// NGX_DLSS_FG_OVERRIDE ("Enable DLSS-FG override") is read with the app
// profile first, then the global one. With it on, NGX loads the NVIDIA App's
// DLSS-G from its cache instead of ac-dlssg\sl's: a note, unless an add-on
// that works only with ac-dlssg\sl's DLSS-G is in use (dlssg_for_sm86 on RTX
// 20/30, MFG-Unlocker on RTX 40).
TEST(DriverProfile_TheFgOverrideIsInheritedAndOnlyAWarningForTheAddOns) {
    struct Case {
        DrsValue app, global;
        const char* user;
        std::vector<DriverProfileNote> expected;
    };
    const Case cases[] = {
        {DrsValue(), Set(1), "", {Note(kOverrideNote)}},
        {DrsValue(), Set(1), "dlssg_for_sm86", {Warning(FgWarning("global", 1, "dlssg_for_sm86"))}},
        {Set(1), DrsValue(), "MFG-Unlocker", {Warning(FgWarning("app", 1, "MFG-Unlocker"))}},
        {Set(1), Set(0), "", {Note(kOverrideNote)}},
        {Set(0), Set(1), "dlssg_for_sm86", {}},
        {Set(0), Set(1), "", {}},
        {DrsValue(), Set(0), "MFG-Unlocker", {}},
        {DrsValue(), DrsValue(), "dlssg_for_sm86", {}},
    };
    for (const Case& c : cases) {
        DriverProfileReport r;
        r.nvapi_ok = true;
        r.fg_override_app = c.app;
        r.fg_override_base = c.global;
        DriverProfileContext ctx;
        ctx.pinned_dlssg_user = c.user;
        const auto got = DriverProfileNotes(r, ctx);
        if (got != c.expected) Print("DLSS-FG override", got);
        CHECK(got == c.expected);
    }
}

// The profile of GitHub issue 10's i10fixed log (RTX 50, the NVIDIA App's
// global DLSS overrides on, the app profile's Streamline override 0): one
// note. With an add-on that needs ac-dlssg\sl's DLSS-G, the DLSS-FG warning
// alone, because "this is normal" would then be wrong.
TEST(DriverProfile_TheNvidiaAppsGlobalOverridesGiveOneNote) {
    DriverProfileReport r;
    r.nvapi_ok = true;
    r.app_profile = L"Assetto Corsa";
    r.dlssg_mode_app = Set(0);
    r.dlssg_mode_base = Set(2);
    r.sl_override_app = Set(0);
    r.sl_override_base = Set(1);
    r.fg_override_base = Set(1);
    const auto notes = DriverProfileNotes(r, DriverProfileContext());
    CHECK(notes == std::vector<DriverProfileNote>{Note(kOverrideNote)});
    CHECK_EQ(DriverNotesPanelCode(notes), 2u);
    CHECK_EQ(DriverNotesPanelText(notes), std::string(kOverrideNote));

    DriverProfileContext mfg;
    mfg.pinned_dlssg_user = "MFG-Unlocker";
    const auto warned = DriverProfileNotes(r, mfg);
    CHECK(warned == std::vector<DriverProfileNote>{Warning(FgWarning("global", 1, "MFG-Unlocker"))});
    CHECK_EQ(DriverNotesPanelCode(warned), 1u);
}

// Warnings come first, and the panel shows the first warning; a note only
// when there is no warning (StatusLayout::driverWarning 1 or 2).
TEST(DriverProfile_WarningsComeFirstAndThePanelShowsTheFirstOne) {
    DriverProfileReport r;
    r.nvapi_ok = true;
    r.sl_override_app = Set(0);
    r.sl_override_base = Set(1);  // a note
    r.dlssg_mode_base = Set(kNgxDlssgModeOff);  // a warning
    const auto notes = DriverProfileNotes(r, DriverProfileContext());
    CHECK(notes == (std::vector<DriverProfileNote>{Warning(kDlssgGlobal), Note(kOverrideNote)}));
    CHECK_EQ(DriverNotesPanelCode(notes), 1u);
    CHECK_EQ(DriverNotesPanelText(notes), std::string(kDlssgGlobal));
    CHECK_EQ(DriverNotesPanelCode({Note("n1"), Note("n2")}), 2u);
    CHECK_EQ(DriverNotesPanelText({Note("n1"), Note("n2")}), std::string("n1"));
    CHECK_EQ(DriverNotesPanelText({Note("n1"), Warning("w1")}), std::string("w1"));
    static_assert(static_cast<uint32_t>(DriverNoteKind::Warning) == kPanelDriverWarning);
    static_assert(static_cast<uint32_t>(DriverNoteKind::Note) == kPanelDriverNote);
}

// Every text fits the panel's warning field whole. The override settings
// hold small values (0 to 3), so one digit is the realistic width.
TEST(DriverProfile_EveryTextFitsThePanelsWarningField) {
    std::vector<std::string> texts = {kDlssgApp, kDlssgGlobal, kOverrideNote, SlWarning("global", 9),
                                      FgWarning("global", 9, "dlssg_for_sm86"), FgWarning("global", 9, "MFG-Unlocker")};
    for (const auto& t : texts) {
        if (t.size() >= kPanelReasonChars) std::printf("  %zu characters: %s\n", t.size(), t.c_str());
        CHECK(t.size() < kPanelReasonChars);
    }
}

// The "DLSS-G provider" line (GitHub issue 10): NGX's cache copy of the
// NVIDIA App's DLSS-G (\NGX\models\dlssg\versions\<n>\files\*.bin, loaded
// with the DLSS-FG override) and the driver's own nvngx_dlssg.dll in the
// DriverStore (NVIDIA Smooth Motion's) are told apart from ac-dlssg\sl's.
TEST(DlssgProvider_TheKindOfEveryDlssgFile) {
    CHECK(NgxClassifyDlssgFile(L"C:\\ProgramData\\NVIDIA\\NGX\\models\\dlssg\\versions\\20318480\\files/160_E658700.bin") ==
          NgxDlssgFile::NgxCache);
    CHECK(NgxClassifyDlssgFile(L"c:/programdata/nvidia/ngx/models/DLSSG/versions/20318480/files/x.bin") ==
          NgxDlssgFile::NgxCache);
    CHECK(NgxClassifyDlssgFile(
              L"C:\\WINDOWS\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_20f49c6eb5c9b39b\\nvngx_dlssg.dll") ==
          NgxDlssgFile::DriverStore);
    CHECK(NgxClassifyDlssgFile(L"D:\\Steam\\steamapps\\common\\assettocorsa\\ac-dlssg\\sl\\nvngx_dlssg.dll") ==
          NgxDlssgFile::Dll);
    CHECK(NgxClassifyDlssgFile(L"D:\\Steam\\steamapps\\common\\assettocorsa\\ac-dlssg\\sl\\NVNGX_DLSSG.DLL") ==
          NgxDlssgFile::Dll);
    // The DLSS (super resolution) cache, other NGX modules and look-alikes are none.
    CHECK(NgxClassifyDlssgFile(L"C:\\ProgramData\\NVIDIA\\NGX\\models\\dlss\\versions\\20316673\\files\\160_E658703.bin") ==
          NgxDlssgFile::None);
    CHECK(NgxClassifyDlssgFile(L"C:\\WINDOWS\\System32\\DriverStore\\FileRepository\\nv\\_nvngx.dll") == NgxDlssgFile::None);
    CHECK(NgxClassifyDlssgFile(L"D:\\AC\\my_nvngx_dlssg.dll") == NgxDlssgFile::None);
    CHECK(NgxClassifyDlssgFile(L"") == NgxDlssgFile::None);
    CHECK(NgxClassifyDlssgFile(nullptr) == NgxDlssgFile::None);
}

// The cache folder's number is the DLSS-G version: 20318480 = 0x01360910 is
// 310.9.16 (0x136, 0x09, 0x10).
TEST(DlssgProvider_TheCacheFolderGivesTheVersion) {
    CHECK_EQ(NgxCacheDlssgVersion(L"C:\\ProgramData\\NVIDIA\\NGX\\models\\dlssg\\versions\\20318480\\files/160_E658700.bin"),
             std::string("310.9.16"));
    CHECK_EQ(NgxCacheDlssgVersion(L"C:/ProgramData/NVIDIA/NGX/models/dlssg/VERSIONS/20318465/files/a.bin"),
             std::string("310.9.1"));
    CHECK_EQ(NgxCacheDlssgVersion(L"C:\\ProgramData\\NVIDIA\\NGX\\models\\dlssg\\versions\\0\\files\\a.bin"), std::string());
    CHECK_EQ(NgxCacheDlssgVersion(L"C:\\ProgramData\\NVIDIA\\NGX\\models\\dlssg\\versions\\12ab\\files\\a.bin"), std::string());
    CHECK_EQ(NgxCacheDlssgVersion(L"D:\\AC\\ac-dlssg\\sl\\nvngx_dlssg.dll"), std::string());
}

TEST(DlssgProvider_TheLineNamesTheProviderAndTheOtherCopies) {
    const DlssgModuleInfo sl{NgxDlssgFile::Dll, "D:/AC/ac-dlssg/sl/nvngx_dlssg.dll", "310.9.1.0"};
    const DlssgModuleInfo cache{NgxDlssgFile::NgxCache,
                                "C:\\ProgramData\\NVIDIA\\NGX\\models\\dlssg\\versions\\20318480\\files/160_E658700.bin",
                                "310.9.16"};
    const DlssgModuleInfo store{NgxDlssgFile::DriverStore, "C:\\WINDOWS\\System32\\DriverStore\\nv\\nvngx_dlssg.dll",
                                "310.2.1.0"};
    CHECK_EQ(DlssgProviderLine({sl}), std::string("DLSS-G provider: D:/AC/ac-dlssg/sl/nvngx_dlssg.dll, version 310.9.1.0"));
    CHECK_EQ(DlssgProviderLine({}),
             std::string("DLSS-G provider: no DLSS-G is loaded (no nvngx_dlssg.dll and no DLSS-G from the NGX cache)"));
    CHECK_EQ(DlssgProviderLine({cache}),
             std::string("DLSS-G provider: the NVIDIA App's DLSS-G 310.9.16 from the NGX cache (DLSS-FG override), ") +
                 cache.path);
    // The cache copy is the provider even when NGX also loaded ac-dlssg\sl's.
    CHECK_EQ(DlssgProviderLine({sl, cache}),
             std::string("DLSS-G provider: the NVIDIA App's DLSS-G 310.9.16 from the NGX cache (DLSS-FG override), ") +
                 cache.path + "; also loaded: D:/AC/ac-dlssg/sl/nvngx_dlssg.dll, version 310.9.1.0");
    CHECK_EQ(DlssgProviderLine({store, sl}),
             std::string("DLSS-G provider: D:/AC/ac-dlssg/sl/nvngx_dlssg.dll, version 310.9.1.0; also loaded: the "
                         "driver's own nvngx_dlssg.dll (NVIDIA Smooth Motion's), ") +
                 store.path + ", version 310.2.1.0");
    CHECK_EQ(DlssgProviderLine({store}),
             std::string("DLSS-G provider: only the driver's own nvngx_dlssg.dll (NVIDIA Smooth Motion's), ") + store.path +
                 ", version 310.2.1.0");
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
