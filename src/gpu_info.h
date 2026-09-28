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

// An SM86 Ampere chip (GA102 to GA107), the architecture dlssg_for_sm86
// targets: every GeForce RTX 30 ID, desktop and laptop, from the RTX 3050 up,
// and the workstation and mining IDs of the same dies. GA100 (SM80) is not.
bool IsAmpereSm86(uint32_t vendorId, uint32_t deviceId);

// The user-mode driver version from IDXGIAdapter::CheckInterfaceSupport as
// "a.b.c.d"; for NVIDIA also the marketing version, which is the last five
// digits: "32.0.16.1664 (NVIDIA 616.64)".
std::string DriverVersionText(uint32_t vendorId, int64_t umdVersion);

}  // namespace acdb
