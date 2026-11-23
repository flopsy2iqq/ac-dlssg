#include "smooth_motion.h"

#include <cstdio>

namespace acdb {

SmoothMotionOffPlan PlanSmoothMotionOff(const ProfileValue& app, const ProfileValue& global) {
    SmoothMotionOffPlan p;
    if (app.present) {
        p.effective = app.value;
        p.source = "app";
    } else if (global.present) {
        p.effective = global.value;
        p.source = "global";
    }
    if (p.effective & kSmoothMotionApiDx11) {
        p.action = SmoothMotionAction::Write;
        p.value = p.effective & ~kSmoothMotionApiDx11;
    }
    return p;
}

RestorePlan PlanRestore(const SmoothMotionRecord& record, const ProfileValue& current) {
    RestorePlan p;
    if (!current.present || current.value != record.written) return p;  // Leave
    if (record.before.present && !record.before.predefined) {
        p.action = RestoreAction::WriteOld;
        p.value = record.before.value;
    } else {
        p.action = RestoreAction::Delete;
    }
    return p;
}

std::string SmoothMotionApisText(uint32_t mask) {
    std::string text;
    const struct {
        uint32_t bit;
        const char* name;
    } apis[] = {{kSmoothMotionApiDx12, "DX12"}, {kSmoothMotionApiDx11, "DX11"}, {kSmoothMotionApiVulkan, "Vulkan"}};
    for (const auto& a : apis) {
        if (mask & a.bit) text += (text.empty() ? "" : ", ") + std::string(a.name);
    }
    const uint32_t other = mask & ~(kSmoothMotionApiDx12 | kSmoothMotionApiDx11 | kSmoothMotionApiVulkan);
    if (other) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "other bits 0x%08X", static_cast<unsigned>(other));
        text += (text.empty() ? "" : ", ") + std::string(buf);
    }
    return text.empty() ? "none" : text;
}

}  // namespace acdb
