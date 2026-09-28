#include "fence_pair.h"

#include <cstdio>
#include <new>

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace acdb {
namespace {

// Upper bound for one wait on the shared auto-reset event. Two threads waiting
// at once can consume each other's wake-up; the slice bounds that delay.
constexpr DWORD kWaitSliceMs = 10;

std::string Failure(const char* what, HRESULT hr) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s failed: 0x%08lX", what, static_cast<unsigned long>(hr));
    return buf;
}

}  // namespace

std::unique_ptr<FencePair> FencePair::Create(ID3D12Device* device12, ID3D11Device* device11,
                                             std::string* error) {
    std::string err;
    std::unique_ptr<FencePair> result;
    try {
        if (!device12 || !device11) {
            err = "FencePair: null device";
        } else {
            std::unique_ptr<FencePair> p(new (std::nothrow) FencePair());
            if (!p) {
                err = "FencePair: out of memory";
            } else {
                HRESULT hr = device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&p->shared12_));
                if (FAILED(hr)) err = Failure("ID3D12Device::CreateFence(SHARED)", hr);

                ComPtr<ID3D11Device5> device5;
                if (err.empty()) {
                    hr = device11->QueryInterface(IID_PPV_ARGS(&device5));
                    if (FAILED(hr)) err = Failure("QueryInterface(ID3D11Device5)", hr);
                }

                HANDLE shared = nullptr;
                if (err.empty()) {
                    hr = device12->CreateSharedHandle(p->shared12_.Get(), nullptr, GENERIC_ALL, nullptr, &shared);
                    if (FAILED(hr)) err = Failure("ID3D12Device::CreateSharedHandle(fence)", hr);
                }
                if (err.empty()) {
                    hr = device5->OpenSharedFence(shared, IID_PPV_ARGS(&p->shared11_));
                    if (FAILED(hr)) err = Failure("ID3D11Device5::OpenSharedFence", hr);
                }
                if (shared) CloseHandle(shared);

                if (err.empty()) {
                    hr = device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p->progress_));
                    if (FAILED(hr)) err = Failure("ID3D12Device::CreateFence(progress)", hr);
                }
                if (err.empty()) {
                    p->event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    if (!p->event_) err = Failure("CreateEventW", HRESULT_FROM_WIN32(GetLastError()));
                }
                if (err.empty()) result = std::move(p);
            }
        }
    } catch (...) {
        result.reset();
        err = "FencePair: exception during creation";
    }
    if (!result && error) {
        try {
            *error = err;
        } catch (...) {
        }
    }
    return result;
}

FencePair::~FencePair() {
    if (event_) CloseHandle(event_);
}

uint64_t FencePair::Next() { return counter_.fetch_add(1) + 1; }

uint64_t FencePair::ProgressValue() const { return progress_ ? progress_->GetCompletedValue() : 0; }

bool FencePair::CpuWaitProgress(uint64_t value, DWORD timeoutMs) {
    if (!progress_ || !event_) return false;
    if (progress_->GetCompletedValue() >= value) return true;
    if (FAILED(progress_->SetEventOnCompletion(value, event_))) return progress_->GetCompletedValue() >= value;

    const ULONGLONG start = GetTickCount64();
    for (;;) {
        // Checked before every wait: the auto-reset event may have been
        // consumed by a stale registration or by another waiting thread.
        if (progress_->GetCompletedValue() >= value) return true;
        DWORD slice = kWaitSliceMs;
        if (timeoutMs != INFINITE) {
            const ULONGLONG elapsed = GetTickCount64() - start;
            if (elapsed >= timeoutMs) return false;
            const ULONGLONG left = timeoutMs - elapsed;
            if (left < slice) slice = static_cast<DWORD>(left);
        }
        WaitForSingleObject(event_, slice);
    }
}

void FencePair::CpuSignalShared(uint64_t value) {
    if (!shared12_ || value == 0) return;
    // Never move the fence backwards: GPU waits already satisfied by a higher
    // value must stay satisfied. After device removal it reads UINT64_MAX.
    if (shared12_->GetCompletedValue() >= value) return;
    const HRESULT hr = shared12_->Signal(value);
    if (FAILED(hr))
        LOGW("FencePair: CPU signal of the shared fence to %llu failed: 0x%08lX",
             static_cast<unsigned long long>(value), static_cast<unsigned long>(hr));
}

}  // namespace acdb
