#include "compat.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <cmath>

#include "ini.h"

namespace acdb {
namespace {

std::wstring Join(const std::wstring& dir, const wchar_t* rest) {
    if (dir.empty()) return rest;
    const wchar_t last = dir.back();
    return (last == L'\\' || last == L'/') ? dir + rest : dir + L"\\" + rest;
}

bool EqualsNoCase(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 'a' + 'A');
        if (x != y) return false;
    }
    return i == a.size() && b[i] == '\0';
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// The DLSS 5 add-ons hook CSP's DLSS call; CSP's own Neural Rendering runs
// together with ac-dlssg, so the reason offers it instead.
std::string Dlss5AddOnRefusal(const std::string& addOn) {
    return addOn +
           " cannot run together with ac-dlssg yet. Use CSP's built-in Neural Rendering instead, which works with "
           "ac-dlssg, or remove the add-on for frame generation.";
}

CompatResult Refuse(std::string reason) {
    CompatResult r;
    r.ok = false;
    r.reason = std::move(reason);
    return r;
}

// A profile name for a reason: at most 20 bytes, cut at a whole UTF-8
// character and marked with "...".
std::string ShortProfileName(const std::string& name) {
    constexpr size_t kMax = 20;
    if (name.size() <= kMax) return name;
    size_t n = kMax - 3;
    while (n > 0 && (static_cast<unsigned char>(name[n]) & 0xC0) == 0x80) --n;
    return name.substr(0, n) + "...";
}

}  // namespace

SmoothMotionFix SmoothMotionFixOf(const CompatInputs& in) {
    if (in.sm_profile.empty()) return SmoothMotionFix::InstallBat;
    if (in.sm_dx11_excluded) return SmoothMotionFix::GlobalProfile;
    if (in.sm_profile_foreign) return SmoothMotionFix::ForeignProfile;
    return SmoothMotionFix::InstallBat;
}

std::string SmoothMotionRefusal(const CompatInputs& in) {
    switch (SmoothMotionFixOf(in)) {
        case SmoothMotionFix::GlobalProfile:
            return "NVIDIA Smooth Motion is still on for AC, though the profile \"" + ShortProfileName(in.sm_profile) +
                   "\" keeps DX11 out; DLSS-G cannot run with it. In NVIDIA Profile Inspector set Smooth Motion - "
                   "Enabled APIs to 5 in the global profile (\"Base Profile\") too, then restart AC.";
        case SmoothMotionFix::ForeignProfile:
            return "NVIDIA Smooth Motion is on for AC; DLSS-G cannot run with it. acs.exe gets another game's NVIDIA "
                   "profile \"" +
                   ShortProfileName(in.sm_profile) +
                   "\", which install.bat leaves alone: in NVIDIA Profile Inspector set its Smooth Motion - Enabled "
                   "APIs to 5, then restart AC.";
        case SmoothMotionFix::InstallBat:
            break;
    }
    return "NVIDIA Smooth Motion (the driver's own frame generation) is active for Assetto Corsa, even if the NVIDIA "
           "App shows it off. It cannot run with DLSS-G. Run install.bat again to turn it off for AC (or see the "
           "README), then restart the game.";
}

std::string VrRefusal(const std::optional<std::string>& cameraMode) {
    if (!cameraMode) return {};
    for (const char* vr : {"OPENVR", "OCULUS", "__EXT_OPENXR"}) {
        if (EqualsNoCase(*cameraMode, vr))
            return "VR (MODE=" + *cameraMode +
                   "): DLSS Frame Generation works only on a monitor, not in a headset; ac-dlssg stays off in VR";
    }
    return {};
}

bool AspectMismatch(double videoW, double videoH, double swapW, double swapH) {
    if (videoW == 0 || videoH == 0 || swapW == 0 || swapH == 0) return false;
    // |(vW/vH) / (sW/sH) - 1| > 0.005, rearranged to
    // 200 * |vW*sH - vH*sW| > |vH*sW| so that integer sizes compare exactly
    // (0.005 has no exact binary representation).
    return 200.0 * std::fabs(videoW * swapH - videoH * swapW) > std::fabs(videoH * swapW);
}

CompatResult EvaluateCompat(const CompatInputs& in, unsigned swapWidth, unsigned swapHeight) {
    if (in.nvpresent_caller) return Refuse(SmoothMotionRefusal(in));
    if (in.old_swapchain == 1) return Refuse("OLD_SWAPCHAIN=1 is not supported");
    if (in.exclusive_fullscreen == 1) return Refuse("EXCLUSIVE_FULLSCREEN=1 is not supported");
    if (in.hdr_enabled == 1) return Refuse("HDR output is not supported");
    if (in.fsr_active.value_or(0) != 1 || in.fsr_old_implementation.value_or(0) != 3)
        return Refuse("CSP upscaler is not DLSS (need [FSR] ACTIVE=1, OLD_IMPLEMENTATION=3)");
    if (in.aasamples && *in.aasamples > 1)
        return Refuse("MSAA is on: AASAMPLES=" + std::to_string(*in.aasamples) + " in " +
                      (in.video_ini_path.empty() ? std::string("video.ini") : in.video_ini_path) +
                      " (read at game start); set Anti-aliasing to Off in Content Manager > Settings > Assetto Corsa > "
                      "Video");
    // Triple-screen mode with [debug] triple_diag=1 goes on to the next
    // rules: the diagnostics of triple_diag.h.
    if (in.camera_mode && !EqualsNoCase(*in.camera_mode, "DEFAULT") &&
        !(in.triple_diag && EqualsNoCase(*in.camera_mode, "TRIPLE"))) {
        const std::string vr = VrRefusal(in.camera_mode);
        if (!vr.empty()) return Refuse(vr);
        const std::string& mode = *in.camera_mode;
        if (EqualsNoCase(mode, "TRIPLE")) return Refuse("camera mode " + mode + " (triple screen) is not supported");
        if (mode.size() > 6 && EqualsNoCase(mode.substr(0, 6), "__EXT_"))
            return Refuse("custom rendering mode " + mode + " (360, fisheye or split screen) is not supported");
        return Refuse("camera mode " + mode + " is not supported");
    }
    if (in.allow_stretching.value_or(0) != 1 && in.video_width && in.video_height &&
        AspectMismatch(static_cast<double>(*in.video_width), static_cast<double>(*in.video_height), swapWidth,
                       swapHeight))
        return Refuse("video.ini aspect ratio differs from the window; letterboxed output is not supported in v1");
    if (in.dlss5_bridge_loaded)
        return Refuse(Dlss5AddOnRefusal("DLSS 5 Bridge (" +
                                        (in.dlss5_bridge_file.empty() ? std::string("dlss5-bridge.addon64")
                                                                      : in.dlss5_bridge_file) +
                                        ")"));
    if (in.renodx_dlss5_loaded) return Refuse(Dlss5AddOnRefusal("RenoDX DLSS5 (renodx-dlss5.addon64)"));
    if (!in.hags_on) return Refuse("hardware-accelerated GPU scheduling is off");
    return CompatResult();
}

CompatInputs ReadCompatInputs(const std::wstring& gameDir, const std::wstring& docsAcDir) {
    const std::wstring cspBase = Join(gameDir, L"extension\\config\\");
    const std::wstring cspOver = Join(docsAcDir, L"cfg\\extension\\");
    const auto tweaksBase = IniFile::Load(cspBase + L"dxgi_tweaks.ini");
    const auto tweaksOver = IniFile::Load(cspOver + L"dxgi_tweaks.ini");
    const auto gfxBase = IniFile::Load(cspBase + L"graphics_adjustments.ini");
    const auto gfxOver = IniFile::Load(cspOver + L"graphics_adjustments.ini");
    const auto fxBase = IniFile::Load(cspBase + L"extra_fx.ini");
    const auto fxOver = IniFile::Load(cspOver + L"extra_fx.ini");
    const std::wstring videoPath = Join(docsAcDir, L"cfg\\video.ini");
    const auto video = IniFile::Load(videoPath);

    CompatInputs in;
    in.old_swapchain = ToInt(LayeredGet(tweaksBase, tweaksOver, "COMPATIBILITY", "OLD_SWAPCHAIN"));
    in.exclusive_fullscreen = ToInt(LayeredGet(tweaksBase, tweaksOver, "COMPATIBILITY", "EXCLUSIVE_FULLSCREEN"));
    in.allow_stretching = ToInt(LayeredGet(tweaksBase, tweaksOver, "COMPATIBILITY", "ALLOW_STRETCHING"));
    in.hdr_enabled = ToInt(LayeredGet(tweaksBase, tweaksOver, "HDR", "ENABLED"));
    in.fsr_active = ToInt(LayeredGet(gfxBase, gfxOver, "FSR", "ACTIVE"));
    in.fsr_old_implementation = ToInt(LayeredGet(gfxBase, gfxOver, "FSR", "OLD_IMPLEMENTATION"));
    in.extra_fx_enabled = ToInt(LayeredGet(fxBase, fxOver, "BASIC", "ENABLED"));
    in.extra_fx_allow_in_triple = ToInt(LayeredGet(fxBase, fxOver, "BASIC", "ALLOW_IN_TRIPLE"));
    in.video_ini_path = ToUtf8(videoPath);
    if (video) {
        in.aasamples = ToInt(video->Get("VIDEO", "AASAMPLES"));
        in.video_width = ToInt(video->Get("VIDEO", "WIDTH"));
        in.video_height = ToInt(video->Get("VIDEO", "HEIGHT"));
        in.camera_mode = video->Get("CAMERA", "MODE");
    }
    in.hags_on = ReadHagsEnabled();
    in.dlss5_bridge_file = LoadedDlss5BridgeFile();
    in.dlss5_bridge_loaded = !in.dlss5_bridge_file.empty();
    in.renodx_dlss5_loaded = GetModuleHandleW(L"renodx-dlss5.addon64") != nullptr;
    return in;
}

std::string LoadedDlss5BridgeFile() {
    for (const wchar_t* name : {L"dlss5-bridge.addon64", L"dlss5-dx11-bridge.addon64"}) {
        if (GetModuleHandleW(name)) {
            std::string file;
            for (const wchar_t* c = name; *c; ++c) file.push_back(static_cast<char>(*c));
            return file;
        }
    }
    return {};
}

bool ReadHagsEnabled() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    const LSTATUS st = RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
                                    L"HwSchMode", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return st == ERROR_SUCCESS && value == 2;
}

std::wstring DocumentsAcDir() {
    const DWORD needed = GetEnvironmentVariableW(L"ACDLSSG_DOCS_DIR", nullptr, 0);
    if (needed > 1) {
        std::wstring env(needed, L'\0');
        const DWORD n = GetEnvironmentVariableW(L"ACDLSSG_DOCS_DIR", env.data(), needed);
        if (n > 0 && n < needed) {
            env.resize(n);
            return env;
        }
    }

    PWSTR docs = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &docs)) && docs)
        result = Join(docs, L"Assetto Corsa");
    CoTaskMemFree(docs);
    return result;
}

}  // namespace acdb
