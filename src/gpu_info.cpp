#include "gpu_info.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

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

constexpr IdRange kRanges[] = {
    {0x1E02, 0x1FF9, GpuArch::Turing},     // TU102, TU104, TU106, TU117
    {0x2080, 0x20FF, GpuArch::Ampere},     // GA100
    {0x2182, 0x21D1, GpuArch::Turing},     // TU116
    {0x2200, 0x223F, GpuArch::Ampere},     // GA102
    {0x2414, 0x25FB, GpuArch::Ampere},     // GA103, GA104, GA106, GA107
    {0x2681, 0x28F8, GpuArch::Ada},        // AD102, AD103, AD104, AD106, AD107
    {0x2B85, 0x2DF9, GpuArch::Blackwell},  // GB202, GB203, GB206, GB207
    {0x2F04, 0x2F58, GpuArch::Blackwell},  // GB205
};

// The lowest Turing ID (TU102, TITAN RTX). NVIDIA IDs below it are older chips.
constexpr uint32_t kFirstTuringId = 0x1E02;

// Every ID the same pci.ids version names GeForce RTX 3070, 3070 Ti, 3080,
// 3080 Ti, 3090 or 3090 Ti, including laptop and engineering-sample IDs.
constexpr uint32_t kRtx30From3070[] = {
    0x2203,  // GA102 RTX 3090 Ti
    0x2204,  // GA102 RTX 3090
    0x2205,  // GA102 RTX 3080 Ti 20GB
    0x2206,  // GA102 RTX 3080
    0x2207,  // GA102 RTX 3070 Ti
    0x2208,  // GA102 RTX 3080 Ti
    0x220A,  // GA102 RTX 3080 12GB
    0x2216,  // GA102 RTX 3080 Lite Hash Rate
    0x222B,  // GA102 RTX 3090 Engineering Sample
    0x222F,  // GA102 RTX 3080 11GB / 12GB Engineering Sample
    0x2420,  // GA103M RTX 3080 Ti Mobile
    0x2460,  // GA103M RTX 3080 Ti Laptop GPU
    0x2482,  // GA104 RTX 3070 Ti
    0x2484,  // GA104 RTX 3070
    0x2488,  // GA104 RTX 3070 Lite Hash Rate
    0x248C,  // GA104 RTX 3070 Ti
    0x248D,  // GA104 RTX 3070
    0x249C,  // GA104M RTX 3080 Mobile / Max-Q 8GB/16GB
    0x249D,  // GA104M RTX 3070 Mobile / Max-Q
    0x24A0,  // GA104 RTX 3070 Ti Laptop GPU
    0x24AF,  // GA104 RTX 3070 Engineering Sample
    0x24BF,  // GA104 RTX 3070 Engineering Sample
    0x24C8,  // GA104 RTX 3070 GDDR6X
    0x24DC,  // GA104M RTX 3080 Mobile / Max-Q 8GB/16GB
    0x24DD,  // GA104M RTX 3070 Mobile / Max-Q
    0x24E0,  // GA104M RTX 3070 Ti Laptop GPU
};

constexpr uint32_t kNvidia = 0x10DE;

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

bool IsRtx30From3070(uint32_t vendorId, uint32_t deviceId) {
    if (vendorId != kNvidia) return false;
    return std::find(std::begin(kRtx30From3070), std::end(kRtx30From3070), deviceId) != std::end(kRtx30From3070);
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
        const unsigned nv = (c % 10) * 10000 + d;
        std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " (NVIDIA %u.%02u)", nv / 100, nv % 100);
    }
    return buf;
}

}  // namespace acdb
