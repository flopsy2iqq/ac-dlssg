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
}
