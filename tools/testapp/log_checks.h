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

}  // namespace testapp
