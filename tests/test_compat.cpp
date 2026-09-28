#include <windows.h>

#include <cwctype>
#include <string>

#include "compat.h"
#include "temp_dir.h"
#include "test_framework.h"

using namespace acdb;
using acdb_test::TempDir;

namespace {

// Everything a supported setup reports: rules 1..10 all pass at 1920x1080.
CompatInputs GoodInputs() {
    CompatInputs in;
    in.old_swapchain = 0;
    in.exclusive_fullscreen = 0;
    in.allow_stretching = 0;
    in.hdr_enabled = 0;
    in.fsr_active = 1;
    in.fsr_old_implementation = 3;
    in.aasamples = 1;
    in.video_width = 1920;
    in.video_height = 1080;
    in.camera_mode = "DEFAULT";
    in.hags_on = true;
    in.dlss5_bridge_loaded = false;
    in.renodx_dlss5_loaded = false;
    return in;
}

const char* kOld = "OLD_SWAPCHAIN=1 is not supported";
const char* kExcl = "EXCLUSIVE_FULLSCREEN=1 is not supported";
const char* kHdr = "HDR output is not supported";
const char* kFsr = "CSP upscaler is not DLSS (need [FSR] ACTIVE=1, OLD_IMPLEMENTATION=3)";
const char* kMsaa = "MSAA (AASAMPLES>1) is not supported";
const char* kAspect = "video.ini aspect ratio differs from the window; letterboxed output is not supported in v1";
const char* kDlss5 = "dlss5-bridge.addon64 is loaded";
const char* kRenodx = "renodx-dlss5.addon64 is loaded";
const char* kHags = "hardware-accelerated GPU scheduling is off";

std::string Reason(const CompatInputs& in, unsigned w = 1920, unsigned h = 1080) {
    auto r = EvaluateCompat(in, w, h);
    if (r.ok && !r.reason.empty()) return "<ok with reason>";
    return r.ok ? std::string() : r.reason;
}

}  // namespace

TEST(Compat_GoodInputsPass) {
    auto r = EvaluateCompat(GoodInputs(), 1920, 1080);
    CHECK(r.ok);
    CHECK(r.reason.empty());
}

TEST(Compat_EachRuleAlone) {
    auto in = GoodInputs();
    in.old_swapchain = 1;
    CHECK_EQ(Reason(in), kOld);

    in = GoodInputs();
    in.exclusive_fullscreen = 1;
    CHECK_EQ(Reason(in), kExcl);

    in = GoodInputs();
    in.hdr_enabled = 1;
    CHECK_EQ(Reason(in), kHdr);

    in = GoodInputs();
    in.fsr_active = 0;
    CHECK_EQ(Reason(in), kFsr);
    in = GoodInputs();
    in.fsr_old_implementation = 2;
    CHECK_EQ(Reason(in), kFsr);

    in = GoodInputs();
    in.aasamples = 2;
    CHECK_EQ(Reason(in), kMsaa);
    in.aasamples = 8;
    CHECK_EQ(Reason(in), kMsaa);

    in = GoodInputs();
    in.camera_mode = "TRIPLE";
    CHECK_EQ(Reason(in), "camera mode TRIPLE (VR or triple screen) is not supported");
    in.camera_mode = "OCULUS";
    CHECK_EQ(Reason(in), "camera mode OCULUS (VR or triple screen) is not supported");

    in = GoodInputs();
    in.video_height = 1200;
    CHECK_EQ(Reason(in), kAspect);

    in = GoodInputs();
    in.dlss5_bridge_loaded = true;
    CHECK_EQ(Reason(in), kDlss5);

    in = GoodInputs();
    in.renodx_dlss5_loaded = true;
    CHECK_EQ(Reason(in), kRenodx);

    in = GoodInputs();
    in.hags_on = false;
    CHECK_EQ(Reason(in), kHags);
}

TEST(Compat_RulesAreReportedInOrder) {
    CompatInputs in;  // every key missing, HAGS off: fails from rule 4 on
    in.old_swapchain = 1;
    in.exclusive_fullscreen = 1;
    in.hdr_enabled = 1;
    in.aasamples = 4;
    in.camera_mode = "OPENVR";
    in.video_width = 1920;
    in.video_height = 1200;
    in.dlss5_bridge_loaded = true;
    in.renodx_dlss5_loaded = true;
    in.hags_on = false;

    CHECK_EQ(Reason(in), kOld);
    in.old_swapchain = 0;
    CHECK_EQ(Reason(in), kExcl);
    in.exclusive_fullscreen = 0;
    CHECK_EQ(Reason(in), kHdr);
    in.hdr_enabled = 0;
    CHECK_EQ(Reason(in), kFsr);
    in.fsr_active = 1;
    in.fsr_old_implementation = 3;
    CHECK_EQ(Reason(in), kMsaa);
    in.aasamples = 1;
    CHECK_EQ(Reason(in), "camera mode OPENVR (VR or triple screen) is not supported");
    in.camera_mode = "default";
    CHECK_EQ(Reason(in), kAspect);
    in.video_height = 1080;
    CHECK_EQ(Reason(in), kDlss5);
    in.dlss5_bridge_loaded = false;
    CHECK_EQ(Reason(in), kRenodx);
    in.renodx_dlss5_loaded = false;
    CHECK_EQ(Reason(in), kHags);
    in.hags_on = true;
    CHECK_EQ(Reason(in), "");
}

TEST(Compat_MissingKeyDefaults) {
    CompatInputs in;
    in.hags_on = true;
    // FSR keys missing count as 0.
    CHECK_EQ(Reason(in), kFsr);
    in.fsr_active = 1;
    CHECK_EQ(Reason(in), kFsr);
    in.fsr_old_implementation = 3;
    in.fsr_active = std::nullopt;
    CHECK_EQ(Reason(in), kFsr);
    in.fsr_active = 1;
    // Everything else missing passes: no OLD_SWAPCHAIN, no MSAA, no camera,
    // no video size (aspect rule skipped).
    CHECK_EQ(Reason(in), "");
    // One video size alone does not trigger the aspect rule.
    in.video_width = 1920;
    CHECK_EQ(Reason(in, 1920, 1200), "");
    // ALLOW_STRETCHING missing counts as 0.
    in.video_height = 1080;
    CHECK_EQ(Reason(in, 1920, 1200), kAspect);
}

TEST(Compat_CameraModeCaseInsensitive) {
    auto in = GoodInputs();
    in.camera_mode = "default";
    CHECK_EQ(Reason(in), "");
    in.camera_mode = "Default";
    CHECK_EQ(Reason(in), "");
    in.camera_mode = std::nullopt;
    CHECK_EQ(Reason(in), "");
}

TEST(Compat_AllowStretchingDisablesAspectRule) {
    auto in = GoodInputs();
    in.video_width = 1920;
    in.video_height = 1200;
    CHECK_EQ(Reason(in, 1920, 1080), kAspect);
    in.allow_stretching = 1;
    CHECK_EQ(Reason(in, 1920, 1080), "");
    in.allow_stretching = 0;
    CHECK_EQ(Reason(in, 1920, 1080), kAspect);
    // Zero swap size: AspectMismatch is false.
    CHECK_EQ(Reason(in, 0, 0), "");
}

TEST(Compat_AspectMismatchBoundaries) {
    // |ratio - 1| exactly 0.005 is not a mismatch; just above it is.
    CHECK(!AspectMismatch(2010, 2000, 1, 1));
    CHECK(AspectMismatch(2011, 2000, 1, 1));
    CHECK(!AspectMismatch(1990, 2000, 1, 1));
    CHECK(AspectMismatch(1989, 2000, 1, 1));
    CHECK(!AspectMismatch(1, 1, 2000, 2010));
    CHECK(!AspectMismatch(1, 1, 2010, 2000));
    CHECK(!AspectMismatch(2000, 2000, 2000, 2000));
    // 16:9 vs 16:10.
    CHECK(AspectMismatch(1920, 1080, 1920, 1200));
    CHECK(AspectMismatch(1920, 1200, 1920, 1080));
    // Same aspect at different sizes, and a one-pixel difference.
    CHECK(!AspectMismatch(1280, 720, 2560, 1440));
    CHECK(!AspectMismatch(1920, 1080, 1920, 1081));
    CHECK(!AspectMismatch(3440, 1440, 3440, 1440));
    // 21:9 vs 16:9.
    CHECK(AspectMismatch(3440, 1440, 2560, 1440));
    // Zero arguments.
    CHECK(!AspectMismatch(0, 1080, 1920, 1080));
    CHECK(!AspectMismatch(1920, 0, 1920, 1080));
    CHECK(!AspectMismatch(1920, 1080, 0, 1080));
    CHECK(!AspectMismatch(1920, 1080, 1920, 0));
}

TEST(Compat_ReadInputsLayered) {
    TempDir dir(L"compat");
    const std::wstring game = dir.Str() + L"\\game";
    const std::wstring docs = dir.Str() + L"\\docs";
    dir.Write(L"game\\extension\\config\\dxgi_tweaks.ini",
              "[COMPATIBILITY]\r\nOLD_SWAPCHAIN=0\r\nEXCLUSIVE_FULLSCREEN=1 ; Exclusive\r\nALLOW_STRETCHING=0\r\n"
              "[HDR]\r\nENABLED=0\r\n");
    dir.Write(L"docs\\cfg\\extension\\dxgi_tweaks.ini",
              "[COMPATIBILITY]\r\nEXCLUSIVE_FULLSCREEN=0\r\n[HDR]\r\nENABLED=1\r\n");
    dir.Write(L"game\\extension\\config\\graphics_adjustments.ini", "[FSR]\r\nACTIVE=0\r\nOLD_IMPLEMENTATION=3\r\n");
    dir.Write(L"docs\\cfg\\extension\\graphics_adjustments.ini", "[FSR]\r\nACTIVE=1 ; Active; 1 or 0\r\n");
    dir.Write(L"docs\\cfg\\video.ini",
              "[VIDEO]\r\nWIDTH=2560\r\nHEIGHT=1440\r\nAASAMPLES=4\r\n[CAMERA]\r\nMODE=TRIPLE\r\n");

    CompatInputs in = ReadCompatInputs(game, docs);
    CHECK(in.old_swapchain == std::optional<long long>(0));
    CHECK(in.exclusive_fullscreen == std::optional<long long>(0));
    CHECK(in.allow_stretching == std::optional<long long>(0));
    CHECK(in.hdr_enabled == std::optional<long long>(1));
    CHECK(in.fsr_active == std::optional<long long>(1));
    CHECK(in.fsr_old_implementation == std::optional<long long>(3));
    CHECK(in.aasamples == std::optional<long long>(4));
    CHECK(in.video_width == std::optional<long long>(2560));
    CHECK(in.video_height == std::optional<long long>(1440));
    CHECK(in.camera_mode == std::optional<std::string>("TRIPLE"));
    CHECK_EQ(in.hags_on, ReadHagsEnabled());
    CHECK(!in.dlss5_bridge_loaded);
    CHECK(!in.renodx_dlss5_loaded);

    // Trailing separators on the directories make no difference.
    CompatInputs in2 = ReadCompatInputs(game + L"\\", docs + L"\\");
    CHECK(in2.hdr_enabled == std::optional<long long>(1));
    CHECK(in2.video_width == std::optional<long long>(2560));
}

TEST(Compat_ReadInputsOverrideOnlyAndEmptyTree) {
    TempDir dir(L"compat");
    const std::wstring game = dir.Str() + L"\\game";
    const std::wstring docs = dir.Str() + L"\\docs";
    CompatInputs empty = ReadCompatInputs(game, docs);
    CHECK(!empty.old_swapchain.has_value());
    CHECK(!empty.exclusive_fullscreen.has_value());
    CHECK(!empty.allow_stretching.has_value());
    CHECK(!empty.hdr_enabled.has_value());
    CHECK(!empty.fsr_active.has_value());
    CHECK(!empty.fsr_old_implementation.has_value());
    CHECK(!empty.aasamples.has_value());
    CHECK(!empty.video_width.has_value());
    CHECK(!empty.video_height.has_value());
    CHECK(!empty.camera_mode.has_value());

    dir.Write(L"docs\\cfg\\extension\\dxgi_tweaks.ini", "[COMPATIBILITY]\nALLOW_STRETCHING=1\nOLD_SWAPCHAIN=x\n");
    CompatInputs in = ReadCompatInputs(game, docs);
    CHECK(in.allow_stretching == std::optional<long long>(1));
    CHECK(!in.old_swapchain.has_value());  // non-numeric reads as missing
}

TEST(Compat_ReadHagsMatchesRegistry) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    HKEY key = nullptr;
    bool expected = false;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", 0, KEY_READ,
                      &key) == ERROR_SUCCESS) {
        DWORD type = 0;
        if (RegQueryValueExW(key, L"HwSchMode", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) ==
                ERROR_SUCCESS &&
            type == REG_DWORD)
            expected = value == 2;
        RegCloseKey(key);
    }
    CHECK_EQ(ReadHagsEnabled(), expected);
}

TEST(Compat_DocumentsAcDirHonoursEnv) {
    wchar_t prev[32768] = {};
    const DWORD prevLen = GetEnvironmentVariableW(L"ACDLSSG_DOCS_DIR", prev, 32768);

    SetEnvironmentVariableW(L"ACDLSSG_DOCS_DIR", L"C:\\acdb\\fixture docs");
    CHECK(DocumentsAcDir() == L"C:\\acdb\\fixture docs");

    SetEnvironmentVariableW(L"ACDLSSG_DOCS_DIR", nullptr);
    const std::wstring real = DocumentsAcDir();
    const std::wstring suffix = L"\\Assetto Corsa";
    CHECK(real.size() > suffix.size());
    CHECK(real.size() > suffix.size() && real.compare(real.size() - suffix.size(), suffix.size(), suffix) == 0);

    SetEnvironmentVariableW(L"ACDLSSG_DOCS_DIR", prevLen ? prev : nullptr);
}
