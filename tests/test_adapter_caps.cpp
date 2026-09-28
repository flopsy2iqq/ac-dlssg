// AdapterCaps: per-adapter HAGS and hybrid-graphics role through D3DKMT.
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>

#include "adapter_caps.h"
#include "gpu_test_devices.h"
#include "test_framework.h"

using Microsoft::WRL::ComPtr;
using namespace acdb;

TEST(AdapterCaps_HagsFromWddm27CapsBits) {
    // Bit 0 HwSchSupported, bit 1 HwSchEnabled (D3DKMT_WDDM_2_7_CAPS).
    AdapterHags h = HagsFromWddm27Caps(0x0);
    CHECK(h.state == HagsState::Off);
    CHECK(!h.supported);
    CHECK(h.reason.empty());
    h = HagsFromWddm27Caps(0x1);
    CHECK(h.state == HagsState::Off);
    CHECK(h.supported);
    h = HagsFromWddm27Caps(0x3);
    CHECK(h.state == HagsState::On);
    CHECK(h.supported);
    // HwSchEnabledByDefault and IndependentVidPnVSyncControl do not count.
    h = HagsFromWddm27Caps(0xD);
    CHECK(h.state == HagsState::Off);
    CHECK(h.supported);
    h = HagsFromWddm27Caps(0xFFFFFFFF);
    CHECK(h.state == HagsState::On);
}

TEST(AdapterCaps_HybridRoleFromAdapterTypeBits) {
    // Bit 4 HybridDiscrete, bit 5 HybridIntegrated (D3DKMT_ADAPTERTYPE).
    CHECK(HybridRoleFromAdapterType(0x0) == HybridRole::Neither);
    CHECK(HybridRoleFromAdapterType(0x3) == HybridRole::Neither);   // render + display
    CHECK(HybridRoleFromAdapterType(0x11) == HybridRole::Discrete);  // render + hybrid discrete
    CHECK(HybridRoleFromAdapterType(0x23) == HybridRole::Integrated);
    CHECK(HybridRoleFromAdapterType(0x30) == HybridRole::Unknown);  // both: contradictory
    CHECK_EQ(std::string(HybridRoleName(HybridRole::Discrete)), "hybrid discrete");
    CHECK_EQ(std::string(HybridRoleName(HybridRole::Integrated)), "hybrid integrated");
    CHECK_EQ(std::string(HybridRoleName(HybridRole::Neither)), "not hybrid");
    CHECK_EQ(std::string(HybridRoleName(HybridRole::Unknown)), "unknown");
    CHECK_EQ(std::string(HagsStateName(HagsState::On)), "on");
    CHECK_EQ(std::string(HagsStateName(HagsState::Off)), "off");
    CHECK_EQ(std::string(HagsStateName(HagsState::Unknown)), "unknown");
}

TEST(AdapterCaps_LuidText) {
    LUID luid{};
    luid.LowPart = 0x140D9;
    luid.HighPart = 0;
    CHECK_EQ(LuidText(luid), "00000000:000140D9");
    luid.LowPart = 0xFFFFFFFF;
    luid.HighPart = -1;
    CHECK_EQ(LuidText(luid), "FFFFFFFF:FFFFFFFF");
}

TEST(AdapterCaps_UnknownLuidIsUnknownWithTheReason) {
    LUID bogus{};
    bogus.LowPart = 0xFFFFFFF0;
    bogus.HighPart = 0x7FFFFFF0;
    const AdapterKmtInfo info = QueryAdapterKmt(bogus);
    CHECK(info.hags.state == HagsState::Unknown);
    CHECK(info.hags.reason.find("D3DKMTOpenAdapterFromLuid") != std::string::npos);
    CHECK(info.hybrid == HybridRole::Unknown);
    CHECK(info.hybrid_reason.find("D3DKMTOpenAdapterFromLuid") != std::string::npos);
    std::printf("  %s\n", info.hags.reason.c_str());
}

// Every hardware adapter of a WDDM 2.7+ system (Windows 10 2004 and later)
// answers both queries; the result is printed for the record.
TEST(AdapterCaps_QueriesEveryDxgiAdapter) {
    ComPtr<IDXGIFactory1> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    int hardware = 0;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i, adapter.Reset()) {
        DXGI_ADAPTER_DESC1 d{};
        REQUIRE(SUCCEEDED(adapter->GetDesc1(&d)));
        const AdapterKmtInfo info = QueryAdapterKmt(d.AdapterLuid);
        std::printf("  adapter %u %ls (LUID %s): HAGS %s%s%s, supported %s; %s%s%s\n", i, d.Description,
                    LuidText(d.AdapterLuid).c_str(), HagsStateName(info.hags.state),
                    info.hags.reason.empty() ? "" : ": ", info.hags.reason.c_str(), info.hags.supported ? "yes" : "no",
                    HybridRoleName(info.hybrid), info.hybrid_reason.empty() ? "" : ": ", info.hybrid_reason.c_str());
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        ++hardware;
        CHECK(info.hags.state != HagsState::Unknown);
        CHECK(info.hybrid != HybridRole::Unknown);
    }
    if (hardware == 0) std::printf("  no hardware adapter; only printed\n");
}
