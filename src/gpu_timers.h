#pragma once
// Non-blocking GPU timing of the bridge's own work, for success criterion 3.
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

namespace acdb {

// D3D11: a ring of 4 {TIMESTAMP_DISJOINT, TIMESTAMP begin, TIMESTAMP end}
// query sets. Begin/End bracket the bridge's copy on CSP's context.
// Collect() reads finished sets with D3D11_ASYNC_GETDATA_DONOTFLUSH and never
// blocks; a set that is not ready is skipped (its slot is reused later).
class GpuTimer11 {
public:
    bool Init(ID3D11Device* device);
    void Begin(ID3D11DeviceContext* ctx);
    void End(ID3D11DeviceContext* ctx);
    // Adds finished measurements; returns how many were added.
    int Collect(ID3D11DeviceContext* ctx, double* sumMs);

private:
    static constexpr int kRing = 4;
    Microsoft::WRL::ComPtr<ID3D11Query> disjoint_[kRing], begin_[kRing], end_[kRing];
    bool in_flight_[kRing] = {};
    int index_ = 0;
    bool ok_ = false;
};

// D3D12: a timestamp query heap with 2 queries per frame slot and a readback
// buffer. Begin/End record timestamps on a command list; Resolve records the
// resolve for that slot; Read(slot) reads it after the slot's frame retired.
class GpuTimer12 {
public:
    bool Init(ID3D12Device* device, ID3D12CommandQueue* queue, UINT slots);
    void Begin(ID3D12GraphicsCommandList* list, UINT slot);
    void End(ID3D12GraphicsCommandList* list, UINT slot);  // also records the resolve
    // Call only after the frame that used slot has completed on the GPU.
    bool Read(UINT slot, double* ms);

private:
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
    uint64_t frequency_ = 0;
    UINT slots_ = 0;
    bool ok_ = false;
};

}  // namespace acdb
