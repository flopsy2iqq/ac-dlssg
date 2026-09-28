#pragma once
// CameraChannel (spec 6.6): the bridge's read side of the shared section the
// CSP Lua app writes the camera into, plus the per-capture freshness latch.
//
// The DLL creates the section at bootstrap with PAGE_READWRITE, because CSP's
// ac.writeMemoryMappedFile opens the existing object and maps it for read and
// write; the DLL itself maps a FILE_MAP_READ view only. If CSP created the
// section first, the DLL opens that one instead (it keeps CSP's size).
#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>

#include "camera_layout.h"

namespace acdb {

constexpr wchar_t kCameraSectionName[] = L"Local\\AcDlssg.Camera.v1";
// Larger than the layout, so a later layout version still fits a section the
// DLL created. An existing section keeps its size when opened again.
constexpr DWORD kCameraSectionSize = 4096;

class CameraChannel {
public:
    // The process-wide channel on kCameraSectionName.
    static CameraChannel& Get();

    // Tests use their own section names, so they never touch the section of
    // a game that runs at the same time.
    explicit CameraChannel(const wchar_t* sectionName = kCameraSectionName);
    ~CameraChannel();
    CameraChannel(const CameraChannel&) = delete;
    CameraChannel& operator=(const CameraChannel&) = delete;

    // CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
    // kCameraSectionSize, name) and a FILE_MAP_READ view of sizeof(CameraLayout)
    // bytes. Idempotent; safe when the section already exists. On failure it
    // returns false, fills error and leaves the channel without a section.
    bool Create(std::string* error);

    enum class ReadResult { Ok, NoSection, NotWritten, Torn, WriteFailed };
    // Seqlock reader (spec 6.6), at most 2 attempts. An attempt fails when seq
    // is odd or changes during the copy; two failed attempts give Torn. A
    // stable record whose magic or version is not ours gives NotWritten, one
    // with kCamWriteFailed set gives WriteFailed. *out is written only on Ok.
    // Never blocks and never writes to the section.
    ReadResult Read(CameraLayout* out) const;

    const std::wstring& SectionName() const { return name_; }

private:
    const std::wstring name_;
    std::mutex create_mu_;
    HANDLE section_ = nullptr;
    std::atomic<const CameraLayout*> view_{nullptr};
};

// Freshness rule of spec 6.6. At every captured evaluate the caller latches the
// snapshot it read; the snapshot is fresh only if its frame is greater than
// the frame latched at the previous captured evaluate, which means the Lua app
// wrote during this frame. The first latch after construction or Reset has
// nothing to compare with and is not fresh. Every call records the snapshot's
// frame, so a writer that restarted with a lower counter is fresh again one
// frame later.
class CameraLatch {
public:
    bool Latch(const CameraLayout& snapshot);
    void Reset();

private:
    bool have_ = false;
    uint32_t frame_ = 0;
};

}  // namespace acdb
