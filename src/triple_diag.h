#pragma once
// Triple-screen diagnostics (1.3.0). Frame generation for triple screens is
// planned for 1.4 and needs to know how CSP lays out its DLSS for the three
// screens. With [debug] triple_diag=1 in ac-dlssg.ini and AC in triple-screen
// mode (video.ini [CAMERA] MODE=TRIPLE), the bridge proxies the game's swap
// chain as usual and keeps frame generation off (kTripleDiagFgReason in
// fg_policy.h), and bridge.log gets, for kTripleDiagFrames frames from the
// first DLSS evaluate, every counted CSP DLSS evaluate: its feature, the
// create's and the block's sizes, the render subrect and the subrect base
// keys, jitter, MV scale, and the Color, Output, Depth and MotionVectors
// textures; and for each frame the number of evaluates and the swap chain's
// size. A DLSS feature created later (a resolution change) starts another
// such window, at most kTripleDiagWindows in a session; a window stops early
// at kTripleDiagMaxEvaluateLinesPerWindow evaluate lines, and the session
// at kTripleDiagMaxEvaluateLines. Without the switch, triple-screen mode
// stays refused (compat.h).
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include "ngx_hook.h"

namespace acdb {

constexpr uint32_t kTripleDiagFrames = 300;
constexpr uint32_t kTripleDiagWindows = 4;
// Caps on the evaluate lines (review finding B-4), so that a window whose
// frames never end (no Present after the presenter stopped) still stops:
// per window (10 a frame on average), and in the whole session.
constexpr uint32_t kTripleDiagMaxEvaluateLinesPerWindow = 3000;
constexpr uint32_t kTripleDiagMaxEvaluateLines = 6000;

// Pure: the switch is set and the camera mode is TRIPLE (case-insensitive).
bool TripleDiagActive(bool configured, const std::optional<std::string>& cameraMode);

// One texture of an evaluate.
struct TripleDiagTexture {
    bool present = false;    // the key holds a resource
    bool texture2d = false;  // ... and it is a Texture2D, whose description follows
    uint32_t width = 0, height = 0, format = 0, samples = 0, arraySize = 0, mipLevels = 0;
    const void* resource = nullptr;  // the resource, to tell textures apart (only logged)
};

// One counted evaluate; an unset optional is a key the block does not set.
struct TripleDiagEvaluate {
    uint64_t featureKey = 0;
    // The evaluate's D3D11 context (only logged), whether it is an immediate
    // one, and whether it belongs to the game's D3D11 device, the device of
    // the proxied swap chain (unset: not known). The coordinator sets
    // gameDevice, since the diagnostics run before its own device check.
    const void* context = nullptr;
    bool immediate = false;
    std::optional<bool> gameDevice;
    bool createObserved = false;
    uint32_t createWidth = 0, createHeight = 0, createOutWidth = 0, createOutHeight = 0, createFlags = 0;
    std::optional<uint32_t> width, height, outWidth, outHeight;  // Width, Height, OutWidth, OutHeight
    std::optional<uint32_t> subrectWidth, subrectHeight;          // DLSS.Render.Subrect.Dimensions.*
    // DLSS.Input.Color.Subrect.Base.X/Y and the same for Depth, MV and
    // DLSS.Output (nvsdk_ngx_defs.h). CSP 0.3.x sets none of them as far as
    // its dwrite.dll's strings tell; the diagnostics show whether it does.
    std::optional<uint32_t> colorBaseX, colorBaseY, depthBaseX, depthBaseY, mvBaseX, mvBaseY, outputBaseX,
        outputBaseY;
    std::optional<int> outputSubrects;  // DLSS.Enable.Output.Subrects
    float jitterX = 0, jitterY = 0, mvScaleX = 0, mvScaleY = 0;
    bool reset = false;
    TripleDiagTexture color, output, depth, mvec;
};

// Reads in.params (CSP's block, valid only during the evaluate) and the
// four textures' descriptions; create is the feature's create when it was
// observed. Only reads, and never keeps a pointer.
TripleDiagEvaluate ReadTripleDiagEvaluate(const NgxEvaluateInputs& in, const NgxCreateInfo* create);

// Pure: "triple-diag: frame <f> evaluate <i>: feature 0x<key>; context <ptr>
// immediate, on the game's device; ...", each texture with its pointer.
std::string TripleDiagEvaluateLine(uint32_t frame, uint32_t index, const TripleDiagEvaluate& e);
// Pure: "triple-diag: frame <f>: <n> DLSS evaluates; swap chain <w>x<h> format <fmt>".
std::string TripleDiagFrameLine(uint32_t frame, uint32_t evaluates, uint32_t swapWidth, uint32_t swapHeight,
                                uint32_t swapFormat);

// The windows of logged frames. The coordinator reports the counted creates
// and evaluates (render thread), the presenter the Present that ends each
// frame; the lines go to log (bridge.log). Thread-safe.
class TripleDiagLog {
public:
    explicit TripleDiagLog(std::function<void(const std::string&)> log, uint32_t frames = kTripleDiagFrames,
                           uint32_t windows = kTripleDiagWindows,
                           uint32_t maxLinesPerWindow = kTripleDiagMaxEvaluateLinesPerWindow,
                           uint32_t maxLines = kTripleDiagMaxEvaluateLines);
    // A counted CreateFeature: logged, and the next evaluate starts a new
    // window while windows remain.
    void OnCreate(uint64_t featureKey, uint32_t width, uint32_t height, uint32_t outWidth, uint32_t outHeight);
    // A counted evaluate: starts a window when one is due; logged in a window.
    void OnEvaluate(const TripleDiagEvaluate& e);
    // The Present that ends the frame: its line while a window is open.
    void EndFrame(uint32_t swapWidth, uint32_t swapHeight, uint32_t swapFormat);
    // True while an evaluate would be logged or would start a window, so
    // that the caller reads the block only then.
    bool WantsEvaluates() const;

private:
    void Log(const std::string& line);

    std::function<void(const std::string&)> log_;
    const uint32_t frames_, windows_, maxLinesPerWindow_, maxLines_;
    mutable std::mutex mu_;
    uint32_t windowsUsed_ = 0;
    uint32_t framesLeft_ = 0;  // > 0 while a window is open
    bool armed_ = true;        // the next evaluate opens a window
    uint32_t frame_ = 0;       // the frame in the open window, from 1
    uint32_t evaluates_ = 0;   // counted evaluates of that frame
    uint32_t creates_ = 0;     // creates logged in this session
    uint32_t windowLines_ = 0;  // evaluate lines of the open window
    uint32_t lines_ = 0;        // evaluate lines of the session
};

}  // namespace acdb
