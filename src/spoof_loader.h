#pragma once
// dlssg_for_sm86 (spec 10) is a version.dll proxy next to acs.exe. On
// Windows 10 the loader binds acs.exe's VERSION.dll import to it. On Windows
// 11 25H2 a system DLL that acs.exe imports earlier (setupapi and friends,
// which resolve their own imports from System32 only) loads System32's
// VERSION.dll first, and the spoof never runs. The bootstrap then loads the
// spoof by full path, before slInit, when it is the pinned release.
#include <windows.h>

#include <string>

namespace acdb {

// SHA-256 of version.dll from sdli1995/dlssg_for_sm86 commit 9621db57 (tag
// 0.3.5), the release the installer pins.
constexpr char kPinnedSpoofSha256[] = "c3934a09399f022504227c72df0bf8c0de55f9a08880dddde898c5262cefa838";

struct SpoofInputs {
    bool version_loaded = false;        // a module named VERSION.dll is loaded
    bool loaded_from_game_dir = false;  // ... and it is <game>\version.dll
    std::wstring loaded_path;           // its full path, for the log
    bool game_file_exists = false;      // <game>\version.dll exists
    std::string game_file_sha256;       // lower-case hex; empty when unknown
    bool allow_any = false;             // ac-dlssg.ini spoof_load_any=1
};

enum class SpoofAction { AlreadyLoaded, NotPresent, LoadExplicitly, Refused };

struct SpoofDecision {
    SpoofAction action = SpoofAction::NotPresent;
    std::string reason;  // for the log
};

// Pure.
SpoofDecision DecideSpoof(const SpoofInputs& in);

// Lower-case hex SHA-256 of a file (CryptoAPI in ADVAPI32).
bool Sha256File(const std::wstring& path, std::string* hex, std::string* error);

// LoadLibraryExW(path, LOAD_WITH_ALTERED_SEARCH_PATH). Not from DllMain.
HMODULE LoadSpoofModule(const std::wstring& path, std::string* error);

}  // namespace acdb
