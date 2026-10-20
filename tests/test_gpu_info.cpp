#include <cstring>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>

#include "gpu_info.h"
#include "gpu_test_devices.h"
#include "temp_dir.h"
#include "test_framework.h"

using namespace acdb;

namespace {
constexpr uint32_t kNv = 0x10DE;
}

TEST(GpuInfo_Rtx3080IsSm86Ampere) {
    CHECK(ArchFromIds(kNv, 0x2206) == GpuArch::Ampere);
    CHECK(IsAmpereSm86(kNv, 0x2206));
}

// Every GeForce RTX 30 ID counts, not only the RTX 3070 and up: the laptop
// RTX 3050 Ti (0x25A0, GA107) of the second test machine included.
TEST(GpuInfo_EveryGeForceRtx30IdIsSm86) {
    // The RTX 3070 to 3090 Ti IDs (desktop, laptop and engineering samples)
    // that the old check listed, then RTX 3050 to 3060 Ti desktop and laptop
    // IDs of GA103, GA104, GA106 and GA107.
    const uint32_t ids[] = {0x2203, 0x2204, 0x2205, 0x2206, 0x2207, 0x2208, 0x220A, 0x2216, 0x222B, 0x222F,
                            0x2420, 0x2460, 0x2482, 0x2484, 0x2488, 0x248C, 0x248D, 0x249C, 0x249D, 0x24A0,
                            0x24AF, 0x24BF, 0x24C8, 0x24DC, 0x24DD, 0x24E0,
                            0x2414,   // RTX 3060 Ti (GA103)
                            0x2486,   // RTX 3060 Ti (GA104)
                            0x2503,   // RTX 3060 (GA106)
                            0x2507,   // RTX 3050 (GA106)
                            0x2520,   // RTX 3060 Laptop GPU (GA106)
                            0x2560,   // RTX 3060 Laptop GPU (GA106)
                            0x25A0,   // RTX 3050 Ti Laptop GPU (GA107), the Nitro 5 AN515-57
                            0x25A2,   // RTX 3050 Laptop GPU (GA107)
                            0x25E0};  // RTX 3050 Ti Laptop GPU (GA107)
    for (uint32_t id : ids) {
        if (!IsAmpereSm86(kNv, id)) std::printf("  0x%04X not recognised\n", id);
        CHECK(IsAmpereSm86(kNv, id));
        CHECK(ArchFromIds(kNv, id) == GpuArch::Ampere);
    }
}

// The check follows the GA102..GA107 ID ranges: workstation and mining chips
// of those dies are SM86 too and count; GA100 (SM80) and other architectures
// do not.
TEST(GpuInfo_Sm86FollowsTheGa10xRanges) {
    CHECK(IsAmpereSm86(kNv, 0x2200));   // first GA102 ID
    CHECK(IsAmpereSm86(kNv, 0x223F));   // last GA102 ID
    CHECK(IsAmpereSm86(kNv, 0x2230));   // RTX A6000 (GA102)
    CHECK(IsAmpereSm86(kNv, 0x220D));   // CMP 90HX (GA102)
    CHECK(IsAmpereSm86(kNv, 0x2414));   // first GA103 ID
    CHECK(IsAmpereSm86(kNv, 0x25FB));   // last GA107 ID
    CHECK(!IsAmpereSm86(kNv, 0x20B0));  // A100 (GA100, SM80)
    CHECK(!IsAmpereSm86(kNv, 0x2080));
    CHECK(!IsAmpereSm86(kNv, 0x20FF));
    CHECK(!IsAmpereSm86(kNv, 0x21FF));  // between TU116 and GA102
    CHECK(!IsAmpereSm86(kNv, 0x2240));  // after GA102
    CHECK(!IsAmpereSm86(kNv, 0x25FC));  // after GA107
    CHECK(!IsAmpereSm86(kNv, 0x2684));  // RTX 4090
    CHECK(!IsAmpereSm86(kNv, 0x1E84));  // RTX 2070 SUPER
    CHECK(!IsAmpereSm86(kNv, 0x2B85));  // RTX 5090
    CHECK(!IsAmpereSm86(0x1002, 0x2206));
    CHECK(!IsAmpereSm86(0x8086, 0x25A0));
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
}

// The RTX 2050 is a GA107 (SM86) despite its name: the ID decides, and a
// tester ran 2X, 3X and 4X on an RTX 2050 laptop through dlssg_for_sm86.
TEST(GpuInfo_Rtx2050IsAmpereSm86) {
    for (uint32_t id : {0x25A9u, 0x25ADu, 0x25EDu}) {
        CHECK(ArchFromIds(kNv, id) == GpuArch::Ampere);
        CHECK(IsAmpereSm86(kNv, id));
    }
    CHECK(SpoofGpuOf(kNv, 0x25A9, "NVIDIA GeForce RTX 2050 Laptop GPU") == SpoofGpu::AmpereSm86);
    CHECK(!IsTuringRtx(kNv, 0x25A9, "NVIDIA GeForce RTX 2050 Laptop GPU"));
}

// RTX 20 through dlssg_for_sm86 (its README: since 0.3.1, with the same
// dlssg_sm86.ini as RTX 30): a TU102, TU104 or TU106 ID (the Turing dies
// with tensor cores) whose adapter name has "RTX" in it.
TEST(GpuInfo_TuringRtxNeedsTheIdAndTheName) {
    CHECK(IsTuringRtx(kNv, 0x1F08, "NVIDIA GeForce RTX 2060"));                        // desktop
    CHECK(IsTuringRtx(kNv, 0x1F12, "NVIDIA GeForce RTX 2060 with Max-Q Design"));      // laptop
    CHECK(IsTuringRtx(kNv, 0x1E84, "NVIDIA GeForce RTX 2070 SUPER"));                  // desktop
    CHECK(IsTuringRtx(kNv, 0x1E91, "NVIDIA GeForce RTX 2070 Super with Max-Q Design")); // laptop
    CHECK(IsTuringRtx(kNv, 0x1E07, "NVIDIA GeForce RTX 2080 Ti"));
    CHECK(IsTuringRtx(kNv, 0x1E02, "NVIDIA TITAN RTX"));
    CHECK(IsTuringRtx(kNv, 0x1EB1, "NVIDIA Quadro RTX 4000"));
    CHECK(IsTuringRtx(kNv, 0x1E84, "nvidia geforce rtx 2070 super"));  // the name rule ignores case
    // GTX 16 on TU106: the name decides.
    CHECK(!IsTuringRtx(kNv, 0x1F09, "NVIDIA GeForce GTX 1660 SUPER"));
    CHECK(!IsTuringRtx(kNv, 0x1F0A, "NVIDIA GeForce GTX 1650"));
    // TU116 and TU117 (GTX 16, MX, T-series): the ID decides.
    CHECK(!IsTuringRtx(kNv, 0x1F82, "NVIDIA GeForce GTX 1650"));
    CHECK(!IsTuringRtx(kNv, 0x1F97, "NVIDIA GeForce MX450"));
    CHECK(!IsTuringRtx(kNv, 0x1FF0, "NVIDIA T1000 8GB"));
    CHECK(!IsTuringRtx(kNv, 0x21C4, "NVIDIA GeForce GTX 1660 SUPER"));
    CHECK(!IsTuringRtx(kNv, 0x1F82, "NVIDIA GeForce RTX 2050"));
    CHECK(!IsTuringRtx(kNv, 0x1E84, ""));
    CHECK(!IsTuringRtx(0x1002, 0x1E84, "RTX"));
    CHECK(!IsTuringRtx(kNv, 0x2206, "NVIDIA GeForce RTX 3080"));  // Ampere is not Turing
    CHECK(!IsTuringRtx(kNv, 0x2684, "NVIDIA GeForce RTX 4090"));
}

TEST(GpuInfo_SpoofGpuClassesAndLabels) {
    CHECK(SpoofGpuOf(kNv, 0x2206, "NVIDIA GeForce RTX 3080") == SpoofGpu::AmpereSm86);
    CHECK(SpoofGpuOf(kNv, 0x2206, "") == SpoofGpu::AmpereSm86);  // no name rule for Ampere
    CHECK(SpoofGpuOf(kNv, 0x1E84, "NVIDIA GeForce RTX 2070 SUPER") == SpoofGpu::TuringRtx);
    CHECK(SpoofGpuOf(kNv, 0x1F09, "NVIDIA GeForce GTX 1660 SUPER") == SpoofGpu::None);
    CHECK(SpoofGpuOf(kNv, 0x2684, "NVIDIA GeForce RTX 4090") == SpoofGpu::None);
    CHECK(SpoofGpuOf(kNv, 0x20B0, "NVIDIA A100") == SpoofGpu::None);
    CHECK_EQ(std::string(SpoofGpuLabel(SpoofGpu::AmpereSm86)), "RTX 30 or RTX 2050 (Ampere SM86)");
    CHECK_EQ(std::string(SpoofGpuLabel(SpoofGpu::TuringRtx)), "RTX 20 (experimental)");
    CHECK_EQ(std::string(SpoofGpuLabel(SpoofGpu::None)), "");
    // bridge.log's adapter lines.
    CHECK_EQ(std::string(SpoofGpuLogTag(SpoofGpu::AmpereSm86)), " (SM86)");
    CHECK_EQ(std::string(SpoofGpuLogTag(SpoofGpu::TuringRtx)), " (RTX 20 class, SM75: dlssg_for_sm86 is experimental there)");
    CHECK_EQ(std::string(SpoofGpuLogTag(SpoofGpu::None)), "");
}

namespace {
const char* ArchToken(GpuArch a) {
    switch (a) {
        case GpuArch::Unknown: return "Unknown";
        case GpuArch::NonNvidia: return "NonNvidia";
        case GpuArch::OlderNvidia: return "OlderNvidia";
        case GpuArch::Turing: return "Turing";
        case GpuArch::Ampere: return "Ampere";
        case GpuArch::Ada: return "Ada";
        case GpuArch::Blackwell: return "Blackwell";
    }
    return "?";
}
const char* SpoofGpuToken(SpoofGpu g) {
    switch (g) {
        case SpoofGpu::None: return "None";
        case SpoofGpu::AmpereSm86: return "AmpereSm86";
        case SpoofGpu::TuringRtx: return "TuringRtx";
    }
    return "?";
}
}  // namespace

// tests/gpu_cases.txt is checked here and by tools/test-dev-scripts.ps1
// SP0 against tools/dev-common.ps1, so the installer and the bridge classify
// every listed adapter the same way.
TEST(GpuInfo_SharedCasesFileMatchesTheBridge) {
    const std::string text = acdb_test::ReadAll(std::filesystem::path(ACDB_SOURCE_DIR) / "tests" / "gpu_cases.txt");
    REQUIRE(!text.empty());
    std::istringstream in(text);
    std::string line;
    int cases = 0, rtx20 = 0, none = 0, sm86 = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::string f[4];
        size_t start = 0;
        int n = 0;
        for (; n < 4; ++n) {
            const size_t bar = line.find('|', start);
            f[n] = line.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
            if (bar == std::string::npos) break;
            start = bar + 1;
        }
        REQUIRE(n == 3 && f[0].size() == 9 && f[0][4] == ':');
        const uint32_t vendor = std::stoul(f[0].substr(0, 4), nullptr, 16);
        const uint32_t device = std::stoul(f[0].substr(5, 4), nullptr, 16);
        const std::string arch = ArchToken(ArchFromIds(vendor, device));
        const SpoofGpu g = SpoofGpuOf(vendor, device, f[1]);
        const std::string cls = SpoofGpuToken(g);
        if (arch != f[2] || cls != f[3])
            std::printf("  %s '%s': %s %s, expected %s %s\n", f[0].c_str(), f[1].c_str(), arch.c_str(), cls.c_str(),
                        f[2].c_str(), f[3].c_str());
        CHECK_EQ(arch, f[2]);
        CHECK_EQ(cls, f[3]);
        CHECK_EQ(IsAmpereSm86(vendor, device), g == SpoofGpu::AmpereSm86);
        ++cases;
        rtx20 += g == SpoofGpu::TuringRtx;
        sm86 += g == SpoofGpu::AmpereSm86;
        none += g == SpoofGpu::None;
    }
    CHECK(cases >= 45);
    CHECK(rtx20 >= 20 && sm86 >= 5 && none >= 15);
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

TEST(GpuInfo_NvidiaDriverVersion) {
    CHECK_EQ(NvidiaDriverVersion(Umd(32, 0, 16, 1664)), 61664u);  // 616.64, the reference PC
    CHECK_EQ(NvidiaDriverVersion(Umd(32, 0, 16, 1088)), 61088u);  // 610.88, the Nitro 5 laptop
    CHECK_EQ(NvidiaDriverVersion(Umd(32, 0, 15, 8129)), 58129u);
    CHECK_EQ(NvidiaDriverVersion(Umd(31, 0, 15, 5222)), 55222u);
    CHECK_EQ(NvidiaDriverVersion(Umd(32, 0, 16, 105)), 60105u);
}

// Two separate thresholds: 581.29 (the Optimus degradation fix) and R580
// (dlssg_for_sm86's native cubins).
TEST(GpuInfo_NvidiaDriverWarnings) {
    const char* const kOptimus = "NVIDIA driver %s is older than 581.29, the release with the Optimus degradation fix";
    const char* const kR580 =
        "NVIDIA driver %s is older than R580; dlssg_for_sm86 needs R580 or newer for its native cubins";
    const auto text = [](const char* fmt, const char* version) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), fmt, version);
        return std::string(buf);
    };
    CHECK(NvidiaDriverWarnings(61664).empty());
    CHECK(NvidiaDriverWarnings(61088).empty());
    CHECK(NvidiaDriverWarnings(58129).empty());
    auto w = NvidiaDriverWarnings(58128);
    REQUIRE(w.size() == 1);
    CHECK_EQ(w[0], text(kOptimus, "581.28"));
    w = NvidiaDriverWarnings(58000);
    REQUIRE(w.size() == 1);
    CHECK_EQ(w[0], text(kOptimus, "580.00"));
    w = NvidiaDriverWarnings(57999);
    REQUIRE(w.size() == 2);
    CHECK_EQ(w[0], text(kOptimus, "579.99"));
    CHECK_EQ(w[1], text(kR580, "579.99"));
    w = NvidiaDriverWarnings(56094);
    REQUIRE(w.size() == 2);
    CHECK_EQ(w[1], text(kR580, "560.94"));
}
