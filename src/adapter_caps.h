#pragma once
// Per-adapter facts from the kernel-mode thunks (D3DKMT): hardware-accelerated
// GPU scheduling and the hybrid-graphics (Optimus) role. The D3DKMT functions
// are resolved at run time from System32\gdi32.dll, so the bridge's static
// imports do not change (spec 6.1).
//
// HAGS is a per-adapter state: on a hybrid laptop the registry value
// HwSchMode can be absent while the discrete GPU schedules in hardware
// (dxdiag: "Hardware Scheduling: ... Enabled:True").
#include <windows.h>

#include <cstdint>
#include <string>

namespace acdb {

enum class HagsState { On, Off, Unknown };

struct AdapterHags {
    HagsState state = HagsState::Unknown;
    bool supported = false;  // HwSchSupported; meaningful only when state is not Unknown
    std::string reason;      // why Unknown; empty otherwise
};

enum class HybridRole { Neither, Discrete, Integrated, Unknown };

struct AdapterKmtInfo {
    AdapterHags hags;
    HybridRole hybrid = HybridRole::Unknown;
    std::string hybrid_reason;  // why Unknown; empty otherwise
};

// "HHHHHHHH:LLLLLLLL" (HighPart:LowPart), as the log prints LUIDs.
std::string LuidText(const LUID& luid);

// Pure: D3DKMT_WDDM_2_7_CAPS.Value. HwSchEnabled (bit 1) gives On, else Off;
// supported is HwSchSupported (bit 0).
AdapterHags HagsFromWddm27Caps(uint32_t capsValue);
// Pure: D3DKMT_ADAPTERTYPE.Value. HybridDiscrete (bit 4) or HybridIntegrated
// (bit 5); neither bit is Neither, both bits are Unknown.
HybridRole HybridRoleFromAdapterType(uint32_t adapterTypeValue);

const char* HagsStateName(HagsState state);  // "on", "off", "unknown"
// "hybrid discrete", "hybrid integrated", "not hybrid", "unknown"
const char* HybridRoleName(HybridRole role);

// Pure: the banner's per-adapter text, for example
// "hybrid discrete, HAGS supported and enabled, 0 outputs". Unknown parts
// read "hybrid role unknown (<reason>)" and "HAGS unknown (<reason>)".
std::string AdapterKmtText(const AdapterKmtInfo& info, unsigned outputs);

// D3DKMTOpenAdapterFromLuid, then D3DKMTQueryAdapterInfo with
// KMTQAITYPE_WDDM_2_7_CAPS (HAGS) and KMTQAITYPE_ADAPTERTYPE (hybrid role),
// then D3DKMTCloseAdapter. Never throws. A part that cannot be queried stays
// Unknown, its reason naming the failing call and the NTSTATUS (or why gdi32
// or its export is unavailable).
AdapterKmtInfo QueryAdapterKmt(const LUID& luid);

}  // namespace acdb
