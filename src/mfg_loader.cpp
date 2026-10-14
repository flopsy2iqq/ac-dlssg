#include "mfg_loader.h"

#include <tlhelp32.h>

#include <cstdio>
#include <vector>

#include "spoof_loader.h"

namespace acdb {

namespace {

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring ModulePath(HMODULE module) {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        if (buf.size() >= 32768) return {};
        buf.resize(buf.size() * 2);
    }
}

}  // namespace

std::wstring MfgPath(const std::wstring& dataDir) { return dataDir + L"\\mfg\\version.dll"; }

PredictedAdapter PredictRenderAdapter(const std::vector<AdapterId>& adapters) {
    constexpr uint32_t kNvidia = 0x10DE;
    const AdapterId* first = nullptr;
    const AdapterId* onlyNvidia = nullptr;
    int nvidia = 0;
    for (const AdapterId& a : adapters) {
        if (a.software) continue;
        if (!first) first = &a;
        if (a.vendor == kNvidia) {
            ++nvidia;
            onlyNvidia = &a;
        }
    }
    PredictedAdapter p;
    const AdapterId* pick = first;
    if (first && first->vendor != kNvidia && nvidia == 1) pick = onlyNvidia;
    if (!pick) return p;
    p.known = true;
    p.vendor = pick->vendor;
    p.device = pick->device;
    p.name = pick->name;
    return p;
}

MfgDecision DecideMfg(const MfgInputs& in) {
    MfgDecision d;
    const std::string pinned = std::string("MFG-Unlocker ") + kMfgPin.version;
    if (in.other_copy_loaded) {
        d.action = MfgAction::AlreadyLoaded;
        d.reason = "MFG-Unlocker is already loaded from " + ToUtf8(in.other_copy_path) +
                   " (a copy installed outside ac-dlssg); the one in ac-dlssg\\mfg is not loaded next to it";
        return d;
    }
    if (!in.file_exists) {
        d.action = MfgAction::NotInstalled;
        d.reason = "not installed (no ac-dlssg\\mfg\\version.dll)";
        return d;
    }
    d.action = MfgAction::Skip;
    if (!in.enabled) {
        d.reason = pinned + " is installed but not loaded: the bridge is disabled in ac-dlssg.ini";
        return d;
    }
    if (!in.configured) {
        d.reason = pinned + " is installed but not loaded: mfg_unlocker=0 in ac-dlssg.ini";
        return d;
    }
    if (in.spoof_loaded && in.spoof_unidentified) {
        d.reason = pinned + " is installed but not loaded: an unidentified version.dll from the game folder is "
                            "loaded (not the pinned dlssg_for_sm86 and not MFG-Unlocker; it may be another "
                            "dlssg_for_sm86 build), and MFG-Unlocker is never loaded next to a spoof; remove that "
                            "version.dll to use MFG-Unlocker";
        return d;
    }
    if (in.spoof_loaded) {
        d.reason = pinned + " is installed but not loaded: dlssg_for_sm86 (version.dll in the game folder) is "
                            "loaded, and the two are never loaded together";
        return d;
    }
    if (!in.render_adapter_known) {
        d.reason = pinned + " is installed but not loaded: the render adapter is unknown (" +
                   ArchName(in.render_arch) + ")";
        return d;
    }
    if (in.render_arch != GpuArch::Ada) {
        d.reason = pinned + " is installed but not loaded: the render adapter " + in.render_adapter + " is " +
                   ArchName(in.render_arch) + ", not an RTX 40 (Ada)";
        return d;
    }
    if (in.file_sha256 != kMfgPin.dllSha256) {
        d.warn = true;
        d.reason = "ac-dlssg\\mfg\\version.dll (SHA-256 " +
                   (in.file_sha256.empty() ? std::string("unknown") : in.file_sha256) + ") is not the pinned " +
                   pinned + ", so it is not loaded; run install.bat again to get the pinned copy";
        return d;
    }
    d.action = MfgAction::Load;
    d.reason = "loading the pinned " + pinned + " for the RTX 40 " + in.render_adapter +
               " (unofficial: it lifts NVIDIA's RTX 50-only limit on 3X/4X)";
    return d;
}

HMODULE LoadMfgModule(const std::wstring& path, double* ms, std::string* error) {
    LARGE_INTEGER freq{}, start{}, end{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    const HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    const DWORD err = GetLastError();
    QueryPerformanceCounter(&end);
    if (ms) *ms = freq.QuadPart > 0 ? 1000.0 * static_cast<double>(end.QuadPart - start.QuadPart) / freq.QuadPart : 0.0;
    if (!m && error) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "LoadLibraryExW failed (error %lu)", err);
        *error = buf;
    }
    return m;
}

std::string MfgLoadedLine(const std::string& path, double ms, const std::string& fileVersion, bool exportsQuery) {
    char time[32];
    std::snprintf(time, sizeof(time), "%.0f", ms < 0 ? 0.0 : ms);
    std::string line = "loaded " + path + " (file version " + (fileVersion.empty() ? std::string("unknown") : fileVersion) +
                       ") in " + time + " ms";
    if (!exportsQuery)
        line += std::string("; it does not export ") + kMfgQueryExport + ", so it may not be MFG-Unlocker";
    return line;
}

std::string MfgRenderAdapterNote(bool mfgLoaded, bool renderIsAda, const std::string& renderAdapter) {
    if (!mfgLoaded || renderIsAda) return {};
    return "MFG-Unlocker is loaded, but the game renders on " + renderAdapter +
           ", not an RTX 40; it unlocks nothing there (set a GPU preference for acs.exe in Windows' graphics settings "
           "or the NVIDIA app)";
}

bool FindLoadedMfgCopy(std::wstring* path) {
    HANDLE snap = INVALID_HANDLE_VALUE;
    // ERROR_BAD_LENGTH: the module list changed while it was read; retry.
    for (int attempt = 0; attempt < 5 && snap == INVALID_HANDLE_VALUE; ++attempt) {
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
        if (snap == INVALID_HANDLE_VALUE && GetLastError() != ERROR_BAD_LENGTH) break;
    }
    if (snap == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Module32FirstW(snap, &entry); ok && !found; ok = Module32NextW(snap, &entry)) {
        if (!GetProcAddress(entry.hModule, kMfgQueryExport)) continue;
        found = true;
        if (path) {
            *path = ModulePath(entry.hModule);
            if (path->empty()) *path = entry.szExePath;
        }
    }
    CloseHandle(snap);
    return found;
}

}  // namespace acdb
