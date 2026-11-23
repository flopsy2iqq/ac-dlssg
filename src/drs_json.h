#pragma once
// A small JSON reader and writer for ac-dlssg-drs.exe (drs_tool.h): it
// writes its results as JSON, and reads back the record of its write, which
// the installer keeps in its manifest and writes out again with Windows
// PowerShell 5.1's ConvertTo-Json. Objects, arrays, strings, numbers, true,
// false and null; UTF-8 in and out, an optional BOM.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace acdb {

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    // String: the decoded text (UTF-8); Number: the number as written.
    std::string text;
    std::vector<JsonValue> items;                             // Array
    std::vector<std::pair<std::string, JsonValue>> members;  // Object, in file order
    // Object: the first member of that name; null when there is none or
    // this is not an object.
    const JsonValue* Find(const std::string& key) const;
};

// Parses one JSON value (whitespace around it allowed, nothing else);
// false with *error ("<why> at byte <n>"). Nesting deeper than 64 is
// refused, and so is a string with a lone surrogate or a raw control
// character.
bool ParseJson(const std::string& utf8, JsonValue* out, std::string* error);

// A whole number 0..4294967295 as written: digits only, no sign, fraction
// or exponent.
bool JsonUint32(const JsonValue& v, uint32_t* out);

// "\"...\"" with ", \ and the control characters escaped (\n, \r, \t, \b,
// \f, else \u00XX); other UTF-8 stays as it is.
std::string JsonQuote(const std::string& utf8);

}  // namespace acdb
