#pragma once
// The CSP Lua app's settings window as the bridge sees it, for the test
// app's --fake-panel (spec 6.9, 11). Header-only, so that
// tests/test_testapp_fakes.cpp and tests/test_presenter.cpp check it against
// the bridge's own channels; the test app itself links nothing of the bridge
// and uses only the layouts of src/panel_status.h.
//
//  - Open: the status section as ac.readMemoryMappedFile opens it (the
//    existing section, read-only: it fails while the bridge has not created
//    it) and the control section as ac.writeMemoryMappedFile (created or
//    opened with PAGE_READWRITE, mapped for read and write); the request
//    counter continues from the section's, as the Lua app's does.
//  - ReadStatus: readStatus() of apps/lua/AcDlssg/AcDlssg.lua, a seqlock
//    reader, repeated until a copy is stable.
//  - Request: sendRequest(), statement by statement: seq odd, barrier, magic,
//    version, the desired switches and multiplier, the new counter, barrier,
//    seq even.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "panel_status.h"

namespace testapp {

class FakePanel {
public:
    FakePanel() = default;
    ~FakePanel() { Close(); }
    FakePanel(const FakePanel&) = delete;
    FakePanel& operator=(const FakePanel&) = delete;

    bool Open(const wchar_t* statusName, const wchar_t* controlName, std::string* error) {
        Close();
        status_section_ = OpenFileMappingW(FILE_MAP_READ, FALSE, statusName);
        if (!status_section_) return Failed("OpenFileMappingW(status)", error);
        status_ = static_cast<const acdb::StatusLayout*>(
            MapViewOfFile(status_section_, FILE_MAP_READ, 0, 0, sizeof(acdb::StatusLayout)));
        if (!status_) return Failed("MapViewOfFile(status)", error);
        control_section_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                              static_cast<DWORD>(sizeof(acdb::ControlLayout)), controlName);
        if (!control_section_) return Failed("CreateFileMappingW(control)", error);
        control_ = static_cast<acdb::ControlLayout*>(
            MapViewOfFile(control_section_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(acdb::ControlLayout)));
        if (!control_) return Failed("MapViewOfFile(control)", error);
        counter_ = Load(control_->requestCounter);
        return true;
    }

    void Close() {
        if (status_) UnmapViewOfFile(status_);
        status_ = nullptr;
        if (status_section_) CloseHandle(status_section_);
        status_section_ = nullptr;
        if (control_) UnmapViewOfFile(control_);
        control_ = nullptr;
        if (control_section_) CloseHandle(control_section_);
        control_section_ = nullptr;
    }

    bool IsOpen() const { return status_ && control_; }

    // A stable status record with the bridge's magic and version; false when
    // there is none (no write yet, or every attempt raced a write).
    bool ReadStatus(acdb::StatusLayout* out) const {
        if (!status_) return false;
        constexpr size_t kWords = sizeof(acdb::StatusLayout) / sizeof(uint32_t);
        const auto* words = reinterpret_cast<const uint32_t*>(status_);
        for (int attempt = 0; attempt < 1000; ++attempt) {
            const uint32_t s1 = Load(status_->seq);
            if (s1 & 1u) continue;
            std::atomic_thread_fence(std::memory_order_seq_cst);  // memoryBarrier()
            uint32_t copy[kWords];
            for (size_t i = 0; i < kWords; ++i) copy[i] = Load(words[i]);
            std::atomic_thread_fence(std::memory_order_seq_cst);  // memoryBarrier()
            if (Load(status_->seq) != s1) continue;
            std::memcpy(out, copy, sizeof(copy));
            return out->magic == acdb::kStatusMagic && out->version == acdb::kStatusVersion;
        }
        return false;
    }

    // multiplier: the 2X/3X/4X button (desiredMultiplier); 0 keeps the bridge's.
    void Request(bool fg, bool flip, bool negate, bool save, uint32_t multiplier = 0) {
        if (!control_) return;
        acdb::ControlLayout* c = control_;
        const uint32_t s = (Load(c->seq) | 1u) & 0x7FFFFFFFu;  // bit.band(bit.bor(ctl.seq, 1), 0x7FFFFFFF)
        Store(c->seq, s);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        counter_ = counter_ % 0x7FFFFFFFu + 1u;
        Store(c->magic, acdb::kControlMagic);
        Store(c->version, acdb::kControlVersion);
        Store(c->fgEnabled, fg ? 1u : 0u);
        Store(c->cameraFlipHandedness, flip ? 1u : 0u);
        Store(c->cameraNegateSide, negate ? 1u : 0u);
        Store(c->saveAsDefault, save ? 1u : 0u);
        Store(c->desiredMultiplier, multiplier);
        Store(c->requestCounter, counter_);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        Store(c->seq, s + 1);
    }

    // The counter of the last request.
    uint32_t Counter() const { return counter_; }

    // Test hook: leaves seq as a writer in the middle of a write would.
    void SetControlSeqForTest(uint32_t seq) {
        if (control_) Store(control_->seq, seq);
    }
    uint32_t ControlSeq() const { return control_ ? Load(control_->seq) : 0; }

private:
    static uint32_t Load(const uint32_t& v) {
        return std::atomic_ref<uint32_t>(const_cast<uint32_t&>(v)).load(std::memory_order_relaxed);
    }
    static void Store(uint32_t& v, uint32_t value) {
        std::atomic_ref<uint32_t>(v).store(value, std::memory_order_relaxed);
    }

    bool Failed(const char* what, std::string* error) {
        const DWORD code = GetLastError();
        Close();
        if (error) *error = std::string(what) + " failed: error " + std::to_string(code);
        return false;
    }

    HANDLE status_section_ = nullptr;
    const acdb::StatusLayout* status_ = nullptr;
    HANDLE control_section_ = nullptr;
    acdb::ControlLayout* control_ = nullptr;
    uint32_t counter_ = 0;
};

}  // namespace testapp
