#include "fg_policy.h"

#include <cstdio>

#include "camera_layout.h"
#include "compat.h"

namespace acdb {

namespace {

constexpr uint64_t kMiB = 1024ull * 1024ull;
constexpr size_t kMaxReasons = 256;

FgGateResult Off(std::string reason, bool perFrame) {
    FgGateResult r;
    r.on = false;
    r.reason = std::move(reason);
    r.perFrame = perFrame;
    return r;
}

}  // namespace

FgGateResult DecideFg(const FgGateInputs& in) {
    if (!in.supported) return Off("not supported on this adapter", false);
    if (!in.userOn) return Off(UserOffReason(in.userSource), false);
    if (!in.stateFailure.empty()) return Off("DLSS-G status " + in.stateFailure, false);
    if (in.stalled) return Off("D3D12 stall", true);
    if (in.windowMinimized) return Off("game window minimized", true);
    if (!in.aspectRefusal.empty()) return Off(in.aspectRefusal, false);
    if (!in.captured)
        return Off(in.captureReason.empty() ? std::string("no DLSS evaluate this frame") : in.captureReason, true);
    if (in.forcedOff) return Off(in.captureReason.empty() ? std::string("capture forced off") : in.captureReason, true);
    if (in.mvScaleMissing) return Off("MV scale missing", true);
    if (!in.cameraOk) return Off("camera: " + (in.cameraReason.empty() ? std::string("missing") : in.cameraReason), true);
    if (!in.cameraFresh) return Off("camera not fresh", true);
    if (in.cameraFlags & kCamPaused) return Off("game paused", true);
    if (in.cameraFlags & kCamMainMenu) return Off("in-game menu open", true);
    if (in.cameraFlags & (kCamVR | kCamTriple)) return Off("VR or triple-screen mode", true);
    if (!in.constantsOk) return Off("frame constants refused: " + in.constantsWhy, true);
    if (!in.vramRefusal.empty()) return Off(in.vramRefusal, false);
    return FgGateResult{true, std::string(), false};
}

std::string UserOffReason(const std::string& source) {
    return "off by the user (" + (source.empty() ? std::string("start_with_fg, the hotkey or the panel") : source) + ")";
}

bool ShouldTag(const FgGateInputs& in, const FgGateResult& gate, bool tagWithoutFg) {
    if (gate.on) return true;
    return tagWithoutFg && in.captured && in.cameraOk && in.cameraFresh && in.constantsOk;
}

std::string TagPathReason(const FgGateInputs& in, const FgGateResult& gate, bool tagWithoutFg) {
    if (!tagWithoutFg || gate.on || gate.perFrame) return {};
    // The gate with every lasting condition met leaves only the per-frame ones.
    FgGateInputs perFrame = in;
    perFrame.supported = true;
    perFrame.userOn = true;
    perFrame.stateFailure.clear();
    perFrame.aspectRefusal.clear();
    perFrame.vramRefusal.clear();
    const FgGateResult r = DecideFg(perFrame);
    return r.on ? std::string() : r.reason;
}

SlotAction DecideSlot(bool matches, bool forceRecreate, bool hasTextures, uint64_t progress, uint64_t lastUse) {
    if (matches && !forceRecreate) return SlotAction::Copy;
    if (!hasTextures || progress >= lastUse) return SlotAction::Recreate;
    return SlotAction::Skip;
}

VramCheck CheckVideoMemory(uint64_t budgetBytes, uint64_t usageBytes, uint64_t estimateBytes, unsigned headroomMib) {
    const uint64_t free = budgetBytes > usageBytes ? budgetBytes - usageBytes : 0;
    const uint64_t need = estimateBytes + static_cast<uint64_t>(headroomMib) * kMiB;
    VramCheck r;
    if (free >= need) return r;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "video memory: need %llu MiB, free %llu MiB",
                  static_cast<unsigned long long>((need + kMiB - 1) / kMiB),
                  static_cast<unsigned long long>(free / kMiB));
    r.ok = false;
    r.reason = buf;
    return r;
}

bool VramGuard::CheckDue(uint64_t frame) const {
    if (passed_) return false;
    return !checked_ || frame - last_ >= kRecheckFrames;
}

VramCheck DecideVram(const VramInputs& in) {
    if (!in.budgetKnown) return VramCheck{};
    return CheckVideoMemory(in.budgetBytes, in.usageBytes, in.estimateOk ? in.estimateBytes : 0, in.headroomMib);
}

bool VramGuard::Record(uint64_t frame, const VramCheck& result) {
    const bool changed = !checked_ || passed_ != result.ok;
    checked_ = true;
    last_ = frame;
    passed_ = result.ok;
    refusal_ = result.ok ? std::string() : result.reason;
    return changed;
}

bool HotkeyChordDown(const Hotkey& hotkey, bool keyDown, bool ctrlDown, bool shiftDown, bool altDown) {
    return keyDown && ctrlDown == hotkey.ctrl && shiftDown == hotkey.shift && altDown == hotkey.alt;
}

bool ReasonThrottle::ShouldLog(const std::string& reason, uint64_t nowMs) {
    const auto it = last_.find(reason);
    if (it != last_.end()) {
        if (nowMs - it->second < interval_) return false;
        it->second = nowMs;
        return true;
    }
    if (last_.size() >= kMaxReasons) {
        // Forget the reasons whose window is over; if none is, forget all.
        for (auto i = last_.begin(); i != last_.end();) {
            if (nowMs - i->second >= interval_)
                i = last_.erase(i);
            else
                ++i;
        }
        if (last_.size() >= kMaxReasons) last_.clear();
    }
    last_.emplace(reason, nowMs);
    return true;
}

std::string DlssgUnsupportedMessage(const std::string& why, bool sm86, bool spoofLoaded) {
    std::string text = "DLSS-G is not supported on this adapter (" + why + ")";
    if (sm86 && !spoofLoaded) text += "; RTX 30 needs dlssg_for_sm86 (version.dll) in the game folder";
    return text;
}

std::string DlssgStatusText(uint32_t status) {
    if (status == 0) return "eOk";
    static const char* const kNames[] = {
        "eFailResolutionTooLow",          "eFailReflexNotDetectedAtRuntime",
        "eFailHDRFormatNotSupported",     "eFailCommonConstantsInvalid",
        "eFailGetCurrentBackBufferIndexNotCalled", "eReserved5",
    };
    std::string text;
    uint32_t unknown = 0;
    for (uint32_t bit = 0; bit < 32; ++bit) {
        const uint32_t mask = 1u << bit;
        if (!(status & mask)) continue;
        if (bit < sizeof(kNames) / sizeof(kNames[0])) {
            if (!text.empty()) text += '|';
            text += kNames[bit];
        } else {
            unknown |= mask;
        }
    }
    if (unknown) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%X", unknown);
        if (!text.empty()) text += '|';
        text += buf;
    }
    return text;
}

std::string RuntimeAspectRefusal(bool allowStretching, uint32_t outW, uint32_t outH, uint32_t chainW,
                                 uint32_t chainH) {
    if (allowStretching || !AspectMismatch(outW, outH, chainW, chainH)) return {};
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "aspect: the DLSS output %ux%u differs from the window %ux%u (letterboxed output is not supported)",
                  outW, outH, chainW, chainH);
    return buf;
}

uint32_t GeneratedFramesAtPresent(uint32_t numFramesActuallyPresented) {
    return numFramesActuallyPresented > 1 ? numFramesActuallyPresented - 1 : 0;
}

void StatusPollClock::Frame() {
    if (since_ < kFrames) ++since_;
}

bool StatusPollClock::Due(bool dlssgOn) const { return dlssgOn && since_ >= kFrames; }

void StatusPollClock::Polled() { since_ = 0; }

}  // namespace acdb
