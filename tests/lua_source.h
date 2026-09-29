#pragma once
// Reading the CSP Lua app (apps/lua/AcDlssg) from the source tree, for the
// tests that check it against the C++ side: the body of a `local NAME = [[
// ... ]]` layout string, its declarations, a `local NAME = <number>` constant
// and the text of a `local function name(` ... `\nend` block.
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

#include "temp_dir.h"

namespace acdb_test {

inline std::string ReadLuaAppFile(const char* file) {
    return ReadAll(std::filesystem::path(ACDB_SOURCE_DIR) / "apps" / "lua" / "AcDlssg" / file);
}

// The body of `local <name> = [[ ... ]]`, or empty.
inline std::string LuaLayoutBody(const std::string& lua, const std::string& name) {
    const std::string open = "local " + name + " = [[";
    const size_t begin = lua.find(open);
    if (begin == std::string::npos) return {};
    const size_t body = begin + open.size();
    const size_t end = lua.find("]]", body);
    if (end == std::string::npos) return {};
    return lua.substr(body, end - body);
}

struct LuaDecl {
    std::string type;
    std::string name;
    unsigned count = 1;
};

// Splits a C struct body into declarations; an unparsable one comes back
// with an empty type and its text as the name.
inline std::vector<LuaDecl> ParseLuaDecls(const std::string& body) {
    static const std::regex decl(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[\s*(\d+)\s*\])?\s*$)");
    std::vector<LuaDecl> out;
    size_t start = 0;
    while (start < body.size()) {
        size_t semi = body.find(';', start);
        if (semi == std::string::npos) semi = body.size();
        const std::string text = body.substr(start, semi - start);
        start = semi + 1;
        if (text.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        std::smatch m;
        LuaDecl d;
        if (std::regex_match(text, m, decl)) {
            d.type = m[1];
            d.name = m[2];
            if (m[3].matched) d.count = static_cast<unsigned>(std::stoul(m[3]));
        } else {
            d.name = text;
        }
        out.push_back(d);
    }
    return out;
}

// The number assigned by `local <name> = <number>` (decimal or 0x hex), or -1.
inline long long LuaNumber(const std::string& lua, const std::string& name) {
    const std::regex re("local\\s+" + name + "\\s*=\\s*(0[xX][0-9A-Fa-f]+|\\d+)\\b");
    std::smatch m;
    if (!std::regex_search(lua, m, re)) return -1;
    return std::stoll(m[1].str(), nullptr, 0);
}

// The string assigned by `local <name> = '<text>'`, or empty.
inline std::string LuaString(const std::string& lua, const std::string& name) {
    const std::regex re("local\\s+" + name + "\\s*=\\s*'([^']*)'");
    std::smatch m;
    if (!std::regex_search(lua, m, re)) return {};
    return m[1];
}

// From `local function <name>(` to the first line that is exactly `end`, or empty.
inline std::string LuaFunctionBody(const std::string& lua, const std::string& name) {
    const size_t begin = lua.find("local function " + name + "(");
    if (begin == std::string::npos) return {};
    const size_t end = lua.find("\nend", begin);
    if (end == std::string::npos) return {};
    return lua.substr(begin, end - begin);
}

// The Lua source without comments and string literals, so that keywords can
// be counted: `--[[ ]]`/`--[=[ ]=]` and `--` comments, `[[ ]]` long strings
// and quoted strings (with escapes) become one space each.
inline std::string LuaCodeOnly(const std::string& lua) {
    std::string out;
    size_t i = 0;
    const auto longBracketEnd = [&](size_t at, size_t* level) -> bool {
        // at points at '['; accepts [[ and [=*[
        size_t j = at + 1;
        size_t eq = 0;
        while (j < lua.size() && lua[j] == '=') ++eq, ++j;
        if (j < lua.size() && lua[j] == '[') {
            *level = eq;
            return true;
        }
        return false;
    };
    const auto skipLong = [&](size_t at, size_t level) -> size_t {
        const std::string close = "]" + std::string(level, '=') + "]";
        const size_t e = lua.find(close, at);
        return e == std::string::npos ? lua.size() : e + close.size();
    };
    while (i < lua.size()) {
        const char c = lua[i];
        if (c == '-' && i + 1 < lua.size() && lua[i + 1] == '-') {
            size_t level = 0;
            if (i + 2 < lua.size() && lua[i + 2] == '[' && longBracketEnd(i + 2, &level)) {
                i = skipLong(i + 2, level);
            } else {
                const size_t e = lua.find('\n', i);
                i = e == std::string::npos ? lua.size() : e;
            }
            out.push_back(' ');
            continue;
        }
        size_t level = 0;
        if (c == '[' && longBracketEnd(i, &level)) {
            i = skipLong(i, level);
            out.push_back(' ');
            continue;
        }
        if (c == '\'' || c == '"') {
            ++i;
            while (i < lua.size() && lua[i] != c && lua[i] != '\n') i += lua[i] == '\\' ? 2 : 1;
            ++i;
            out.push_back(' ');
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

// How often the keyword appears as a whole word in code (LuaCodeOnly).
inline size_t LuaKeywordCount(const std::string& code, const std::string& keyword) {
    const std::regex re("(^|[^A-Za-z0-9_.:])" + keyword + "(?![A-Za-z0-9_])");
    return static_cast<size_t>(std::distance(std::sregex_iterator(code.begin(), code.end(), re), std::sregex_iterator()));
}

}  // namespace acdb_test
