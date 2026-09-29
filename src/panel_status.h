#pragma once
// The in-game panel's shared sections (spec 6.9). The panel is a window of
// the CSP Lua app (apps/lua/AcDlssg/AcDlssg.lua), in ReShade mode and in
// standalone mode alike:
//  - Local\AcDlssg.Status.v1: the bridge publishes its state, once per
//    second from the statistics path and at once on every DLSS-G mode
//    change and applied panel request; the Lua app only reads it
//    (ac.readMemoryMappedFile).
//  - Local\AcDlssg.Control.v1: the Lua app writes the user's requests
//    (ac.writeMemoryMappedFile); the bridge reads them at the start of every
//    PresentFrame (D3D12Presenter).
// The bridge creates both at bootstrap with PAGE_READWRITE and 4096 bytes,
// like the camera section (spec 6.6). Both records are seqlocks: seq is odd
// while the writer writes and even when the record is stable.
//
// The structs mirror the Lua ffi layout strings STATUS_LAYOUT and
// CONTROL_LAYOUT field by field; LuaJIT lays them out with natural C
// alignment, like MSVC here. The static_asserts pin every offset, and
// tests/test_panel.cpp parses the Lua file and compares its layout strings
// with these structs.
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace acdb {

constexpr uint32_t kStatusMagic = 0x54534C44;   // 'DLST' little-endian
constexpr uint32_t kStatusVersion = 1;
constexpr uint32_t kControlMagic = 0x43534C44;  // 'DLSC' little-endian
constexpr uint32_t kControlVersion = 1;

// StatusLayout::bridgeState.
enum PanelBridgeState : uint32_t {
    kPanelNotLoaded = 0,    // bootstrap ran, no main-window swap chain was decided yet (stateReason says more)
    kPanelPassThrough = 1,  // the game's chain passes through: stateReason says why
    kPanelProxyNoFg = 2,    // proxied, but DLSS-G cannot run: stateReason says why
    kPanelFgAvailable = 3,  // proxied with DLSS-G
};

// StatusLayout::mode.
enum PanelModeCode : uint32_t {
    kPanelModeUnknown = 0,
    kPanelModeReShade = 1,     // ReShade's [PROXY] ProxyLibrary (BridgeMode::Proxy)
    kPanelModeStandalone = 2,  // the game folder's dxgi.dll
};

constexpr size_t kPanelReasonChars = 160;

struct StatusLayout {
    uint32_t magic;       // kStatusMagic
    uint32_t version;     // kStatusVersion
    uint32_t seq;         // seqlock: odd while the bridge writes
    uint32_t heartbeat;   // +1 per publish; a running presenter publishes at least once per second
    uint32_t ownerPid;    // the process whose bridge publishes
    uint32_t bridgeState; // PanelBridgeState
    uint32_t mode;        // PanelModeCode
    uint32_t fgOn;        // Streamline has DLSS-G on
    uint32_t fgUserOn;    // the user's switch: start_with_fg, then the hotkey and the panel
    uint32_t spoofLoaded; // dlssg_for_sm86's version.dll is loaded from the game folder
    uint32_t rtx30;       // the render GPU is an SM86 Ampere chip (RTX 30)
    uint32_t vsyncNote;   // VSync was asked for but is not available with DLSS-G here
    uint32_t driverWarning;  // a driver profile setting changes DLSS-G (warning holds the text)
    uint32_t cameraFlipHandedness;  // the current camera switches (spec 6.7)
    uint32_t cameraNegateSide;
    uint32_t startWithFg;     // start_with_fg as in ac-dlssg.ini (after the last save)
    uint32_t controlApplied;  // requestCounter of the last control request the bridge applied
    uint32_t saveCounter;     // requestCounter of the last "Save as default" (0: none yet)
    uint32_t saveOk;          // that save succeeded
    float baseFps;            // CSP frames per second
    float presentedFps;       // presented frames per second, generated ones included
    float bridgeGpuMs;        // the bridge's GPU time per frame (both copies); negative when not measured
    uint32_t vramUsageMib;    // the render adapter's local video memory
    uint32_t vramBudgetMib;   // 0: unknown
    float capturesPerSec;     // Presents paired with a DLSS capture
    float cameraFreshPerSec;  // ... whose camera snapshot was fresh
    float taggedPerSec;       // Presents with tags and constants
    char reason[kPanelReasonChars];       // "on", or why DLSS-G is off now
    char stateReason[kPanelReasonChars];  // why bridgeState is below kPanelFgAvailable
    char warning[kPanelReasonChars];      // the first driver-profile warning
    char gpuName[64];                     // the render adapter
    char hotkey[32];                      // e.g. "Ctrl+F10"
    char bridgeVersion[32];               // ACDB_VERSION
};

static_assert(sizeof(StatusLayout) == 716);
static_assert(alignof(StatusLayout) == 4);
static_assert(offsetof(StatusLayout, magic) == 0);
static_assert(offsetof(StatusLayout, version) == 4);
static_assert(offsetof(StatusLayout, seq) == 8);
static_assert(offsetof(StatusLayout, heartbeat) == 12);
static_assert(offsetof(StatusLayout, ownerPid) == 16);
static_assert(offsetof(StatusLayout, bridgeState) == 20);
static_assert(offsetof(StatusLayout, mode) == 24);
static_assert(offsetof(StatusLayout, fgOn) == 28);
static_assert(offsetof(StatusLayout, fgUserOn) == 32);
static_assert(offsetof(StatusLayout, spoofLoaded) == 36);
static_assert(offsetof(StatusLayout, rtx30) == 40);
static_assert(offsetof(StatusLayout, vsyncNote) == 44);
static_assert(offsetof(StatusLayout, driverWarning) == 48);
static_assert(offsetof(StatusLayout, cameraFlipHandedness) == 52);
static_assert(offsetof(StatusLayout, cameraNegateSide) == 56);
static_assert(offsetof(StatusLayout, startWithFg) == 60);
static_assert(offsetof(StatusLayout, controlApplied) == 64);
static_assert(offsetof(StatusLayout, saveCounter) == 68);
static_assert(offsetof(StatusLayout, saveOk) == 72);
static_assert(offsetof(StatusLayout, baseFps) == 76);
static_assert(offsetof(StatusLayout, presentedFps) == 80);
static_assert(offsetof(StatusLayout, bridgeGpuMs) == 84);
static_assert(offsetof(StatusLayout, vramUsageMib) == 88);
static_assert(offsetof(StatusLayout, vramBudgetMib) == 92);
static_assert(offsetof(StatusLayout, capturesPerSec) == 96);
static_assert(offsetof(StatusLayout, cameraFreshPerSec) == 100);
static_assert(offsetof(StatusLayout, taggedPerSec) == 104);
static_assert(offsetof(StatusLayout, reason) == 108);
static_assert(offsetof(StatusLayout, stateReason) == 268);
static_assert(offsetof(StatusLayout, warning) == 428);
static_assert(offsetof(StatusLayout, gpuName) == 588);
static_assert(offsetof(StatusLayout, hotkey) == 652);
static_assert(offsetof(StatusLayout, bridgeVersion) == 684);

struct ControlLayout {
    uint32_t magic;           // kControlMagic once the app has written a request
    uint32_t version;         // kControlVersion
    uint32_t seq;             // seqlock: odd while the app writes
    uint32_t requestCounter;  // +1 per user action, 1..0x7FFFFFFF (never 0 once written)
    uint32_t fgEnabled;       // the desired state of every switch, not only the one clicked
    uint32_t cameraFlipHandedness;
    uint32_t cameraNegateSide;
    uint32_t saveAsDefault;   // also write start_with_fg and the camera switches into ac-dlssg.ini
};

static_assert(sizeof(ControlLayout) == 32);
static_assert(alignof(ControlLayout) == 4);
static_assert(offsetof(ControlLayout, magic) == 0);
static_assert(offsetof(ControlLayout, version) == 4);
static_assert(offsetof(ControlLayout, seq) == 8);
static_assert(offsetof(ControlLayout, requestCounter) == 12);
static_assert(offsetof(ControlLayout, fgEnabled) == 16);
static_assert(offsetof(ControlLayout, cameraFlipHandedness) == 20);
static_assert(offsetof(ControlLayout, cameraNegateSide) == 24);
static_assert(offsetof(ControlLayout, saveAsDefault) == 28);

constexpr wchar_t kStatusSectionName[] = L"Local\\AcDlssg.Status.v1";
constexpr wchar_t kControlSectionName[] = L"Local\\AcDlssg.Control.v1";
// Larger than both layouts, as the camera section (an existing section keeps its size).
constexpr DWORD kPanelSectionSize = 4096;

// text into a fixed char field: NUL-terminated, cut at a UTF-8 character
// boundary when it does not fit, the rest of the field zeroed.
void CopyText(char* dst, size_t size, const std::string& text);
template <size_t N>
void CopyText(char (&dst)[N], const std::string& text) {
    CopyText(dst, N, text);
}
// A fixed char field up to its NUL (or its end).
std::string TextOf(const char* src, size_t size);
template <size_t N>
std::string TextOf(const char (&src)[N]) {
    return TextOf(src, N);
}

class PanelStatusChannel {
public:
    // The process-wide channel on kStatusSectionName.
    static PanelStatusChannel& Get();

    // Tests use their own section names, so they never touch the section of
    // a game that runs at the same time.
    explicit PanelStatusChannel(const wchar_t* sectionName = kStatusSectionName);
    ~PanelStatusChannel();
    PanelStatusChannel(const PanelStatusChannel&) = delete;
    PanelStatusChannel& operator=(const PanelStatusChannel&) = delete;

    // CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
    // 4096, name) and a FILE_MAP_WRITE view of sizeof(StatusLayout) bytes.
    // When the section already holds a stable record published by another
    // process's bridge (a test app while the game runs), this process never
    // writes it: Owned() stays false, ForeignOwner() names that process and
    // Create still returns true. Otherwise the channel owns the section and
    // publishes its first record: magic, version, ownerPid, bridgeVersion,
    // bridgeState kPanelNotLoaded. Idempotent. False with *error when the
    // section cannot be created or mapped.
    bool Create(std::string* error);
    bool Owned() const;
    uint32_t ForeignOwner() const;  // 0 unless another process owns the section

    // Changes this process's record under the channel's lock, then publishes
    // all of it: seq odd, every other field, seq even, heartbeat + 1 (seq and
    // heartbeat stay below 2^31, like the Lua app's counters). Nothing
    // happens (change is not called) unless the channel owns the section.
    template <class F>
    void Update(F&& change) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!owned_) return;
        change(record_);
        PublishLocked();
    }

    enum class ReadResult { Ok, NoSection, NotWritten, Torn };
    // Seqlock reader of the section as the Lua app reads it, whoever owns it
    // (tests, diagnostics): 2 attempts; a stable record without our magic
    // and version is NotWritten. Never writes.
    ReadResult Read(StatusLayout* out) const;

    const std::wstring& SectionName() const { return name_; }

private:
    void PublishLocked();

    const std::wstring name_;
    std::mutex mu_;
    HANDLE section_ = nullptr;
    std::atomic<StatusLayout*> view_{nullptr};
    std::atomic<bool> owned_{false};
    std::atomic<uint32_t> foreign_{0};
    StatusLayout record_{};
};

class PanelControlChannel {
public:
    // The process-wide channel on kControlSectionName.
    static PanelControlChannel& Get();

    explicit PanelControlChannel(const wchar_t* sectionName = kControlSectionName);
    ~PanelControlChannel();
    PanelControlChannel(const PanelControlChannel&) = delete;
    PanelControlChannel& operator=(const PanelControlChannel&) = delete;

    // CreateFileMappingW(PAGE_READWRITE, 4096) and a FILE_MAP_READ view of
    // sizeof(ControlLayout) bytes; the Lua app opens the same section with
    // ac.writeMemoryMappedFile. Idempotent.
    bool Create(std::string* error);
    bool Ready() const;

    // The record's seq, one atomic load: the presenter compares it with the
    // last one it saw at the start of every frame. 0 without a section.
    uint32_t Seq() const;

    enum class ReadResult { Ok, NoSection, NotWritten, Torn };
    // Seqlock reader, at most 2 attempts (odd seq, or a seq that changed
    // during the copy, is Torn); a stable record without our magic and
    // version is NotWritten. *out is written only on Ok. Never blocks.
    ReadResult Read(ControlLayout* out) const;

    const std::wstring& SectionName() const { return name_; }

private:
    const std::wstring name_;
    std::mutex create_mu_;
    HANDLE section_ = nullptr;
    std::atomic<const ControlLayout*> view_{nullptr};
};

}  // namespace acdb
