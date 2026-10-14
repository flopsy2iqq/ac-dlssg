#pragma once
// dlssg_for_sm86 (spec 10) is a version.dll proxy next to acs.exe. On
// Windows 10 the loader binds acs.exe's VERSION.dll import to it. On Windows
// 11 25H2 a system DLL that acs.exe imports earlier (setupapi and friends,
// which resolve their own imports from System32 only) loads System32's
// VERSION.dll first, and the spoof never runs. The bootstrap then loads the
// spoof by full path, before slInit, when it is the pinned release.
//
// Other mods use the name version.dll too, MFG-Unlocker among them (issue
// 2), so a game-folder version.dll is identified before it is called the
// spoof: by the pinned hash, by MFG-Unlocker's export, or as unknown.
#include <windows.h>

#include <cstdint>
#include <string>

namespace acdb {

// SHA-256 of version.dll from sdli1995/dlssg_for_sm86 commit 9621db57 (tag
// 0.3.5), the release the installer pins.
constexpr char kPinnedSpoofSha256[] = "c3934a09399f022504227c72df0bf8c0de55f9a08880dddde898c5262cefa838";

// The export every MFG-Unlocker build has (github.com/dashdogy/MFG-Unlocker;
// it refuses to chain a module with it, so as not to load itself twice).
constexpr char kMfgQueryExport[] = "MfgUnlockSingleModuleQuery";

// What a game-folder version.dll is.
enum class VersionDllKind { Sm86Pin, MfgUnlocker, Unknown };

// Pure: the pinned dlssg_for_sm86 by its SHA-256 (lower-case hex), else
// MFG-Unlocker when the file exports kMfgQueryExport, else unknown.
VersionDllKind IdentifyVersionDll(const std::string& sha256, bool exportsMfgQuery);

struct SpoofInputs {
    bool version_loaded = false;        // a module named VERSION.dll is loaded
    bool loaded_from_game_dir = false;  // ... and it is <game>\version.dll
    std::wstring loaded_path;           // its full path, for the log
    std::string loaded_sha256;          // ... the SHA-256 of that file; empty when unknown
    bool loaded_is_mfg = false;         // ... and it exports kMfgQueryExport
    bool game_file_exists = false;      // <game>\version.dll exists
    std::string game_file_sha256;       // lower-case hex; empty when unknown
    bool game_file_is_mfg = false;      // the file exports kMfgQueryExport (FileExportsName)
    bool allow_any = false;             // ac-dlssg.ini spoof_load_any=1
};

// NotTheSpoof: the game folder's version.dll is MFG-Unlocker, loaded by the
// process or not; it is never loaded or counted as the spoof.
enum class SpoofAction { AlreadyLoaded, NotPresent, LoadExplicitly, Refused, NotTheSpoof };

struct SpoofDecision {
    SpoofAction action = SpoofAction::NotPresent;
    VersionDllKind kind = VersionDllKind::Unknown;  // of the game folder's version.dll
    std::string reason;  // for the log
};

// Pure. A game-folder version.dll the process loaded is AlreadyLoaded when
// it is the pinned spoof or unknown (another dlssg_for_sm86 build or another
// mod, named as unknown), NotTheSpoof when it is MFG-Unlocker. One it did
// not load: LoadExplicitly when it is the pinned spoof, or unknown with
// spoof_load_any=1; NotTheSpoof when it is MFG-Unlocker, also with
// spoof_load_any=1; Refused otherwise.
SpoofDecision DecideSpoof(const SpoofInputs& in);

// The panel's StatusLayout::spoofLoaded (PanelSpoofCode) for a decision
// whose version.dll is loaded: kPanelSpoofPinned for the pinned spoof,
// kPanelSpoofUnidentified for an unknown one; kPanelSpoofNone for
// MFG-Unlocker and for every action that loads nothing.
uint32_t SpoofPanelCode(const SpoofDecision& d);

// True when the PE file exports a function of that name. The file is mapped
// as an image resource (LOAD_LIBRARY_AS_DATAFILE |
// LOAD_LIBRARY_AS_IMAGE_RESOURCE): none of its code runs. False with *error
// when it cannot be mapped or its export table is broken.
bool FileExportsName(const std::wstring& path, const char* name, std::string* error);

// Lower-case hex SHA-256 of a file (CryptoAPI in ADVAPI32).
bool Sha256File(const std::wstring& path, std::string* hex, std::string* error);

// LoadLibraryExW(path, LOAD_WITH_ALTERED_SEARCH_PATH). Not from DllMain.
HMODULE LoadSpoofModule(const std::wstring& path, std::string* error);

}  // namespace acdb
