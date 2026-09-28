#pragma once
// Scratch directory <test exe dir>\test_tmp\<tag>_<pid>_<n> for tests that need
// real files; removed with everything in it when the object goes away. It lives
// next to the test binary so that tests leave nothing outside the build tree.
#include <windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

namespace acdb_test {

class TempDir {
public:
    explicit TempDir(const wchar_t* tag) {
        static std::atomic<unsigned> counter{0};
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        path_ = std::filesystem::path(exe).parent_path() / L"test_tmp" /
                (std::wstring(tag) + L"_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                 std::to_wstring(counter++));
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& Path() const { return path_; }
    std::wstring Str() const { return path_.wstring(); }

    // Writes bytes as given (no newline translation), creating parent directories.
    std::filesystem::path Write(const std::wstring& relative, const std::string& bytes) const {
        const auto full = path_ / relative;
        std::error_code ec;
        std::filesystem::create_directories(full.parent_path(), ec);
        std::ofstream f(full, std::ios::binary | std::ios::trunc);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return full;
    }

private:
    std::filesystem::path path_;
};

inline std::string ReadAll(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

}  // namespace acdb_test
