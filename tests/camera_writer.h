#pragma once
// The Lua app's side of a camera section of the test's own (never the game's
// Local\AcDlssg.Camera.v1), for tests that need a camera writer on the same
// thread as the reader. Every Write publishes a complete, stable record.
#include <windows.h>

#include <atomic>
#include <cstring>
#include <string>

#include "camera_layout.h"

namespace acdb_test {

class TestCameraWriter {
public:
    TestCameraWriter() {
        static std::atomic<unsigned> counter{0};
        name_ = L"Local\\AcDlssg.Camera.writer." + std::to_wstring(GetCurrentProcessId()) + L"." +
                std::to_wstring(counter++);
        handle_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, name_.c_str());
        if (handle_)
            view_ = static_cast<acdb::CameraLayout*>(MapViewOfFile(handle_, FILE_MAP_WRITE, 0, 0, sizeof(acdb::CameraLayout)));
    }
    ~TestCameraWriter() {
        if (view_) UnmapViewOfFile(view_);
        if (handle_) CloseHandle(handle_);
    }
    TestCameraWriter(const TestCameraWriter&) = delete;
    TestCameraWriter& operator=(const TestCameraWriter&) = delete;

    const std::wstring& Name() const { return name_; }
    bool Ok() const { return view_ != nullptr; }

    // A camera at z = frame * 0.1 looking down +z, with the given render size.
    static acdb::CameraLayout Camera(uint32_t frame, uint32_t flags = 0, float renderW = 64.0f,
                                     float renderH = 36.0f) {
        acdb::CameraLayout c{};
        c.magic = acdb::kCameraMagic;
        c.version = acdb::kCameraVersion;
        c.frame = frame;
        c.pos[2] = static_cast<float>(frame) * 0.1f;
        c.fwd[2] = 1.0f;
        c.up[1] = 1.0f;
        c.side[0] = 1.0f;
        c.fovVDeg = 60.0f;
        c.clipNear = 0.1f;
        c.clipFar = 1000.0f;
        c.renderW = renderW;
        c.renderH = renderH;
        c.flags = flags;
        c.dt = 0.016f;
        return c;
    }

    void Write(const acdb::CameraLayout& c) {
        if (!view_) return;
        seq_ += 2;
        acdb::CameraLayout copy = c;
        copy.seq = seq_;
        std::memcpy(view_, &copy, sizeof(copy));
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }
    void Write(uint32_t frame, uint32_t flags = 0, float renderW = 64.0f, float renderH = 36.0f) {
        Write(Camera(frame, flags, renderW, renderH));
    }

private:
    std::wstring name_;
    HANDLE handle_ = nullptr;
    acdb::CameraLayout* view_ = nullptr;
    uint32_t seq_ = 0;
};

}  // namespace acdb_test
