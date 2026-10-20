#pragma once
// GPU architecture from PCI IDs (spec 6.10: never from NVAPI, which the
// dlssg_for_sm86 spoof rewrites).
#include <cstdint>
#include <string>
#include <vector>

namespace acdb {

enum class GpuArch { Unknown, NonNvidia, OlderNvidia, Turing, Ampere, Ada, Blackwell };

// vendorId 0x10DE is NVIDIA. deviceId ranges come from the public pci.ids
// database; anything NVIDIA not in a known range is OlderNvidia when it is
// below the first Turing ID and Unknown otherwise.
GpuArch ArchFromIds(uint32_t vendorId, uint32_t deviceId);
const char* ArchName(GpuArch arch);

// An SM86 Ampere chip (GA102 to GA107), the architecture dlssg_for_sm86
// targets: every GeForce RTX 30 ID, desktop and laptop, from the RTX 3050 up,
// the RTX 2050 (GA107 despite its name), and the workstation and mining IDs
// of the same dies. GA100 (SM80) is not.
bool IsAmpereSm86(uint32_t vendorId, uint32_t deviceId);

// An RTX-class Turing chip (SM75: GeForce RTX 20, TITAN RTX, Quadro RTX,
// desktop and laptop): a device ID of TU102, TU104 or TU106, the Turing dies
// with tensor cores, AND an adapter name that contains "RTX" (any case).
// The ID rule keeps out TU116 and TU117 (GTX 16, MX and T-series); the name
// rule keeps out the GTX cards built on TU106 (GTX 1660 SUPER 0x1F09, GTX 1650
// 0x1F0A) and the CMP 40HX. None of those has tensor cores or DLSS.
bool IsTuringRtx(uint32_t vendorId, uint32_t deviceId, const std::string& adapterName);

// What the render GPU is for dlssg_for_sm86. Its README: SM86 Ampere, and
// since 0.3.1 also RTX 20 (Turing) with the same stock dlssg_sm86.ini as RTX
// 30, tested by its author on an RTX 2080 Ti; ac-dlssg calls RTX 20
// experimental. tools\dev-common.ps1 (New-GpuAdapter) classifies the same
// way; tests/gpu_cases.txt holds the cases both sides check.
enum class SpoofGpu { None = 0, AmpereSm86 = 1, TuringRtx = 2 };
SpoofGpu SpoofGpuOf(uint32_t vendorId, uint32_t deviceId, const std::string& adapterName);

// "RTX 30 or RTX 2050 (Ampere SM86)", "RTX 20 (experimental)", or "" for None.
const char* SpoofGpuLabel(SpoofGpu gpu);

// What bridge.log's adapter line adds: " (SM86)", " (RTX 20 class, SM75:
// dlssg_for_sm86 is experimental there)", or "" for None.
const char* SpoofGpuLogTag(SpoofGpu gpu);

// The user-mode driver version from IDXGIAdapter::CheckInterfaceSupport as
// "a.b.c.d"; for NVIDIA also the marketing version, which is the last five
// digits: "32.0.16.1664 (NVIDIA 616.64)".
std::string DriverVersionText(uint32_t vendorId, int64_t umdVersion);

// NVIDIA's marketing version times 100 from the same user-mode driver
// version: 32.0.16.1664 gives 61664 (616.64).
unsigned NvidiaDriverVersion(int64_t umdVersion);

// Pure: one warning per threshold the NVIDIA driver version (as from
// NvidiaDriverVersion) is below, empty when none:
//  - below 581.29: "NVIDIA driver <v> is older than 581.29, the release with the Optimus degradation fix"
//  - below R580:   "NVIDIA driver <v> is older than R580; dlssg_for_sm86 needs R580 or newer for its native cubins"
std::vector<std::string> NvidiaDriverWarnings(unsigned version);

}  // namespace acdb
