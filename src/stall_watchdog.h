#pragma once
// Watchdog thread that releases CSP's D3D11 queue when the D3D12 side stops
// making progress (spec 6.4 "Stall release"). It only reads fence values,
// reads the device-removed reason and CPU-signals the shared fence.
#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "fence_pair.h"

namespace acdb {

class StallWatchdog {
public:
    StallWatchdog(FencePair* fences, ID3D12Device* device, DWORD periodMs = 100, DWORD stallMs = 500);
    ~StallWatchdog();  // Stop()
    StallWatchdog(const StallWatchdog&) = delete;
    StallWatchdog& operator=(const StallWatchdog&) = delete;

    void Start();
    void Stop();  // joins; idempotent

    // Every period: read progress. If it advanced, remember the time. If
    // progress < pending_wait and progress has not advanced for stallMs, and
    // pending_wait has not been released yet: CpuSignalShared(pending_wait),
    // set stall_pending, log once per released value. If
    // GetDeviceRemovedReason() != S_OK: CpuSignalShared(pending_wait), set
    // device_removed and stall_pending.
    bool TakeStallPending() { return stall_pending_.exchange(false); }
    bool DeviceRemoved() const { return device_removed_.load(); }
    // Milliseconds since progress last advanced (for the 4 s rule in the presenter).
    DWORD MsSinceProgress() const;

private:
    void Run();
    FencePair* fences_;
    ID3D12Device* device_;
    DWORD period_ms_, stall_ms_;
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::atomic<bool> stall_pending_{false};
    std::atomic<bool> device_removed_{false};
    std::atomic<ULONGLONG> last_advance_tick_{0};
    uint64_t last_progress_ = 0;
    uint64_t released_value_ = 0;
};

}  // namespace acdb
