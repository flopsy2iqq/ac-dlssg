#pragma once
// How a CSP Present call maps onto the D3D12 chain's Present (spec 7 step 6,
// 6.3). Pure function, unit-tested.
#include <windows.h>

namespace acdb {

struct PresentPlan {
    bool is_test = false;   // CSP passed DXGI_PRESENT_TEST: not a frame
    UINT sync = 0;          // sync interval for the D3D12 Present
    UINT flags = 0;         // flags for the D3D12 Present
    bool vsync_unavailable_with_fg = false;  // VSync was requested but FG cannot do it
};

// Rules:
//  - is_test = (cspFlags & DXGI_PRESENT_TEST) != 0. For a test present the
//    other fields are not used.
//  - sync = min(cspSync, 1).
//  - flags = cspFlags without DXGI_PRESENT_DO_NOT_WAIT.
//  - DXGI_PRESENT_ALLOW_TEARING is kept only when CSP passed it, sync == 0,
//    chainHasTearing and chainWindowed; otherwise it is stripped. It is never
//    added.
//  - If fgOn and sync == 1 and !fgVsyncSupported: sync = 0, ALLOW_TEARING is
//    stripped, vsync_unavailable_with_fg = true.
PresentPlan PlanPresent(UINT cspSync, UINT cspFlags, bool chainHasTearing, bool chainWindowed,
                        bool fgOn, bool fgVsyncSupported);

}  // namespace acdb
