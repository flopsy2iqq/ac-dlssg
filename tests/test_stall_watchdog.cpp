// StallWatchdog against a real FencePair (hardware adapter, else WARP), with
// short periods so that the tests stay fast. Each test skips with a note when
// no adapter creates both devices.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>

#include "fence_pair.h"
#include "gpu_test_devices.h"
#include "stall_watchdog.h"
#include "test_framework.h"

using Microsoft::WRL::ComPtr;
using namespace acdb;

namespace {

constexpr DWORD kPeriodMs = 20;
constexpr DWORD kStallMs = 200;

struct Rig {
    acdb_test::GpuTestDevices d;
    std::unique_ptr<FencePair> fences;
    ComPtr<ID3D12CommandQueue> queue;
    // A failed check can leave the D3D11 queue waiting on the shared fence;
    // release it so that the devices can be torn down.
    ~Rig() {
        if (fences) fences->CpuSignalShared(fences->Next() + 1000000);
    }
};

// False (with a printed note) when the test should skip.
bool MakeRig(Rig* rig) {
    if (!acdb_test::CreateGpuTestDevices(&rig->d)) {
        std::printf("  SKIP: no adapter creates both a D3D11 and a D3D12 device\n");
        return false;
    }
    std::string err;
    rig->fences = FencePair::Create(rig->d.device12.Get(), rig->d.device11.Get(), &err);
    if (!rig->fences) std::printf("  FencePair::Create: %s\n", err.c_str());
    REQUIRE(rig->fences != nullptr);
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    REQUIRE(SUCCEEDED(rig->d.device12->CreateCommandQueue(&desc, IID_PPV_ARGS(&rig->queue))));
    return true;
}

uint64_t Shared(const Rig& rig) { return rig.fences->Shared12()->GetCompletedValue(); }

// Polls until the shared fence reaches value; returns the milliseconds taken,
// or MAXDWORD on timeout.
DWORD WaitShared(const Rig& rig, uint64_t value, ULONGLONG since, DWORD timeoutMs) {
    for (;;) {
        const ULONGLONG elapsed = GetTickCount64() - since;
        if (Shared(rig) >= value) return static_cast<DWORD>(elapsed);
        if (elapsed >= timeoutMs) return MAXDWORD;
        Sleep(2);
    }
}

// TakeStallPending is set right after the CPU signal, on the watchdog thread.
bool WaitTakeStallPending(StallWatchdog& wd, DWORD timeoutMs) {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        if (wd.TakeStallPending()) return true;
        if (GetTickCount64() - start >= timeoutMs) return false;
        Sleep(1);
    }
}

}  // namespace

TEST(StallWatchdog_ReleasesStuckD3D11Wait) {
    Rig rig;
    if (!MakeRig(&rig)) return;
    ComPtr<ID3D11DeviceContext4> ctx4;
    REQUIRE(SUCCEEDED(rig.d.ctx11.As(&ctx4)));
    D3D11_QUERY_DESC qdesc{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> query;
    REQUIRE(SUCCEEDED(rig.d.device11->CreateQuery(&qdesc, &query)));

    // As the presenter would leave it: values 1..5 allocated, D3D11 waits for
    // 5, and the D3D12 queue never signals.
    for (int i = 0; i < 5; ++i) rig.fences->Next();
    REQUIRE(SUCCEEDED(ctx4->Wait(rig.fences->Shared11(), 5)));
    rig.d.ctx11->End(query.Get());
    rig.d.ctx11->Flush();
    rig.fences->pending_wait = 5;

    StallWatchdog wd(rig.fences.get(), rig.d.device12.Get(), kPeriodMs, kStallMs);
    const ULONGLONG start = GetTickCount64();
    wd.Start();
    Sleep(80);
    CHECK_EQ(Shared(rig), 0ull);  // too early
    CHECK(!wd.TakeStallPending());
    CHECK(rig.d.ctx11->GetData(query.Get(), nullptr, 0, 0) == S_FALSE);

    const DWORD released = WaitShared(rig, 5, start, 3000);
    std::printf("  released after %lu ms\n", static_cast<unsigned long>(released));
    CHECK(released != MAXDWORD);
    CHECK(released >= kStallMs);
    CHECK(released < 1500);
    CHECK_EQ(Shared(rig), 5ull);
    CHECK(WaitTakeStallPending(wd, 1000));
    CHECK(!wd.TakeStallPending());  // exactly once
    CHECK(!wd.DeviceRemoved());
    CHECK(wd.MsSinceProgress() >= kStallMs);
    CHECK_EQ(rig.fences->ProgressValue(), 0ull);

    // The CPU signal released CSP's D3D11 queue.
    const ULONGLONG q0 = GetTickCount64();
    bool done = false;
    while (!done && GetTickCount64() - q0 < 5000) {
        done = rig.d.ctx11->GetData(query.Get(), nullptr, 0, 0) == S_OK;
        if (!done) Sleep(1);
    }
    CHECK(done);

    // The same value is not released twice.
    Sleep(4 * kPeriodMs + 20);
    CHECK(!wd.TakeStallPending());

    // A later wait while progress is still stuck is released on the next tick.
    rig.fences->Next();
    rig.fences->Next();
    rig.fences->pending_wait = 7;
    const ULONGLONG second = GetTickCount64();
    const DWORD released2 = WaitShared(rig, 7, second, 3000);
    CHECK(released2 != MAXDWORD);
    CHECK(released2 < kStallMs);
    CHECK(WaitTakeStallPending(wd, 1000));
    CHECK(!wd.TakeStallPending());
    wd.Stop();
}

TEST(StallWatchdog_NoStallWhileProgressAdvances) {
    Rig rig;
    if (!MakeRig(&rig)) return;
    rig.fences->pending_wait = 1000000;  // always ahead of progress

    StallWatchdog wd(rig.fences.get(), rig.d.device12.Get(), kPeriodMs, kStallMs);
    wd.Start();
    std::atomic<bool> stop{false};
    std::thread signaller([&rig, &stop] {
        uint64_t v = 0;
        while (!stop.load()) {
            rig.queue->Signal(rig.fences->Progress(), ++v);
            Sleep(50);
        }
    });
    Sleep(4 * kStallMs);
    const DWORD since = wd.MsSinceProgress();
    const bool stalled = wd.TakeStallPending();
    stop = true;
    signaller.join();

    CHECK(!stalled);
    CHECK_EQ(Shared(rig), 0ull);
    CHECK(since < kStallMs);
    CHECK(rig.fences->ProgressValue() >= 5ull);
    CHECK(!wd.DeviceRemoved());
    wd.Stop();
}

TEST(StallWatchdog_IdleGapIsNotAStall) {
    Rig rig;
    if (!MakeRig(&rig)) return;
    const uint64_t v1 = rig.fences->Next();
    REQUIRE(SUCCEEDED(rig.queue->Signal(rig.fences->Progress(), v1)));
    REQUIRE(rig.fences->CpuWaitProgress(v1, 5000));
    rig.fences->pending_wait = v1;  // caught up: nothing outstanding

    StallWatchdog wd(rig.fences.get(), rig.d.device12.Get(), kPeriodMs, kStallMs);
    wd.Start();
    Sleep(2 * kStallMs);  // no frames for longer than the stall limit

    // The next frame's wait; its D3D12 work is still queued.
    const uint64_t v2 = rig.fences->Next();
    rig.fences->pending_wait = v2;
    const ULONGLONG start = GetTickCount64();
    Sleep(kStallMs / 2);
    CHECK_EQ(Shared(rig), 0ull);
    CHECK(!wd.TakeStallPending());

    // The stall clock started with the outstanding work, so it still fires.
    const DWORD released = WaitShared(rig, v2, start, 3000);
    CHECK(released != MAXDWORD);
    CHECK(released >= kStallMs - kPeriodMs - 20);
    CHECK(WaitTakeStallPending(wd, 1000));
    wd.Stop();
}

TEST(StallWatchdog_MsSinceProgressGrowsWhileStuck) {
    Rig rig;
    if (!MakeRig(&rig)) return;
    rig.fences->pending_wait = 1000000;

    StallWatchdog wd(rig.fences.get(), rig.d.device12.Get(), kPeriodMs, 60000);
    wd.Start();
    Sleep(100);
    const DWORD a = wd.MsSinceProgress();
    Sleep(250);
    const DWORD b = wd.MsSinceProgress();
    CHECK(b > a);
    CHECK(b >= 300);

    // Progress resets it.
    REQUIRE(SUCCEEDED(rig.queue->Signal(rig.fences->Progress(), 1)));
    REQUIRE(rig.fences->CpuWaitProgress(1, 5000));
    Sleep(3 * kPeriodMs);
    const DWORD c = wd.MsSinceProgress();
    CHECK(c < 150);
    CHECK(!wd.TakeStallPending());
    CHECK_EQ(Shared(rig), 0ull);
    wd.Stop();
}

TEST(StallWatchdog_ClockRunsOnlyWhileWorkIsOutstanding) {
    Rig rig;
    if (!MakeRig(&rig)) return;

    StallWatchdog wd(rig.fences.get(), rig.d.device12.Get(), kPeriodMs, kStallMs);
    wd.Start();
    Sleep(2 * kStallMs);  // idle: nothing submitted, nothing waited for
    CHECK(wd.MsSinceProgress() < 100);

    // Submitted but not yet waited for by D3D11: the clock runs, but there is
    // no D3D11 wait to release.
    rig.fences->last_submitted = 5;
    Sleep(2 * kStallMs);
    CHECK(wd.MsSinceProgress() >= kStallMs);
    CHECK(!wd.TakeStallPending());
    CHECK_EQ(Shared(rig), 0ull);
    wd.Stop();
}

TEST(StallWatchdog_StopIsIdempotent) {
    Rig rig;
    if (!MakeRig(&rig)) return;

    {
        StallWatchdog never(rig.fences.get(), rig.d.device12.Get(), kPeriodMs, kStallMs);
        never.Stop();
        never.Stop();
    }  // destructor after Stop without Start

    StallWatchdog wd(rig.fences.get(), rig.d.device12.Get(), kPeriodMs, kStallMs);
    wd.Start();
    wd.Start();  // already running: no second thread
    Sleep(50);
    wd.Stop();
    wd.Stop();
    wd.Start();  // restart after Stop
    Sleep(50);
    wd.Stop();
    wd.Stop();

    // Stop wakes the thread instead of waiting out a long period.
    StallWatchdog slow(rig.fences.get(), rig.d.device12.Get(), 10000, kStallMs);
    slow.Start();
    Sleep(30);
    const ULONGLONG start = GetTickCount64();
    slow.Stop();
    CHECK(GetTickCount64() - start < 1000);
    slow.Stop();
    CHECK(!wd.TakeStallPending());
    CHECK(!slow.TakeStallPending());
}

// Last in this file: it removes its own D3D12 device (D3D12 devices are
// per-adapter singletons while referenced, so nothing may keep it alive).
TEST(StallWatchdog_DeviceRemovedReleasesAndFlags) {
    Rig rig;
    if (!MakeRig(&rig)) return;
    ComPtr<ID3D12Device5> device5;
    if (FAILED(rig.d.device12.As(&device5))) {
        std::printf("  SKIP: ID3D12Device5 not available\n");
        return;
    }
    rig.fences->Next();
    rig.fences->Next();
    rig.fences->Next();
    rig.fences->pending_wait = 3;

    StallWatchdog wd(rig.fences.get(), rig.d.device12.Get(), kPeriodMs, 60000);
    wd.Start();
    Sleep(3 * kPeriodMs);
    CHECK(!wd.DeviceRemoved());
    CHECK(!wd.TakeStallPending());

    device5->RemoveDevice();
    const ULONGLONG start = GetTickCount64();
    while (!wd.DeviceRemoved() && GetTickCount64() - start < 3000) Sleep(2);
    CHECK(wd.DeviceRemoved());
    CHECK(WaitTakeStallPending(wd, 1000));
    CHECK(!wd.TakeStallPending());
    CHECK(Shared(rig) >= 3ull);  // removal signals every fence of the device anyway
    wd.Stop();
    device5.Reset();
}
