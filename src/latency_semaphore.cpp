#include "latency_semaphore.h"

namespace acdb {
namespace {

constexpr unsigned kMaxLatency = 16;

unsigned Clamp(unsigned n) { return n < 1 ? 1 : (n > kMaxLatency ? kMaxLatency : n); }

}  // namespace

LatencySemaphore::~LatencySemaphore() {
    if (sem_) CloseHandle(sem_);
}

bool LatencySemaphore::Init(bool enabled, unsigned configOverride) {
    std::lock_guard<std::mutex> lock(mu_);
    if (sem_) {
        CloseHandle(sem_);
        sem_ = nullptr;
    }
    enabled_ = enabled;
    fixed_ = configOverride != 0;
    latency_ = fixed_ ? Clamp(configOverride) : 1;
    swallow_ = 0;
    if (!enabled_) return true;
    sem_ = CreateSemaphoreW(nullptr, static_cast<LONG>(latency_), static_cast<LONG>(kMaxLatency), nullptr);
    if (!sem_) {
        enabled_ = false;
        return false;
    }
    return true;
}

bool LatencySemaphore::Enabled() const {
    std::lock_guard<std::mutex> lock(mu_);
    return enabled_;
}

HANDLE LatencySemaphore::DuplicateForCaller() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!enabled_ || !sem_) return nullptr;
    HANDLE out = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), sem_, GetCurrentProcess(), &out, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return nullptr;
    return out;
}

HRESULT LatencySemaphore::SetMaximum(UINT n) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!enabled_ || !sem_) return DXGI_ERROR_INVALID_CALL;
    if (fixed_) return S_OK;
    const unsigned target = Clamp(n);
    if (target > latency_) {
        unsigned diff = target - latency_;
        const unsigned cancel = diff < swallow_ ? diff : swallow_;
        swallow_ -= cancel;
        diff -= cancel;
        // One at a time: a batch release that would pass the maximum count
        // fails as a whole, and a game that never waits sits at the maximum.
        for (unsigned i = 0; i < diff; ++i) {
            if (!ReleaseSemaphore(sem_, 1, nullptr)) break;
        }
    } else if (target < latency_) {
        swallow_ += latency_ - target;
    }
    latency_ = target;
    return S_OK;
}

HRESULT LatencySemaphore::GetMaximum(UINT* n) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!enabled_ || !sem_ || !n) return DXGI_ERROR_INVALID_CALL;
    *n = latency_;
    return S_OK;
}

void LatencySemaphore::ReleaseOne() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!enabled_ || !sem_) return;
    if (swallow_ > 0) {
        --swallow_;
        return;
    }
    // Fails harmlessly at the maximum count, when the game does not wait.
    ReleaseSemaphore(sem_, 1, nullptr);
}

unsigned LatencySemaphore::CurrentLatency() const {
    std::lock_guard<std::mutex> lock(mu_);
    return latency_;
}

unsigned LatencySemaphore::PendingSwallow() const {
    std::lock_guard<std::mutex> lock(mu_);
    return swallow_;
}

}  // namespace acdb
