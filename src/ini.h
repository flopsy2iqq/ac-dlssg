#pragma once
// Minimal INI reader for CSP/AC config files and our own config.
// Rules: sections "[NAME]"; "KEY=VALUE" lines; section and key names are
// case-insensitive; lines starting with ';' or '#' are comments; a value ends
// at the first ';' (CSP writes "ACTIVE=0 ; Active; 1 or 0"); values and names
// are trimmed; for a repeated key the last one wins; a UTF-8 BOM is skipped.
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

using IniKeyValues = std::vector<std::pair<std::string, std::string>>;

// Key-level edit of INI text, read with IniFile's rules (the panel's "Save as
// default", spec 6.9). Every byte stays as it was except:
//  - in every section named `section` (case-insensitive; the reader merges
//    repeated sections), every line of one of the keys gets the new value in
//    place of its old one: the key's spelling, the spaces around '=' and a
//    trailing "; comment" stay; comment lines are not key lines;
//  - a key with no line there is added as "key=value" after the last key
//    line of the last such section (right after its header when it has
//    none), in the order given;
//  - without such a section, "[section]" and the keys are appended at the end.
// New lines use the text's own line ending (CRLF when it has none yet); a
// last line without one gets one before anything is added after it.
std::string SetIniKeys(const std::string& text, const std::string& section, const IniKeyValues& keys);

// SetIniKeys on the file at path (a missing file counts as empty, its folder
// is created), written as <path>.new, flushed, then renamed over path with
// MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH). A file that is not UTF-8
// text (a UTF-16 BOM, or any NUL byte) is refused. On any failure the file is
// left as it was, <path>.new is removed and *error says what failed.
bool WriteIniKeys(const std::wstring& path, const std::string& section, const IniKeyValues& keys,
                  std::string* error);

}  // namespace acdb
