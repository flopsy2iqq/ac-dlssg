#include "triple_diag.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <vector>

namespace acdb {
namespace {

bool EqualsNoCase(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 'a' + 'A');
        if (x != y) return false;
    }
    return i == a.size() && b[i] == '\0';
}

std::optional<uint32_t> U32(const NgxParameter* p, const char* key) {
    unsigned int v = 0;
    if (p->Get(key, &v) == kNgxSuccess) return v;
    return std::nullopt;
}

// An int key that a block may also hold as an unsigned one.
std::optional<int> I32(const NgxParameter* p, const char* key) {
    int v = 0;
    if (p->Get(key, &v) == kNgxSuccess) return v;
    unsigned int u = 0;
    if (p->Get(key, &u) == kNgxSuccess) return static_cast<int>(u);
    return std::nullopt;
}

TripleDiagTexture TextureOf(ID3D11Resource* r) {
    TripleDiagTexture t;
    if (!r) return t;
    t.present = true;
    t.resource = r;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return t;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (FAILED(r->QueryInterface(IID_PPV_ARGS(&tex)))) return t;
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    t.texture2d = true;
    t.width = d.Width;
    t.height = d.Height;
    t.format = static_cast<uint32_t>(d.Format);
    t.samples = d.SampleDesc.Count;
    t.arraySize = d.ArraySize;
    t.mipLevels = d.MipLevels;
    return t;
}

TripleDiagTexture TextureKey(const NgxParameter* p, const char* key) {
    ID3D11Resource* r = nullptr;
    if (p->Get(key, &r) != kNgxSuccess) return TripleDiagTexture();
    return TextureOf(r);
}

std::string Opt(const std::optional<uint32_t>& v) { return v ? std::to_string(*v) : std::string("unset"); }

std::string Base(const std::optional<uint32_t>& x, const std::optional<uint32_t>& y) {
    if (!x && !y) return "unset";
    return Opt(x) + "," + Opt(y);
}

// "<name> <pointer> 1920x360 format 10 ...": the pointer tells the textures
// of the evaluates apart (review finding B-2).
std::string TextureText(const char* name, const TripleDiagTexture& t) {
    if (!t.present) return std::string(name) + " none";
    char buf[160];
    if (!t.texture2d) {
        std::snprintf(buf, sizeof(buf), "%s %p not a 2D texture", name, t.resource);
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "%s %p %ux%u format %u samples %u array %u mips %u", name, t.resource, t.width,
                  t.height, t.format, t.samples, t.arraySize, t.mipLevels);
    return buf;
}

// "context <pointer> immediate, on the game's device".
std::string ContextText(const TripleDiagEvaluate& e) {
    if (!e.context) return "context none";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "context %p %s, ", e.context, e.immediate ? "immediate" : "deferred");
    return buf + std::string(!e.gameDevice ? "device unknown" : *e.gameDevice ? "on the game's device"
                                                                               : "not on the game's device");
}

}  // namespace

bool TripleDiagActive(bool configured, const std::optional<std::string>& cameraMode) {
    return configured && cameraMode && EqualsNoCase(*cameraMode, "TRIPLE");
}

TripleDiagEvaluate ReadTripleDiagEvaluate(const NgxEvaluateInputs& in, const NgxCreateInfo* create) {
    TripleDiagEvaluate e;
    e.featureKey = in.featureKey;
    e.context = in.ctx;
    if (in.ctx) e.immediate = in.ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
    if (create) {
        e.createObserved = true;
        e.createWidth = create->width;
        e.createHeight = create->height;
        e.createOutWidth = create->outWidth;
        e.createOutHeight = create->outHeight;
        e.createFlags = create->createFlags;
    }
    e.jitterX = in.jitterX;
    e.jitterY = in.jitterY;
    e.mvScaleX = in.mvScaleX;
    e.mvScaleY = in.mvScaleY;
    e.reset = in.reset;
    // The hook already read Depth and MotionVectors.
    e.depth = TextureOf(in.depth);
    e.mvec = TextureOf(in.mvec);
    const NgxParameter* p = in.params;
    if (!p) return e;
    e.width = U32(p, ngxkey::kWidth);
    e.height = U32(p, ngxkey::kHeight);
    e.outWidth = U32(p, ngxkey::kOutWidth);
    e.outHeight = U32(p, ngxkey::kOutHeight);
    e.subrectWidth = U32(p, ngxkey::kSubrectWidth);
    e.subrectHeight = U32(p, ngxkey::kSubrectHeight);
    // nvsdk_ngx_defs.h (DLSS SDK 310.x).
    e.colorBaseX = U32(p, "DLSS.Input.Color.Subrect.Base.X");
    e.colorBaseY = U32(p, "DLSS.Input.Color.Subrect.Base.Y");
    e.depthBaseX = U32(p, "DLSS.Input.Depth.Subrect.Base.X");
    e.depthBaseY = U32(p, "DLSS.Input.Depth.Subrect.Base.Y");
    e.mvBaseX = U32(p, "DLSS.Input.MV.Subrect.Base.X");
    e.mvBaseY = U32(p, "DLSS.Input.MV.Subrect.Base.Y");
    e.outputBaseX = U32(p, "DLSS.Output.Subrect.Base.X");
    e.outputBaseY = U32(p, "DLSS.Output.Subrect.Base.Y");
    e.outputSubrects = I32(p, "DLSS.Enable.Output.Subrects");
    e.color = TextureKey(p, "Color");
    e.output = TextureKey(p, "Output");
    return e;
}

std::string TripleDiagEvaluateLine(uint32_t frame, uint32_t index, const TripleDiagEvaluate& e) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "triple-diag: frame %u evaluate %u: feature 0x%llX; ", frame, index,
                  static_cast<unsigned long long>(e.featureKey));
    std::string line = buf + ContextText(e) + "; ";
    if (e.createObserved) {
        std::snprintf(buf, sizeof(buf), "create render %ux%u output %ux%u flags 0x%X; ", e.createWidth,
                      e.createHeight, e.createOutWidth, e.createOutHeight, e.createFlags);
        line += buf;
    } else {
        line += "create not seen; ";
    }
    line += "block Width=" + Opt(e.width) + " Height=" + Opt(e.height) + " OutWidth=" + Opt(e.outWidth) +
            " OutHeight=" + Opt(e.outHeight) + "; ";
    line += (e.subrectWidth || e.subrectHeight) ? "subrect " + Opt(e.subrectWidth) + "x" + Opt(e.subrectHeight)
                                                : std::string("subrect unset");
    line += "; bases color " + Base(e.colorBaseX, e.colorBaseY) + " depth " + Base(e.depthBaseX, e.depthBaseY) +
            " mv " + Base(e.mvBaseX, e.mvBaseY) + " output " + Base(e.outputBaseX, e.outputBaseY) +
            "; output subrects " + (e.outputSubrects ? std::to_string(*e.outputSubrects) : std::string("unset"));
    std::snprintf(buf, sizeof(buf), "; jitter %.4f,%.4f mv scale %.1f,%.1f reset %d; ", e.jitterX, e.jitterY,
                  e.mvScaleX, e.mvScaleY, e.reset ? 1 : 0);
    line += buf;
    line += TextureText("Color", e.color) + "; " + TextureText("Output", e.output) + "; " +
            TextureText("Depth", e.depth) + "; " + TextureText("MotionVectors", e.mvec);
    return line;
}

namespace {

// "not counted: 2 on a deferred context, 1 failed (last result 0x...)", or
// empty when the hook counted every evaluate.
std::string UncountedText(const TripleDiagFrameCounts& c) {
    if (!c.deferred && !c.failed) return {};
    std::string text = "not counted: ";
    if (c.deferred) text += std::to_string(c.deferred) + " on a deferred context";
    if (c.failed) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s%u failed (last result 0x%08X)", c.deferred ? ", " : "", c.failed,
                      c.lastFailResult);
        text += buf;
    }
    return text;
}

}  // namespace

std::string TripleDiagFrameLine(const TripleDiagFrameCounts& c, uint32_t swapWidth, uint32_t swapHeight,
                                uint32_t swapFormat) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "triple-diag: frame %u: %u DLSS evaluates; ", c.frame, c.evaluates);
    std::string line = buf;
    const std::string uncounted = UncountedText(c);
    if (!uncounted.empty()) line += uncounted + "; ";
    std::snprintf(buf, sizeof(buf), "swap chain %ux%u format %u", swapWidth, swapHeight, swapFormat);
    return line + buf;
}

TripleDiagLog::TripleDiagLog(std::function<void(const std::string&)> log, uint32_t frames, uint32_t windows,
                             uint32_t maxLinesPerWindow, uint32_t maxLines, uint32_t noEvaluateFrames)
    : log_(std::move(log)),
      frames_(frames),
      windows_(windows),
      maxLinesPerWindow_(maxLinesPerWindow),
      maxLines_(maxLines),
      noEvaluateFrames_(noEvaluateFrames) {}

void TripleDiagLog::Log(const std::string& line) {
    if (log_) log_(line);
}

void TripleDiagLog::OnCreate(uint64_t featureKey, uint32_t width, uint32_t height, uint32_t outWidth,
                             uint32_t outHeight) {
    std::string line;
    {
        std::lock_guard<std::mutex> lock(mu_);
        constexpr uint32_t kMaxCreates = 32;  // a bound for a game that re-creates its feature often
        if (creates_ >= kMaxCreates) return;
        ++creates_;
        char buf[192];
        std::snprintf(buf, sizeof(buf), "triple-diag: DLSS feature 0x%llX created: render %ux%u, output %ux%u",
                      static_cast<unsigned long long>(featureKey), width, height, outWidth, outHeight);
        line = buf;
        if (framesLeft_ == 0 && windowsUsed_ > 0) {
            if (windowsUsed_ < windows_) {
                armed_ = true;
                line += "; its first evaluate starts window " + std::to_string(windowsUsed_ + 1) + " of " +
                        std::to_string(windows_);
            } else {
                line += "; no window left";
            }
        }
    }
    Log(line);
}

void TripleDiagLog::OnEvaluate(const TripleDiagEvaluate& e) {
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (framesLeft_ == 0) {
            if (!armed_ || windowsUsed_ >= windows_) return;
            armed_ = false;
            ++windowsUsed_;
            framesLeft_ = frames_;
            frame_ = 1;
            counts_ = TripleDiagFrameCounts();
            windowLines_ = 0;
            lines.push_back("triple-diag: window " + std::to_string(windowsUsed_) + " of " +
                            std::to_string(windows_) + ": logging every DLSS evaluate of the next " +
                            std::to_string(frames_) + " frames (frame generation stays off)");
        }
        ++counts_.evaluates;
        ++windowLines_;
        ++lines_;
        lines.push_back(TripleDiagEvaluateLine(frame_, counts_.evaluates, e));
        // The caps (review finding B-4): the window, or every window, ends.
        if (lines_ >= maxLines_) {
            lines.push_back("triple-diag: " + std::to_string(lines_) +
                            " evaluate lines in this session, the most; nothing more is logged");
            framesLeft_ = 0;
            armed_ = false;
            windowsUsed_ = windows_;
        } else if (windowLines_ >= maxLinesPerWindow_) {
            lines.push_back("triple-diag: window " + std::to_string(windowsUsed_) + " stopped at frame " +
                            std::to_string(frame_) + " after " + std::to_string(windowLines_) +
                            " evaluate lines, the most for one window; " +
                            (windowsUsed_ < windows_ ? "a DLSS feature created later starts window " +
                                                           std::to_string(windowsUsed_ + 1)
                                                     : std::string("that was the last window")));
            framesLeft_ = 0;
        }
    }
    for (const std::string& l : lines) Log(l);
}

void TripleDiagLog::EndFrame(uint32_t swapWidth, uint32_t swapHeight, uint32_t swapFormat) {
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (framesLeft_ == 0) {
            // Review finding B-6: before the first window, once, that no
            // counted evaluate came, and what CSP needs for one.
            if (windowsUsed_ == 0 && idleFrames_ < noEvaluateFrames_ && ++idleFrames_ == noEvaluateFrames_) {
                const std::string uncounted = UncountedText(before_);
                lines.push_back("triple-diag: no counted DLSS evaluate in " + std::to_string(noEvaluateFrames_) +
                                " frames" + (uncounted.empty() ? std::string() : " (" + uncounted + ")") +
                                ": CSP runs DLSS only with Extra FX on, and in triple-screen mode only with its "
                                "\"Allow in triple\" option (extra_fx.ini [BASIC] ALLOW_IN_TRIPLE=1), and with DLSS as "
                                "its upscaler; the compat lines at the start show both. The diagnostics start with "
                                "the first evaluate");
            }
        } else {
            counts_.frame = frame_;
            lines.push_back(TripleDiagFrameLine(counts_, swapWidth, swapHeight, swapFormat));
            counts_ = TripleDiagFrameCounts();
            ++frame_;
            if (--framesLeft_ == 0) {
                lines.push_back("triple-diag: window " + std::to_string(windowsUsed_) + " done after " +
                                std::to_string(frames_) + " frames; " +
                                (windowsUsed_ < windows_ ? "a DLSS feature created later starts window " +
                                                               std::to_string(windowsUsed_ + 1)
                                                         : std::string("that was the last window")));
            }
        }
    }
    for (const std::string& l : lines) Log(l);
}

void TripleDiagLog::OnUncountedEvaluate(bool deferred, NgxResult result) {
    std::lock_guard<std::mutex> lock(mu_);
    // In the open window's frame; before the first window, for the line of
    // noEvaluateFrames_ (review finding B-6).
    TripleDiagFrameCounts* c = framesLeft_ > 0 ? &counts_ : windowsUsed_ == 0 ? &before_ : nullptr;
    if (!c) return;
    if (deferred) {
        ++c->deferred;
    } else {
        ++c->failed;
        c->lastFailResult = static_cast<uint32_t>(result);
    }
}

bool TripleDiagLog::WantsEvaluates() const {
    std::lock_guard<std::mutex> lock(mu_);
    return framesLeft_ > 0 || (armed_ && windowsUsed_ < windows_);
}

}  // namespace acdb
