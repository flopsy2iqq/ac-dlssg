// The pure decisions of ac-dlssg-drs.exe for NVIDIA Smooth Motion
// (smooth_motion.h): what the installer writes into the "Assetto Corsa"
// driver profile, and what the uninstaller restores.
#include <string>

#include "smooth_motion.h"
#include "test_framework.h"

using namespace acdb;

namespace {

ProfileValue None() { return ProfileValue(); }

ProfileValue Own(uint32_t v, bool predefined = false) {
    ProfileValue p;
    p.present = true;
    p.value = v;
    p.predefined = predefined;
    return p;
}

SmoothMotionRecord Record(const ProfileValue& before, uint32_t written) {
    SmoothMotionRecord r;
    r.before = before;
    r.written = written;
    return r;
}

}  // namespace

static_assert(kDrsSmoothMotionApis == 0xB0CC0875u && kDrsSmoothMotionEnable == 0xB0D384C0u);
static_assert(kSmoothMotionApiDx12 == 1u && kSmoothMotionApiDx11 == 2u && kSmoothMotionApiVulkan == 4u &&
              kSmoothMotionApisDefault == 7u);

// ---------------------------------------------------------------- off

// Neither profile has the setting: every API is allowed (7), so the app
// profile gets 5, DX12 and Vulkan.
TEST(SmoothMotionPlan_NoValueAnywhereWritesFiveInTheAppProfile) {
    const SmoothMotionOffPlan p = PlanSmoothMotionOff(None(), None());
    CHECK(p.action == SmoothMotionAction::Write);
    CHECK_EQ(p.value, 5u);
    CHECK_EQ(p.effective, 7u);
    CHECK_EQ(std::string(p.source), std::string("default"));
}

TEST(SmoothMotionPlan_AppFiveIsLeftAlone) {
    const SmoothMotionOffPlan p = PlanSmoothMotionOff(Own(5), None());
    CHECK(p.action == SmoothMotionAction::None);
    CHECK_EQ(p.effective, 5u);
    CHECK_EQ(std::string(p.source), std::string("app"));
}

// The fix two users made by hand (GitHub issue 10, i10fixed): DX11 off in
// the global profile already keeps Smooth Motion away from AC.
TEST(SmoothMotionPlan_GlobalFiveWithoutAnAppValueIsLeftAlone) {
    const SmoothMotionOffPlan p = PlanSmoothMotionOff(None(), Own(5));
    CHECK(p.action == SmoothMotionAction::None);
    CHECK_EQ(p.effective, 5u);
    CHECK_EQ(std::string(p.source), std::string("global"));
}

// The app profile's own value wins over the global one.
TEST(SmoothMotionPlan_AppSevenOverGlobalFiveWritesFive) {
    const SmoothMotionOffPlan p = PlanSmoothMotionOff(Own(7), Own(5));
    CHECK(p.action == SmoothMotionAction::Write);
    CHECK_EQ(p.value, 5u);
    CHECK_EQ(p.effective, 7u);
    CHECK_EQ(std::string(p.source), std::string("app"));
}

// Only the DX11 bit goes; every other bit of the effective value stays.
TEST(SmoothMotionPlan_OnlyTheDx11BitIsCleared) {
    struct Case {
        ProfileValue app, global;
        bool write;
        uint32_t value;
    };
    const Case cases[] = {
        {Own(3), None(), true, 1u},
        {None(), Own(6), true, 4u},
        {Own(2), Own(5), true, 0u},
        {Own(0xFFFFFFFFu), None(), true, 0xFFFFFFFDu},
        {Own(0), None(), false, 0u},
        {None(), Own(4), false, 0u},
        {Own(1, true), None(), false, 0u},
        {Own(7, true), None(), true, 5u},
    };
    for (const Case& c : cases) {
        const SmoothMotionOffPlan p = PlanSmoothMotionOff(c.app, c.global);
        CHECK_EQ(p.action == SmoothMotionAction::Write, c.write);
        if (c.write) CHECK_EQ(p.value, c.value);
    }
}

// ---------------------------------------------------------------- restore

// The install created the setting: deleting it gives the driver's default back.
TEST(SmoothMotionRestore_DeletesTheSettingTheInstallCreated) {
    const RestorePlan p = PlanRestore(Record(None(), 5), Own(5));
    CHECK(p.action == RestoreAction::Delete);
}

// The setting existed: its old value goes back.
TEST(SmoothMotionRestore_WritesTheOldValueBack) {
    const RestorePlan p = PlanRestore(Record(Own(7), 5), Own(5));
    CHECK(p.action == RestoreAction::WriteOld);
    CHECK_EQ(p.value, 7u);
}

// NVIDIA's own value in the profile (predefined): deleting the user value
// the install wrote over it brings it back.
TEST(SmoothMotionRestore_DeletesOverThePredefinedValue) {
    const RestorePlan p = PlanRestore(Record(Own(7, true), 5), Own(5));
    CHECK(p.action == RestoreAction::Delete);
}

// Changed since the install (by the user, NVIDIA Profile Inspector or the
// NVIDIA App): left as it is.
TEST(SmoothMotionRestore_LeavesAValueChangedSinceTheInstall) {
    CHECK(PlanRestore(Record(None(), 5), Own(7)).action == RestoreAction::Leave);
    CHECK(PlanRestore(Record(Own(3), 1), Own(3)).action == RestoreAction::Leave);
}

// Gone since the install (a clean driver install, the App's "restore
// defaults"): nothing of the install's is left to restore.
TEST(SmoothMotionRestore_LeavesAValueThatIsGone) {
    CHECK(PlanRestore(Record(None(), 5), None()).action == RestoreAction::Leave);
    CHECK(PlanRestore(Record(Own(7), 5), None()).action == RestoreAction::Leave);
}

// ---------------------------------------------------------------- text

TEST(SmoothMotionApisText_NamesTheApis) {
    CHECK_EQ(SmoothMotionApisText(7), std::string("DX12, DX11, Vulkan"));
    CHECK_EQ(SmoothMotionApisText(5), std::string("DX12, Vulkan"));
    CHECK_EQ(SmoothMotionApisText(2), std::string("DX11"));
    CHECK_EQ(SmoothMotionApisText(0), std::string("none"));
    CHECK_EQ(SmoothMotionApisText(0xFFFFFFFDu), std::string("DX12, Vulkan, other bits 0xFFFFFFF8"));
}
