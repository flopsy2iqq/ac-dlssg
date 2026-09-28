#include <windows.h>

#include "latency_semaphore.h"
#include "test_framework.h"

using namespace acdb;

namespace {

// Takes every available count with zero-timeout waits and returns how many
// there were. The counts are consumed.
unsigned Drain(HANDLE h) {
    unsigned n = 0;
    while (n < 64 && WaitForSingleObject(h, 0) == WAIT_OBJECT_0) ++n;
    return n;
}

}  // namespace

TEST(Latency_InitialCountIsDefaultOne) {
    LatencySemaphore s;
    REQUIRE(s.Init(true, 0));
    CHECK(s.Enabled());
    CHECK_EQ(s.CurrentLatency(), 1u);
    CHECK_EQ(s.PendingSwallow(), 0u);
    REQUIRE(s.RawHandleForTests() != nullptr);
    CHECK_EQ(Drain(s.RawHandleForTests()), 1u);
    UINT n = 0;
    CHECK_EQ(s.GetMaximum(&n), S_OK);
    CHECK_EQ(n, 1u);
}

TEST(Latency_InitialCountIsOverride) {
    LatencySemaphore s;
    REQUIRE(s.Init(true, 4));
    CHECK_EQ(s.CurrentLatency(), 4u);
    CHECK_EQ(Drain(s.RawHandleForTests()), 4u);
}

TEST(Latency_RaiseReleasesDifference) {
    LatencySemaphore s;
    REQUIRE(s.Init(true, 0));
    CHECK_EQ(s.SetMaximum(3), S_OK);
    CHECK_EQ(s.CurrentLatency(), 3u);
    UINT n = 0;
    CHECK_EQ(s.GetMaximum(&n), S_OK);
    CHECK_EQ(n, 3u);
    CHECK_EQ(Drain(s.RawHandleForTests()), 3u);
}

TEST(Latency_LowerSwallowsFutureReleases) {
    LatencySemaphore s;
    REQUIRE(s.Init(true, 0));
    CHECK_EQ(s.SetMaximum(4), S_OK);
    CHECK_EQ(Drain(s.RawHandleForTests()), 4u);  // four frames in flight
    CHECK_EQ(s.SetMaximum(2), S_OK);
    CHECK_EQ(s.CurrentLatency(), 2u);
    CHECK_EQ(s.PendingSwallow(), 2u);
    s.ReleaseOne();
    s.ReleaseOne();
    CHECK_EQ(s.PendingSwallow(), 0u);
    CHECK_EQ(Drain(s.RawHandleForTests()), 0u);
    s.ReleaseOne();
    s.ReleaseOne();
    CHECK_EQ(Drain(s.RawHandleForTests()), 2u);
}

TEST(Latency_RaiseCancelsPendingSwallowsFirst) {
    LatencySemaphore s;
    REQUIRE(s.Init(true, 0));
    CHECK_EQ(s.SetMaximum(3), S_OK);  // count 3
    CHECK_EQ(s.SetMaximum(1), S_OK);  // 2 to swallow, count still 3
    CHECK_EQ(s.PendingSwallow(), 2u);
    CHECK_EQ(s.SetMaximum(3), S_OK);  // cancels both, releases nothing
    CHECK_EQ(s.PendingSwallow(), 0u);
    CHECK_EQ(s.CurrentLatency(), 3u);
    CHECK_EQ(Drain(s.RawHandleForTests()), 3u);

    LatencySemaphore t;
    REQUIRE(t.Init(true, 0));
    CHECK_EQ(t.SetMaximum(4), S_OK);  // count 4
    CHECK_EQ(t.SetMaximum(2), S_OK);  // 2 to swallow
    CHECK_EQ(t.SetMaximum(5), S_OK);  // cancels 2, releases 1
    CHECK_EQ(t.PendingSwallow(), 0u);
    CHECK_EQ(Drain(t.RawHandleForTests()), 5u);
}

TEST(Latency_Clamping) {
    LatencySemaphore s;
    REQUIRE(s.Init(true, 0));
    CHECK_EQ(s.SetMaximum(20), S_OK);
    CHECK_EQ(s.CurrentLatency(), 16u);
    CHECK_EQ(Drain(s.RawHandleForTests()), 16u);
    CHECK_EQ(s.SetMaximum(0), S_OK);
    CHECK_EQ(s.CurrentLatency(), 1u);
    CHECK_EQ(s.PendingSwallow(), 15u);
}

TEST(Latency_ConfigOverrideIgnoresSetMaximum) {
    LatencySemaphore s;
    REQUIRE(s.Init(true, 3));
    CHECK_EQ(s.SetMaximum(8), S_OK);
    CHECK_EQ(s.CurrentLatency(), 3u);
    CHECK_EQ(s.SetMaximum(1), S_OK);
    CHECK_EQ(s.CurrentLatency(), 3u);
    CHECK_EQ(s.PendingSwallow(), 0u);
    UINT n = 0;
    CHECK_EQ(s.GetMaximum(&n), S_OK);
    CHECK_EQ(n, 3u);
    CHECK_EQ(Drain(s.RawHandleForTests()), 3u);
}

TEST(Latency_DisabledMode) {
    LatencySemaphore s;
    REQUIRE(s.Init(false, 0));
    CHECK(!s.Enabled());
    CHECK(s.DuplicateForCaller() == nullptr);
    CHECK_EQ(s.SetMaximum(2), DXGI_ERROR_INVALID_CALL);
    UINT n = 77;
    CHECK_EQ(s.GetMaximum(&n), DXGI_ERROR_INVALID_CALL);
    CHECK_EQ(n, 77u);
    s.ReleaseOne();  // no-op, no crash
    CHECK(s.RawHandleForTests() == nullptr);

    LatencySemaphore never;  // Init not called
    CHECK(!never.Enabled());
    CHECK(never.DuplicateForCaller() == nullptr);
    CHECK_EQ(never.SetMaximum(2), DXGI_ERROR_INVALID_CALL);
}

TEST(Latency_GetMaximumNullPointer) {
    LatencySemaphore s;
    REQUIRE(s.Init(true, 0));
    CHECK(FAILED(s.GetMaximum(nullptr)));
}

TEST(Latency_DuplicateIsIndependentHandleToSameSemaphore) {
    LatencySemaphore s;
    REQUIRE(s.Init(true, 0));
    HANDLE raw = s.RawHandleForTests();
    HANDLE dup = s.DuplicateForCaller();
    REQUIRE(dup != nullptr);
    CHECK(dup != raw);
    HANDLE dup2 = s.DuplicateForCaller();
    REQUIRE(dup2 != nullptr);
    CHECK(dup2 != dup);

    CHECK_EQ(WaitForSingleObject(dup, 0), static_cast<DWORD>(WAIT_OBJECT_0));  // takes the only count
    CHECK_EQ(WaitForSingleObject(raw, 0), static_cast<DWORD>(WAIT_TIMEOUT));
    CHECK_EQ(WaitForSingleObject(dup2, 0), static_cast<DWORD>(WAIT_TIMEOUT));

    CHECK(CloseHandle(dup));
    s.ReleaseOne();
    CHECK_EQ(WaitForSingleObject(dup2, 0), static_cast<DWORD>(WAIT_OBJECT_0));
    s.ReleaseOne();
    CHECK_EQ(WaitForSingleObject(raw, 0), static_cast<DWORD>(WAIT_OBJECT_0));
    CHECK(CloseHandle(dup2));
}

TEST(Latency_ReleaseAtMaximumIsHarmless) {
    LatencySemaphore s;
    REQUIRE(s.Init(true, 0));
    for (int i = 0; i < 20; ++i) s.ReleaseOne();  // a game that never waits
    CHECK_EQ(Drain(s.RawHandleForTests()), 16u);
    CHECK_EQ(s.CurrentLatency(), 1u);
}

TEST(Latency_ReinitReplacesSemaphore) {
    LatencySemaphore s;
    REQUIRE(s.Init(true, 2));
    REQUIRE(s.Init(true, 0));
    CHECK_EQ(s.CurrentLatency(), 1u);
    CHECK_EQ(Drain(s.RawHandleForTests()), 1u);
}
