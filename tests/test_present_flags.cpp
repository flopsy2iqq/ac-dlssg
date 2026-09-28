#include <windows.h>
#include <dxgi1_5.h>

#include "present_flags.h"
#include "test_framework.h"

using namespace acdb;

namespace {
constexpr UINT kTest = DXGI_PRESENT_TEST;
constexpr UINT kNoWait = DXGI_PRESENT_DO_NOT_WAIT;
constexpr UINT kTear = DXGI_PRESENT_ALLOW_TEARING;
constexpr UINT kRestart = DXGI_PRESENT_RESTART;
}  // namespace

TEST(PresentFlags_TestPresentIsDetected) {
    CHECK(PlanPresent(1, kTest, true, true, false, false).is_test);
    CHECK(PlanPresent(0, kTest | kTear, true, true, true, false).is_test);
    CHECK(!PlanPresent(1, 0, true, true, false, false).is_test);
    CHECK(!PlanPresent(0, kTear, true, true, false, false).is_test);
}

TEST(PresentFlags_SyncIsClampedToOne) {
    CHECK_EQ(PlanPresent(0, 0, false, true, false, false).sync, 0u);
    CHECK_EQ(PlanPresent(1, 0, false, true, false, false).sync, 1u);
    CHECK_EQ(PlanPresent(2, 0, false, true, false, false).sync, 1u);
    CHECK_EQ(PlanPresent(4, 0, false, true, false, false).sync, 1u);
}

TEST(PresentFlags_DoNotWaitIsStripped) {
    CHECK_EQ(PlanPresent(1, kNoWait, false, true, false, false).flags, 0u);
    CHECK_EQ(PlanPresent(1, kNoWait | kRestart, false, true, false, false).flags, kRestart);
    CHECK_EQ(PlanPresent(0, kNoWait | kTear, true, true, false, false).flags, kTear);
}

TEST(PresentFlags_OtherFlagsPassThrough) {
    CHECK_EQ(PlanPresent(1, kRestart, false, true, false, false).flags, kRestart);
    CHECK_EQ(PlanPresent(1, 0, false, true, false, false).flags, 0u);
}

TEST(PresentFlags_TearingKeptOnlyWhenAllowed) {
    // Kept: CSP passed it, sync 0, chain has tearing, windowed.
    CHECK_EQ(PlanPresent(0, kTear, true, true, false, false).flags, kTear);
    CHECK_EQ(PlanPresent(0, kTear, true, true, true, true).flags, kTear);
    // Stripped: sync interval 1 (also when CSP asked for 2 or more).
    CHECK_EQ(PlanPresent(1, kTear, true, true, false, false).flags, 0u);
    CHECK_EQ(PlanPresent(3, kTear | kRestart, true, true, false, false).flags, kRestart);
    // Stripped: the chain was created without ALLOW_TEARING.
    CHECK_EQ(PlanPresent(0, kTear, false, true, false, false).flags, 0u);
    // Stripped: the chain is in fullscreen state.
    CHECK_EQ(PlanPresent(0, kTear, true, false, false, false).flags, 0u);
    // Never added.
    CHECK_EQ(PlanPresent(0, 0, true, true, false, false).flags, 0u);
    CHECK_EQ(PlanPresent(0, 0, true, true, true, false).flags, 0u);
}

TEST(PresentFlags_FgVsyncFallback) {
    // FG on, VSync requested, FG cannot do VSync: present with sync 0, no tearing.
    PresentPlan p = PlanPresent(1, 0, true, true, true, false);
    CHECK_EQ(p.sync, 0u);
    CHECK_EQ(p.flags, 0u);
    CHECK(p.vsync_unavailable_with_fg);

    p = PlanPresent(2, kTear, true, true, true, false);
    CHECK_EQ(p.sync, 0u);
    CHECK_EQ(p.flags, 0u);
    CHECK(p.vsync_unavailable_with_fg);

    // FG supports VSync: unchanged.
    p = PlanPresent(1, 0, true, true, true, true);
    CHECK_EQ(p.sync, 1u);
    CHECK(!p.vsync_unavailable_with_fg);

    // FG off: unchanged.
    p = PlanPresent(1, 0, true, true, false, false);
    CHECK_EQ(p.sync, 1u);
    CHECK(!p.vsync_unavailable_with_fg);

    // No VSync requested: nothing to fall back from.
    p = PlanPresent(0, kTear, true, true, true, false);
    CHECK_EQ(p.sync, 0u);
    CHECK_EQ(p.flags, kTear);
    CHECK(!p.vsync_unavailable_with_fg);
}
