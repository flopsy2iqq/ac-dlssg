// FencePair and GpuTimer11/12 against real devices (hardware adapter, else
// WARP). Each GPU test skips with a note when no adapter creates both devices.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "fence_pair.h"
#include "gpu_test_devices.h"
#include "gpu_timers.h"
#include "test_framework.h"

using Microsoft::WRL::ComPtr;
using namespace acdb;

namespace {

bool GetDevices(acdb_test::GpuTestDevices* d) {
    if (acdb_test::CreateGpuTestDevices(d)) return true;
    std::printf("  SKIP: no adapter creates both a D3D11 and a D3D12 device\n");
    return false;
}

ComPtr<ID3D12CommandQueue> MakeQueue(ID3D12Device* device) {
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)))) return nullptr;
    return queue;
}

ComPtr<ID3D11Query> MakeEventQuery(ID3D11Device* device) {
    D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> query;
    if (FAILED(device->CreateQuery(&desc, &query))) return nullptr;
    return query;
}

// GetData without DONOTFLUSH flushes on every poll.
bool WaitQuery(ID3D11DeviceContext* ctx, ID3D11Query* query, DWORD timeoutMs) {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        if (ctx->GetData(query, nullptr, 0, 0) == S_OK) return true;
        if (GetTickCount64() - start >= timeoutMs) return false;
        Sleep(1);
    }
}

bool WaitFence12(ID3D12Fence* fence, uint64_t value, DWORD timeoutMs) {
    if (fence->GetCompletedValue() >= value) return true;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return false;
    bool ok = SUCCEEDED(fence->SetEventOnCompletion(value, event)) &&
              WaitForSingleObject(event, timeoutMs) == WAIT_OBJECT_0;
    CloseHandle(event);
    return ok || fence->GetCompletedValue() >= value;
}

// A failed check can leave the D3D11 queue waiting on the shared fence;
// release it so that the devices can be torn down.
struct SharedFenceReleaser {
    FencePair* fences;
    ~SharedFenceReleaser() {
        if (fences) fences->CpuSignalShared(fences->Next() + 1000000);
    }
};

ComPtr<ID3D11Texture2D> MakeTexture11(ID3D11Device* device, UINT size) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = size;
    desc.Height = size;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &tex))) return nullptr;
    return tex;
}

ComPtr<ID3D12Resource> MakeBuffer12(ID3D12Device* device, UINT64 bytes) {
    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    if (FAILED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                               nullptr, IID_PPV_ARGS(&buffer))))
        return nullptr;
    return buffer;
}

}  // namespace

// ------------------------------------------------------------------ FencePair

TEST(FencePair_CreateRejectsNullDevices) {
    std::string err;
    CHECK(FencePair::Create(nullptr, nullptr, &err) == nullptr);
    CHECK(!err.empty());
    CHECK(FencePair::Create(nullptr, nullptr, nullptr) == nullptr);
}

TEST(FencePair_CreateOpensSharedFenceOnD3D11) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    std::string err;
    auto fp = FencePair::Create(d.device12.Get(), d.device11.Get(), &err);
    if (!fp) std::printf("  error: %s\n", err.c_str());
    REQUIRE(fp != nullptr);
    CHECK(fp->Shared12() != nullptr);
    CHECK(fp->Shared11() != nullptr);
    CHECK(fp->Progress() != nullptr);
    CHECK(fp->Shared12() != fp->Progress());
    CHECK_EQ(fp->ProgressValue(), 0ull);
    CHECK_EQ(fp->pending_wait.load(), 0ull);
    CHECK_EQ(fp->last_submitted.load(), 0ull);
}

TEST(FencePair_NextStartsAtOneAndIsMonotonicAcrossThreads) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    auto fp = FencePair::Create(d.device12.Get(), d.device11.Get(), nullptr);
    REQUIRE(fp != nullptr);
    CHECK_EQ(fp->Next(), 1ull);
    CHECK_EQ(fp->Next(), 2ull);

    constexpr int kThreads = 4;
    constexpr int kPerThread = 5000;
    std::vector<std::vector<uint64_t>> seen(kThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&fp, &seen, t] {
            seen[t].reserve(kPerThread);
            for (int i = 0; i < kPerThread; ++i) seen[t].push_back(fp->Next());
        });
    }
    for (auto& th : threads) th.join();

    int notIncreasing = 0;
    std::vector<uint64_t> all;
    for (const auto& values : seen) {
        for (size_t i = 1; i < values.size(); ++i)
            if (values[i] <= values[i - 1]) ++notIncreasing;
        all.insert(all.end(), values.begin(), values.end());
    }
    CHECK_EQ(notIncreasing, 0);
    std::sort(all.begin(), all.end());
    int gaps = 0;
    for (size_t i = 0; i < all.size(); ++i)
        if (all[i] != 3 + i) ++gaps;  // every value 3..N exactly once
    CHECK_EQ(gaps, 0);
    CHECK_EQ(fp->Next(), 3ull + static_cast<uint64_t>(kThreads) * kPerThread);
}

TEST(FencePair_D3D11SignalIsSeenByD3D12) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    auto fp = FencePair::Create(d.device12.Get(), d.device11.Get(), nullptr);
    REQUIRE(fp != nullptr);
    ComPtr<ID3D11DeviceContext4> ctx4;
    REQUIRE(SUCCEEDED(d.ctx11.As(&ctx4)));

    const uint64_t v = fp->Next();
    CHECK_EQ(fp->Shared12()->GetCompletedValue(), 0ull);
    REQUIRE(SUCCEEDED(ctx4->Signal(fp->Shared11(), v)));
    d.ctx11->Flush();  // without it the signal may sit unsubmitted
    CHECK(WaitFence12(fp->Shared12(), v, 5000));
    CHECK_EQ(fp->Shared12()->GetCompletedValue(), v);
    CHECK_EQ(fp->Shared11()->GetCompletedValue(), v);
}

TEST(FencePair_D3D12SignalReleasesD3D11Wait) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    auto fp = FencePair::Create(d.device12.Get(), d.device11.Get(), nullptr);
    REQUIRE(fp != nullptr);
    SharedFenceReleaser releaser{fp.get()};
    ComPtr<ID3D11DeviceContext4> ctx4;
    REQUIRE(SUCCEEDED(d.ctx11.As(&ctx4)));
    auto queue = MakeQueue(d.device12.Get());
    REQUIRE(queue != nullptr);
    auto query = MakeEventQuery(d.device11.Get());
    REQUIRE(query != nullptr);

    const uint64_t v = fp->Next();
    REQUIRE(SUCCEEDED(ctx4->Wait(fp->Shared11(), v)));
    d.ctx11->End(query.Get());
    d.ctx11->Flush();
    Sleep(100);
    CHECK(d.ctx11->GetData(query.Get(), nullptr, 0, 0) == S_FALSE);  // held by the wait

    REQUIRE(SUCCEEDED(queue->Signal(fp->Shared12(), v)));
    CHECK(WaitQuery(d.ctx11.Get(), query.Get(), 5000));
    CHECK_EQ(fp->Shared11()->GetCompletedValue(), v);
}

TEST(FencePair_CpuWaitProgressTimesOutAndSucceeds) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    auto fp = FencePair::Create(d.device12.Get(), d.device11.Get(), nullptr);
    REQUIRE(fp != nullptr);
    auto queue = MakeQueue(d.device12.Get());
    REQUIRE(queue != nullptr);

    CHECK(fp->CpuWaitProgress(0, 0));
    ULONGLONG start = GetTickCount64();
    CHECK(!fp->CpuWaitProgress(1, 100));
    ULONGLONG elapsed = GetTickCount64() - start;
    CHECK(elapsed >= 100);
    CHECK(elapsed < 2000);
    CHECK(!fp->CpuWaitProgress(1, 0));

    REQUIRE(SUCCEEDED(queue->Signal(fp->Progress(), 1)));
    CHECK(fp->CpuWaitProgress(1, 5000));
    CHECK_EQ(fp->ProgressValue(), 1ull);

    // Signalled by the queue while the CPU is already waiting.
    std::thread signaller([&queue, &fp] {
        Sleep(60);
        queue->Signal(fp->Progress(), 2);
    });
    start = GetTickCount64();
    const bool reached = fp->CpuWaitProgress(2, 5000);
    elapsed = GetTickCount64() - start;
    signaller.join();
    CHECK(reached);
    CHECK(elapsed < 4000);
    CHECK_EQ(fp->ProgressValue(), 2ull);
    CHECK(fp->CpuWaitProgress(1, 0));

    // A stale event signal left by an earlier timed-out wait does not end a
    // later wait early.
    CHECK(!fp->CpuWaitProgress(5, 30));  // leaves a registration for 5
    REQUIRE(SUCCEEDED(queue->Signal(fp->Progress(), 5)));
    REQUIRE(WaitFence12(fp->Progress(), 5, 5000));  // that registration has fired
    start = GetTickCount64();
    CHECK(!fp->CpuWaitProgress(10, 80));
    CHECK(GetTickCount64() - start >= 80);
}

TEST(FencePair_CpuSignalSharedReleasesD3D11Wait) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    auto fp = FencePair::Create(d.device12.Get(), d.device11.Get(), nullptr);
    REQUIRE(fp != nullptr);
    SharedFenceReleaser releaser{fp.get()};
    ComPtr<ID3D11DeviceContext4> ctx4;
    REQUIRE(SUCCEEDED(d.ctx11.As(&ctx4)));
    auto query = MakeEventQuery(d.device11.Get());
    REQUIRE(query != nullptr);

    const uint64_t v = fp->Next();
    REQUIRE(SUCCEEDED(ctx4->Wait(fp->Shared11(), v)));
    d.ctx11->End(query.Get());
    d.ctx11->Flush();
    Sleep(100);
    CHECK(d.ctx11->GetData(query.Get(), nullptr, 0, 0) == S_FALSE);

    fp->CpuSignalShared(v);
    CHECK_EQ(fp->Shared12()->GetCompletedValue(), v);
    CHECK(WaitQuery(d.ctx11.Get(), query.Get(), 5000));
    CHECK_EQ(fp->ProgressValue(), 0ull);  // the progress fence is untouched

    // The CPU signal never moves the fence backwards.
    const uint64_t higher = fp->Next();
    fp->CpuSignalShared(higher);
    fp->CpuSignalShared(v);
    CHECK_EQ(fp->Shared12()->GetCompletedValue(), higher);
}

// ------------------------------------------------------------------ GpuTimer11

TEST(GpuTimer11_UninitialisedIsANoOp) {
    GpuTimer11 timer;
    CHECK(!timer.Init(nullptr));
    timer.Begin(nullptr);
    timer.End(nullptr);
    double sum = 0.0;
    CHECK_EQ(timer.Collect(nullptr, &sum), 0);
    CHECK(sum == 0.0);
}

TEST(GpuTimer11_MeasuresCopy) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    auto src = MakeTexture11(d.device11.Get(), 1024);
    auto dst = MakeTexture11(d.device11.Get(), 1024);
    REQUIRE(src && dst);
    GpuTimer11 timer;
    REQUIRE(timer.Init(d.device11.Get()));

    int samples = 0;
    double sum = 0.0;
    // A disjoint set (clock change) is dropped, so allow a few attempts.
    for (int attempt = 0; attempt < 3 && samples == 0; ++attempt) {
        timer.Begin(d.ctx11.Get());
        d.ctx11->CopyResource(dst.Get(), src.Get());
        timer.End(d.ctx11.Get());
        for (int i = 0; i < 500 && samples == 0; ++i) {
            d.ctx11->Flush();
            samples += timer.Collect(d.ctx11.Get(), &sum);
            if (samples == 0) Sleep(2);
        }
    }
    std::printf("  %d sample(s), %.4f ms\n", samples, sum);
    CHECK(samples >= 1);
    CHECK(sum >= 0.0);
    CHECK(sum < 1000.0);
    double again = 0.0;
    CHECK_EQ(timer.Collect(d.ctx11.Get(), &again), 0);  // each set is reported once
}

TEST(GpuTimer11_RingKeepsAtMostFourSets) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    auto src = MakeTexture11(d.device11.Get(), 256);
    auto dst = MakeTexture11(d.device11.Get(), 256);
    REQUIRE(src && dst);
    GpuTimer11 timer;
    REQUIRE(timer.Init(d.device11.Get()));

    for (int i = 0; i < 6; ++i) {
        timer.Begin(d.ctx11.Get());
        d.ctx11->CopyResource(dst.Get(), src.Get());
        timer.End(d.ctx11.Get());
    }
    int samples = 0;
    double sum = 0.0;
    for (int i = 0; i < 1000 && samples < 4; ++i) {
        d.ctx11->Flush();
        samples += timer.Collect(d.ctx11.Get(), &sum);
        if (samples < 4) Sleep(2);
    }
    CHECK(samples >= 1);
    CHECK(samples <= 4);
    CHECK(sum >= 0.0);
}

// ------------------------------------------------------------------ GpuTimer12

TEST(GpuTimer12_InitRejectsBadArguments) {
    GpuTimer12 timer;
    CHECK(!timer.Init(nullptr, nullptr, 2));
    double ms = 0.0;
    CHECK(!timer.Read(0, &ms));
    timer.Begin(nullptr, 0);
    timer.End(nullptr, 0);

    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    auto queue = MakeQueue(d.device12.Get());
    REQUIRE(queue != nullptr);
    CHECK(!timer.Init(d.device12.Get(), queue.Get(), 0));
}

TEST(GpuTimer12_MeasuresCopy) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    auto queue = MakeQueue(d.device12.Get());
    REQUIRE(queue != nullptr);
    ComPtr<ID3D12CommandAllocator> allocator;
    REQUIRE(SUCCEEDED(d.device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))));
    ComPtr<ID3D12GraphicsCommandList> list;
    REQUIRE(SUCCEEDED(d.device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                    IID_PPV_ARGS(&list))));
    auto src = MakeBuffer12(d.device12.Get(), 4 << 20);
    auto dst = MakeBuffer12(d.device12.Get(), 4 << 20);
    REQUIRE(src && dst);
    ComPtr<ID3D12Fence> fence;
    REQUIRE(SUCCEEDED(d.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));

    GpuTimer12 timer;
    REQUIRE(timer.Init(d.device12.Get(), queue.Get(), 2));
    double ms = -1.0;
    CHECK(!timer.Read(0, &ms));  // never resolved
    CHECK(!timer.Read(2, &ms));  // out of range

    timer.Begin(list.Get(), 1);
    list->CopyResource(dst.Get(), src.Get());  // buffers promote from COMMON implicitly
    timer.End(list.Get(), 1);
    REQUIRE(SUCCEEDED(list->Close()));
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    REQUIRE(SUCCEEDED(queue->Signal(fence.Get(), 1)));
    REQUIRE(WaitFence12(fence.Get(), 1, 5000));

    CHECK(timer.Read(1, &ms));
    std::printf("  copy of 4 MiB: %.4f ms\n", ms);
    CHECK(ms >= 0.0);
    CHECK(ms < 1000.0);
    CHECK(!timer.Read(0, &ms));  // slot 0 still unused
}
