#include "present_flags.h"

#include <dxgi1_5.h>

namespace acdb {

PresentPlan PlanPresent(UINT cspSync, UINT cspFlags, bool chainHasTearing, bool chainWindowed, bool fgOn,
                        bool fgVsyncSupported) {
    PresentPlan plan;
    plan.is_test = (cspFlags & DXGI_PRESENT_TEST) != 0;
    if (plan.is_test) return plan;

    plan.sync = cspSync > 1 ? 1u : cspSync;
    plan.flags = cspFlags & ~static_cast<UINT>(DXGI_PRESENT_DO_NOT_WAIT);

    const bool keepTearing =
        (cspFlags & DXGI_PRESENT_ALLOW_TEARING) != 0 && plan.sync == 0 && chainHasTearing && chainWindowed;
    if (!keepTearing) plan.flags &= ~static_cast<UINT>(DXGI_PRESENT_ALLOW_TEARING);

    if (fgOn && plan.sync == 1 && !fgVsyncSupported) {
        plan.sync = 0;
        plan.flags &= ~static_cast<UINT>(DXGI_PRESENT_ALLOW_TEARING);
        plan.vsync_unavailable_with_fg = true;
    }
    return plan;
}

}  // namespace acdb
