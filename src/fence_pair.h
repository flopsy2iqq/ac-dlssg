#pragma once
// Shared D3D12/D3D11 fence plus the non-shared D3D12 progress fence
// (spec 6.4 "Fences").
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace acdb {

class FencePair {
public:
    // shared: ID3D12Device::CreateFence(0, D3D12_FENCE_FLAG_SHARED), shared
    // with CreateSharedHandle(GENERIC_ALL) and opened with
    // ID3D11Device5::OpenSharedFence; the handle is closed afterwards.
    // progress: CreateFence(0, D3D12_FENCE_FLAG_NONE). An auto-reset event is
    // created for CPU waits.
    static std::unique_ptr<FencePair> Create(ID3D12Device* device12, ID3D11Device* device11,
                                             std::string* error);
    ~FencePair();

    // Monotonic values from one counter; the first call returns 1.
    uint64_t Next();

    ID3D12Fence* Shared12() const { return shared12_.Get(); }
    ID3D11Fence* Shared11() const { return shared11_.Get(); }
    ID3D12Fence* Progress() const { return progress_.Get(); }

    uint64_t ProgressValue() const;  // progress->GetCompletedValue()
    // Waits until progress >= value or timeoutMs passes. True if reached.
    bool CpuWaitProgress(uint64_t value, DWORD timeoutMs);
    // ID3D12Fence::Signal(value) on the shared fence from the CPU.
    void CpuSignalShared(uint64_t value);

    // Highest value any ctx4->Wait has been issued for (spec 6.4 "Stall release").
    std::atomic<uint64_t> pending_wait{0};
    // Highest value submitted to the D3D12 queue for the progress fence.
    std::atomic<uint64_t> last_submitted{0};

private:
    FencePair() = default;
    Microsoft::WRL::ComPtr<ID3D12Fence> shared12_;
    Microsoft::WRL::ComPtr<ID3D11Fence> shared11_;
    Microsoft::WRL::ComPtr<ID3D12Fence> progress_;
    HANDLE event_ = nullptr;
    std::atomic<uint64_t> counter_{0};
};

}  // namespace acdb
