#include "panel_status.h"

#include <cstdio>
#include <cstring>

namespace acdb {
namespace {

// The views are memory another thread (or process) writes, so every access
// is an atomic load or store: the compiler can neither cache, tear nor
// reorder them across the fences. On x64 each one is a plain mov.
uint32_t LoadWord(const uint32_t* words, size_t i, std::memory_order order) {
    return std::atomic_ref<uint32_t>(const_cast<uint32_t&>(words[i])).load(order);
}

void StoreWord(uint32_t* words, size_t i, uint32_t value, std::memory_order order) {
    std::atomic_ref<uint32_t>(words[i]).store(value, order);
}

// Seqlock read of a record of whole 32-bit words (Boehm, "Can Seqlocks Get
// Along With Programming Language Memory Models?"): acquire-load seq,
// relaxed-load the data, acquire fence, load seq again. At most 2 attempts;
// false when both saw a write in progress.
template <class T>
bool SeqlockRead(const T* view, T* out) {
    static_assert(sizeof(T) % sizeof(uint32_t) == 0);
    constexpr size_t kWords = sizeof(T) / sizeof(uint32_t);
    constexpr size_t kSeqWord = offsetof(T, seq) / sizeof(uint32_t);
    const auto* words = reinterpret_cast<const uint32_t*>(view);
    for (int attempt = 0; attempt < 2; ++attempt) {
        const uint32_t seq1 = LoadWord(words, kSeqWord, std::memory_order_acquire);
        if (seq1 & 1u) continue;
        uint32_t copy[kWords];
        for (size_t i = 0; i < kWords; ++i) copy[i] = LoadWord(words, i, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t seq2 = LoadWord(words, kSeqWord, std::memory_order_relaxed);
        if (seq1 != seq2 || copy[kSeqWord] != seq1) continue;
        std::memcpy(out, copy, sizeof(T));
        return true;
    }
    return false;
}

std::string Win32Error(const char* what, const std::wstring& name, DWORD code) {
    char narrow[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, narrow, static_cast<int>(sizeof(narrow)) - 1, nullptr,
                        nullptr);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s(%s) failed: error %lu", what, narrow, static_cast<unsigned long>(code));
    return buf;
}

// The counters the Lua app compares stay below 2^31, like its own.
constexpr uint32_t kCounterMask = 0x7FFFFFFFu;

}  // namespace

void CopyText(char* dst, size_t size, const std::string& text) {
    if (!dst || size == 0) return;
    size_t n = text.size() < size - 1 ? text.size() : size - 1;
    // Cut before a UTF-8 continuation byte, never in the middle of a character.
    if (n < text.size()) {
        while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) --n;
    }
    std::memcpy(dst, text.data(), n);
    std::memset(dst + n, 0, size - n);
}

std::string TextOf(const char* src, size_t size) {
    if (!src) return {};
    size_t n = 0;
    while (n < size && src[n] != '\0') ++n;
    return std::string(src, n);
}

// ---------------------------------------------------------------- status

PanelStatusChannel& PanelStatusChannel::Get() {
    static PanelStatusChannel instance;
    return instance;
}

PanelStatusChannel::PanelStatusChannel(const wchar_t* sectionName) : name_(sectionName ? sectionName : L"") {}

PanelStatusChannel::~PanelStatusChannel() {
    if (StatusLayout* view = view_.exchange(nullptr)) UnmapViewOfFile(view);
    if (section_) CloseHandle(section_);
}

bool PanelStatusChannel::Create(std::string* error) {
    std::lock_guard<std::mutex> lock(mu_);
    if (view_.load(std::memory_order_acquire)) return true;
    HANDLE section =
        CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kPanelSectionSize, name_.c_str());
    if (!section) {
        if (error) *error = Win32Error("CreateFileMappingW", name_, GetLastError());
        return false;
    }
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    auto* view = static_cast<StatusLayout*>(MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, sizeof(StatusLayout)));
    if (!view) {
        if (error) *error = Win32Error("MapViewOfFile", name_, GetLastError());
        CloseHandle(section);
        return false;
    }
    section_ = section;
    view_.store(view, std::memory_order_release);

    const uint32_t pid = GetCurrentProcessId();
    if (existed) {
        // A record another bridge keeps publishing: a torn read is retried,
        // since that writer finishes a write in microseconds.
        StatusLayout current{};
        bool stable = false;
        for (int i = 0; i < 100 && !stable; ++i) {
            stable = SeqlockRead(view, &current);
            if (!stable) Sleep(0);
        }
        if (stable && current.magic == kStatusMagic && current.ownerPid != 0 && current.ownerPid != pid) {
            foreign_.store(current.ownerPid);
            return true;
        }
        // Zeros (the Lua app opened it first), or our own process's record.
        record_.seq = current.seq & ~1u;
        record_.heartbeat = current.heartbeat;
    }
    owned_.store(true);
    record_.bridgeState = kPanelNotLoaded;
    CopyText(record_.bridgeVersion, ACDB_VERSION);
    PublishLocked();
    return true;
}

bool PanelStatusChannel::Owned() const { return owned_.load(); }

uint32_t PanelStatusChannel::ForeignOwner() const { return foreign_.load(); }

void PanelStatusChannel::PublishLocked() {
    StatusLayout* view = view_.load(std::memory_order_acquire);
    if (!view) return;
    constexpr size_t kWords = sizeof(StatusLayout) / sizeof(uint32_t);
    constexpr size_t kSeqWord = offsetof(StatusLayout, seq) / sizeof(uint32_t);
    record_.magic = kStatusMagic;
    record_.version = kStatusVersion;
    record_.ownerPid = GetCurrentProcessId();
    record_.heartbeat = (record_.heartbeat + 1) & kCounterMask;
    const uint32_t odd = ((record_.seq | 1u) & kCounterMask) | 1u;
    record_.seq = (odd + 1) & kCounterMask;
    uint32_t words[kWords];
    std::memcpy(words, &record_, sizeof(words));
    auto* out = reinterpret_cast<uint32_t*>(view);
    // Seqlock writer: seq odd, release fence, the data, seq even (release).
    StoreWord(out, kSeqWord, odd, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (size_t i = 0; i < kWords; ++i) {
        if (i != kSeqWord) StoreWord(out, i, words[i], std::memory_order_relaxed);
    }
    StoreWord(out, kSeqWord, record_.seq, std::memory_order_release);
}

PanelStatusChannel::ReadResult PanelStatusChannel::Read(StatusLayout* out) const {
    const StatusLayout* view = view_.load(std::memory_order_acquire);
    if (!view) return ReadResult::NoSection;
    StatusLayout copy{};
    if (!SeqlockRead(view, &copy)) return ReadResult::Torn;
    if (copy.magic != kStatusMagic || copy.version != kStatusVersion) return ReadResult::NotWritten;
    *out = copy;
    return ReadResult::Ok;
}

// ---------------------------------------------------------------- control

PanelControlChannel& PanelControlChannel::Get() {
    static PanelControlChannel instance;
    return instance;
}

PanelControlChannel::PanelControlChannel(const wchar_t* sectionName) : name_(sectionName ? sectionName : L"") {}

PanelControlChannel::~PanelControlChannel() {
    if (const ControlLayout* view = view_.exchange(nullptr)) UnmapViewOfFile(view);
    if (section_) CloseHandle(section_);
}

bool PanelControlChannel::Create(std::string* error) {
    std::lock_guard<std::mutex> lock(create_mu_);
    if (view_.load(std::memory_order_acquire)) return true;
    HANDLE section =
        CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kPanelSectionSize, name_.c_str());
    if (!section) {
        if (error) *error = Win32Error("CreateFileMappingW", name_, GetLastError());
        return false;
    }
    void* view = MapViewOfFile(section, FILE_MAP_READ, 0, 0, sizeof(ControlLayout));
    if (!view) {
        if (error) *error = Win32Error("MapViewOfFile", name_, GetLastError());
        CloseHandle(section);
        return false;
    }
    section_ = section;
    view_.store(static_cast<const ControlLayout*>(view), std::memory_order_release);
    return true;
}

bool PanelControlChannel::Ready() const { return view_.load(std::memory_order_acquire) != nullptr; }

uint32_t PanelControlChannel::Seq() const {
    const ControlLayout* view = view_.load(std::memory_order_acquire);
    if (!view) return 0;
    return LoadWord(reinterpret_cast<const uint32_t*>(view), offsetof(ControlLayout, seq) / sizeof(uint32_t),
                    std::memory_order_acquire);
}

PanelControlChannel::ReadResult PanelControlChannel::Read(ControlLayout* out) const {
    const ControlLayout* view = view_.load(std::memory_order_acquire);
    if (!view) return ReadResult::NoSection;
    ControlLayout copy{};
    if (!SeqlockRead(view, &copy)) return ReadResult::Torn;
    if (copy.magic != kControlMagic || copy.version != kControlVersion) return ReadResult::NotWritten;
    *out = copy;
    return ReadResult::Ok;
}

}  // namespace acdb
