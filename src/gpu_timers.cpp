#include "gpu_timers.h"

namespace acdb {

// ---------------------------------------------------------------- GpuTimer11

bool GpuTimer11::Init(ID3D11Device* device) {
    ok_ = false;
    index_ = 0;
    for (int i = 0; i < kRing; ++i) {
        disjoint_[i].Reset();
        begin_[i].Reset();
        end_[i].Reset();
        in_flight_[i] = false;
    }
    if (!device) return false;

    D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    D3D11_QUERY_DESC stamp{D3D11_QUERY_TIMESTAMP, 0};
    for (int i = 0; i < kRing; ++i) {
        if (FAILED(device->CreateQuery(&disjoint, &disjoint_[i])) || FAILED(device->CreateQuery(&stamp, &begin_[i])) ||
            FAILED(device->CreateQuery(&stamp, &end_[i])))
            return false;
    }
    ok_ = true;
    return true;
}

void GpuTimer11::Begin(ID3D11DeviceContext* ctx) {
    if (!ok_ || !ctx) return;
    // An unread set in this slot is dropped: Collect never waits for it.
    in_flight_[index_] = false;
    ctx->Begin(disjoint_[index_].Get());
    ctx->End(begin_[index_].Get());
}

void GpuTimer11::End(ID3D11DeviceContext* ctx) {
    if (!ok_ || !ctx) return;
    ctx->End(end_[index_].Get());
    ctx->End(disjoint_[index_].Get());
    in_flight_[index_] = true;
    index_ = (index_ + 1) % kRing;
}

int GpuTimer11::Collect(ID3D11DeviceContext* ctx, double* sumMs) {
    if (!ok_ || !ctx) return 0;
    int added = 0;
    for (int i = 0; i < kRing; ++i) {
        if (!in_flight_[i]) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        if (ctx->GetData(disjoint_[i].Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        UINT64 t0 = 0, t1 = 0;
        if (ctx->GetData(begin_[i].Get(), &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ctx->GetData(end_[i].Get(), &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        in_flight_[i] = false;
        // A disjoint interval (clock change) makes the timestamps meaningless.
        if (disjoint.Disjoint || disjoint.Frequency == 0 || t1 < t0) continue;
        if (sumMs) *sumMs += static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(disjoint.Frequency);
        ++added;
    }
    return added;
}

// ---------------------------------------------------------------- GpuTimer12

bool GpuTimer12::Init(ID3D12Device* device, ID3D12CommandQueue* queue, UINT slots) {
    ok_ = false;
    heap_.Reset();
    readback_.Reset();
    frequency_ = 0;
    slots_ = 0;
    if (!device || !queue || slots == 0) return false;

    UINT64 frequency = 0;
    if (FAILED(queue->GetTimestampFrequency(&frequency)) || frequency == 0) return false;

    D3D12_QUERY_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heapDesc.Count = slots * 2;
    if (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&heap_)))) return false;

    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(slots) * 2 * sizeof(uint64_t);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&readback_)))) {
        heap_.Reset();
        return false;
    }
    frequency_ = frequency;
    slots_ = slots;
    ok_ = true;
    return true;
}

void GpuTimer12::Begin(ID3D12GraphicsCommandList* list, UINT slot) {
    if (!ok_ || !list || slot >= slots_) return;
    list->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
}

void GpuTimer12::End(ID3D12GraphicsCommandList* list, UINT slot) {
    if (!ok_ || !list || slot >= slots_) return;
    list->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
    list->ResolveQueryData(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, readback_.Get(),
                           static_cast<UINT64>(slot) * 2 * sizeof(uint64_t));
}

bool GpuTimer12::Read(UINT slot, double* ms) {
    if (!ok_ || !ms || slot >= slots_) return false;
    const SIZE_T offset = static_cast<SIZE_T>(slot) * 2 * sizeof(uint64_t);
    const D3D12_RANGE readRange{offset, offset + 2 * sizeof(uint64_t)};
    void* data = nullptr;
    if (FAILED(readback_->Map(0, &readRange, &data)) || !data) return false;
    const auto* stamps = reinterpret_cast<const uint64_t*>(static_cast<const unsigned char*>(data) + offset);
    const uint64_t t0 = stamps[0];
    const uint64_t t1 = stamps[1];
    const D3D12_RANGE written{0, 0};
    readback_->Unmap(0, &written);
    // Zeros: the slot was never resolved (committed resources start zeroed).
    if ((t0 == 0 && t1 == 0) || t1 < t0) return false;
    *ms = static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(frequency_);
    return true;
}

}  // namespace acdb
