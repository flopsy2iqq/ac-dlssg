#include <cstring>
#include <set>
#include <string>

#include "gpu_info.h"
#include "gpu_test_devices.h"
#include "test_framework.h"

using namespace acdb;

namespace {
constexpr uint32_t kNv = 0x10DE;
}

TEST(GpuInfo_Rtx3080IsAmpereAndSupportedRtx30) {
    CHECK(ArchFromIds(kNv, 0x2206) == GpuArch::Ampere);
    CHECK(IsRtx30From3070(kNv, 0x2206));
}

TEST(GpuInfo_Rtx3060IsAmpereButNotSupportedRtx30) {
    CHECK(ArchFromIds(kNv, 0x2503) == GpuArch::Ampere);
    CHECK(!IsRtx30From3070(kNv, 0x2503));
    // RTX 3060 Ti (GA103, GA104), workstation GA102 and a mining GA102.
    CHECK(!IsRtx30From3070(kNv, 0x2414));
    CHECK(!IsRtx30From3070(kNv, 0x2486));
    CHECK(!IsRtx30From3070(kNv, 0x2230));
    CHECK(!IsRtx30From3070(kNv, 0x220D));
}

TEST(GpuInfo_EveryRtx3070To3090TiId) {
    // Desktop, laptop and engineering-sample IDs named RTX 3070/3070 Ti/3080/
    // 3080 Ti/3090/3090 Ti in pci.ids 2026.09.25.
    const uint32_t ids[] = {0x2203, 0x2204, 0x2205, 0x2206, 0x2207, 0x2208, 0x220A, 0x2216, 0x222B,
                            0x222F, 0x2420, 0x2460, 0x2482, 0x2484, 0x2488, 0x248C, 0x248D, 0x249C,
                            0x249D, 0x24A0, 0x24AF, 0x24BF, 0x24C8, 0x24DC, 0x24DD, 0x24E0};
    for (uint32_t id : ids) {
        if (!IsRtx30From3070(kNv, id)) std::printf("  0x%04X not recognised\n", id);
        CHECK(IsRtx30From3070(kNv, id));
        CHECK(ArchFromIds(kNv, id) == GpuArch::Ampere);
    }
}

TEST(GpuInfo_OtherArchitectures) {
    CHECK(ArchFromIds(kNv, 0x1E02) == GpuArch::Turing);  // TITAN RTX, first Turing ID
    CHECK(ArchFromIds(kNv, 0x1E84) == GpuArch::Turing);  // RTX 2070 SUPER
    CHECK(ArchFromIds(kNv, 0x1F82) == GpuArch::Turing);  // GTX 1650 (TU117)
    CHECK(ArchFromIds(kNv, 0x2182) == GpuArch::Turing);  // GTX 1660 Ti (TU116)
    CHECK(ArchFromIds(kNv, 0x20B0) == GpuArch::Ampere);  // A100
    CHECK(ArchFromIds(kNv, 0x25A0) == GpuArch::Ampere);  // RTX 3050 Ti Mobile (GA107)
    CHECK(ArchFromIds(kNv, 0x2684) == GpuArch::Ada);     // RTX 4090
    CHECK(ArchFromIds(kNv, 0x2882) == GpuArch::Ada);     // RTX 4060 (AD107)
    CHECK(ArchFromIds(kNv, 0x2B85) == GpuArch::Blackwell);  // RTX 5090
    CHECK(ArchFromIds(kNv, 0x2C02) == GpuArch::Blackwell);  // RTX 5080
    CHECK(ArchFromIds(kNv, 0x2F04) == GpuArch::Blackwell);  // RTX 5070 (GB205)
    CHECK(ArchFromIds(kNv, 0x2D83) == GpuArch::Blackwell);  // GB207
    CHECK(!IsRtx30From3070(kNv, 0x2684));
    CHECK(!IsRtx30From3070(kNv, 0x1E84));
}

TEST(GpuInfo_OlderUnknownAndNonNvidia) {
    CHECK(ArchFromIds(kNv, 0x1B80) == GpuArch::OlderNvidia);  // GTX 1080
    CHECK(ArchFromIds(kNv, 0x1DB1) == GpuArch::OlderNvidia);  // V100
    CHECK(ArchFromIds(kNv, 0x1E01) == GpuArch::OlderNvidia);  // just below the first Turing ID
    CHECK(ArchFromIds(kNv, 0x0000) == GpuArch::OlderNvidia);
    CHECK(ArchFromIds(kNv, 0x2330) == GpuArch::Unknown);  // H100 (Hopper)
    CHECK(ArchFromIds(kNv, 0x22BA) == GpuArch::Unknown);  // an AD102 audio function
    CHECK(ArchFromIds(kNv, 0x3400) == GpuArch::Unknown);
    CHECK(ArchFromIds(kNv, 0xFFFF) == GpuArch::Unknown);
    CHECK(ArchFromIds(0x1002, 0x2206) == GpuArch::NonNvidia);
    CHECK(ArchFromIds(0x8086, 0x56A0) == GpuArch::NonNvidia);
    CHECK(ArchFromIds(0x1414, 0x008C) == GpuArch::NonNvidia);  // Microsoft Basic Render (WARP)
    CHECK(!IsRtx30From3070(0x1002, 0x2206));
}

TEST(GpuInfo_ArchNamesAreDistinct) {
    const GpuArch all[] = {GpuArch::Unknown, GpuArch::NonNvidia, GpuArch::OlderNvidia, GpuArch::Turing,
                           GpuArch::Ampere,  GpuArch::Ada,       GpuArch::Blackwell};
    std::set<std::string> names;
    for (GpuArch a : all) {
        const char* n = ArchName(a);
        REQUIRE(n != nullptr);
        CHECK(std::strlen(n) > 0);
        names.insert(n);
    }
    CHECK_EQ(names.size(), 7u);
    CHECK_EQ(std::string(ArchName(GpuArch::Ampere)), "Ampere");
}

TEST(GpuTestDevices_CreatesBothDevicesOnOneAdapter) {
    acdb_test::GpuTestDevices d;
    REQUIRE(acdb_test::CreateGpuTestDevices(&d));
    REQUIRE(d.factory && d.adapter && d.device11 && d.ctx11 && d.device12);
    CHECK(d.device11->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0);

    DXGI_ADAPTER_DESC1 desc{};
    REQUIRE(SUCCEEDED(d.adapter->GetDesc1(&desc)));
    CHECK(d.warp == ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0));

    const LUID luid12 = d.device12->GetAdapterLuid();
    CHECK(luid12.LowPart == desc.AdapterLuid.LowPart && luid12.HighPart == desc.AdapterLuid.HighPart);

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDev;
    REQUIRE(SUCCEEDED(d.device11.As(&dxgiDev)));
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter11;
    REQUIRE(SUCCEEDED(dxgiDev->GetAdapter(&adapter11)));
    DXGI_ADAPTER_DESC desc11{};
    REQUIRE(SUCCEEDED(adapter11->GetDesc(&desc11)));
    CHECK(desc11.AdapterLuid.LowPart == desc.AdapterLuid.LowPart &&
          desc11.AdapterLuid.HighPart == desc.AdapterLuid.HighPart);
    std::printf("  adapter: %ls (vendor 0x%04X device 0x%04X)%s\n", desc.Description, desc.VendorId,
                desc.DeviceId, d.warp ? " [WARP]" : "");
}

namespace {
int64_t Umd(unsigned a, unsigned b, unsigned c, unsigned d) {
    return static_cast<int64_t>((static_cast<uint64_t>(a) << 48) | (static_cast<uint64_t>(b) << 32) |
                                (static_cast<uint64_t>(c) << 16) | d);
}
}  // namespace

TEST(GpuInfo_DriverVersionText) {
    // The reference driver of success criterion 1.
    CHECK(DriverVersionText(kNv, Umd(32, 0, 16, 1664)) == "32.0.16.1664 (NVIDIA 616.64)");
    CHECK(DriverVersionText(kNv, Umd(32, 0, 15, 6094)) == "32.0.15.6094 (NVIDIA 560.94)");
    CHECK(DriverVersionText(kNv, Umd(31, 0, 15, 5222)) == "31.0.15.5222 (NVIDIA 552.22)");
    CHECK(DriverVersionText(kNv, Umd(32, 0, 16, 105)) == "32.0.16.105 (NVIDIA 601.05)");
    CHECK(DriverVersionText(0x1002, Umd(31, 0, 24033, 1003)) == "31.0.24033.1003");
}
