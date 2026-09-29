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

constexpr char kBom[] = "\xEF\xBB\xBF";

// One line of a text: [begin, contentEnd) without its line ending,
// [contentEnd, end) the ending itself ("\r\n", "\n" or nothing at the end).
struct LineSpan {
    size_t begin = 0;
    size_t contentEnd = 0;
    size_t end = 0;
};

std::vector<LineSpan> SplitLines(const std::string& text) {
    std::vector<LineSpan> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        LineSpan l;
        l.begin = pos;
        const size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            l.contentEnd = l.end = text.size();
        } else {
            l.end = nl + 1;
            l.contentEnd = nl > pos && text[nl - 1] == '\r' ? nl - 1 : nl;
        }
        lines.push_back(l);
        pos = l.end;
    }
    return lines;
}

// content with its value (after '=' and its spaces, up to a ';' comment,
// without trailing spaces) replaced.
std::string WithValue(const std::string& content, size_t eq, const std::string& value) {
    size_t start = eq + 1;
    while (start < content.size() && (content[start] == ' ' || content[start] == '\t')) ++start;
    const size_t semi = content.find(';', start);
    size_t end = semi == std::string::npos ? content.size() : semi;
    while (end > start && IsSpace(content[end - 1])) --end;
    return content.substr(0, start) + value + content.substr(end);
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string Win32Failure(const char* what, const std::wstring& path, DWORD code) {
    return std::string(what) + " " + Narrow(path) + " failed: error " + std::to_string(code);
}

// Creates every missing folder above path.
void CreateParentDirs(const std::wstring& path) {
    for (size_t sep = path.find_first_of(L"\\/", 3); sep != std::wstring::npos;
         sep = path.find_first_of(L"\\/", sep + 1)) {
        CreateDirectoryW(path.substr(0, sep).c_str(), nullptr);
    }
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

std::string SetIniKeys(const std::string& text, const std::string& section, const IniKeyValues& keys) {
    const std::string nl = text.find("\r\n") != std::string::npos ? "\r\n"
                           : text.find('\n') != std::string::npos ? "\n"
                                                                  : "\r\n";
    const std::string wanted = Lower(Trim(section));
    std::vector<std::string> names;
    for (const auto& kv : keys) names.push_back(Lower(Trim(kv.first)));
    std::vector<bool> present(keys.size(), false);

    std::string out;
    out.reserve(text.size() + 48 * keys.size());
    std::string current;               // the section of the line, as IniFile::Parse sees it
    size_t insertAt = std::string::npos;  // in out: after the last key line (or header) of a wanted section
    const std::vector<LineSpan> lines = SplitLines(text);
    for (size_t i = 0; i < lines.size(); ++i) {
        const LineSpan& l = lines[i];
        std::string content = text.substr(l.begin, l.contentEnd - l.begin);
        const size_t from = i == 0 && content.compare(0, 3, kBom) == 0 ? 3 : 0;
        const std::string line = Trim(content.substr(from));
        bool keyLine = false;
        bool header = false;
        if (!line.empty() && line[0] == '[') {
            const size_t close = line.find(']');
            if (close != std::string::npos) {
                current = Lower(Trim(line.substr(1, close - 1)));
                header = true;
            }
        } else if (current == wanted && !line.empty() && line[0] != ';' && line[0] != '#') {
            const size_t eq = content.find('=', from);
            if (eq != std::string::npos) {
                const std::string key = Lower(Trim(content.substr(from, eq - from)));
                if (!key.empty()) {
                    keyLine = true;
                    for (size_t k = 0; k < names.size(); ++k) {
                        if (names[k] != key) continue;
                        content = WithValue(content, eq, keys[k].second);
                        present[k] = true;
                        break;
                    }
                }
            }
        }
        out += content;
        out.append(text, l.contentEnd, l.end - l.contentEnd);
        if ((header || keyLine) && current == wanted) insertAt = out.size();
    }

    std::string added;
    for (size_t k = 0; k < keys.size(); ++k) {
        if (present[k]) continue;
        bool repeated = false;  // a key named twice in keys is added once
        for (size_t j = 0; j < k; ++j) repeated = repeated || (!present[j] && names[j] == names[k]);
        if (!repeated) added += keys[k].first + "=" + keys[k].second + nl;
    }
    if (added.empty()) return out;
    if (insertAt == std::string::npos) {
        if (!out.empty() && out.back() != '\n') out += nl;
        out += "[" + section + "]" + nl + added;
        return out;
    }
    // Only the last line can lack a line ending.
    if (insertAt == out.size() && !out.empty() && out.back() != '\n') added = nl + added;
    out.insert(insertAt, added);
    return out;
}

bool WriteIniKeys(const std::wstring& path, const std::string& section, const IniKeyValues& keys,
                  std::string* error) {
    const auto fail = [error](std::string why) {
        if (error) *error = std::move(why);
        return false;
    };
    std::string text;
    const HANDLE in = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
            return fail(Win32Failure("reading", path, code));
    } else {
        LARGE_INTEGER size{};
        bool ok = GetFileSizeEx(in, &size) && size.QuadPart >= 0 && size.QuadPart < (64ll << 20);
        if (ok) {
            text.resize(static_cast<size_t>(size.QuadPart));
            DWORD read = 0;
            ok = text.empty() || ReadFile(in, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
            text.resize(read);
        }
        const DWORD code = GetLastError();
        CloseHandle(in);
        if (!ok) return fail(Win32Failure("reading", path, code));
    }

    const std::string out = SetIniKeys(text, section, keys);
    CreateParentDirs(path);
    const std::wstring tmp = path + L".new";
    const HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return fail(Win32Failure("creating", tmp, GetLastError()));
    DWORD written = 0;
    bool ok = out.empty() || (WriteFile(f, out.data(), static_cast<DWORD>(out.size()), &written, nullptr) &&
                              written == out.size());
    ok = ok && FlushFileBuffers(f);
    const DWORD code = GetLastError();
    CloseHandle(f);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return fail(Win32Failure("writing", tmp, code));
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD moveCode = GetLastError();
        DeleteFileW(tmp.c_str());
        return fail(Win32Failure("replacing", path, moveCode));
    }
    return true;
}

}  // namespace acdb
