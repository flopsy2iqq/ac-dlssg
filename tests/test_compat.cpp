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

const char* kSmooth =
    "NVIDIA Smooth Motion (the driver's own frame generation) is active for Assetto Corsa, even if the NVIDIA App "
    "shows it off. It cannot run with DLSS-G. Run install.bat again to turn it off for AC (or see the README), then "
    "restart the game.";
// Review finding B-10: after install.bat kept DX11 out of the profile, the
// global profile's step; for another game's predefined profile (A-1), its
// name and the step there.
const char* kSmoothGlobal =
    "NVIDIA Smooth Motion is still on for AC, though the profile \"Assetto Corsa\" keeps DX11 out; DLSS-G cannot run "
    "with it. In NVIDIA Profile Inspector set Smooth Motion - Enabled APIs to 5 in the global profile (\"Base "
    "Profile\") too, then restart AC.";
const char* kSmoothForeign =
    "NVIDIA Smooth Motion is on for AC; DLSS-G cannot run with it. acs.exe gets another game's NVIDIA profile "
    "\"ACS\", which install.bat leaves alone: in NVIDIA Profile Inspector set its Smooth Motion - Enabled APIs to 5, "
    "then restart AC.";
const char* kOld = "OLD_SWAPCHAIN=1 is not supported";
const char* kExcl = "EXCLUSIVE_FULLSCREEN=1 is not supported";
const char* kHdr = "HDR output is not supported";
const char* kFsr = "CSP upscaler is not DLSS (need [FSR] ACTIVE=1, OLD_IMPLEMENTATION=3)";
// MSAA: the value and the file it came from, that it was read at game
// start, and where Content Manager turns it off.
std::string Msaa(long long samples, const std::string& file = "video.ini") {
    return "MSAA is on: AASAMPLES=" + std::to_string(samples) + " in " + file +
           " (read at game start); set Anti-aliasing to Off in Content Manager > Settings > Assetto Corsa > Video";
}
const std::string kMsaa = Msaa(4);

std::string Utf8(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
const char* kAspect = "video.ini aspect ratio differs from the window; letterboxed output is not supported in v1";
// The DLSS 5 add-ons: what cannot run with ac-dlssg yet, and what to use
// instead (CSP's built-in Neural Rendering runs together with ac-dlssg).
std::string Dlss5AddOn(const std::string& addOn) {
    return addOn +
           " cannot run together with ac-dlssg yet. Use CSP's built-in Neural Rendering instead, which works with "
           "ac-dlssg, or remove the add-on for frame generation.";
}
const std::string kDlss5 = Dlss5AddOn("DLSS 5 Bridge (dlss5-bridge.addon64)");
const std::string kRenodx = Dlss5AddOn("RenoDX DLSS5 (renodx-dlss5.addon64)");
const char* kHags = "hardware-accelerated GPU scheduling is off";

std::string VrReason(const char* mode) {
    return std::string("VR (MODE=") + mode +
           "): DLSS Frame Generation works only on a monitor, not in a headset; ac-dlssg stays off in VR";
}

std::string Reason(const CompatInputs& in, unsigned w = 1920, unsigned h = 1080) {
    auto r = EvaluateCompat(in, w, h);
    if (r.ok && !r.reason.empty()) return "<ok with reason>";
    return r.ok ? std::string() : r.reason;
}

}  // namespace

// The Smooth Motion reason fits the panel's 256-character state reason whole.
TEST(Compat_TheSmoothMotionReasonFitsThePanel) { CHECK(std::string(kSmooth).size() < 256); }

// Review finding B-10: the reason follows the driver profile. With DX11
// still in effect it sends the user to install.bat; once the profile keeps
// DX11 out, to the global profile (install.bat would find nothing to do);
// for another game's predefined profile, which install.bat leaves alone,
// it names that profile.
TEST(Compat_TheSmoothMotionReasonFollowsTheDriverProfile) {
    auto in = GoodInputs();
    in.nvpresent_caller = true;
    in.sm_profile = "Assetto Corsa";
    CHECK_EQ(Reason(in), kSmooth);
    in.sm_dx11_excluded = true;
    CHECK_EQ(Reason(in), kSmoothGlobal);

    in = GoodInputs();
    in.nvpresent_caller = true;
    in.sm_profile = "ACS";
    in.sm_profile_foreign = true;
    CHECK_EQ(Reason(in), kSmoothForeign);
    // DX11 kept out there too: the global step, naming the profile.
    in.sm_dx11_excluded = true;
    const std::string r = Reason(in);
    CHECK(r.find("the profile \"ACS\" keeps DX11 out") != std::string::npos);
    CHECK(r.find("in the global profile (\"Base Profile\")") != std::string::npos);
    // Without a profile for acs.exe: install.bat, which then says why.
    in = GoodInputs();
    in.nvpresent_caller = true;
    in.sm_dx11_excluded = true;
    CHECK_EQ(Reason(in), kSmooth);
}

// A long profile name is cut, at a whole UTF-8 character, so that every
// reason fits the panel.
TEST(Compat_TheSmoothMotionReasonsFitThePanelWithALongProfileName) {
    std::string longName;
    for (int i = 0; i < 40; ++i) longName += "\xD0\x96";  // U+0416, two bytes each
    for (int c = 0; c < 2; ++c) {
        auto in = GoodInputs();
        in.nvpresent_caller = true;
        in.sm_profile = longName;
        in.sm_profile_foreign = c == 0;
        in.sm_dx11_excluded = c == 1;
        const std::string r = Reason(in);
        CHECK(r.size() < 256);
        CHECK(r.find("...\"") != std::string::npos);
        const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, r.data(), static_cast<int>(r.size()), nullptr, 0);
        CHECK(n > 0);
    }
}

TEST(Compat_GoodInputsPass) {
    auto r = EvaluateCompat(GoodInputs(), 1920, 1080);
    CHECK(r.ok);
    CHECK(r.reason.empty());
}

TEST(Compat_EachRuleAlone) {
    auto in = GoodInputs();
    in.nvpresent_caller = true;
    CHECK_EQ(Reason(in), kSmooth);

    in = GoodInputs();
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
    CHECK_EQ(Reason(in), Msaa(2));
    in.aasamples = 8;
    CHECK_EQ(Reason(in), Msaa(8));

    in = GoodInputs();
    in.camera_mode = "TRIPLE";
    CHECK_EQ(Reason(in), "camera mode TRIPLE (triple screen) is not supported");
    in.camera_mode = "OCULUS";
    CHECK_EQ(Reason(in), VrReason("OCULUS"));

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
    in.nvpresent_caller = true;

    // NVIDIA Smooth Motion first: it took the swap chain over whatever else holds.
    CHECK_EQ(Reason(in), kSmooth);
    in.nvpresent_caller = false;
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
    CHECK_EQ(Reason(in), VrReason("OPENVR"));
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

// VR: AC's own camera modes OPENVR and OCULUS (acs.exe) and CSP's OpenXR
// mode __EXT_OPENXR (extension\config\data_manifest.ini [CUSTOM_RENDER_MODES]).
// DLSS-G presents through the monitor's swap chain, not to the headset, so
// the bridge stays off in VR and says so in the panel's 160-character field.
TEST(Compat_VrModesGetTheVrReason) {
    for (const char* mode : {"OPENVR", "OCULUS", "__EXT_OPENXR", "openvr", "Oculus", "__ext_openxr"}) {
        CHECK_EQ(VrRefusal(std::string(mode)), VrReason(mode));
        CHECK(VrReason(mode).size() < 160);
        auto in = GoodInputs();
        in.camera_mode = mode;
        CHECK_EQ(Reason(in), VrReason(mode));
    }
    CHECK(VrRefusal(std::nullopt).empty());
    for (const char* mode : {"DEFAULT", "TRIPLE", "__EXT_360", "__EXT_FISHEYE", "__EXT_SPLITSCREEN", "OPENVR2", ""})
        CHECK(VrRefusal(std::string(mode)).empty());
}

// The other modes name what they are: triple screen, or one of CSP's custom
// rendering modes (360, fisheye, split screen), which are not VR.
TEST(Compat_OtherCameraModesNameWhatTheyAre) {
    auto in = GoodInputs();
    in.camera_mode = "TRIPLE";
    CHECK_EQ(Reason(in), "camera mode TRIPLE (triple screen) is not supported");
    in.camera_mode = "__EXT_360";
    CHECK_EQ(Reason(in), "custom rendering mode __EXT_360 (360, fisheye or split screen) is not supported");
    in.camera_mode = "__ext_splitscreen";
    CHECK_EQ(Reason(in), "custom rendering mode __ext_splitscreen (360, fisheye or split screen) is not supported");
    in.camera_mode = "SOMETHING";
    CHECK_EQ(Reason(in), "camera mode SOMETHING is not supported");
}

// [debug] triple_diag=1 (1.3.0, diagnostics only): triple-screen mode is
// let through, so that the bridge proxies the swap chain (frame generation
// stays off, fg_policy.h) and logs CSP's DLSS calls; every other rule still
// applies, and the other camera modes stay refused.
TEST(Compat_TripleDiagLetsTripleScreenThrough) {
    auto in = GoodInputs();
    in.camera_mode = "TRIPLE";
    in.triple_diag = true;
    CHECK(Reason(in).empty());
    in.camera_mode = "triple";
    CHECK(Reason(in).empty());
    in.aasamples = 4;
    CHECK_EQ(Reason(in), kMsaa);
    in = GoodInputs();
    in.camera_mode = "TRIPLE";
    in.triple_diag = true;
    in.hags_on = false;
    CHECK_EQ(Reason(in), "hardware-accelerated GPU scheduling is off");
}

TEST(Compat_TripleDiagKeepsTheOtherModesRefused) {
    for (const char* mode : {"OPENVR", "OCULUS", "__EXT_OPENXR"}) {
        auto in = GoodInputs();
        in.camera_mode = mode;
        in.triple_diag = true;
        CHECK_EQ(Reason(in), VrReason(mode));
    }
    auto in = GoodInputs();
    in.triple_diag = true;
    in.camera_mode = "__EXT_360";
    CHECK_EQ(Reason(in), "custom rendering mode __EXT_360 (360, fisheye or split screen) is not supported");
    in.camera_mode = "SOMETHING";
    CHECK_EQ(Reason(in), "camera mode SOMETHING is not supported");
    in.camera_mode = "DEFAULT";
    CHECK(Reason(in).empty());
}

// The MSAA reason names the video.ini the bridge read (MSAA blocks frame
// generation in every DLSS mode, not only with DLAA); with a long Documents
// path it still fits the panel's state reason whole.
TEST(Compat_MsaaReasonNamesTheValueAndTheFile) {
    auto in = GoodInputs();
    in.aasamples = 4;
    in.video_ini_path = "C:\\Users\\Driver\\Documents\\Assetto Corsa\\cfg\\video.ini";
    CHECK_EQ(Reason(in), Msaa(4, "C:\\Users\\Driver\\Documents\\Assetto Corsa\\cfg\\video.ini"));
    in.video_ini_path = "C:\\Users\\A Long User Name\\OneDrive - A Company Name\\Documents\\Assetto Corsa\\cfg\\video.ini";
    CHECK(Reason(in).size() < 256);
    in.aasamples = 1;
    CHECK_EQ(Reason(in), "");
    in.aasamples = 0;
    CHECK_EQ(Reason(in), "");
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
    // The file the MSAA reason names, as UTF-8.
    CHECK_EQ(in.video_ini_path, Utf8(docs + L"\\cfg\\video.ini"));
    CHECK_EQ(in.hags_on, ReadHagsEnabled());
    CHECK(!in.dlss5_bridge_loaded);
    CHECK(!in.renodx_dlss5_loaded);

    // Review finding B-6: CSP runs DLSS only with Extra FX on, and in
    // triple-screen mode only with its "Allow in triple" (extra_fx.ini
    // [BASIC] ALLOW_IN_TRIPLE), which is 0 by default.
    CHECK(!in.extra_fx_enabled.has_value() && !in.extra_fx_allow_in_triple.has_value());
    dir.Write(L"game\\extension\\config\\extra_fx.ini", "[BASIC]\r\nENABLED=0\r\nALLOW_IN_TRIPLE=0 ; Allow in triple\r\n");
    dir.Write(L"docs\\cfg\\extension\\extra_fx.ini", "[BASIC]\r\nENABLED=1\r\n");
    CompatInputs fx = ReadCompatInputs(game, docs);
    CHECK(fx.extra_fx_enabled == std::optional<long long>(1));
    CHECK(fx.extra_fx_allow_in_triple == std::optional<long long>(0));

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

// DLSS 5 Bridge replaces CSP's DLSS call and hooks the same NGX functions,
// so the two cannot run together yet (GitHub issue 12). The panel says so
// and names the file that was found; its old name, dlss5-dx11-bridge.addon64
// (1.1.0 and earlier), counts too, because ReShade still loads it.
TEST(Compat_Dlss5BridgeReasonSaysTheyCannotRunTogether) {
    auto in = GoodInputs();
    in.dlss5_bridge_loaded = true;
    in.dlss5_bridge_file = "dlss5-dx11-bridge.addon64";
    CHECK_EQ(Reason(in), "DLSS 5 Bridge (dlss5-dx11-bridge.addon64) cannot run together with ac-dlssg yet. Use CSP's "
                         "built-in Neural Rendering instead, which works with ac-dlssg, or remove the add-on for frame "
                         "generation.");
    // The panel shows a pass-through's reason in its 256-character state reason.
    CHECK(Reason(in).size() < 256);
    in.dlss5_bridge_file.clear();  // no name known: the current one
    CHECK_EQ(Reason(in), kDlss5);
    // RenoDX DLSS5 gets the same advice.
    in = GoodInputs();
    in.renodx_dlss5_loaded = true;
    CHECK_EQ(Reason(in), "RenoDX DLSS5 (renodx-dlss5.addon64) cannot run together with ac-dlssg yet. Use CSP's "
                         "built-in Neural Rendering instead, which works with ac-dlssg, or remove the add-on for frame "
                         "generation.");
}

TEST(Compat_TheOldDlss5BridgeFileNameCountsAsDlss5Bridge) {
    REQUIRE(GetModuleHandleW(L"dlss5-bridge.addon64") == nullptr);
    REQUIRE(GetModuleHandleW(L"dlss5-dx11-bridge.addon64") == nullptr);
    CHECK(LoadedDlss5BridgeFile().empty());
    TempDir dir(L"compat_dlss5");
    const std::wstring copy = dir.Str() + L"\\dlss5-dx11-bridge.addon64";
    REQUIRE(CopyFileW(L"" ACDB_SPOOF_FIXTURE_PATH, copy.c_str(), FALSE));
    const HMODULE m = LoadLibraryW(copy.c_str());
    REQUIRE(m != nullptr);
    CHECK_EQ(LoadedDlss5BridgeFile(), std::string("dlss5-dx11-bridge.addon64"));
    const CompatInputs in = ReadCompatInputs(dir.Str() + L"\\game", dir.Str() + L"\\docs");
    CHECK(in.dlss5_bridge_loaded);
    CHECK_EQ(in.dlss5_bridge_file, std::string("dlss5-dx11-bridge.addon64"));
    FreeLibrary(m);
    CHECK(LoadedDlss5BridgeFile().empty());
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
