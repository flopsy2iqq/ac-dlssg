#include "spoof_loader.h"

#include <wincrypt.h>

#include <cstdio>
#include <vector>

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

SpoofDecision DecideSpoof(const SpoofInputs& in) {
    SpoofDecision d;
    if (in.version_loaded && in.loaded_from_game_dir) {
        d.action = SpoofAction::AlreadyLoaded;
        d.reason = "version.dll is loaded from the game folder (dlssg_for_sm86)";
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
