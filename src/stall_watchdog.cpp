#include "stall_watchdog.h"

#include <chrono>

#include "log.h"

namespace acdb {

StallWatchdog::StallWatchdog(FencePair* fences, ID3D12Device* device, DWORD periodMs, DWORD stallMs)
    : fences_(fences), device_(device), period_ms_(periodMs ? periodMs : 1), stall_ms_(stallMs) {
    last_advance_tick_.store(GetTickCount64());
}

StallWatchdog::~StallWatchdog() { Stop(); }

void StallWatchdog::Start() {
    if (thread_.joinable() || !fences_) return;
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = false;
    }
    last_progress_ = fences_->ProgressValue();
    last_advance_tick_.store(GetTickCount64());
    try {
        thread_ = std::thread(&StallWatchdog::Run, this);
    } catch (...) {
        LOGE("StallWatchdog: could not start the watchdog thread");
    }
}

void StallWatchdog::Stop() {
    try {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
    } catch (...) {
        // join() only throws for invalid states, which the checks above exclude.
    }
}

DWORD StallWatchdog::MsSinceProgress() const {
    const ULONGLONG ms = GetTickCount64() - last_advance_tick_.load();
    return ms > MAXDWORD ? MAXDWORD : static_cast<DWORD>(ms);
}

void StallWatchdog::Run() {
    try {
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mu_);
                if (cv_.wait_for(lock, std::chrono::milliseconds(period_ms_), [this] { return stop_; })) return;
            }

            const uint64_t progress = fences_->ProgressValue();
            const uint64_t pending = fences_->pending_wait.load();
            const uint64_t submitted = fences_->last_submitted.load();
            const ULONGLONG now = GetTickCount64();
            if (progress != last_progress_) {
                last_progress_ = progress;
                last_advance_tick_.store(now);
            } else if (progress >= pending && progress >= submitted) {
                // Nothing is outstanding, so no progress is expected. Without
                // this, a pause between frames would count as a stall as soon
                // as the next frame raises pending_wait.
                last_advance_tick_.store(now);
            }

            const HRESULT removed = device_ ? device_->GetDeviceRemovedReason() : S_OK;
            if (removed != S_OK) {
                if (!device_removed_.load()) {
                    LOGE("StallWatchdog: D3D12 device removed (reason 0x%08lX); releasing the D3D11 queue at %llu",
                         static_cast<unsigned long>(removed), static_cast<unsigned long long>(pending));
                    fences_->CpuSignalShared(pending);
                    released_value_ = pending;
                    device_removed_.store(true);
                    stall_pending_.store(true);
                } else if (pending > released_value_) {
                    fences_->CpuSignalShared(pending);
                    released_value_ = pending;
                    stall_pending_.store(true);
                }
                continue;
            }

            const ULONGLONG stuck = now - last_advance_tick_.load();
            if (progress < pending && stuck >= stall_ms_ && pending > released_value_) {
                LOGW("StallWatchdog: D3D12 progress stuck at %llu for %llu ms (pending wait %llu); "
                     "CPU-signalling the shared fence",
                     static_cast<unsigned long long>(progress), static_cast<unsigned long long>(stuck),
                     static_cast<unsigned long long>(pending));
                fences_->CpuSignalShared(pending);
                released_value_ = pending;
                stall_pending_.store(true);
            }
        }
    } catch (...) {
        LOGE("StallWatchdog: unexpected exception; the watchdog thread stops");
    }
}

}  // namespace acdb
