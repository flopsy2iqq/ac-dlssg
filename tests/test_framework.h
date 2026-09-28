#pragma once
// Minimal test framework: TEST(name) registers a function; CHECK/CHECK_EQ
// record failures and continue; REQUIRE stops the current test.
// test_main.cpp runs every test (or those whose name contains argv[1]) and
// returns the number of failed tests.
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace acdb_test {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};
std::vector<TestCase>& Registry();
struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { Registry().push_back({name, std::move(fn)}); }
};
extern int g_failures_in_test;
struct RequireFailed {};

inline void Fail(const char* file, int line, const std::string& what) {
    ++g_failures_in_test;
    std::printf("  FAIL %s:%d: %s\n", file, line, what.c_str());
}

}  // namespace acdb_test

#define ACDB_CAT2(a, b) a##b
#define ACDB_CAT(a, b) ACDB_CAT2(a, b)
#define TEST(name)                                                                          \
    static void name();                                                                     \
    static ::acdb_test::Registrar ACDB_CAT(reg_, name)(#name, name);                        \
    static void name()

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) ::acdb_test::Fail(__FILE__, __LINE__, "CHECK(" #cond ")");              \
    } while (0)

#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        if (!((a) == (b))) ::acdb_test::Fail(__FILE__, __LINE__, "CHECK_EQ(" #a ", " #b ")"); \
    } while (0)

#define REQUIRE(cond)                                                                        \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            ::acdb_test::Fail(__FILE__, __LINE__, "REQUIRE(" #cond ")");                     \
            throw ::acdb_test::RequireFailed{};                                              \
        }                                                                                    \
    } while (0)
