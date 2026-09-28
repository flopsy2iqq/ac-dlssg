#pragma once
// Parsers for the bridge log lines the test app checks: the statistics line
// of d3d12_presenter.cpp (the M2 fields, then the M3 fields appended before
// vram_mib), the line timestamps of log.cpp ("HH:MM:SS.mmm [tid] LEVEL ..."),
// and the once-only and throttled lines of the M3 log line contract.
// Header-only, so that tests/test_testapp_logs.cpp pins them on lines in
// those formats.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace testapp {

// ------------------------------------------------------------------ statistics

// A statistics line: its key=value fields in order.
struct Stats {
    std::string line;
    std::vector<std::pair<std::string, std::string>> fields;

    const std::string* Get(const char* key) const {
        for (const auto& f : fields)
            if (f.first == key) return &f.second;
        return nullptr;
    }
    // The field as a number; -1 when it is absent or not a number.
    double Num(const char* key) const {
        const std::string* v = Get(key);
        if (!v || v->empty()) return -1;
        char* end = nullptr;
        const double d = std::strtod(v->c_str(), &end);
        return end && *end == '\0' ? d : -1;
    }
};

inline Stats ParseStats(const std::string& line) {
    Stats s;
    s.line = line;
    const size_t at = line.find(" stats: ");
    if (at == std::string::npos) return s;
    size_t pos = at + 8;
    while (pos < line.size()) {
        size_t end = line.find(' ', pos);
        if (end == std::string::npos) end = line.size();
        const std::string token = line.substr(pos, end - pos);
        const size_t eq = token.find('=');
        // "bridge_gpu_ms" only labels the d3d11= and d3d12= fields after it.
        if (eq != std::string::npos) s.fields.emplace_back(token.substr(0, eq), token.substr(eq + 1));
        pos = end + 1;
    }
    return s;
}

// The M2 fields in order; the M3 contract keeps them and appends its six
// fields, all or none, before vram_mib, which stays last.
inline constexpr const char* kStatsM2Keys[] = {
    "base_fps", "presented_fps", "skipped", "failed", "occluded",   "uncopied", "max_frame_ms", "max_present_ms",
    "d3d11",    "d3d12",         "fg",      "stalls", "streamline", "reflex",   "pcl_problems"};
inline constexpr const char* kStatsM3Keys[] = {"captures",  "camera_fresh", "tagged",
                                               "fg_frames", "generated",    "double_evaluates"};

inline bool IsCount(const std::string& v) {
    if (v.empty()) return false;
    for (char c : v)
        if (c < '0' || c > '9') return false;
    return true;
}

inline bool HasM3Stats(const Stats& s) { return s.Get(kStatsM3Keys[0]) != nullptr; }

// Empty when the line has that layout, fg on or off, Streamline and Reflex
// on, no PCL problem, counts in the M3 fields (generated may be n/a) and a
// non-zero video memory budget; otherwise what is wrong.
inline std::string StatsProblem(const Stats& s) {
    std::vector<std::string> expected(std::begin(kStatsM2Keys), std::end(kStatsM2Keys));
    if (HasM3Stats(s)) expected.insert(expected.end(), std::begin(kStatsM3Keys), std::end(kStatsM3Keys));
    expected.push_back("vram_mib");
    std::string keys, want;
    for (const auto& f : s.fields) keys += (keys.empty() ? "" : " ") + f.first;
    for (const auto& k : expected) want += (want.empty() ? "" : " ") + k;
    if (keys != want) return "fields \"" + keys + "\", expected \"" + want + "\"";
    if (*s.Get("fg") != "on" && *s.Get("fg") != "off") return "fg is neither on nor off";
    if (*s.Get("streamline") != "on" || *s.Get("reflex") != "on" || *s.Get("pcl_problems") != "0")
        return "not streamline=on reflex=on pcl_problems=0";
    if (HasM3Stats(s)) {
        for (const char* k : kStatsM3Keys)
            if (!IsCount(*s.Get(k)) && !(std::strcmp(k, "generated") == 0 && *s.Get(k) == "n/a"))
                return std::string(k) + " is not a count";
    }
    const std::string& vram = *s.Get("vram_mib");
    const size_t slash = vram.find('/');
    if (slash == std::string::npos || !IsCount(vram.substr(0, slash)) || !IsCount(vram.substr(slash + 1)) ||
        std::strtoull(vram.c_str() + slash + 1, nullptr, 10) == 0)
        return "vram_mib is not <usage>/<budget> with a budget";
    return {};
}

// ------------------------------------------------------------------ releases

// The release lines of proxy_swapchain.cpp and d3d12_presenter.cpp, and the
// failed creation of factory_hook.cpp.
inline constexpr char kProxyReleased[] = "ProxySwapChain released";
inline constexpr char kPresenterReleased[] = "presenter released";
inline constexpr char kProxyCreationFailed[] = "proxy swap chain creation failed";

inline int CountContaining(const std::vector<std::string>& lines, const char* needle) {
    int n = 0;
    for (const auto& l : lines) n += l.find(needle) != std::string::npos ? 1 : 0;
    return n;
}

// Problems with the release lines, given the proxies the test app released.
// A proxy whose creation failed (from M3: a presenter refused because
// Streamline does not support DLSS-G) is released inside that creation,
// together with its presenter when one was built: each failed creation adds
// exactly one proxy release and at most one presenter release.
inline std::vector<std::string> ReleaseProblems(const std::vector<std::string>& lines, int proxiesReleased) {
    std::vector<std::string> problems;
    const int failed = CountContaining(lines, kProxyCreationFailed);
    const int proxies = CountContaining(lines, kProxyReleased);
    const int presenters = CountContaining(lines, kPresenterReleased);
    char buf[160];
    if (proxies != proxiesReleased + failed) {
        std::snprintf(buf, sizeof(buf), "the bridge log shows %d proxy releases, expected %d (%d failed creation(s))",
                      proxies, proxiesReleased + failed, failed);
        problems.push_back(buf);
    }
    if (presenters < proxiesReleased || presenters > proxiesReleased + failed) {
        if (failed == 0) {
            std::snprintf(buf, sizeof(buf), "the bridge log shows %d presenter releases, expected %d", presenters,
                          proxiesReleased);
        } else {
            std::snprintf(buf, sizeof(buf),
                          "the bridge log shows %d presenter releases, expected %d to %d (%d failed creation(s))",
                          presenters, proxiesReleased, proxiesReleased + failed, failed);
        }
        problems.push_back(buf);
    }
    return problems;
}

// ------------------------------------------------------------------ timestamps

// Milliseconds since midnight of a log line; -1 without a timestamp.
inline double LineTimeMs(const std::string& line) {
    unsigned h = 0, m = 0, s = 0, ms = 0;
    if (sscanf_s(line.c_str(), "%2u:%2u:%2u.%3u", &h, &m, &s, &ms) != 4) return -1;
    return ((h * 60.0 + m) * 60.0 + s) * 1000.0 + ms;
}

// ------------------------------------------------------------------ M3 lines

// "capture: first counted evaluate: depth <fmt> <w>x<h>, mvec <fmt> <w>x<h>,
// subrect <w>x<h>, create flags 0x<hex>, mv scale <x>,<y>, jitter <x>,<y>"
struct FirstCapture {
    int depthFormat = 0;
    unsigned depthW = 0, depthH = 0;
    int mvecFormat = 0;
    unsigned mvecW = 0, mvecH = 0, subrectW = 0, subrectH = 0, createFlags = 0;
    float mvScaleX = 0, mvScaleY = 0, jitterX = 0, jitterY = 0;
};

inline constexpr char kFirstCaptureTag[] = "capture: first counted evaluate: ";

inline bool ParseFirstCapture(const std::string& line, FirstCapture* out) {
    const size_t at = line.find(kFirstCaptureTag);
    if (at == std::string::npos) return false;
    FirstCapture c;
    const int n = sscanf_s(line.c_str() + at + sizeof(kFirstCaptureTag) - 1,
                           "depth %d %ux%u, mvec %d %ux%u, subrect %ux%u, create flags 0x%x, mv scale %f,%f, jitter "
                           "%f,%f",
                           &c.depthFormat, &c.depthW, &c.depthH, &c.mvecFormat, &c.mvecW, &c.mvecH, &c.subrectW,
                           &c.subrectH, &c.createFlags, &c.mvScaleX, &c.mvScaleY, &c.jitterX, &c.jitterY);
    if (n != 13) return false;
    *out = c;
    return true;
}

// "camera: first fresh snapshot: pos (...) fwd (...) up (...) fov <v> near <n>
// far <f> render <w>x<h> origin shift (...)"
struct FirstCamera {
    float fov = 0, zNear = 0, zFar = 0, renderW = 0, renderH = 0;
};

inline constexpr char kFirstCameraTag[] = "camera: first fresh snapshot: ";

inline bool ParseFirstCamera(const std::string& line, FirstCamera* out) {
    const size_t at = line.find(kFirstCameraTag);
    if (at == std::string::npos) return false;
    const size_t fov = line.find(" fov ", at);
    if (fov == std::string::npos) return false;
    FirstCamera c;
    if (sscanf_s(line.c_str() + fov, " fov %f near %f far %f render %fx%f", &c.fov, &c.zNear, &c.zFar, &c.renderW,
                 &c.renderH) != 5)
        return false;
    *out = c;
    return true;
}

// "fg: frame without DLSS-G: <reason>", a warning the bridge writes at most
// once per period for each distinct reason.
inline constexpr char kFgFrameWithoutTag[] = "] WARN fg: frame without DLSS-G: ";

// One entry per warning that followed the previous one of the same reason
// within the period ("<reason>: <gap> ms apart"); lines past midnight count
// from the day before.
inline std::vector<std::string> FgReasonThrottleViolations(const std::vector<std::string>& lines, double periodMs) {
    std::vector<std::string> violations;
    std::map<std::string, double> last;
    for (const auto& l : lines) {
        const size_t at = l.find(kFgFrameWithoutTag);
        if (at == std::string::npos) continue;
        const std::string reason = l.substr(at + sizeof(kFgFrameWithoutTag) - 1);
        const double t = LineTimeMs(l);
        const auto it = last.find(reason);
        if (it != last.end() && t >= 0 && it->second >= 0) {
            double gap = t - it->second;
            if (gap < 0) gap += 24.0 * 3600.0 * 1000.0;
            if (gap < periodMs) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), ": %.0f ms apart", gap);
                violations.push_back(reason + buf);
            }
        }
        last[reason] = t;
    }
    return violations;
}

// ------------------------------------------------------------------ --expect-fg

// What the test app's --fake-ngx (and --fake-camera) run must show in the
// bridge log: the render size of the first DLSS feature and the fake camera.
struct FgExpectation {
    bool pipeline = true;  // false: no camera is written (no-camera)
    unsigned renderW = 0, renderH = 0;
    float fovVDeg = 0, clipNear = 0, clipFar = 0;
    double minShare = 0.9;                   // of base_fps, per statistics line after the first
    double reasonPeriodMs = 10000.0 - 50.0;  // the throttle, less the timestamps' jitter
};

struct FgCheckResult {
    std::vector<std::string> problems;
    std::vector<std::string> evidence;  // the lines the verdict rests on
};

// DXGI_FORMAT and NGX values the checks compare with (dxgiformat.h,
// nvsdk_ngx_defs.h), without the headers.
inline constexpr int kDxgiR32Typeless = 39;
inline constexpr int kDxgiR32Float = 41;
inline constexpr int kDxgiR16G16Float = 34;
inline constexpr unsigned kNgxCreateFlagsMVLowRes = 2;

inline bool ContainsNoCase(const std::string& text, const char* needle) {
    std::string t = text, n = needle;
    for (auto& c : t) c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    for (auto& c : n) c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    return t.find(n) != std::string::npos;
}

// The checks of --expect-fg on the bridge log's lines (M3 log line contract).
inline FgCheckResult CheckFgLines(const std::vector<std::string>& lines, const FgExpectation& e) {
    FgCheckResult r;
    auto problem = [&r](const std::string& p) { r.problems.push_back(p); };
    // The one line that contains `needle` (a problem when there is none or more).
    auto onceOnly = [&](const char* needle) -> const std::string* {
        const std::string* found = nullptr;
        int n = 0;
        for (const auto& l : lines)
            if (l.find(needle) != std::string::npos && n++ == 0) found = &l;
        if (n != 1) {
            problem("the bridge log has " + std::to_string(n) + " \"" + needle + "\" lines, expected one");
            return nullptr;
        }
        r.evidence.push_back(*found);
        return found;
    };

    // The hook reached the fake module.
    bool namesFake = false;
    for (const auto& l : lines) {
        if (l.find("] INFO NGX hook: installed on ") == std::string::npos) continue;
        r.evidence.push_back(l);
        namesFake = namesFake || ContainsNoCase(l, "fake_nvngx.dll");
    }
    if (!namesFake) problem("the bridge log has no \"NGX hook: installed on\" line naming fake_nvngx.dll");

    // The first counted evaluate: the test app's inputs at the first feature's size.
    if (const std::string* l = onceOnly(kFirstCaptureTag)) {
        FirstCapture c;
        if (!ParseFirstCapture(*l, &c)) {
            problem("the first counted evaluate line does not parse: " + *l);
        } else if ((c.depthFormat != kDxgiR32Typeless && c.depthFormat != kDxgiR32Float) || c.depthW != e.renderW ||
                   c.depthH != e.renderH || c.mvecFormat != kDxgiR16G16Float || c.mvecW != e.renderW ||
                   c.mvecH != e.renderH || c.subrectW != e.renderW || c.subrectH != e.renderH ||
                   c.createFlags != kNgxCreateFlagsMVLowRes || !(c.mvScaleX < 0 && c.mvScaleY < 0)) {
            char want[200];
            std::snprintf(want, sizeof(want),
                          "depth %d (or %d) %ux%u, mvec %d %ux%u, subrect %ux%u, create flags 0x%X, a negative mv scale",
                          kDxgiR32Typeless, kDxgiR32Float, e.renderW, e.renderH, kDxgiR16G16Float, e.renderW,
                          e.renderH, e.renderW, e.renderH, kNgxCreateFlagsMVLowRes);
            problem(std::string("the first counted evaluate is not the test app's (expected ") + want + "): " + *l);
        }
    }

    const char* const kConstants = "] INFO constants: ";
    const char* const kFirstTags = "fg: first tags and constants set (frame ";
    if (e.pipeline) {
        if (const std::string* l = onceOnly(kFirstCameraTag)) {
            FirstCamera c;
            if (!ParseFirstCamera(*l, &c)) {
                problem("the first fresh camera line does not parse: " + *l);
            } else if (c.fov < e.fovVDeg - 0.01f || c.fov > e.fovVDeg + 0.01f || c.zNear < e.clipNear - 1e-4f ||
                       c.zNear > e.clipNear + 1e-4f || c.zFar < e.clipFar - 0.5f || c.zFar > e.clipFar + 0.5f ||
                       c.renderW != static_cast<float>(e.renderW) || c.renderH != static_cast<float>(e.renderH)) {
                char want[160];
                std::snprintf(want, sizeof(want), "fov %.0f, near %.1f, far %.0f, render %ux%u", e.fovVDeg,
                              e.clipNear, e.clipFar, e.renderW, e.renderH);
                problem(std::string("the first fresh camera is not the test app's (") + want + "): " + *l);
            }
        }
        onceOnly(kConstants);
        onceOnly(kFirstTags);
    } else {
        for (const char* never : {kFirstCameraTag, kFirstTags})
            for (const auto& l : lines)
                if (l.find(never) != std::string::npos) problem("\"" + std::string(never) + "\" without a camera: " + l);
        bool namesCamera = false;
        for (const auto& l : lines) {
            if (l.find(kFgFrameWithoutTag) == std::string::npos) continue;
            r.evidence.push_back(l);
            namesCamera = namesCamera || ContainsNoCase(l.substr(l.find(kFgFrameWithoutTag)), "camera");
        }
        if (!namesCamera) problem("no \"fg: frame without DLSS-G:\" warning names the camera");
    }
    for (const auto& v : FgReasonThrottleViolations(lines, e.reasonPeriodMs))
        problem("\"fg: frame without DLSS-G:\" more than once per 10 s: " + v);

    // The statistics after the first line (warm-up): every frame captured,
    // with a fresh camera and tagged (pipeline), or captured but never
    // tagged (no camera); no double evaluate; every frame delivered.
    int counted = 0;
    bool first = true;
    for (const auto& l : lines) {
        if (l.find(" stats: base_fps=") == std::string::npos) continue;
        const Stats s = ParseStats(l);
        const bool warmUp = first;
        first = false;
        if (!warmUp) ++counted;
        if (!HasM3Stats(s)) {
            problem("statistics without the M3 fields (captures ... double_evaluates): " + l);
            continue;
        }
        if (!e.pipeline && (s.Num("camera_fresh") != 0 || s.Num("tagged") != 0))
            problem("statistics with a fresh camera or tags although no camera is written: " + l);
        if (warmUp) continue;
        const double base = s.Num("base_fps");
        if (e.pipeline) {
            for (const char* k : {"captures", "camera_fresh", "tagged"}) {
                if (s.Num(k) >= e.minShare * base) continue;
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%s=%.0f is below %.0f%% of base_fps=%.1f: ", k, s.Num(k),
                              e.minShare * 100, base);
                problem(buf + l);
            }
        } else if (s.Num("captures") <= 0) {
            problem("no captures: " + l);
        }
        if (s.Num("double_evaluates") != 0) problem("double evaluates: " + l);
        if (s.Num("skipped") != 0 || s.Num("failed") != 0)
            problem("the proxy did not deliver every frame (skipped, failed): " + l);
    }
    if (counted == 0) problem("no statistics line after the first one; the run is too short for --expect-fg");
    return r;
}

}  // namespace testapp