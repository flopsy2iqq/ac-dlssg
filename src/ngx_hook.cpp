#include "ngx_hook.h"

#include <d3d11.h>

#include <atomic>
#include <cstring>
#include <unordered_map>

#include "inline_hook.h"
#include "log.h"

// Everything here is noexcept with a catch-all (spec 9): a detour must never let
// an exception escape into CSP's render thread. The original NGX call is always
// made first and its result returned unchanged; the sink is touched only after,
// and only for counted calls.

namespace acdb {
namespace {

constexpr int kMaxLayers = 16;

// Depth of NGX calls this thread is currently inside. Only the outermost is
// CSP's; anything nested is NGX calling its own plumbing (spec 6.5).
thread_local int t_nest = 0;

// Runtime-resolved imports (spec 6.1: not statically imported).
using PfnEnumProcessModules = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD);
using PfnLdrDllNotification = void(CALLBACK*)(ULONG reason, const void* data, void* ctx);
using PfnLdrRegister = LONG(NTAPI*)(ULONG, PfnLdrDllNotification, void*, void**);
using PfnLdrUnregister = LONG(NTAPI*)(void*);

struct LdrNotificationData {
    ULONG Flags;
    const void* FullDllName;
    const void* BaseDllName;
    void* DllBase;
    ULONG SizeOfImage;
};
constexpr ULONG kLdrLoaded = 1;
constexpr ULONG kLdrUnloaded = 2;

struct Layer {
    HMODULE mod = nullptr;
    bool used = false;
    InlineHook create;
    InlineHook eval;
    InlineHook eval_c;
};

struct FeatureRecord {
    bool supersampling = false;  // SuperSampling feature with no denoiser keys
    uint32_t createFlags = 0;
};

// One load or unload reported by the loader-notification callback.
struct LdrEvent {
    ULONG reason = 0;
    uintptr_t base = 0;
    ULONG size = 0;
};

// The callback's only output: a bounded, lock-free queue (D. Vyukov's bounded
// queue; any number of producers, one consumer). The callback pushes under the
// loader lock and never waits; the consumer is ProcessPendingRescan, Install or
// Uninstall, always under scanLock. A push into a full queue fails and the
// caller raises the overflow flag, which makes the consumer re-verify every
// layer instead.
class LdrEventQueue {
public:
    static constexpr uint32_t kCap = 64;  // a power of two
    LdrEventQueue() { Reset(); }

    // Only while no callback can run (before registering, after unregistering).
    void Reset() {
        for (uint32_t i = 0; i < kCap; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
        head_.store(0, std::memory_order_relaxed);
        tail_ = 0;
    }
    bool Push(const LdrEvent& ev) {
        uint32_t pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& c = cells_[pos & (kCap - 1)];
            const uint32_t seq = c.seq.load(std::memory_order_acquire);
            const int32_t diff = static_cast<int32_t>(seq - pos);
            if (diff == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    c.ev = ev;
                    c.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;  // full
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }
    }
    bool Pop(LdrEvent* out) {
        Cell& c = cells_[tail_ & (kCap - 1)];
        const uint32_t seq = c.seq.load(std::memory_order_acquire);
        if (static_cast<int32_t>(seq - (tail_ + 1)) < 0) return false;  // empty
        *out = c.ev;
        c.seq.store(tail_ + kCap, std::memory_order_release);
        ++tail_;
        return true;
    }

private:
    struct Cell {
        std::atomic<uint32_t> seq{0};
        LdrEvent ev;
    };
    Cell cells_[kCap];
    std::atomic<uint32_t> head_{0};
    uint32_t tail_ = 0;  // consumer only
};

// One process-wide state block behind the singleton.
//
// Locks (spec 6.5, ngx review F1). The loader-notification callback takes none
// of them. scanLock guards the layer table and is the only lock ever held
// across a loader call (module enumeration, GetProcAddress, GetModuleHandleEx);
// only Install, Uninstall and ProcessPendingRescan take it, never a detour.
// recLock guards the feature records and is never held across a loader or an
// NGX call. Order when both are held: scanLock, then recLock.
struct HookState {
    SRWLOCK scanLock = SRWLOCK_INIT;
    SRWLOCK recLock = SRWLOCK_INIT;
    Layer layers[kMaxLayers];
    std::unordered_map<uint64_t, FeatureRecord> records;
    std::atomic<NgxEvaluateSink*> sink{nullptr};
    std::atomic<bool> installed{false};
    std::atomic<uint32_t> hooked{0};  // used layers, for HookedModules

    // Written by the callback, drained under scanLock.
    LdrEventQueue events;
    std::atomic<bool> eventsOverflow{false};
    std::atomic<bool> pendingRescan{false};  // a module loaded: rescan
    std::atomic<bool> workPending{false};    // anything above is set: the gate

    PfnEnumProcessModules enumModules = nullptr;
    PfnLdrRegister ldrRegister = nullptr;
    PfnLdrUnregister ldrUnregister = nullptr;
    void* ldrCookie = nullptr;
    void (*installGapHook)() = nullptr;  // test-only, see SetInstallGapHookForTest
};

HookState& S() {
    static HookState s;
    return s;
}

// What the callback reads instead of S(): a plain pointer set before the
// notification is registered, so the callback never runs a function-local
// static's initialisation guard under the loader lock.
std::atomic<HookState*> g_callbackState{nullptr};

class ExclusiveLock {
public:
    explicit ExclusiveLock(SRWLOCK* l) : l_(l) { AcquireSRWLockExclusive(l_); }
    ~ExclusiveLock() { ReleaseSRWLockExclusive(l_); }
    ExclusiveLock(const ExclusiveLock&) = delete;
    ExclusiveLock& operator=(const ExclusiveLock&) = delete;

private:
    SRWLOCK* l_;
};

class SharedLock {
public:
    explicit SharedLock(SRWLOCK* l) : l_(l) { AcquireSRWLockShared(l_); }
    ~SharedLock() { ReleaseSRWLockShared(l_); }
    SharedLock(const SharedLock&) = delete;
    SharedLock& operator=(const SharedLock&) = delete;

private:
    SRWLOCK* l_;
};

bool ReadBytesSafe(const void* p, uint8_t* out, size_t n) {
    __try {
        std::memcpy(out, p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

HMODULE ModuleOf(const void* addr) {
    HMODULE h = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(addr), &h);
    return h;
}

// An export that is a 5-byte E9 jump into its own module is a thunk-table entry;
// the hook belongs at its destination so the patch does not clobber the entries
// beside it (spec 6.5; adapted from dlss5-bridge, MIT).
void* FollowThunk(void* fn) {
    auto* p = static_cast<uint8_t*>(fn);
    for (int hop = 0; hop < 4; ++hop) {
        uint8_t head[5];
        if (!ReadBytesSafe(p, head, sizeof(head))) break;
        if (head[0] != 0xE9) break;
        int32_t rel;
        std::memcpy(&rel, head + 1, sizeof(rel));
        auto* target = p + 5 + rel;
        const HMODULE here = ModuleOf(p);
        const HMODULE there = ModuleOf(target);
        if (!here || here != there) break;
        p = target;
    }
    return p;
}

bool EndsWithNoCase(const wchar_t* s, const wchar_t* suffix) {
    const size_t ls = wcslen(s), lf = wcslen(suffix);
    if (lf > ls) return false;
    return _wcsicmp(s + (ls - lf), suffix) == 0;
}

}  // namespace

bool NgxIsFillerStub(const void* fn) {
    if (!fn) return false;
    uint8_t b[14];
    if (!ReadBytesSafe(fn, b, sizeof(b))) return false;
    if (b[0] != 0x90 && b[0] != 0xCC) return false;
    for (int i = 1; i < 14; ++i)
        if (b[i] != b[0]) return false;
    return true;
}

NgxSkip NgxClassifyModule(const wchar_t* path, bool isHostExe, const void* create, const void* eval,
                          const void* eval_c) {
    if (isHostExe) return NgxSkip::HostExe;
    if (path) {
        // A case-insensitive search for the models sub-path.
        std::wstring lower(path);
        for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
        if (lower.find(L"\\ngx\\models\\") != std::wstring::npos) return NgxSkip::ModelsPath;
        if (EndsWithNoCase(path, L".bin")) return NgxSkip::ModelsPath;
    }
    if (NgxIsFillerStub(create) || (eval && NgxIsFillerStub(eval)) || (eval_c && NgxIsFillerStub(eval_c)))
        return NgxSkip::FillerStub;
    if ((eval && eval == create) || (eval_c && eval_c == create) || (eval && eval_c && eval == eval_c))
        return NgxSkip::SharedAddress;
    return NgxSkip::None;
}

// ---------------------------------------------------------------- per-slot detours

#define NGX_LAYER_DETOURS(k)                                                                          \
    static NgxResult __cdecl DetourCreate##k(ID3D11DeviceContext* c, uint32_t f, NgxParameter* p,     \
                                             NgxHandle** o) {                                         \
        return NgxHook::Get().DispatchCreate(k, c, f, p, o);                                          \
    }                                                                                                \
    static NgxResult __cdecl DetourEval##k(ID3D11DeviceContext* c, const NgxHandle* h,                \
                                           const NgxParameter* p, void* cb) {                         \
        return NgxHook::Get().DispatchEvaluate(k, false, c, h, p, cb);                                \
    }                                                                                                \
    static NgxResult __cdecl DetourEvalC##k(ID3D11DeviceContext* c, const NgxHandle* h,               \
                                            const NgxParameter* p, void* cb) {                        \
        return NgxHook::Get().DispatchEvaluate(k, true, c, h, p, cb);                                 \
    }

NGX_LAYER_DETOURS(0) NGX_LAYER_DETOURS(1) NGX_LAYER_DETOURS(2) NGX_LAYER_DETOURS(3)
NGX_LAYER_DETOURS(4) NGX_LAYER_DETOURS(5) NGX_LAYER_DETOURS(6) NGX_LAYER_DETOURS(7)
NGX_LAYER_DETOURS(8) NGX_LAYER_DETOURS(9) NGX_LAYER_DETOURS(10) NGX_LAYER_DETOURS(11)
NGX_LAYER_DETOURS(12) NGX_LAYER_DETOURS(13) NGX_LAYER_DETOURS(14) NGX_LAYER_DETOURS(15)

namespace {
struct DetourSet {
    void* create;
    void* eval;
    void* eval_c;
};
#define NGX_DETOUR_ROW(k) {reinterpret_cast<void*>(&DetourCreate##k), reinterpret_cast<void*>(&DetourEval##k), \
                           reinterpret_cast<void*>(&DetourEvalC##k)}
const DetourSet kDetours[kMaxLayers] = {
    NGX_DETOUR_ROW(0),  NGX_DETOUR_ROW(1),  NGX_DETOUR_ROW(2),  NGX_DETOUR_ROW(3),
    NGX_DETOUR_ROW(4),  NGX_DETOUR_ROW(5),  NGX_DETOUR_ROW(6),  NGX_DETOUR_ROW(7),
    NGX_DETOUR_ROW(8),  NGX_DETOUR_ROW(9),  NGX_DETOUR_ROW(10), NGX_DETOUR_ROW(11),
    NGX_DETOUR_ROW(12), NGX_DETOUR_ROW(13), NGX_DETOUR_ROW(14), NGX_DETOUR_ROW(15),
};

bool HasDenoiserKeys(const NgxParameter* p) {
    for (const char* key : kNgxDenoiserKeys) {
        ID3D11Resource* res = nullptr;
        if (p->Get(key, &res) == kNgxSuccess && res != nullptr) return true;
    }
    return false;
}

// Scans loaded modules and hooks any not yet hooked. Caller holds scanLock.
void ScanLocked() {
    HookState& s = S();
    if (!s.enumModules) return;
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!s.enumModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
    const DWORD count = (needed < sizeof(mods) ? needed : sizeof(mods)) / sizeof(HMODULE);
    const HMODULE hostExe = GetModuleHandleW(nullptr);

    for (DWORD i = 0; i < count; ++i) {
        HMODULE m = mods[i];
        bool already = false;
        for (const Layer& L : s.layers)
            if (L.used && L.mod == m) {
                already = true;
                break;
            }
        if (already) continue;

        void* create = reinterpret_cast<void*>(GetProcAddress(m, "NVSDK_NGX_D3D11_CreateFeature"));
        void* eval = reinterpret_cast<void*>(GetProcAddress(m, "NVSDK_NGX_D3D11_EvaluateFeature"));
        void* eval_c = reinterpret_cast<void*>(GetProcAddress(m, "NVSDK_NGX_D3D11_EvaluateFeature_C"));
        if (!create || (!eval && !eval_c)) continue;

        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(m, path, MAX_PATH);
        const NgxSkip skip = NgxClassifyModule(path, m == hostExe, create, eval, eval_c);
        if (skip != NgxSkip::None) {
            LOGD("ngx: skipping %ls (rule %d)", path, static_cast<int>(skip));
            continue;
        }

        int slot = -1;
        for (int k = 0; k < kMaxLayers; ++k)
            if (!s.layers[k].used) {
                slot = k;
                break;
            }
        if (slot < 0) {
            LOGW("ngx: layer table full; %ls not hooked", path);
            break;
        }

        void* createT = FollowThunk(create);
        void* evalT = eval ? FollowThunk(eval) : nullptr;
        void* evalCT = eval_c ? FollowThunk(eval_c) : nullptr;

        Layer& L = s.layers[slot];
        std::string err;
        if (!L.create.Install(createT, kDetours[slot].create, &err)) {
            LOGW("ngx: CreateFeature hook failed for %ls: %s", path, err.c_str());
            L.create.Detach();
            continue;  // create is mandatory; without it the feature filter is blind
        }
        if (evalT && !L.eval.Install(evalT, kDetours[slot].eval, &err))
            LOGW("ngx: EvaluateFeature hook failed for %ls: %s", path, err.c_str());
        if (evalCT && !L.eval_c.Install(evalCT, kDetours[slot].eval_c, &err))
            LOGW("ngx: EvaluateFeature_C hook failed for %ls: %s", path, err.c_str());
        L.mod = m;
        L.used = true;
        LOGI("ngx: hooked %ls (slot %d)", path, slot);
    }
}

// Forgets a layer without touching its module's memory. Caller holds scanLock.
void DetachLayerLocked(Layer& L) {
    L.create.Detach();
    L.eval.Detach();
    L.eval_c.Detach();
    L.mod = nullptr;
    L.used = false;
}

void RecountLocked() {
    HookState& s = S();
    uint32_t n = 0;
    for (const Layer& L : s.layers)
        if (L.used) ++n;
    s.hooked.store(n, std::memory_order_release);
}

// Drops any layer whose module is no longer loaded. Caller holds scanLock.
void VerifyUnloadsLocked() {
    HookState& s = S();
    for (Layer& L : s.layers) {
        if (!L.used) continue;
        HMODULE h = nullptr;
        const BOOL present = GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(L.mod), &h);
        if (!present || h != L.mod) DetachLayerLocked(L);
    }
}

// Applies one unload the callback reported: the layer at that base is dropped
// without a write to its memory. Caller holds scanLock.
void ApplyUnloadLocked(uintptr_t base) {
    HookState& s = S();
    for (Layer& L : s.layers)
        if (L.used && reinterpret_cast<uintptr_t>(L.mod) == base) DetachLayerLocked(L);
}

// Takes everything the callback queued. Unloads are applied here, before any
// rescan can look at the address they freed; loads only ask for a rescan.
// Returns true when the queue overflowed, so the caller re-verifies every layer.
// Caller holds scanLock.
bool DrainEventsLocked() {
    HookState& s = S();
    // An exchange, not a store: reading the callback's `true` orders every push
    // and flag it made before it before the pops below.
    s.workPending.exchange(false, std::memory_order_acq_rel);
    LdrEvent ev;
    while (s.events.Pop(&ev)) {
        if (ev.reason == kLdrUnloaded) ApplyUnloadLocked(ev.base);
    }
    return s.eventsOverflow.exchange(false, std::memory_order_acq_rel);
}

void CALLBACK OnLdrNotification(ULONG reason, const void* data, void*) {
    // Runs under the loader lock, so it must never wait (spec 6.5, ngx review
    // F1): no lock, no allocation, no loader call. It records the event for
    // ProcessPendingRescan and returns. The SEH frame carries no unwinding
    // objects.
    __try {
        HookState* s = g_callbackState.load(std::memory_order_acquire);
        if (s && data && (reason == kLdrLoaded || reason == kLdrUnloaded)) {
            const auto* d = static_cast<const LdrNotificationData*>(data);
            LdrEvent ev;
            ev.reason = reason;
            ev.base = reinterpret_cast<uintptr_t>(d->DllBase);
            ev.size = d->SizeOfImage;
            if (!s->events.Push(ev)) s->eventsOverflow.store(true, std::memory_order_release);
            if (reason == kLdrLoaded) s->pendingRescan.store(true, std::memory_order_release);
            s->workPending.store(true, std::memory_order_release);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// A pointer that cannot be a real argument: the bottom 64 KB is never mapped, so
// a detour entered with a stray register value is caught here rather than by a
// fault (adapted from dlss5-bridge, MIT).
bool Plausible(const void* p) { return reinterpret_cast<uintptr_t>(p) >= 0x10000; }

}  // namespace

// ---------------------------------------------------------------- NgxHook

NgxHook& NgxHook::Get() {
    static NgxHook instance;
    return instance;
}

bool NgxHook::Install(NgxEvaluateSink* sink, std::string* error) {
    HookState& s = S();
    // A previous Install without Uninstall is treated as a fresh start.
    if (s.installed.load(std::memory_order_acquire)) Uninstall();

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    s.enumModules = k32 ? reinterpret_cast<PfnEnumProcessModules>(
                              GetProcAddress(k32, "K32EnumProcessModules"))
                        : nullptr;
    if (!s.enumModules) {
        if (error) *error = "K32EnumProcessModules unavailable";
        return false;
    }
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    s.ldrRegister = nt ? reinterpret_cast<PfnLdrRegister>(
                             GetProcAddress(nt, "LdrRegisterDllNotification"))
                       : nullptr;
    s.ldrUnregister = nt ? reinterpret_cast<PfnLdrUnregister>(
                               GetProcAddress(nt, "LdrUnregisterDllNotification"))
                         : nullptr;

    {
        ExclusiveLock scan(&s.scanLock);
        // No callback is registered here (Uninstall unregistered it), so the
        // queue can be reset.
        s.events.Reset();
        s.eventsOverflow.store(false);
        s.pendingRescan.store(false);
        s.workPending.store(false);
        {
            ExclusiveLock rec(&s.recLock);
            s.records.clear();
        }
        s.sink.store(sink, std::memory_order_release);
        ScanLocked();
        RecountLocked();
    }
    if (s.installGapHook) s.installGapHook();
    // Register only after the first scan. The callback reads only the queue.
    g_callbackState.store(&s, std::memory_order_release);
    if (s.ldrRegister && !s.ldrCookie) {
        if (s.ldrRegister(0, &OnLdrNotification, nullptr, &s.ldrCookie) != 0) {
            s.ldrCookie = nullptr;
            LOGW("ngx: LdrRegisterDllNotification failed; only already-loaded modules are hooked");
        }
    }
    // A module that loaded between the scan above and the registration produced
    // no event: the first ProcessPendingRescan rescans to cover that gap (ngx
    // review F5).
    s.pendingRescan.store(true, std::memory_order_release);
    s.workPending.store(true, std::memory_order_release);
    s.installed.store(true, std::memory_order_release);
    return true;
}

void NgxHook::ProcessPendingRescan() {
    HookState& s = S();
    if (!s.installed.load(std::memory_order_acquire)) return;
    // The per-frame fast path: nothing was reported, so no lock and no loader call.
    if (!s.workPending.load(std::memory_order_acquire)) return;
    ExclusiveLock scan(&s.scanLock);
    const bool overflow = DrainEventsLocked();  // unloads first, before any rescan
    if (overflow) VerifyUnloadsLocked();
    if (s.pendingRescan.exchange(false, std::memory_order_acq_rel) || overflow) ScanLocked();
    RecountLocked();
}

void NgxHook::Uninstall() {
    HookState& s = S();
    // Unregister outside our locks: it waits for a callback in progress, and the
    // callback takes none of them anyway.
    if (s.ldrUnregister && s.ldrCookie) {
        s.ldrUnregister(s.ldrCookie);
        s.ldrCookie = nullptr;
    }
    s.installed.store(false, std::memory_order_release);
    ExclusiveLock scan(&s.scanLock);
    // A module that went away since the last drain is detached, never written to.
    DrainEventsLocked();
    for (Layer& L : s.layers) {
        if (!L.used) continue;
        L.create.Remove();
        L.eval.Remove();
        L.eval_c.Remove();
        L.mod = nullptr;
        L.used = false;
    }
    RecountLocked();
    {
        ExclusiveLock rec(&s.recLock);
        s.records.clear();
    }
    s.sink.store(nullptr, std::memory_order_release);
    s.pendingRescan.store(false);
    s.workPending.store(false);
}

uint32_t NgxHook::HookedModules() const { return S().hooked.load(std::memory_order_acquire); }

void NgxHook::SetInstallGapHookForTest(void (*fn)()) { S().installGapHook = fn; }

void NgxHook::LockStateForTest() {
    HookState& s = S();
    AcquireSRWLockExclusive(&s.scanLock);
    AcquireSRWLockExclusive(&s.recLock);
}

void NgxHook::UnlockStateForTest() {
    HookState& s = S();
    ReleaseSRWLockExclusive(&s.recLock);
    ReleaseSRWLockExclusive(&s.scanLock);
}

NgxResult NgxHook::DispatchCreate(int slot, ID3D11DeviceContext* ctx, uint32_t featureId,
                                  NgxParameter* params, NgxHandle** outHandle) noexcept {
    HookState& s = S();
    // No original (an unused, removed or detached slot): report NGX's generic
    // failure rather than invent a Success with *outHandle unwritten.
    if (slot < 0 || slot >= kMaxLayers) return kNgxFail;
    auto orig = reinterpret_cast<PfnNgxCreateFeature>(s.layers[slot].create.Original());
    if (!orig) return kNgxFail;
    ++t_nest;
    NgxResult r = orig(ctx, featureId, params, outHandle);
    const int nest = t_nest;
    --t_nest;
    if (nest != 1) return r;  // nested NGX-internal create: forward only
    if (r != kNgxSuccess || !outHandle || !*outHandle || !Plausible(params)) return r;
    try {
        const uint64_t key = reinterpret_cast<uint64_t>(*outHandle);
        const bool denoiser = HasDenoiserKeys(params);
        NgxCreateInfo info;
        info.featureId = featureId;
        params->Get(ngxkey::kWidth, &info.width);
        params->Get(ngxkey::kHeight, &info.height);
        params->Get(ngxkey::kOutWidth, &info.outWidth);
        params->Get(ngxkey::kOutHeight, &info.outHeight);
        int flags = 0;
        if (params->Get(ngxkey::kCreateFlags, &flags) == kNgxSuccess)
            info.createFlags = static_cast<uint32_t>(flags);
        info.hasDenoiserKeys = denoiser;
        const bool supersampling = (featureId == kNgxFeatureSuperSampling) && !denoiser;

        {
            ExclusiveLock rec(&s.recLock);
            s.records[key] = FeatureRecord{supersampling, info.createFlags};
        }
        NgxEvaluateSink* sink = s.sink.load(std::memory_order_acquire);
        if (supersampling && sink) sink->OnCreateFeature(key, info);
    } catch (...) {
    }
    return r;
}

NgxResult NgxHook::DispatchEvaluate(int slot, bool isC, ID3D11DeviceContext* ctx, const NgxHandle* handle,
                                    const NgxParameter* params, void* callback) noexcept {
    HookState& s = S();
    if (slot < 0 || slot >= kMaxLayers) return kNgxFail;  // as in DispatchCreate
    InlineHook& hook = isC ? s.layers[slot].eval_c : s.layers[slot].eval;
    auto orig = reinterpret_cast<PfnNgxEvaluateFeature>(hook.Original());
    if (!orig) return kNgxFail;
    ++t_nest;
    NgxResult r = orig(ctx, handle, params, callback);
    const int nest = t_nest;
    --t_nest;
    if (nest != 1) return r;  // nested: touch nothing (spec 6.5)
    try {
        // Deferred contexts cannot be captured (spec 6.5).
        if (!Plausible(ctx)) return r;
        if (ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
            static LONG said = 0;
            if (InterlockedCompareExchange(&said, 1, 0) == 0)
                LOGW("ngx: DLSS evaluate on a deferred context; capture disabled");
            return r;
        }
        // Decision (spec 6.5 open point): a failed original is not captured. The
        // depth and MV a failed DLSS consumed cannot be trusted to pair with a
        // DLSS-G frame, and building constants for it would only feed garbage.
        if (r != kNgxSuccess) return r;
        if (!Plausible(params) || !Plausible(handle)) return r;

        const uint64_t key = reinterpret_cast<uint64_t>(handle);
        bool observed = false, supersampling = false;
        uint32_t recFlags = 0;
        {
            SharedLock rec(&s.recLock);
            auto it = s.records.find(key);
            if (it != s.records.end()) {
                observed = true;
                supersampling = it->second.supersampling;
                recFlags = it->second.createFlags;
            }
        }

        NgxEvaluateInputs in;
        in.ctx = ctx;
        in.featureKey = key;
        in.createObserved = observed;
        if (observed) {
            if (!supersampling) return r;  // another feature: forwarded, not counted
            in.createFlags = recFlags;
        } else {
            // Unobserved create: count only if Depth, MotionVectors and the create
            // flags are all present in this evaluate block (spec 6.5).
            ID3D11Resource* d = nullptr;
            ID3D11Resource* m = nullptr;
            int flags = 0;
            if (params->Get(ngxkey::kDepth, &d) != kNgxSuccess ||
                params->Get(ngxkey::kMotionVectors, &m) != kNgxSuccess ||
                params->Get(ngxkey::kCreateFlags, &flags) != kNgxSuccess)
                return r;
            in.createFlags = static_cast<uint32_t>(flags);
        }

        // Resource pointers are read on every evaluate (CSP ping-pongs its MV
        // textures, spec 6.5).
        ID3D11Resource* depth = nullptr;
        ID3D11Resource* mvec = nullptr;
        if (params->Get(ngxkey::kDepth, &depth) == kNgxSuccess) in.depth = depth;
        if (params->Get(ngxkey::kMotionVectors, &mvec) == kNgxSuccess) in.mvec = mvec;
        params->Get(ngxkey::kJitterOffsetX, &in.jitterX);
        params->Get(ngxkey::kJitterOffsetY, &in.jitterY);
        params->Get(ngxkey::kMvScaleX, &in.mvScaleX);
        params->Get(ngxkey::kMvScaleY, &in.mvScaleY);
        unsigned int sw = 0, sh = 0;
        if (params->Get(ngxkey::kSubrectWidth, &sw) != kNgxSuccess) params->Get(ngxkey::kWidth, &sw);
        if (params->Get(ngxkey::kSubrectHeight, &sh) != kNgxSuccess) params->Get(ngxkey::kHeight, &sh);
        in.subrectW = sw;
        in.subrectH = sh;
        int reset = 0;
        if (params->Get(ngxkey::kReset, &reset) == kNgxSuccess) in.reset = reset != 0;

        NgxEvaluateSink* sink = s.sink.load(std::memory_order_acquire);
        if (sink) sink->OnEvaluate(in);
    } catch (...) {
    }
    return r;
}

}  // namespace acdb
