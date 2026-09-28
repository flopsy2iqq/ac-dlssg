#include "log.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace acdb {
namespace {

std::mutex g_mu;
HANDLE g_file = INVALID_HANDLE_VALUE;  // guarded by g_mu
std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};
std::atomic<bool> g_open{false};

const char* LevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Error: return "ERROR";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Info: return "INFO";
        case LogLevel::Debug: return "DEBUG";
    }
    return "?";
}

// CreateDirectoryW for every missing component of dir.
void CreateDirectories(const std::wstring& dir) {
    if (dir.empty()) return;
    const DWORD attrs = GetFileAttributesW(dir.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) return;
    const size_t sep = dir.find_last_of(L"\\/");
    // Stop at the root: "C:" or a UNC share has no parent to create.
    if (sep != std::wstring::npos && sep > 0 && dir[sep - 1] != L':' && dir[sep - 1] != L'\\')
        CreateDirectories(dir.substr(0, sep));
    CreateDirectoryW(dir.c_str(), nullptr);
}

void CloseLocked() {
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    g_open = false;
}

}  // namespace

bool LogOpen(const std::wstring& path, LogLevel level) {
    try {
        std::lock_guard<std::mutex> lock(g_mu);
        CloseLocked();
        const size_t sep = path.find_last_of(L"\\/");
        if (sep != std::wstring::npos) CreateDirectories(path.substr(0, sep));
        // Shared for reading and writing so the log can be viewed (and the
        // tests can read it) while the game runs.
        g_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_file == INVALID_HANDLE_VALUE) return false;
        g_level = static_cast<int>(level);
        g_open = true;
        return true;
    } catch (...) {
        return false;
    }
}

void LogClose() {
    std::lock_guard<std::mutex> lock(g_mu);
    CloseLocked();
}

void LogSetLevel(LogLevel level) { g_level = static_cast<int>(level); }

LogLevel LogGetLevel() { return static_cast<LogLevel>(g_level.load()); }

void LogWrite(LogLevel level, const char* fmt, ...) {
    if (!g_open || static_cast<int>(level) > g_level.load() || !fmt) return;
    try {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char prefix[64];
        const int plen = std::snprintf(prefix, sizeof(prefix), "%02u:%02u:%02u.%03u [%lu] %s ", st.wHour, st.wMinute,
                                       st.wSecond, st.wMilliseconds, GetCurrentThreadId(), LevelName(level));
        std::string line(prefix, plen > 0 ? static_cast<size_t>(plen) : 0);

        va_list args;
        va_start(args, fmt);
        va_list copy;
        va_copy(copy, args);
        char stackBuf[1024];
        const int n = std::vsnprintf(stackBuf, sizeof(stackBuf), fmt, args);
        va_end(args);
        if (n >= static_cast<int>(sizeof(stackBuf))) {
            std::string big(static_cast<size_t>(n) + 1, '\0');
            std::vsnprintf(big.data(), big.size(), fmt, copy);
            big.resize(static_cast<size_t>(n));
            line += big;
        } else if (n > 0) {
            line.append(stackBuf, static_cast<size_t>(n));
        }
        va_end(copy);

        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        line += '\n';

        std::lock_guard<std::mutex> lock(g_mu);
        if (g_file == INVALID_HANDLE_VALUE) return;
        // Unbuffered WriteFile: the line reaches the OS at once, so a crash
        // right after it keeps it.
        DWORD written = 0;
        WriteFile(g_file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    } catch (...) {
        // Logging must never throw into a hook or COM method.
    }
}

}  // namespace acdb
