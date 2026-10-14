#include "spoof_loader.h"

#include <wincrypt.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "panel_status.h"

namespace acdb {

namespace {

std::string ToUtf8Path(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string LastErrorText(const char* what) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s failed (error %lu)", what, GetLastError());
    return buf;
}

}  // namespace

VersionDllKind IdentifyVersionDll(const std::string& sha256, bool exportsMfgQuery) {
    if (sha256 == kPinnedSpoofSha256) return VersionDllKind::Sm86Pin;
    if (exportsMfgQuery) return VersionDllKind::MfgUnlocker;
    return VersionDllKind::Unknown;
}

SpoofDecision DecideSpoof(const SpoofInputs& in) {
    SpoofDecision d;
    if (in.version_loaded && in.loaded_from_game_dir) {
        d.kind = IdentifyVersionDll(in.loaded_sha256, in.loaded_is_mfg);
        switch (d.kind) {
            case VersionDllKind::Sm86Pin:
                d.action = SpoofAction::AlreadyLoaded;
                d.reason = "version.dll is loaded from the game folder: dlssg_for_sm86 0.3.5 (the pinned SHA-256)";
                break;
            case VersionDllKind::MfgUnlocker:
                d.action = SpoofAction::NotTheSpoof;
                d.reason = std::string("version.dll is loaded from the game folder, and it is MFG-Unlocker (it exports ") +
                           kMfgQueryExport + "), not dlssg_for_sm86";
                break;
            case VersionDllKind::Unknown:
                d.action = SpoofAction::AlreadyLoaded;
                d.reason = "version.dll is loaded from the game folder (SHA-256 " +
                           (in.loaded_sha256.empty() ? std::string("unknown") : in.loaded_sha256) +
                           "); it is not the pinned dlssg_for_sm86 0.3.5 and not MFG-Unlocker, so it is taken for "
                           "another dlssg_for_sm86 build or another mod's version.dll";
                break;
        }
        return d;
    }
    if (!in.game_file_exists) {
        d.action = SpoofAction::NotPresent;
        d.reason = "no version.dll in the game folder";
        return d;
    }
    const std::string bound = in.version_loaded
                                  ? "the process bound VERSION.dll to " + ToUtf8Path(in.loaded_path)
                                  : "no VERSION.dll is loaded yet";
    d.kind = IdentifyVersionDll(in.game_file_sha256, in.game_file_is_mfg);
    if (d.kind == VersionDllKind::MfgUnlocker) {
        d.action = SpoofAction::NotTheSpoof;
        d.reason = bound + "; the game folder's version.dll is MFG-Unlocker (it exports " + kMfgQueryExport +
                   "), not dlssg_for_sm86: it is never loaded as the spoof, also not with spoof_load_any=1. The "
                   "bridge loads only the pinned MFG-Unlocker that install.bat puts into ac-dlssg\\mfg";
        return d;
    }
    if (in.game_file_sha256 == kPinnedSpoofSha256) {
        d.action = SpoofAction::LoadExplicitly;
        d.reason = bound + "; the game folder's version.dll is dlssg_for_sm86 0.3.5, loading it explicitly";
        return d;
    }
    if (in.allow_any) {
        d.action = SpoofAction::LoadExplicitly;
        d.reason = bound + "; the game folder's version.dll (SHA-256 " +
                   (in.game_file_sha256.empty() ? std::string("unknown") : in.game_file_sha256) +
                   ") is not the pinned dlssg_for_sm86 0.3.5, loading it because spoof_load_any=1";
        return d;
    }
    d.action = SpoofAction::Refused;
    d.reason = bound + "; the game folder's version.dll (SHA-256 " +
               (in.game_file_sha256.empty() ? std::string("unknown") : in.game_file_sha256) +
               ") is not the pinned dlssg_for_sm86 0.3.5, so it is not loaded; set spoof_load_any=1 in "
               "ac-dlssg.ini to load it anyway";
    return d;
}

uint32_t SpoofPanelCode(const SpoofDecision& d) {
    if (d.action != SpoofAction::AlreadyLoaded && d.action != SpoofAction::LoadExplicitly) return kPanelSpoofNone;
    switch (d.kind) {
        case VersionDllKind::Sm86Pin: return kPanelSpoofPinned;
        case VersionDllKind::Unknown: return kPanelSpoofUnidentified;
        case VersionDllKind::MfgUnlocker: break;
    }
    return kPanelSpoofNone;
}

bool FileExportsName(const std::wstring& path, const char* name, std::string* error) {
    const HMODULE mapped =
        LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!mapped) {
        if (error) *error = LastErrorText("LoadLibraryExW(LOAD_LIBRARY_AS_IMAGE_RESOURCE)");
        return false;
    }
    // The low bits of a data-file handle are flags; the image starts at the rest.
    const auto* base = reinterpret_cast<const BYTE*>(reinterpret_cast<ULONG_PTR>(mapped) & ~static_cast<ULONG_PTR>(3));
    bool found = false;
    std::string problem;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    DWORD imageSize = 0;
    DWORD exportRva = 0;
    DWORD exportSize = 0;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE) {
        problem = "no PE headers";
    } else if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        const auto& opt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(nt)->OptionalHeader;
        imageSize = opt.SizeOfImage;
        if (opt.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXPORT) {
            exportRva = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
            exportSize = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
        }
    } else if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        const auto& opt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(nt)->OptionalHeader;
        imageSize = opt.SizeOfImage;
        if (opt.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXPORT) {
            exportRva = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
            exportSize = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
        }
    } else {
        problem = "an unknown optional header";
    }
    const auto inside = [&](DWORD rva, DWORD bytes) {
        return rva < imageSize && bytes <= imageSize - rva;
    };
    if (problem.empty() && exportRva != 0 && exportSize != 0) {
        if (!inside(exportRva, sizeof(IMAGE_EXPORT_DIRECTORY))) {
            problem = "the export directory lies outside the image";
        } else {
            const auto* dir = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + exportRva);
            if (dir->NumberOfNames > 65536 || !inside(dir->AddressOfNames, dir->NumberOfNames * 4)) {
                problem = "the export name table lies outside the image";
            } else {
                const auto* names = reinterpret_cast<const DWORD*>(base + dir->AddressOfNames);
                const size_t wanted = std::strlen(name) + 1;
                for (DWORD i = 0; i < dir->NumberOfNames && !found; ++i) {
                    if (!inside(names[i], 1)) continue;
                    const size_t room = imageSize - names[i];
                    if (room >= wanted && std::memcmp(base + names[i], name, wanted) == 0) found = true;
                }
            }
        }
    }
    FreeLibrary(mapped);
    if (!problem.empty()) {
        if (error) *error = problem;
        return false;
    }
    return found;
}

bool Sha256File(const std::wstring& path, std::string* hex, std::string* error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = LastErrorText("CreateFileW");
        return false;
    }
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    bool ok = CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
              CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash);
    if (!ok && error) *error = LastErrorText("CryptoAPI SHA-256");
    std::vector<BYTE> buf(1 << 20);
    while (ok) {
        DWORD got = 0;
        if (!ReadFile(file, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
            ok = false;
            if (error) *error = LastErrorText("ReadFile");
            break;
        }
        if (got == 0) break;
        if (!CryptHashData(hash, buf.data(), got, 0)) {
            ok = false;
            if (error) *error = LastErrorText("CryptHashData");
        }
    }
    if (ok) {
        BYTE digest[32];
        DWORD len = sizeof(digest);
        ok = CryptGetHashParam(hash, HP_HASHVAL, digest, &len, 0) && len == sizeof(digest);
        if (!ok && error) *error = LastErrorText("CryptGetHashParam");
        if (ok && hex) {
            static const char kHex[] = "0123456789abcdef";
            hex->assign(64, '0');
            for (int i = 0; i < 32; ++i) {
                (*hex)[2 * i] = kHex[digest[i] >> 4];
                (*hex)[2 * i + 1] = kHex[digest[i] & 15];
            }
        }
    }
    if (hash) CryptDestroyHash(hash);
    if (prov) CryptReleaseContext(prov, 0);
    CloseHandle(file);
    return ok;
}

HMODULE LoadSpoofModule(const std::wstring& path, std::string* error) {
    const HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m && error) *error = LastErrorText("LoadLibraryExW");
    return m;
}

}  // namespace acdb
