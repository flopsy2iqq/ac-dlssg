#pragma once
// GPU architecture from PCI IDs (spec 6.10: never from NVAPI, which the
// dlssg_for_sm86 spoof rewrites).
#include <cstdint>
#include <string>

namespace acdb {

enum class GpuArch { Unknown, NonNvidia, OlderNvidia, Turing, Ampere, Ada, Blackwell };

// vendorId 0x10DE is NVIDIA. deviceId ranges come from the public pci.ids
// database; anything NVIDIA not in a known range is OlderNvidia when it is
// below the first Turing ID and Unknown otherwise.
GpuArch ArchFromIds(uint32_t vendorId, uint32_t deviceId);
const char* ArchName(GpuArch arch);

// RTX 3070, 3070 Ti, 3080, 3080 Ti, 3090, 3090 Ti (desktop and laptop IDs),
// the RTX 30 cards the spoof supports.
bool IsRtx30From3070(uint32_t vendorId, uint32_t deviceId);

// The user-mode driver version from IDXGIAdapter::CheckInterfaceSupport as
// "a.b.c.d"; for NVIDIA also the marketing version, which is the last five
// digits: "32.0.16.1664 (NVIDIA 616.64)".
std::string DriverVersionText(uint32_t vendorId, int64_t umdVersion);

}  // namespace acdb
