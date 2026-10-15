#include <windows.h>

#include <string>

#include "spoof_loader.h"
#include "test_framework.h"

using namespace acdb;

namespace {

std::wstring TempDir(const wchar_t* leaf) {
    wchar_t buf[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, buf);
    std::wstring dir = std::wstring(buf) + leaf;
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool WriteBytes(const std::wstring& path, const char* data, DWORD size) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(f, data, size, &written, nullptr) && written == size;
    CloseHandle(f);
    return ok;
}

SpoofInputs System32Bound() {
    SpoofInputs in;
    in.version_loaded = true;
    in.loaded_from_game_dir = false;
    in.loaded_path = L"C:\\WINDOWS\\SYSTEM32\\VERSION.dll";
    return in;
}

}  // namespace

TEST(Spoof_Sha256OfAKnownFile) {
    const std::wstring path = TempDir(L"acdb_spoof_sha") + L"\\abc.bin";
    REQUIRE(WriteBytes(path, "abc", 3));
    std::string hex, err;
    REQUIRE(Sha256File(path, &hex, &err));
    CHECK_EQ(hex, std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK(!Sha256File(path + L".missing", &hex, &err));
    CHECK(!err.empty());
    DeleteFileW(path.c_str());
    RemoveDirectoryW(TempDir(L"acdb_spoof_sha").c_str());
}

TEST(Spoof_AlreadyLoadedFromTheGameFolder) {
    SpoofInputs in;
    in.version_loaded = true;
    in.loaded_from_game_dir = true;
    in.game_file_exists = true;
    CHECK(DecideSpoof(in).action == SpoofAction::AlreadyLoaded);
}

TEST(Spoof_NotPresentWhenTheGameFolderHasNoVersionDll) {
    const SpoofInputs in = System32Bound();
    const SpoofDecision d = DecideSpoof(in);
    CHECK(d.action == SpoofAction::NotPresent);
}

// Windows 11 25H2: a system DLL that acs.exe imports before VERSION.dll pulls
// System32's VERSION.dll in first, so the spoof next to acs.exe never loads.
TEST(Spoof_PinnedFileIsLoadedWhenTheOsBoundSystem32) {
    SpoofInputs in = System32Bound();
    in.game_file_exists = true;
    in.game_file_sha256 = kPinnedSpoofSha256;
    const SpoofDecision d = DecideSpoof(in);
    CHECK(d.action == SpoofAction::LoadExplicitly);
    CHECK(d.reason.find("SYSTEM32") != std::string::npos);

    in.version_loaded = false;  // nothing named VERSION.dll loaded yet
    CHECK(DecideSpoof(in).action == SpoofAction::LoadExplicitly);
}

TEST(Spoof_UnknownFileIsRefusedUnlessAllowed) {
    SpoofInputs in = System32Bound();
    in.game_file_exists = true;
    in.game_file_sha256 = "0000000000000000000000000000000000000000000000000000000000000000";
    SpoofDecision d = DecideSpoof(in);
    CHECK(d.action == SpoofAction::Refused);
    CHECK(d.reason.find("spoof_load_any") != std::string::npos);
    CHECK(d.reason.find("00000000") != std::string::npos);

    in.allow_any = true;
    d = DecideSpoof(in);
    CHECK(d.action == SpoofAction::LoadExplicitly);

    in.allow_any = false;
    in.game_file_sha256.clear();  // the hash could not be computed
    CHECK(DecideSpoof(in).action == SpoofAction::Refused);
}

// A DLL named version.dll is loaded by full path next to System32's
// VERSION.dll, which the test executable imports statically.
TEST(Spoof_LoadsASecondVersionDllByFullPath) {
    const HMODULE system = GetModuleHandleW(L"version.dll");
    REQUIRE(system != nullptr);
    const std::wstring dir = TempDir(L"acdb_spoof_load");
    const std::wstring path = dir + L"\\version.dll";
    REQUIRE(CopyFileW(L"" ACDB_SPOOF_FIXTURE_PATH, path.c_str(), FALSE));
    SetEnvironmentVariableW(L"ACDB_SPOOF_FIXTURE_LOADED", nullptr);

    std::string err;
    const HMODULE loaded = LoadSpoofModule(path, &err);
    REQUIRE(loaded != nullptr);
    CHECK(loaded != system);
    wchar_t name[MAX_PATH] = {};
    GetModuleFileNameW(loaded, name, MAX_PATH);
    CHECK(_wcsicmp(name, path.c_str()) == 0);
    wchar_t mark[4] = {};
    CHECK(GetEnvironmentVariableW(L"ACDB_SPOOF_FIXTURE_LOADED", mark, 4) == 1);
    CHECK(GetModuleHandleW(L"version.dll") == system);  // the process's binding is unchanged

    CHECK(LoadSpoofModule(dir + L"\\missing.dll", &err) == nullptr);
    CHECK(!err.empty());
    FreeLibrary(loaded);
    DeleteFileW(path.c_str());
    RemoveDirectoryW(dir.c_str());
}

// ---------------------------------------------------------------------------
// What a game-folder version.dll is: the pinned dlssg_for_sm86 by its hash, a
// copy of MFG-Unlocker by the export every build of it has, else unknown.

namespace {

const std::string kOtherSha = "1111111111111111111111111111111111111111111111111111111111111111";

SpoofInputs LoadedFromGameDir(const std::string& sha, bool mfg) {
    SpoofInputs in;
    in.version_loaded = true;
    in.loaded_from_game_dir = true;
    in.loaded_path = L"D:\\Games\\assettocorsa\\version.dll";
    in.loaded_sha256 = sha;
    in.loaded_is_mfg = mfg;
    in.game_file_exists = true;
    return in;
}

}  // namespace

TEST(Spoof_VersionDllIsIdentifiedByHashThenByExport) {
    CHECK(IdentifyVersionDll(kPinnedSpoofSha256, false) == VersionDllKind::Sm86Pin);
    CHECK(IdentifyVersionDll(kOtherSha, true) == VersionDllKind::MfgUnlocker);
    CHECK(IdentifyVersionDll(kOtherSha, false) == VersionDllKind::Unknown);
    CHECK(IdentifyVersionDll("", false) == VersionDllKind::Unknown);
    CHECK(IdentifyVersionDll("", true) == VersionDllKind::MfgUnlocker);
}

// Windows 10 binds acs.exe's VERSION.dll to whatever version.dll is next to
// it. A user's own MFG-Unlocker there is not dlssg_for_sm86 (issue 2).
TEST(Spoof_ALoadedMfgUnlockerCopyIsNotTakenForTheSpoof) {
    const SpoofDecision d = DecideSpoof(LoadedFromGameDir(kOtherSha, true));
    CHECK(d.action == SpoofAction::NotTheSpoof);
    CHECK(d.kind == VersionDllKind::MfgUnlocker);
    CHECK(d.reason.find("MFG-Unlocker") != std::string::npos);
    CHECK(d.reason.find("not dlssg_for_sm86") != std::string::npos);
    CHECK(SpoofPanelCode(d) == 0u);
}

TEST(Spoof_TheLoadedPinnedSpoofIsNamedWithItsVersion) {
    const SpoofDecision d = DecideSpoof(LoadedFromGameDir(kPinnedSpoofSha256, false));
    CHECK(d.action == SpoofAction::AlreadyLoaded);
    CHECK(d.kind == VersionDllKind::Sm86Pin);
    CHECK(d.reason.find("dlssg_for_sm86 0.3.5") != std::string::npos);
    CHECK(SpoofPanelCode(d) == 1u);
}

// Another dlssg_for_sm86 build, or another mod: loaded by the process, so it
// counts as the spoof for the rest of the bridge, but it is not named as one.
TEST(Spoof_ALoadedUnknownVersionDllIsLabelledUnknown) {
    const SpoofDecision d = DecideSpoof(LoadedFromGameDir(kOtherSha, false));
    CHECK(d.action == SpoofAction::AlreadyLoaded);
    CHECK(d.kind == VersionDllKind::Unknown);
    CHECK(d.reason.find(kOtherSha) != std::string::npos);
    CHECK(d.reason.find("not the pinned dlssg_for_sm86 0.3.5") != std::string::npos);
    CHECK(d.reason.find("(dlssg_for_sm86)") == std::string::npos);
    CHECK(SpoofPanelCode(d) == 2u);
}

// spoof_load_any=1 loads any other version.dll, never MFG-Unlocker: the
// bridge loads only the pinned copy in ac-dlssg\mfg.
TEST(Spoof_MfgUnlockerInTheGameFolderIsNeverLoadedAsTheSpoof) {
    SpoofInputs in = System32Bound();
    in.game_file_exists = true;
    in.game_file_sha256 = kOtherSha;
    in.game_file_is_mfg = true;
    in.allow_any = true;
    const SpoofDecision d = DecideSpoof(in);
    CHECK(d.action == SpoofAction::NotTheSpoof);
    CHECK(d.kind == VersionDllKind::MfgUnlocker);
    CHECK(d.reason.find("spoof_load_any") != std::string::npos);
    CHECK(d.reason.find("ac-dlssg\\mfg") != std::string::npos);
    CHECK(SpoofPanelCode(d) == 0u);

    in.allow_any = false;
    CHECK(DecideSpoof(in).action == SpoofAction::NotTheSpoof);
}

TEST(Spoof_AnUnknownFileLoadedWithSpoofLoadAnyIsLabelledUnknown) {
    SpoofInputs in = System32Bound();
    in.game_file_exists = true;
    in.game_file_sha256 = kOtherSha;
    in.allow_any = true;
    const SpoofDecision d = DecideSpoof(in);
    CHECK(d.action == SpoofAction::LoadExplicitly);
    CHECK(d.kind == VersionDllKind::Unknown);
    CHECK(SpoofPanelCode(d) == 2u);
    in.game_file_sha256 = kPinnedSpoofSha256;
    CHECK(SpoofPanelCode(DecideSpoof(in)) == 1u);
}

// The export probe maps the file as data: its DllMain never runs.
TEST(Spoof_TheExportProbeReadsAFileWithoutRunningIt) {
    SetEnvironmentVariableW(L"ACDB_MFG_FIXTURE_LOADED", nullptr);
    std::string err;
    CHECK(FileExportsName(L"" ACDB_MFG_FIXTURE_PATH, "MfgUnlockSingleModuleQuery", &err));
    CHECK(!FileExportsName(L"" ACDB_SPOOF_FIXTURE_PATH, "MfgUnlockSingleModuleQuery", &err));
    CHECK(FileExportsName(L"" ACDB_SPOOF_FIXTURE_PATH, "AcdbSpoofFixture", &err));
    wchar_t mark[4] = {};
    CHECK(GetEnvironmentVariableW(L"ACDB_MFG_FIXTURE_LOADED", mark, 4) == 0);

    err.clear();
    CHECK(!FileExportsName(TempDir(L"acdb_spoof_probe") + L"\\missing.dll", "MfgUnlockSingleModuleQuery", &err));
    CHECK(!err.empty());
    const std::wstring text = TempDir(L"acdb_spoof_probe") + L"\\not-a-dll.dll";
    REQUIRE(WriteBytes(text, "not a PE file", 13));
    err.clear();
    CHECK(!FileExportsName(text, "MfgUnlockSingleModuleQuery", &err));
    CHECK(!err.empty());
    DeleteFileW(text.c_str());
    RemoveDirectoryW(TempDir(L"acdb_spoof_probe").c_str());
}
