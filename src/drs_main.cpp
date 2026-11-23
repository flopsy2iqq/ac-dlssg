// ac-dlssg-drs.exe, the installer's driver-profile helper (drs_tool.h): the
// console, the files and NVAPI around DrsMain. It imports only KERNEL32
// (tools\check-imports.ps1 -Helper), and loads nvapi64.dll by its System32
// path.
#include <windows.h>

#include <string>
#include <vector>

#include "drs_store.h"
#include "drs_tool.h"

namespace {

constexpr DWORD kMaxFileBytes = 1 << 20;  // a record is a few hundred bytes

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Win32Error(DWORD code) { return "Windows error " + std::to_string(code); }

// UTF-16 into a console, UTF-8 into a pipe or a file.
void Print(const std::string& line) {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    DWORD written = 0;
    if (GetConsoleMode(out, &mode)) {
        const std::wstring w = Wide(line) + L"\r\n";
        WriteConsoleW(out, w.data(), static_cast<DWORD>(w.size()), &written, nullptr);
    } else {
        const std::string bytes = line + "\r\n";
        WriteFile(out, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    }
}

bool ReadUtf8(const std::wstring& path, std::string* text, std::string* error) {
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        *error = Win32Error(GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) != FALSE;
    if (!ok) {
        *error = Win32Error(GetLastError());
    } else if (size.QuadPart > kMaxFileBytes) {
        *error = "the file is larger than 1 MiB";
        ok = false;
    } else {
        text->assign(static_cast<size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        ok = text->empty() || (ReadFile(f, text->data(), static_cast<DWORD>(text->size()), &read, nullptr) &&
                               read == text->size());
        if (!ok) *error = Win32Error(GetLastError());
    }
    CloseHandle(f);
    return ok;
}

// Shares read access (review finding A-3): a reader that holds the file
// open, such as a virus scanner, must not cost the record of a change.
bool WriteUtf8(const std::wstring& path, const std::string& text, std::string* error) {
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        *error = Win32Error(GetLastError());
        return false;
    }
    DWORD written = 0;
    const bool ok = text.empty() || (WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                                     written == text.size());
    if (!ok) *error = Win32Error(GetLastError());
    CloseHandle(f);
    return ok;
}

// The acs.exe one folder above this program's own (it is installed as
// <game>\ac-dlssg\ac-dlssg-drs.exe); empty when there is none.
std::wstring DefaultExe() {
    std::wstring self(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
        if (n == 0) return {};
        if (n < self.size()) {
            self.resize(n);
            break;
        }
        if (self.size() >= 32768) return {};
        self.resize(self.size() * 2);
    }
    for (int up = 0; up < 2; ++up) {
        const size_t sep = self.find_last_of(L"\\/");
        if (sep == std::wstring::npos) return {};
        self.resize(sep);
    }
    const std::wstring exe = self + L"\\acs.exe";
    const DWORD attrs = GetFileAttributesW(exe.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY) ? exe : std::wstring();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // Any DLL loaded by name from here on comes from System32 only: the
    // helper may run elevated from a folder its user can write to.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    const auto store = acdb::MakeNvapiStore();
    acdb::DrsIo io;
    io.print = Print;
    io.readFile = ReadUtf8;
    io.writeFile = WriteUtf8;
    return acdb::DrsMain(args, DefaultExe(), *store, io);
}
