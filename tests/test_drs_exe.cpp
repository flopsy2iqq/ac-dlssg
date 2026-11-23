// The built ac-dlssg-drs.exe as the installer runs it: its exit codes, its
// console text, --json and --log, and its default acs.exe. READ ONLY: no
// test here writes the driver settings (only status runs against the
// driver; restore and the bad command lines stop before NVAPI is touched).
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "drs_json.h"
#include "drs_tool.h"
#include "temp_dir.h"
#include "test_framework.h"

using namespace acdb;
using acdb_test::ReadAll;
using acdb_test::TempDir;

namespace {

struct Ran {
    int code = -1;
    std::string out;  // stdout and stderr, as written
};

// Runs exe with the arguments (already quoted where needed) and waits up to 30 s.
Ran RunExe(const std::wstring& exe, const std::wstring& args) {
    Ran r;
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return r;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    PROCESS_INFORMATION pi{};
    const BOOL started =
        CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writeEnd);
    if (!started) {
        CloseHandle(readEnd);
        return r;
    }
    char buf[512];
    DWORD got = 0;
    while (ReadFile(readEnd, buf, sizeof(buf), &got, nullptr) && got > 0) r.out.append(buf, got);
    CloseHandle(readEnd);
    if (WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        r.code = static_cast<int>(code);
    } else {
        TerminateProcess(pi.hProcess, 1);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::printf("  exit %d:\n%s", r.code, r.out.c_str());
    return r;
}

const std::wstring kExe = ACDB_DRS_EXE_PATH;

bool SystemHasNvapi() {
    wchar_t dir[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    const DWORD attrs = GetFileAttributesW((std::wstring(dir) + L"\\nvapi64.dll").c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::string Member(const std::string& json, const char* key) {
    JsonValue v;
    std::string error;
    if (!ParseJson(json, &v, &error)) return "<not JSON: " + error + ">";
    const JsonValue* m = v.Find(key);
    if (!m) return "<none>";
    return m->type == JsonValue::Type::String ? m->text : "<" + std::string(m->type == JsonValue::Type::Null ? "null" : "other") + ">";
}

}  // namespace

TEST(DrsExe_ABadCommandLineExitsOneWithTheUsage) {
    const Ran r = RunExe(kExe, L"smooth-motion on");
    CHECK_EQ(r.code, kDrsExitUsage);
    CHECK(r.out.find("usage:") != std::string::npos);
    CHECK(r.out.find("smooth-motion restore --json <record>") != std::string::npos);
    CHECK(r.out.find("\r\n") != std::string::npos);
}

// A record that is not there: nothing is read or changed, exit 7.
TEST(DrsExe_RestoreWithoutItsRecordChangesNothing) {
    TempDir dir(L"drs_exe");
    const Ran r = RunExe(kExe, L"smooth-motion restore --json \"" + dir.Str() + L"\\missing.json\"");
    CHECK_EQ(r.code, kDrsExitFile);
    CHECK(r.out.find("could not be read") != std::string::npos);
}

// status against this machine's driver (read only): the JSON result and
// the log, which holds what the console got.
TEST(DrsExe_StatusWritesItsResultAndItsLog) {
    TempDir dir(L"drs_exe");
    const std::wstring json = dir.Str() + L"\\st\xE4tus.json";
    const std::wstring log = dir.Str() + L"\\l\xE4og.txt";
    const Ran r = RunExe(kExe, L"smooth-motion status --exe \"C:\\Games\\assettocorsa\\acs.exe\" --json \"" + json +
                                   L"\" --log \"" + log + L"\"");
    if (!SystemHasNvapi()) {
        CHECK_EQ(r.code, kDrsExitNoDriver);
        CHECK_EQ(Member(ReadAll(json), "result"), std::string("no-driver"));
        return;
    }
    CHECK(r.code == kDrsExitOk || r.code == kDrsExitNoProfile);
    const std::string text = ReadAll(json);
    CHECK_EQ(Member(text, "command"), std::string("status"));
    CHECK_EQ(Member(text, "exe"), std::string("C:\\Games\\assettocorsa\\acs.exe"));
    CHECK_EQ(Member(text, "result"), std::string(r.code == kDrsExitOk ? "read" : "no-profile"));
    CHECK_EQ(ReadAll(log), r.out);
}

// The bridge never changes driver settings (driver_profile.h): the
// nvapi_QueryInterface ids of NvAPI_DRS_SetSetting, DeleteProfileSetting
// and SaveSettings are in the helper, never in ac-dlssg.dll.
TEST(DrsExe_OnlyTheHelperHoldsTheNvapiWriteIds) {
    const unsigned ids[] = {0x577dd202u, 0xe4a26362u, 0xfcbc7e14u};
    const std::string dll = ReadAll(std::filesystem::path(ACDB_DLL_PATH));
    const std::string exe = ReadAll(std::filesystem::path(kExe));
    REQUIRE(!dll.empty() && !exe.empty());
    for (const unsigned id : ids) {
        const char bytes[4] = {static_cast<char>(id & 0xFF), static_cast<char>((id >> 8) & 0xFF),
                               static_cast<char>((id >> 16) & 0xFF), static_cast<char>((id >> 24) & 0xFF)};
        const std::string le(bytes, 4);
        CHECK(exe.find(le) != std::string::npos);
        CHECK(dll.find(le) == std::string::npos);
    }
}

// Installed as <game>\ac-dlssg\ac-dlssg-drs.exe, the helper takes
// <game>\acs.exe without --exe; without an acs.exe there, status needs it.
TEST(DrsExe_TheDefaultAcsExeIsTheOneAboveItsFolder) {
    TempDir dir(L"drs_exe");
    const std::filesystem::path game = dir.Path() / L"game";
    std::filesystem::create_directories(game / L"ac-dlssg");
    const std::filesystem::path copy = game / L"ac-dlssg" / L"ac-dlssg-drs.exe";
    std::error_code ec;
    std::filesystem::copy_file(kExe, copy, ec);
    REQUIRE(!ec);
    Ran r = RunExe(copy.wstring(), L"smooth-motion status");
    CHECK_EQ(r.code, kDrsExitUsage);
    CHECK(r.out.find("--exe") != std::string::npos);
    dir.Write(L"game\\acs.exe", "");
    const std::wstring json = dir.Str() + L"\\s.json";
    r = RunExe(copy.wstring(), L"smooth-motion status --json \"" + json + L"\"");
    CHECK(r.code != kDrsExitUsage);
    CHECK_EQ(Member(ReadAll(json), "exe"), (game / L"acs.exe").string());
}
