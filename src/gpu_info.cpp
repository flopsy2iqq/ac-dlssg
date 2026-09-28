#include "gpu_info.h"

#include <cstdio>

namespace acdb {
namespace {

// Device IDs from the PCI ID Repository, https://pci-ids.ucw.cz/v2.2/pci.ids,
// version 2026.09.25 (fetched 2026-09-28), vendor 10de. Each range spans the
// lowest to the highest GPU-function ID of the listed chips (HD Audio, USB
// and NVSwitch functions excluded); adjacent chips of one architecture were
// merged only where no other NVIDIA ID lies between them.
struct IdRange {
    uint32_t first;
    uint32_t last;
    GpuArch arch;
};

// GA100 (A100) is SM80; every other Ampere chip below is SM86.
constexpr uint32_t kGa100First = 0x2080;
constexpr uint32_t kGa100Last = 0x20FF;

constexpr IdRange kRanges[] = {
    {0x1E02, 0x1FF9, GpuArch::Turing},     // TU102, TU104, TU106, TU117
    {kGa100First, kGa100Last, GpuArch::Ampere},  // GA100
    {0x2182, 0x21D1, GpuArch::Turing},     // TU116
    {0x2200, 0x223F, GpuArch::Ampere},     // GA102
    {0x2414, 0x25FB, GpuArch::Ampere},     // GA103, GA104, GA106, GA107
    {0x2681, 0x28F8, GpuArch::Ada},        // AD102, AD103, AD104, AD106, AD107
    {0x2B85, 0x2DF9, GpuArch::Blackwell},  // GB202, GB203, GB206, GB207
    {0x2F04, 0x2F58, GpuArch::Blackwell},  // GB205
};

// The lowest Turing ID (TU102, TITAN RTX). NVIDIA IDs below it are older chips.
constexpr uint32_t kFirstTuringId = 0x1E02;

constexpr uint32_t kNvidia = 0x10DE;

// Driver thresholds, as NvidiaDriverVersion values.
constexpr unsigned kOptimusFixDriver = 58129;  // 581.29
constexpr unsigned kR580Driver = 58000;        // R580, the first branch dlssg_for_sm86 supports

}  // namespace

GpuArch ArchFromIds(uint32_t vendorId, uint32_t deviceId) {
    if (vendorId != kNvidia) return GpuArch::NonNvidia;
    for (const auto& r : kRanges) {
        if (deviceId >= r.first && deviceId <= r.last) return r.arch;
    }
    return deviceId < kFirstTuringId ? GpuArch::OlderNvidia : GpuArch::Unknown;
}

const char* ArchName(GpuArch arch) {
    switch (arch) {
        case GpuArch::Unknown: return "Unknown";
        case GpuArch::NonNvidia: return "Non-NVIDIA";
        case GpuArch::OlderNvidia: return "Older NVIDIA";
        case GpuArch::Turing: return "Turing";
        case GpuArch::Ampere: return "Ampere";
        case GpuArch::Ada: return "Ada";
        case GpuArch::Blackwell: return "Blackwell";
    }
    return "Unknown";
}

bool IsAmpereSm86(uint32_t vendorId, uint32_t deviceId) {
    return ArchFromIds(vendorId, deviceId) == GpuArch::Ampere && (deviceId < kGa100First || deviceId > kGa100Last);
}

std::string DriverVersionText(uint32_t vendorId, int64_t umdVersion) {
    const auto v = static_cast<uint64_t>(umdVersion);
    const unsigned a = static_cast<unsigned>((v >> 48) & 0xFFFF);
    const unsigned b = static_cast<unsigned>((v >> 32) & 0xFFFF);
    const unsigned c = static_cast<unsigned>((v >> 16) & 0xFFFF);
    const unsigned d = static_cast<unsigned>(v & 0xFFFF);
    char buf[64];
    int n = std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", a, b, c, d);
    if (vendorId == kNvidia && n > 0) {
        const unsigned nv = NvidiaDriverVersion(umdVersion);
        std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " (NVIDIA %u.%02u)", nv / 100, nv % 100);
    }
    return buf;
}

unsigned NvidiaDriverVersion(int64_t umdVersion) {
    const auto v = static_cast<uint64_t>(umdVersion);
    const unsigned c = static_cast<unsigned>((v >> 16) & 0xFFFF);
    const unsigned d = static_cast<unsigned>(v & 0xFFFF);
    return (c % 10) * 10000 + d;
}

std::vector<std::string> NvidiaDriverWarnings(unsigned version) {
    char v[24];
    std::snprintf(v, sizeof(v), "%u.%02u", version / 100, version % 100);
    std::vector<std::string> warnings;
    if (version < kOptimusFixDriver)
        warnings.push_back(std::string("NVIDIA driver ") + v +
                           " is older than 581.29, the release with the Optimus degradation fix");
    if (version < kR580Driver)
        warnings.push_back(std::string("NVIDIA driver ") + v +
                           " is older than R580; dlssg_for_sm86 needs R580 or newer for its native cubins");
    return warnings;
}

}  // namespace acdb
