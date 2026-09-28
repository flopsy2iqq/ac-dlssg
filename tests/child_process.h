#pragma once
// Tests of process-wide state (StreamlineRuntime initialises once per process,
// and Streamline itself allows one lifetime) run in a fresh copy of the test
// exe. A test named Child_<something> is skipped in a normal run; RunChildTest
// starts this exe with only that test enabled and returns its exit code (the
// number of failed tests, so 0 means it passed; a child that finds no test of
// that name exits non-zero). The child's output is echoed.
#include <windows.h>

#include <cstdio>
#include <string>

namespace acdb_test {

constexpr char kChildTestPrefix[] = "Child_";
constexpr wchar_t kChildTestEnv[] = L"ACDB_TEST_CHILD";

// True in any process RunChildTest started.
inline bool IsChildProcess() { return GetEnvironmentVariableW(kChildTestEnv, nullptr, 0) > 0; }

// True in the child process started for exactly this test.
inline bool IsSelectedChildTest(const char* name) {
    wchar_t selected[256] = {};
    const DWORD n = GetEnvironmentVariableW(kChildTestEnv, selected, 256);
    if (n == 0 || n >= 256) return false;
    std::wstring wide;
    for (const char* p = name; *p; ++p) wide.push_back(static_cast<wchar_t>(*p));
    return wide == selected;
}

// Returns the child's exit code, or -1 when it could not run or timed out.
inline int RunChildTest(const char* name, DWORD timeoutMs = 60000) {
    wchar_t exe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) return -1;
    std::wstring wname;
    for (const char* p = name; *p; ++p) wname.push_back(static_cast<wchar_t>(*p));
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" " + wname;

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return -1;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    // The child inherits the environment, so it runs only the selected test.
    SetEnvironmentVariableW(kChildTestEnv, wname.c_str());
    const BOOL started =
        CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    SetEnvironmentVariableW(kChildTestEnv, nullptr);
    CloseHandle(writeEnd);
    if (!started) {
        CloseHandle(readEnd);
        return -1;
    }

    // Echo the child's output while it runs; polled, so a hung child cannot
    // block the parent past the timeout.
    std::string line;
    auto echo = [&line](const char* data, DWORD size) {
        for (DWORD i = 0; i < size; ++i) {
            if (data[i] == '\r') continue;
            if (data[i] == '\n') {
                std::printf("  | %s\n", line.c_str());
                line.clear();
            } else {
                line.push_back(data[i]);
            }
        }
    };
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    bool exited = false;
    for (;;) {
        DWORD avail = 0;
        char buf[512];
        DWORD got = 0;
        if (PeekNamedPipe(readEnd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            if (ReadFile(readEnd, buf, sizeof(buf), &got, nullptr) && got > 0) echo(buf, got);
            continue;
        }
        if (exited) break;  // drained after exit
        exited = WaitForSingleObject(pi.hProcess, 10) == WAIT_OBJECT_0;
        if (!exited && GetTickCount64() > deadline) break;
    }
    if (!line.empty()) std::printf("  | %s\n", line.c_str());
    CloseHandle(readEnd);

    DWORD code = static_cast<DWORD>(-1);
    if (!exited) {
        std::printf("  child %s timed out after %lu ms\n", name, static_cast<unsigned long>(timeoutMs));
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    } else {
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

}  // namespace acdb_test
