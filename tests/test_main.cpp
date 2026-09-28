#include <windows.h>

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

#include "test_framework.h"

namespace acdb_test {

std::vector<TestCase>& Registry() {
    static std::vector<TestCase> tests;
    return tests;
}

int g_failures_in_test = 0;

}  // namespace acdb_test

namespace {

// Runs one test body; C++ failures are handled here, SEH ones in RunGuarded.
void RunBody(void* ctx) {
    auto* tc = static_cast<acdb_test::TestCase*>(ctx);
    try {
        tc->fn();
    } catch (const acdb_test::RequireFailed&) {
        // Already counted by REQUIRE.
    } catch (const std::exception& e) {
        acdb_test::Fail(__FILE__, __LINE__, std::string("unexpected exception: ") + e.what());
    } catch (...) {
        acdb_test::Fail(__FILE__, __LINE__, "unexpected non-standard exception");
    }
}

// A crash (access violation and the like) in one test must not take down the
// whole run. __try cannot share a function with C++ objects that need unwinding.
DWORD RunGuarded(void (*body)(void*), void* ctx) {
    __try {
        body(ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* filter = argc > 1 ? argv[1] : nullptr;

    int passed = 0;
    int failed = 0;
    int skipped = 0;
    for (auto& tc : acdb_test::Registry()) {
        if (filter && !std::strstr(tc.name, filter)) {
            ++skipped;
            continue;
        }
        acdb_test::g_failures_in_test = 0;
        const DWORD seh = RunGuarded(&RunBody, &tc);
        if (seh != 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "SEH exception 0x%08lX", static_cast<unsigned long>(seh));
            acdb_test::Fail(__FILE__, __LINE__, buf);
        }
        if (acdb_test::g_failures_in_test == 0) {
            ++passed;
            std::printf("PASS %s\n", tc.name);
        } else {
            ++failed;
            std::printf("FAIL %s\n", tc.name);
        }
    }
    std::printf("\n%d passed, %d failed, %d filtered out\n", passed, failed, skipped);
    return failed;
}
