#pragma once
// Emulated frame-latency waitable object (spec 6.3 "Frame latency").
#include <windows.h>

#include <mutex>

namespace acdb {

class LatencySemaphore {
public:
    LatencySemaphore() = default;
    ~LatencySemaphore();
    LatencySemaphore(const LatencySemaphore&) = delete;
    LatencySemaphore& operator=(const LatencySemaphore&) = delete;

    // enabled: the game's desc has DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT.
    // configOverride: 0 = unset; 1..16 = fixed latency from the config, which
    // then ignores the game's SetMaximum calls (they still succeed).
    // Creates CreateSemaphoreW(nullptr, L, 16, nullptr) with L = the override
    // or DXGI's default of 1. Returns false if the semaphore cannot be created.
    bool Init(bool enabled, unsigned configOverride);

    bool Enabled() const;

    // NULL when disabled. Otherwise a new DuplicateHandle of the semaphore,
    // owned (and closed) by the caller.
    HANDLE DuplicateForCaller() const;

    // Disabled: DXGI_ERROR_INVALID_CALL. Otherwise clamps n to 1..16.
    // n > L first cancels up to n - L pending swallows and releases the rest
    // of n - L at once (releasing while swallows are pending would let more
    // than n frames queue and can overflow the semaphore's maximum of 16);
    // n < L adds L - n to the releases that ReleaseOne will swallow. With a
    // config override the call only returns S_OK.
    HRESULT SetMaximum(UINT n);
    // Disabled: DXGI_ERROR_INVALID_CALL. Otherwise writes L.
    HRESULT GetMaximum(UINT* n) const;

    // Called once per non-test Present on every return path. Swallows one
    // pending release if any, otherwise ReleaseSemaphore(+1). No-op when
    // disabled.
    void ReleaseOne();

    // For tests.
    unsigned CurrentLatency() const;
    unsigned PendingSwallow() const;
    HANDLE RawHandleForTests() const { return sem_; }

private:
    mutable std::mutex mu_;
    HANDLE sem_ = nullptr;
    bool enabled_ = false;
    bool fixed_ = false;
    unsigned latency_ = 1;
    unsigned swallow_ = 0;
};

}  // namespace acdb
