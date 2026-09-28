#pragma once
// Minimal INI reader for CSP/AC config files and our own config.
// Rules: sections "[NAME]"; "KEY=VALUE" lines; section and key names are
// case-insensitive; lines starting with ';' or '#' are comments; a value ends
// at the first ';' (CSP writes "ACTIVE=0 ; Active; 1 or 0"); values and names
// are trimmed; for a repeated key the last one wins; a UTF-8 BOM is skipped.
#include <map>
#include <optional>
#include <string>

namespace acdb {

class IniFile {
public:
    static IniFile Parse(const std::string& text);
    // nullopt when the file does not exist or cannot be read.
    static std::optional<IniFile> Load(const std::wstring& path);

    std::optional<std::string> Get(const std::string& section, const std::string& key) const;

private:
    std::map<std::string, std::map<std::string, std::string>> data_;  // lower-cased names
};

// The override file wins when it has the key; otherwise the base file is used.
std::optional<std::string> LayeredGet(const std::optional<IniFile>& base,
                                      const std::optional<IniFile>& over,
                                      const std::string& section, const std::string& key);

// Parses a decimal integer, ignoring surrounding spaces. nullopt for
// missing or non-numeric values.
std::optional<long long> ToInt(const std::optional<std::string>& value);

}  // namespace acdb
