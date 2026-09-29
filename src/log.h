#pragma once
// Thread-safe file log. Every line is flushed at once so that a crash keeps
// everything written before it.
#include <string>

namespace acdb {

enum class LogLevel : int { Error = 0, Warn = 1, Info = 2, Debug = 3 };

// Creates the parent directories, truncates the file and writes nothing else.
// Calling it again reopens the log at the new path.
bool LogOpen(const std::wstring& path, LogLevel level);
void LogClose();
void LogSetLevel(LogLevel level);
LogLevel LogGetLevel();

// Line format: "HH:MM:SS.mmm [tid] LEVEL message\n". No-op while the log is
// closed or when level is above the configured level.
void LogWrite(LogLevel level, const char* fmt, ...);

}  // namespace acdb

#define LOGE(...) ::acdb::LogWrite(::acdb::LogLevel::Error, __VA_ARGS__)
#define LOGW(...) ::acdb::LogWrite(::acdb::LogLevel::Warn, __VA_ARGS__)
#define LOGI(...) ::acdb::LogWrite(::acdb::LogLevel::Info, __VA_ARGS__)
#define LOGD(...) ::acdb::LogWrite(::acdb::LogLevel::Debug, __VA_ARGS__)

#include <atomic>

namespace acdb {
// Step tracing for in-game hangs (log_level=debug): TraceBudget::Take(n)
// returns true for at most n calls per site. g_trace_frames counts Presents
// still to trace after a DLSS feature create.
inline std::atomic<int> g_trace_frames{0};
}  // namespace acdb
#define LOGT_ONCE_N(n, ...)                                          \
    do {                                                             \
        static std::atomic<int> acdb_trace_left_{(n)};               \
        if (acdb_trace_left_.fetch_sub(1) > 0) LOGD(__VA_ARGS__);    \
    } while (0)
