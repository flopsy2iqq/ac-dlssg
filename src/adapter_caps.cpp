#include "adapter_caps.h"

// NTSTATUS and OBJECT_ATTRIBUTES for the D3DKMT header.
#include <winternl.h>
#include <d3dkmthk.h>

#include <cstdio>

namespace acdb {
namespace {

constexpr uint32_t kHwSchSupported = 1u << 0;   // D3DKMT_WDDM_2_7_CAPS
constexpr uint32_t kHwSchEnabled = 1u << 1;
constexpr uint32_t kHybridDiscrete = 1u << 4;   // D3DKMT_ADAPTERTYPE
constexpr uint32_t kHybridIntegrated = 1u << 5;

struct KmtApi {
    PFND3DKMT_OPENADAPTERFROMLUID open = nullptr;
    PFND3DKMT_QUERYADAPTERINFO query = nullptr;
    PFND3DKMT_CLOSEADAPTER close = nullptr;
    std::string error;  // why the functions are not available
};

// gdi32.dll from System32 only; resolved once and never freed.
const KmtApi& Kmt() {
    static const KmtApi api = [] {
        KmtApi a;
        const HMODULE gdi = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!gdi) {
            a.error = "LoadLibraryExW(System32\\gdi32.dll) failed: error " + std::to_string(GetLastError());
            return a;
        }
        a.open = reinterpret_cast<PFND3DKMT_OPENADAPTERFROMLUID>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid"));
        a.query = reinterpret_cast<PFND3DKMT_QUERYADAPTERINFO>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"));
        a.close = reinterpret_cast<PFND3DKMT_CLOSEADAPTER>(GetProcAddress(gdi, "D3DKMTCloseAdapter"));
        if (!a.open || !a.query || !a.close) {
            a.error = "gdi32.dll does not export D3DKMTOpenAdapterFromLuid, D3DKMTQueryAdapterInfo and "
                      "D3DKMTCloseAdapter";
            a.open = nullptr;
        }
        return a;
    }();
    return api;
}

std::string NtFailure(const std::string& what, NTSTATUS status) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(status));
    return what + " failed: NTSTATUS " + buf;
}

}  // namespace

std::string LuidText(const LUID& luid) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%08lX:%08lX", static_cast<unsigned long>(luid.HighPart),
                  static_cast<unsigned long>(luid.LowPart));
    return buf;
}

AdapterHags HagsFromWddm27Caps(uint32_t capsValue) {
    AdapterHags h;
    h.state = (capsValue & kHwSchEnabled) ? HagsState::On : HagsState::Off;
    h.supported = (capsValue & kHwSchSupported) != 0;
    return h;
}

HybridRole HybridRoleFromAdapterType(uint32_t adapterTypeValue) {
    const bool discrete = (adapterTypeValue & kHybridDiscrete) != 0;
    const bool integrated = (adapterTypeValue & kHybridIntegrated) != 0;
    if (discrete && integrated) return HybridRole::Unknown;
    if (discrete) return HybridRole::Discrete;
    if (integrated) return HybridRole::Integrated;
    return HybridRole::Neither;
}

const char* HagsStateName(HagsState state) {
    switch (state) {
        case HagsState::On: return "on";
        case HagsState::Off: return "off";
        case HagsState::Unknown: return "unknown";
    }
    return "unknown";
}

const char* HybridRoleName(HybridRole role) {
    switch (role) {
        case HybridRole::Neither: return "not hybrid";
        case HybridRole::Discrete: return "hybrid discrete";
        case HybridRole::Integrated: return "hybrid integrated";
        case HybridRole::Unknown: return "unknown";
    }
    return "unknown";
}

AdapterKmtInfo QueryAdapterKmt(const LUID& luid) {
    AdapterKmtInfo info;
    const KmtApi& kmt = Kmt();
    if (!kmt.open) {
        info.hags.reason = info.hybrid_reason = kmt.error;
        return info;
    }

    D3DKMT_OPENADAPTERFROMLUID open{};
    open.AdapterLuid = luid;
    NTSTATUS status = kmt.open(&open);
    if (status < 0) {
        info.hags.reason = info.hybrid_reason = NtFailure("D3DKMTOpenAdapterFromLuid(" + LuidText(luid) + ")", status);
        return info;
    }

    D3DKMT_WDDM_2_7_CAPS caps{};
    D3DKMT_QUERYADAPTERINFO query{};
    query.hAdapter = open.hAdapter;
    query.Type = KMTQAITYPE_WDDM_2_7_CAPS;
    query.pPrivateDriverData = &caps;
    query.PrivateDriverDataSize = sizeof(caps);
    status = kmt.query(&query);
    if (status >= 0) {
        info.hags = HagsFromWddm27Caps(caps.Value);
    } else {
        info.hags.reason = NtFailure("D3DKMTQueryAdapterInfo(KMTQAITYPE_WDDM_2_7_CAPS)", status);
    }

    D3DKMT_ADAPTERTYPE type{};
    query.Type = KMTQAITYPE_ADAPTERTYPE;
    query.pPrivateDriverData = &type;
    query.PrivateDriverDataSize = sizeof(type);
    status = kmt.query(&query);
    if (status >= 0) {
        info.hybrid = HybridRoleFromAdapterType(type.Value);
        if (info.hybrid == HybridRole::Unknown) info.hybrid_reason = "both HybridDiscrete and HybridIntegrated are set";
    } else {
        info.hybrid_reason = NtFailure("D3DKMTQueryAdapterInfo(KMTQAITYPE_ADAPTERTYPE)", status);
    }

    D3DKMT_CLOSEADAPTER close{};
    close.hAdapter = open.hAdapter;
    kmt.close(&close);
    return info;
}

}  // namespace acdb
