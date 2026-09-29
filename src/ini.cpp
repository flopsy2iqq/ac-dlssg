#include "ini.h"

#include <windows.h>

#include <charconv>

namespace acdb {
namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

std::string Trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && IsSpace(s[b])) ++b;
    while (e > b && IsSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::string Lower(std::string s) {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

}  // namespace

IniFile IniFile::Parse(const std::string& text) {
    IniFile ini;
    size_t pos = 0;
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) pos = 3;

    std::string section;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = Trim(text.substr(pos, end - pos));
        pos = end + 1;

        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            const size_t close = line.find(']');
            if (close != std::string::npos) section = Lower(Trim(line.substr(1, close - 1)));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Lower(Trim(line.substr(0, eq)));
        if (key.empty()) continue;
        std::string value = line.substr(eq + 1);
        const size_t semi = value.find(';');
        if (semi != std::string::npos) value.resize(semi);
        ini.data_[section][key] = Trim(value);
        ini.all_[section][key].push_back(Trim(value));
    }
    return ini;
}

std::optional<IniFile> IniFile::Load(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return std::nullopt;
    std::string text;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart >= 0 && size.QuadPart < (64ll << 20);
    if (ok) {
        text.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = text.empty() || ReadFile(f, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
        text.resize(read);
    }
    CloseHandle(f);
    if (!ok) return std::nullopt;
    return Parse(text);
}

std::optional<std::string> IniFile::Get(const std::string& section, const std::string& key) const {
    const auto s = data_.find(Lower(section));
    if (s == data_.end()) return std::nullopt;
    const auto k = s->second.find(Lower(key));
    if (k == s->second.end()) return std::nullopt;
    return k->second;
}

std::optional<std::string> LayeredGet(const std::optional<IniFile>& base, const std::optional<IniFile>& over,
                                      const std::string& section, const std::string& key) {
    if (over) {
        if (auto v = over->Get(section, key)) return v;
    }
    if (base) return base->Get(section, key);
    return std::nullopt;
}

std::optional<long long> ToInt(const std::optional<std::string>& value) {
    if (!value) return std::nullopt;
    const std::string s = Trim(*value);
    if (s.empty()) return std::nullopt;
    const char* first = s.data();
    const char* last = s.data() + s.size();
    if (*first == '+') ++first;  // from_chars rejects a leading '+'
    if (first == last || (first != s.data() && *first == '-')) return std::nullopt;
    long long out = 0;
    const auto res = std::from_chars(first, last, out, 10);
    if (res.ec != std::errc() || res.ptr != last) return std::nullopt;
    return out;
}

std::map<std::string, std::vector<std::string>> IniFile::Repeats(const std::string& section) const {
    std::map<std::string, std::vector<std::string>> out;
    const auto s = all_.find(Lower(section));
    if (s == all_.end()) return out;
    for (const auto& [key, values] : s->second) {
        if (values.size() > 1) out[key] = values;
    }
    return out;
}

}  // namespace acdb
