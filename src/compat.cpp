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

CompatResult Refuse(std::string reason) {
    CompatResult r;
    r.ok = false;
    r.reason = std::move(reason);
    return r;
}

}  // namespace

bool AspectMismatch(double videoW, double videoH, double swapW, double swapH) {
    if (videoW == 0 || videoH == 0 || swapW == 0 || swapH == 0) return false;
    // |(vW/vH) / (sW/sH) - 1| > 0.005, rearranged to
    // 200 * |vW*sH - vH*sW| > |vH*sW| so that integer sizes compare exactly
    // (0.005 has no exact binary representation).
    return 200.0 * std::fabs(videoW * swapH - videoH * swapW) > std::fabs(videoH * swapW);
}

CompatResult EvaluateCompat(const CompatInputs& in, unsigned swapWidth, unsigned swapHeight) {
    if (in.old_swapchain == 1) return Refuse("OLD_SWAPCHAIN=1 is not supported");
    if (in.exclusive_fullscreen == 1) return Refuse("EXCLUSIVE_FULLSCREEN=1 is not supported");
    if (in.hdr_enabled == 1) return Refuse("HDR output is not supported");
    if (in.fsr_active.value_or(0) != 1 || in.fsr_old_implementation.value_or(0) != 3)
        return Refuse("CSP upscaler is not DLSS (need [FSR] ACTIVE=1, OLD_IMPLEMENTATION=3)");
    if (in.aasamples && *in.aasamples > 1) return Refuse("MSAA (AASAMPLES>1) is not supported");
    if (in.camera_mode && !EqualsNoCase(*in.camera_mode, "DEFAULT"))
        return Refuse("camera mode " + *in.camera_mode + " (VR or triple screen) is not supported");
    if (in.allow_stretching.value_or(0) != 1 && in.video_width && in.video_height &&
        AspectMismatch(static_cast<double>(*in.video_width), static_cast<double>(*in.video_height), swapWidth,
                       swapHeight))
        return Refuse("video.ini aspect ratio differs from the window; letterboxed output is not supported in v1");
    if (in.dlss5_bridge_loaded) return Refuse("dlss5-bridge.addon64 is loaded");
    if (in.renodx_dlss5_loaded) return Refuse("renodx-dlss5.addon64 is loaded");
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
    const auto video = IniFile::Load(Join(docsAcDir, L"cfg\\video.ini"));

    CompatInputs in;
    in.old_swapchain = ToInt(LayeredGet(tweaksBase, tweaksOver, "COMPATIBILITY", "OLD_SWAPCHAIN"));
    in.exclusive_fullscreen = ToInt(LayeredGet(tweaksBase, tweaksOver, "COMPATIBILITY", "EXCLUSIVE_FULLSCREEN"));
    in.allow_stretching = ToInt(LayeredGet(tweaksBase, tweaksOver, "COMPATIBILITY", "ALLOW_STRETCHING"));
    in.hdr_enabled = ToInt(LayeredGet(tweaksBase, tweaksOver, "HDR", "ENABLED"));
    in.fsr_active = ToInt(LayeredGet(gfxBase, gfxOver, "FSR", "ACTIVE"));
    in.fsr_old_implementation = ToInt(LayeredGet(gfxBase, gfxOver, "FSR", "OLD_IMPLEMENTATION"));
    if (video) {
        in.aasamples = ToInt(video->Get("VIDEO", "AASAMPLES"));
        in.video_width = ToInt(video->Get("VIDEO", "WIDTH"));
        in.video_height = ToInt(video->Get("VIDEO", "HEIGHT"));
        in.camera_mode = video->Get("CAMERA", "MODE");
    }
    in.hags_on = ReadHagsEnabled();
    in.dlss5_bridge_loaded = GetModuleHandleW(L"dlss5-bridge.addon64") != nullptr;
    in.renodx_dlss5_loaded = GetModuleHandleW(L"renodx-dlss5.addon64") != nullptr;
    return in;
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
