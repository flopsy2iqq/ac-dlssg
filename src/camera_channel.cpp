#include "camera_channel.h"

#include <cstdio>
#include <cstring>

namespace acdb {
namespace {

constexpr size_t kWords = sizeof(CameraLayout) / sizeof(uint32_t);
static_assert(sizeof(CameraLayout) % sizeof(uint32_t) == 0);
constexpr size_t kSeqWord = offsetof(CameraLayout, seq) / sizeof(uint32_t);

// The view is read-only memory that another thread (or process) writes, so
// every access is an atomic load: the compiler can neither cache nor tear
// nor reorder them across the fences below. On x64 each one is a plain mov.
uint32_t LoadWord(const uint32_t* words, size_t i, std::memory_order order) {
    return std::atomic_ref<uint32_t>(const_cast<uint32_t&>(words[i])).load(order);
}

std::string Win32Error(const char* what, const std::wstring& name, DWORD code) {
    char narrow[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, narrow, static_cast<int>(sizeof(narrow)) - 1, nullptr,
                        nullptr);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s(%s) failed: error %lu", what, narrow, static_cast<unsigned long>(code));
    return buf;
}

}  // namespace

CameraChannel& CameraChannel::Get() {
    static CameraChannel instance;
    return instance;
}

CameraChannel::CameraChannel(const wchar_t* sectionName) : name_(sectionName ? sectionName : L"") {}

CameraChannel::~CameraChannel() {
    if (const CameraLayout* view = view_.exchange(nullptr)) UnmapViewOfFile(view);
    if (section_) CloseHandle(section_);
}

bool CameraChannel::Create(std::string* error) {
    std::lock_guard<std::mutex> lock(create_mu_);
    if (view_.load(std::memory_order_acquire)) return true;

    // An existing section (CSP's app ran first) is opened with its own size.
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kCameraSectionSize,
                                       name_.c_str());
    if (!section) {
        if (error) *error = Win32Error("CreateFileMappingW", name_, GetLastError());
        return false;
    }
    // sizeof(CameraLayout), not the section size: a section CSP created is
    // only as large as its layout struct.
    void* view = MapViewOfFile(section, FILE_MAP_READ, 0, 0, sizeof(CameraLayout));
    if (!view) {
        if (error) *error = Win32Error("MapViewOfFile", name_, GetLastError());
        CloseHandle(section);
        return false;
    }
    section_ = section;
    view_.store(static_cast<const CameraLayout*>(view), std::memory_order_release);
    return true;
}

CameraChannel::ReadResult CameraChannel::Read(CameraLayout* out) const {
    const CameraLayout* view = view_.load(std::memory_order_acquire);
    if (!view) return ReadResult::NoSection;
    const auto* words = reinterpret_cast<const uint32_t*>(view);

    for (int attempt = 0; attempt < 2; ++attempt) {
        // Seqlock read (Boehm, "Can Seqlocks Get Along With Programming
        // Language Memory Models?"): acquire-load seq, relaxed-load the data,
        // acquire fence, load seq again.
        const uint32_t seq1 = LoadWord(words, kSeqWord, std::memory_order_acquire);
        if (seq1 & 1u) continue;  // a write is in progress
        uint32_t copy[kWords];
        for (size_t i = 0; i < kWords; ++i) copy[i] = LoadWord(words, i, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t seq2 = LoadWord(words, kSeqWord, std::memory_order_relaxed);
        if (seq1 != seq2 || copy[kSeqWord] != seq1) continue;  // a write happened during the copy

        CameraLayout snapshot;
        std::memcpy(&snapshot, copy, sizeof(snapshot));
        // A record of another layout (or none yet): its flags mean nothing.
        if (snapshot.magic != kCameraMagic || snapshot.version != kCameraVersion) return ReadResult::NotWritten;
        if (snapshot.flags & kCamWriteFailed) return ReadResult::WriteFailed;
        *out = snapshot;
        return ReadResult::Ok;
    }
    return ReadResult::Torn;
}

bool CameraLatch::Latch(const CameraLayout& snapshot) {
    const bool fresh = have_ && snapshot.frame > frame_;
    have_ = true;
    frame_ = snapshot.frame;
    return fresh;
}

void CameraLatch::Reset() {
    have_ = false;
    frame_ = 0;
}

}  // namespace acdb
