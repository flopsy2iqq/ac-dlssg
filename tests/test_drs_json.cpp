// The small JSON reader and writer of ac-dlssg-drs.exe (drs_json.h): it
// reads back the record it wrote, after the installer kept it in its
// manifest and Windows PowerShell 5.1 wrote it out again.
#include <cstdio>
#include <string>

#include "drs_json.h"
#include "test_framework.h"

using namespace acdb;

namespace {

JsonValue Parse(const std::string& text) {
    JsonValue v;
    std::string error;
    const bool ok = ParseJson(text, &v, &error);
    if (!ok) std::printf("  parse error: %s\n", error.c_str());
    CHECK(ok);
    return v;
}

bool Refused(const std::string& text) {
    JsonValue v;
    std::string error;
    const bool ok = ParseJson(text, &v, &error);
    if (!ok) CHECK(!error.empty());
    return !ok;
}

}  // namespace

TEST(DrsJson_ParsesObjectsArraysAndScalars) {
    const JsonValue v = Parse(
        "{\"profile\":\"Assetto Corsa\",\"before\":{\"present\":false,\"value\":null},\"written\":5,"
        "\"list\":[1,true,\"x\"],\"ratio\":-1.5e2}");
    REQUIRE(v.type == JsonValue::Type::Object);
    REQUIRE(v.Find("profile") != nullptr);
    CHECK(v.Find("profile")->type == JsonValue::Type::String);
    CHECK_EQ(v.Find("profile")->text, std::string("Assetto Corsa"));
    const JsonValue* before = v.Find("before");
    REQUIRE(before != nullptr && before->type == JsonValue::Type::Object);
    REQUIRE(before->Find("present") != nullptr);
    CHECK(before->Find("present")->type == JsonValue::Type::Bool && !before->Find("present")->boolean);
    REQUIRE(before->Find("value") != nullptr);
    CHECK(before->Find("value")->type == JsonValue::Type::Null);
    uint32_t written = 0;
    REQUIRE(v.Find("written") != nullptr);
    CHECK(JsonUint32(*v.Find("written"), &written));
    CHECK_EQ(written, 5u);
    const JsonValue* list = v.Find("list");
    REQUIRE(list != nullptr && list->type == JsonValue::Type::Array);
    REQUIRE(list->items.size() == 3);
    CHECK(list->items[1].type == JsonValue::Type::Bool && list->items[1].boolean);
    CHECK_EQ(list->items[2].text, std::string("x"));
    CHECK(v.Find("ratio") != nullptr && v.Find("ratio")->type == JsonValue::Type::Number);
    CHECK(v.Find("missing") == nullptr);
}

// ConvertTo-Json of Windows PowerShell 5.1: a BOM when written with one,
// CRLF, its odd indentation, two spaces after a colon, \u0027 for an
// apostrophe, and a Cyrillic game folder as plain UTF-8.
TEST(DrsJson_ReadsWhatWindowsPowerShellWrites) {
    const std::string text =
        "\xEF\xBB\xBF{\r\n"
        "    \"profile\":  \"Assetto Corsa\",\r\n"
        "    \"exe\":  \"D:\\\\\xD0\x98\xD0\xB3\xD1\x80\xD1\x8B\\\\Kunos\\u0027 AC\\\\acs.exe\",\r\n"
        "    \"before\":  {\r\n"
        "                   \"present\":  true,\r\n"
        "                   \"value\":  7\r\n"
        "               },\r\n"
        "    \"written\":  5\r\n"
        "}\r\n";
    const JsonValue v = Parse(text);
    REQUIRE(v.type == JsonValue::Type::Object);
    REQUIRE(v.Find("exe") != nullptr);
    CHECK_EQ(v.Find("exe")->text, std::string("D:\\\xD0\x98\xD0\xB3\xD1\x80\xD1\x8B\\Kunos' AC\\acs.exe"));
    REQUIRE(v.Find("before") != nullptr && v.Find("before")->Find("value") != nullptr);
    uint32_t value = 0;
    CHECK(JsonUint32(*v.Find("before")->Find("value"), &value));
    CHECK_EQ(value, 7u);
}

TEST(DrsJson_DecodesEscapes) {
    const JsonValue v = Parse("\"q\\\" b\\\\ s\\/ \\b\\f\\n\\r\\t \\u00e9 \\ud83d\\ude00\"");
    CHECK(v.type == JsonValue::Type::String);
    CHECK_EQ(v.text, std::string("q\" b\\ s/ \b\f\n\r\t \xC3\xA9 \xF0\x9F\x98\x80"));
}

TEST(DrsJson_RefusesMalformedText) {
    CHECK(Refused(""));
    CHECK(Refused("{"));
    CHECK(Refused("{\"a\" 1}"));
    CHECK(Refused("{\"a\":1,}"));
    CHECK(Refused("[1 2]"));
    CHECK(Refused("\"unterminated"));
    CHECK(Refused("\"bad \\x escape\""));
    CHECK(Refused("\"lone \\ud83d surrogate\""));
    CHECK(Refused("{\"a\":1} trailing"));
    CHECK(Refused("tru"));
    CHECK(Refused("01"));
    CHECK(Refused("\"control \x01 char\""));
    // Nesting is bounded, so a hostile file cannot exhaust the stack.
    CHECK(Refused(std::string(1000, '[') + std::string(1000, ']')));
}

// Whole numbers 0..4294967295 as written, nothing else.
TEST(DrsJson_Uint32TakesWholeNumbersInRange) {
    uint32_t n = 0;
    CHECK(JsonUint32(Parse("4294967295"), &n));
    CHECK_EQ(n, 4294967295u);
    CHECK(JsonUint32(Parse("0"), &n));
    CHECK_EQ(n, 0u);
    CHECK(!JsonUint32(Parse("4294967296"), &n));
    CHECK(!JsonUint32(Parse("-1"), &n));
    CHECK(!JsonUint32(Parse("1.5"), &n));
    CHECK(!JsonUint32(Parse("1e3"), &n));
    CHECK(!JsonUint32(Parse("\"5\""), &n));
    CHECK(!JsonUint32(Parse("null"), &n));
}

TEST(DrsJson_QuoteEscapesWhatJsonRequires) {
    CHECK_EQ(JsonQuote("Assetto Corsa"), std::string("\"Assetto Corsa\""));
    CHECK_EQ(JsonQuote("C:\\Games\\\"AC\""), std::string("\"C:\\\\Games\\\\\\\"AC\\\"\""));
    CHECK_EQ(JsonQuote("a\nb\tc\x01"), std::string("\"a\\nb\\tc\\u0001\""));
    CHECK_EQ(JsonQuote("\xD0\x98\xD0\xB3\xD1\x80\xD1\x8B"), std::string("\"\xD0\x98\xD0\xB3\xD1\x80\xD1\x8B\""));
    // What it writes, it reads back.
    const std::string raw = "D:\\\xD0\x98\xD0\xB3\xD1\x80\xD1\x8B\\\"x\"\r\n";
    CHECK_EQ(Parse(JsonQuote(raw)).text, raw);
}
