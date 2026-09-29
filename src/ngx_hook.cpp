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

// Depth of sink calls this thread is inside (it then holds sinkLock shared).
thread_local int t_sinkDepth = 0;

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

// One hooked module. A base address alone does not identify it: after an
// unload another module can load at the same base (ngx review F6), so the
// image size and full path are kept too.
struct Layer {
    HMODULE mod = nullptr;
    ULONG sizeOfImage = 0;
    std::wstring path;
    bool used = false;
    // A denoiser snippet (DLSS NR / RR), or a module whose entry points resolve
    // into one: hooked so nesting is tracked, forwarded, never counted (F4).
    // Set before any of the layer's patches goes live; read by the detours.
    std::atomic<bool> neverCount{false};
    InlineHook create;
    InlineHook eval;
    InlineHook eval_c;
};

struct FeatureRecord {
    bool supersampling = false;  // SuperSampling feature with no denoiser keys
    uint32_t createFlags = 0;
    uint32_t width = 0, height = 0;  // the create's input size (subrect fallback)
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
// NGX call. Order when both are held: scanLock, then recLock. sinkLock is held
// shared for every call into the sink and exclusive by SetSink, so SetSink
// returns only when no call into the old sink is in progress; it is never
// taken while scanLock or recLock is held.
struct HookState {
    SRWLOCK scanLock = SRWLOCK_INIT;
    SRWLOCK recLock = SRWLOCK_INIT;
    SRWLOCK sinkLock = SRWLOCK_INIT;
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

// Calls fn(sink) with sinkLock held shared, so SetSink can wait for it. A call
// made from inside another sink call on this thread already holds the lock and
// does not take it again (SRW locks are not recursive).
template <class Fn>
void CallSink(Fn&& fn) {
    HookState& s = S();
    const bool take = t_sinkDepth == 0;
    if (take) AcquireSRWLockShared(&s.sinkLock);
    struct Leave {
        HookState& s;
        bool took;
        ~Leave() {
            --t_sinkDepth;
            if (took) ReleaseSRWLockShared(&s.sinkLock);
        }
    };
    ++t_sinkDepth;
    Leave leave{s, take};
    if (NgxEvaluateSink* sink = s.sink.load(std::memory_order_acquire)) fn(sink);
}

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

// SizeOfImage from the PE header at base, or 0 when it cannot be read.
ULONG ImageSizeAt(const void* base) {
    IMAGE_DOS_HEADER dos{};
    if (!ReadBytesSafe(base, reinterpret_cast<uint8_t*>(&dos), sizeof(dos))) return 0;
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) return 0;
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadBytesSafe(static_cast<const uint8_t*>(base) + dos.e_lfanew, reinterpret_cast<uint8_t*>(&nt),
                       sizeof(nt)))
        return 0;
    if (nt.Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt.OptionalHeader.SizeOfImage;
}

std::wstring ModulePath(HMODULE m) {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(m, path, MAX_PATH);
    return std::wstring(path, n < MAX_PATH ? n : MAX_PATH);
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

bool NgxIsDenoiserModule(const wchar_t* path) {
    if (!path || !*path) return false;
    const wchar_t* base = path;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') base = p + 1;
    return _wcsicmp(base, L"nvngx_dlssnr.dll") == 0 || _wcsicmp(base, L"nvngx_dlssd.dll") == 0;
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

// The first denoiser input resource key (DLSS-D or DLSS NR) present in p, or
// nullptr. A key counts when it holds a non-null D3D11, D3D12 or untyped
// resource. Only these disqualify a SuperSampling feature (F4).
const char* DenoiserKeyIn(const NgxParameter* p) {
    for (const char* key : kNgxDenoiserKeys) {
        ID3D11Resource* r11 = nullptr;
        if (p->Get(key, &r11) == kNgxSuccess && r11) return key;
        ID3D12Resource* r12 = nullptr;
        if (p->Get(key, &r12) == kNgxSuccess && r12) return key;
        void* untyped = nullptr;
        if (p->Get(key, &untyped) == kNgxSuccess && untyped) return key;
    }
    return nullptr;
}

// The first denoiser scalar key present in p, or nullptr; logged only. CSP
// 0.3.0-preview622 sets DLSSNR.Hint.Render.Preset on its SuperSampling create
// block, so a scalar hint is no evidence of a denoiser. NR and RR are told
// apart by feature id and by module (nvngx_dlssnr.dll, nvngx_dlssd.dll).
const char* DenoiserScalarKeyIn(const NgxParameter* p) {
    for (const char* key : kNgxDenoiserScalarKeys) {
        unsigned int u = 0;
        int i = 0;
        float f = 0;
        if (p->Get(key, &u) == kNgxSuccess || p->Get(key, &i) == kNgxSuccess || p->Get(key, &f) == kNgxSuccess)
            return key;
    }
    return nullptr;
}

bool LayerIsLiveLocked(const Layer& L);
void DetachLayerLocked(Layer& L);
bool HookModuleLocked(HMODULE m, bool isHostExe);

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
        Layer* stale = nullptr;
        bool already = false;
        for (Layer& L : s.layers)
            if (L.used && L.mod == m) {
                already = LayerIsLiveLocked(L);
                if (!already) stale = &L;
                break;
            }
        if (already) continue;
        if (stale) {
            // Another module now sits at a hooked module's base (a missed unload):
            // forget the old layer without writing, then treat m as new (F6).
            LOGI("ngx: the module at a hooked base changed (was %ls); old hooks dropped", stale->path.c_str());
            DetachLayerLocked(*stale);
        }

        // Pin m while its exports are read and patched, so it cannot unload under us.
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(m), &pinned))
            continue;
        const bool tableFull = pinned == m && !HookModuleLocked(m, m == hostExe);
        FreeLibrary(pinned);
        if (tableFull) break;
    }
}

// Hooks one pinned module if it exports the NGX D3D11 entry points and passes
// the skip rules. Returns false only when the layer table is full. Caller holds
// scanLock.
bool HookModuleLocked(HMODULE m, bool isHostExe) {
    HookState& s = S();
    void* create = reinterpret_cast<void*>(GetProcAddress(m, "NVSDK_NGX_D3D11_CreateFeature"));
    void* eval = reinterpret_cast<void*>(GetProcAddress(m, "NVSDK_NGX_D3D11_EvaluateFeature"));
    void* eval_c = reinterpret_cast<void*>(GetProcAddress(m, "NVSDK_NGX_D3D11_EvaluateFeature_C"));
    if (!create || (!eval && !eval_c)) return true;

    const std::wstring path = ModulePath(m);
    const NgxSkip skip = NgxClassifyModule(path.c_str(), isHostExe, create, eval, eval_c);
    if (skip != NgxSkip::None) {
        LOGD("ngx: skipping %ls (rule %d)", path.c_str(), static_cast<int>(skip));
        return true;
    }

    int slot = -1;
    for (int k = 0; k < kMaxLayers; ++k)
        if (!s.layers[k].used) {
            slot = k;
            break;
        }
    if (slot < 0) {
        LOGW("ngx: layer table full; %ls not hooked", path.c_str());
        return false;
    }

    void* createT = FollowThunk(create);
    void* evalT = eval ? FollowThunk(eval) : nullptr;
    void* evalCT = eval_c ? FollowThunk(eval_c) : nullptr;

    // Entry points that resolve to code another layer already patched (a module
    // whose exports forward into a hooked one): a second patch would chain two
    // detours, and restoring them would leave a jump into freed memory (F4).
    for (const Layer& other : s.layers) {
        if (!other.used) continue;
        if (other.create.Target() == createT || (evalT && other.eval.Target() == evalT) ||
            (evalCT && other.eval_c.Target() == evalCT)) {
            LOGD("ngx: %ls resolves to code already hooked in %ls; skipped", path.c_str(), other.path.c_str());
            return true;
        }
    }

    Layer& L = s.layers[slot];
    // DLSS NR / RR snippets, by name or by where the entry points resolve to,
    // are hooked (so the nesting counter sees them) but never counted (F4).
    auto resolvesIntoDenoiser = [](const void* fn) {
        const HMODULE h = fn ? ModuleOf(fn) : nullptr;
        return h && NgxIsDenoiserModule(ModulePath(h).c_str());
    };
    L.neverCount = NgxIsDenoiserModule(path.c_str()) || resolvesIntoDenoiser(createT) ||
                   resolvesIntoDenoiser(evalT) || resolvesIntoDenoiser(evalCT);
    std::string err;
    if (!L.create.Install(createT, kDetours[slot].create, &err)) {
        LOGW("ngx: CreateFeature hook failed for %ls: %s", path.c_str(), err.c_str());
        L.create.Detach();
        L.neverCount = false;
        return true;  // create is mandatory; without it the feature filter is blind
    }
    if (evalT && !L.eval.Install(evalT, kDetours[slot].eval, &err))
        LOGW("ngx: EvaluateFeature hook failed for %ls: %s", path.c_str(), err.c_str());
    if (evalCT && !L.eval_c.Install(evalCT, kDetours[slot].eval_c, &err))
        LOGW("ngx: EvaluateFeature_C hook failed for %ls: %s", path.c_str(), err.c_str());
    L.mod = m;
    L.sizeOfImage = ImageSizeAt(m);
    L.path = path;
    L.used = true;
    LOGI("ngx: hooked %ls (slot %d)%s", path.c_str(), slot,
         L.neverCount ? ", a denoiser snippet: forwarded, never counted" : "");
    return true;
}

// Forgets a layer without touching its module's memory. Caller holds scanLock.
void DetachLayerLocked(Layer& L) {
    L.create.Detach();
    L.eval.Detach();
    L.eval_c.Detach();
    L.mod = nullptr;
    L.sizeOfImage = 0;
    L.path.clear();
    L.used = false;
    L.neverCount = false;
}

void RecountLocked() {
    HookState& s = S();
    uint32_t n = 0;
    for (const Layer& L : s.layers)
        if (L.used) ++n;
    s.hooked.store(n, std::memory_order_release);
}

// True when the layer's own module is still loaded at its base and still holds
// our patches: same base, same SizeOfImage, same full path, and every installed
// jump intact. A module loaded anew at that base (even the same DLL again) never
// holds our jumps, so it is never taken for the hooked one (ngx review F6).
// Caller holds scanLock.
bool LayerIsLiveLocked(const Layer& L) {
    HMODULE h = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(L.mod), &h) ||
        h != L.mod)
        return false;
    if (ImageSizeAt(L.mod) != L.sizeOfImage) return false;
    if (_wcsicmp(ModulePath(L.mod).c_str(), L.path.c_str()) != 0) return false;
    if (!L.create.PatchIntact()) return false;
    if (L.eval.Active() && !L.eval.PatchIntact()) return false;
    if (L.eval_c.Active() && !L.eval_c.PatchIntact()) return false;
    return true;
}

// Drops every layer that is no longer its own live module. Caller holds scanLock.
void VerifyLayersLocked() {
    HookState& s = S();
    for (Layer& L : s.layers)
        if (L.used && !LayerIsLiveLocked(L)) DetachLayerLocked(L);
}

// Applies one unload the callback reported: the layer at that base is dropped
// without a write to its memory. A layer there that is still live belongs to a
// module a later scan hooked at the reused base, and stays. Caller holds
// scanLock.
void ApplyUnloadLocked(uintptr_t base) {
    HookState& s = S();
    for (Layer& L : s.layers)
        if (L.used && reinterpret_cast<uintptr_t>(L.mod) == base && !LayerIsLiveLocked(L)) DetachLayerLocked(L);
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

    SetSink(sink);  // before any hook exists; never under scanLock
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
    if (overflow) VerifyLayersLocked();
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
    SetSink(nullptr);  // waits for sink calls in progress; never under scanLock
    ExclusiveLock scan(&s.scanLock);
    // A module that went away since the last drain is detached, never written to.
    DrainEventsLocked();
    for (Layer& L : s.layers) {
        if (!L.used) continue;
        // Pin the module so it cannot unload between the identity check and the
        // restore; restore only into the module that was hooked (F6).
        HMODULE pinned = nullptr;
        const bool pin = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                            reinterpret_cast<LPCWSTR>(L.mod), &pinned) != FALSE;
        if (pin && pinned == L.mod && LayerIsLiveLocked(L)) {
            L.create.Remove();
            L.eval.Remove();
            L.eval_c.Remove();
        }
        DetachLayerLocked(L);  // frees whatever Remove did not
        if (pin) FreeLibrary(pinned);
    }
    RecountLocked();
    {
        ExclusiveLock rec(&s.recLock);
        s.records.clear();
    }
    s.pendingRescan.store(false);
    s.workPending.store(false);
}

uint32_t NgxHook::HookedModules() const { return S().hooked.load(std::memory_order_acquire); }

void NgxHook::SetInstallGapHookForTest(void (*fn)()) { S().installGapHook = fn; }

void NgxHook::SetSink(NgxEvaluateSink* sink) {
    HookState& s = S();
    if (t_sinkDepth > 0) {
        // Called from inside a sink call on this thread: this thread holds
        // sinkLock shared, so waiting would wait on itself. Swap only.
        s.sink.store(sink, std::memory_order_release);
        return;
    }
    // Exclusive waits until every call into the old sink has returned; new calls
    // queue behind it and then see the new sink.
    ExclusiveLock lock(&s.sinkLock);
    s.sink.store(sink, std::memory_order_release);
}

void NgxHook::LockStateForTest() {
    HookState& s = S();
    AcquireSRWLockExclusive(&s.scanLock);
    AcquireSRWLockExclusive(&s.recLock);
    AcquireSRWLockExclusive(&s.sinkLock);
}

void NgxHook::UnlockStateForTest() {
    HookState& s = S();
    ReleaseSRWLockExclusive(&s.sinkLock);
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
    const bool neverCount = s.layers[slot].neverCount;
    ++t_nest;
    NgxResult r = orig(ctx, featureId, params, outHandle);
    const int nest = t_nest;
    --t_nest;
    if (nest != 1) return r;  // nested NGX-internal create: forward only
    if (r != kNgxSuccess || !outHandle || !*outHandle || !Plausible(params)) return r;
    try {
        const uint64_t key = reinterpret_cast<uint64_t>(*outHandle);
        if (neverCount) {
            // A denoiser snippet's own entry point (F4): the handle is recorded as
            // not ours, so no layer counts an evaluate on it.
            ExclusiveLock rec(&s.recLock);
            s.records[key] = FeatureRecord{};
            return r;
        }
        const char* denoiserKey = DenoiserKeyIn(params);
        const bool denoiser = denoiserKey != nullptr;
        if (denoiser && featureId == kNgxFeatureSuperSampling) {
            // Shown once, so an in-game log says why a SuperSampling feature was
            // not mirrored if CSP ever shares one parameter block with its NR.
            static LONG said = 0;
            if (InterlockedCompareExchange(&said, 1, 0) == 0)
                LOGW("ngx: a SuperSampling create carries the denoiser input %s; not captured", denoiserKey);
        } else if (featureId == kNgxFeatureSuperSampling) {
            if (const char* hint = DenoiserScalarKeyIn(params)) {
                static LONG saidHint = 0;
                if (InterlockedCompareExchange(&saidHint, 1, 0) == 0)
                    LOGI("ngx: a SuperSampling create carries the denoiser scalar %s; captured anyway", hint);
            }
        }
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
            s.records[key] = FeatureRecord{supersampling, info.createFlags, info.width, info.height};
        }
        if (supersampling) CallSink([&](NgxEvaluateSink* sink) { sink->OnCreateFeature(key, info); });
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
    const bool neverCount = s.layers[slot].neverCount;
    ++t_nest;
    NgxResult r = orig(ctx, handle, params, callback);
    const int nest = t_nest;
    --t_nest;
    if (nest != 1) return r;  // nested: touch nothing (spec 6.5)
    if (neverCount) return r;  // a denoiser snippet's own entry point (F4)
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
        uint32_t recFlags = 0, recWidth = 0, recHeight = 0;
        {
            SharedLock rec(&s.recLock);
            auto it = s.records.find(key);
            if (it != s.records.end()) {
                observed = true;
                supersampling = it->second.supersampling;
                recFlags = it->second.createFlags;
                recWidth = it->second.width;
                recHeight = it->second.height;
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
            // A denoiser's block (DLSS NR / RR) never counts, whatever else it
            // carries (F4).
            if (DenoiserKeyIn(params)) return r;
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
        // The render subrect. Absent or 0 means "not set" (ngx review F3): NVIDIA's
        // evaluate helper always writes the key, as 0 when the app does not use
        // dynamic resolution, and DLSS then takes the create-time input size
        // (DLSS programming guide 3.17). So: a non-zero subrect, else a non-zero
        // Width/Height from this block, else the observed create's input size.
        auto pick = [params](const char* subrectKey, const char* sizeKey, uint32_t createSize) -> uint32_t {
            unsigned int v = 0;
            if (params->Get(subrectKey, &v) == kNgxSuccess && v != 0) return v;
            v = 0;
            if (params->Get(sizeKey, &v) == kNgxSuccess && v != 0) return v;
            return createSize;
        };
        in.subrectW = pick(ngxkey::kSubrectWidth, ngxkey::kWidth, recWidth);
        in.subrectH = pick(ngxkey::kSubrectHeight, ngxkey::kHeight, recHeight);
        int reset = 0;
        if (params->Get(ngxkey::kReset, &reset) == kNgxSuccess) in.reset = reset != 0;

        CallSink([&](NgxEvaluateSink* sink) { sink->OnEvaluate(in); });
    } catch (...) {
    }
    return r;
}

}  // namespace acdb
