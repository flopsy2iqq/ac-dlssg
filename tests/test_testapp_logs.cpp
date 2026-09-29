// The test app's bridge-log parsers (tools/testapp/log_checks.h), on lines in
// the formats of the M2 log and of the M3 log line contract. The scenarios
// can only show the negative side until the bridge writes the M3 lines; these
// tests pin the positive side.
#include <string>
#include <vector>

#include "log_checks.h"
#include "test_framework.h"

using namespace testapp;

namespace {

const char kM2Stats[] =
    "20:07:57.031 [18708] INFO stats: base_fps=323.0 presented_fps=323.0 skipped=0 failed=0 occluded=0 uncopied=0 "
    "max_frame_ms=4.6 max_present_ms=2.9 bridge_gpu_ms d3d11=0.013 d3d12=0.011 fg=off stalls=0 streamline=on "
    "reflex=on pcl_problems=0 vram_mib=48/9283";

std::string M3Stats(const std::string& m3, const std::string& fg = "off", const std::string& pcl = "0",
                    const std::string& vram = "48/9283") {
    return "20:07:58.031 [18708] INFO stats: base_fps=320.0 presented_fps=320.0 skipped=0 failed=0 occluded=0 "
           "uncopied=0 max_frame_ms=4.6 max_present_ms=2.9 bridge_gpu_ms d3d11=0.013 d3d12=0.011 fg=" +
           fg + " stalls=0 streamline=on reflex=on pcl_problems=" + pcl + " " + m3 + (m3.empty() ? "" : " ") +
           "vram_mib=" + vram;
}

const char kM3Fields[] =
    "captures=320 camera_fresh=319 tagged=319 fg_frames=0 generated=n/a double_evaluates=0 fg_mult=2";

}  // namespace

TEST(TestappLogs_M2StatisticsLinePasses) {
    const Stats s = ParseStats(kM2Stats);
    CHECK_EQ(StatsProblem(s), std::string());
    CHECK(!HasM3Stats(s));
    CHECK(s.Num("base_fps") == 323.0);
    CHECK(s.Num("d3d12") == 0.011);
    CHECK(*s.Get("fg") == "off");
    CHECK(s.Num("captures") == -1);
}

TEST(TestappLogs_M3StatisticsLinePasses) {
    const Stats s = ParseStats(M3Stats(kM3Fields));
    CHECK_EQ(StatsProblem(s), std::string());
    CHECK(HasM3Stats(s));
    CHECK(s.Num("captures") == 320);
    CHECK(s.Num("camera_fresh") == 319);
    CHECK(s.Num("double_evaluates") == 0);
    CHECK(*s.Get("generated") == "n/a");
    CHECK(s.Num("generated") == -1);
    CHECK(s.Num("fg_mult") == 2);
    CHECK_EQ(StatsProblem(ParseStats(M3Stats(
                 "captures=320 camera_fresh=319 tagged=319 fg_frames=319 generated=318 double_evaluates=0 fg_mult=2",
                 "on"))),
             std::string());
    // 4X: three generated frames per DLSS-G Present.
    CHECK_EQ(StatsProblem(ParseStats(M3Stats(
                 "captures=320 camera_fresh=319 tagged=319 fg_frames=319 generated=957 double_evaluates=0 fg_mult=4",
                 "on"))),
             std::string());
}

TEST(TestappLogs_StatisticsLineProblems) {
    // Some M3 fields but not all, a wrong order, or after vram_mib.
    CHECK(!StatsProblem(ParseStats(M3Stats("captures=320 camera_fresh=319 tagged=319"))).empty());
    CHECK(!StatsProblem(ParseStats(M3Stats(
                            "camera_fresh=319 captures=320 tagged=319 fg_frames=0 generated=n/a double_evaluates=0 "
                            "fg_mult=2")))
               .empty());
    CHECK(!StatsProblem(ParseStats(std::string(kM2Stats) + " " + kM3Fields)).empty());
    // Values.
    CHECK(!StatsProblem(ParseStats(M3Stats(kM3Fields, "maybe"))).empty());
    CHECK(!StatsProblem(ParseStats(M3Stats(kM3Fields, "off", "2"))).empty());
    CHECK(!StatsProblem(ParseStats(M3Stats(kM3Fields, "off", "0", "48/0"))).empty());
    CHECK(!StatsProblem(ParseStats(M3Stats(kM3Fields, "off", "0", "n/a"))).empty());
    CHECK(!StatsProblem(ParseStats(M3Stats(
                            "captures=x camera_fresh=319 tagged=319 fg_frames=0 generated=n/a double_evaluates=0 "
                            "fg_mult=2")))
               .empty());
    CHECK(!StatsProblem(ParseStats(M3Stats(
                            "captures=320 camera_fresh=319 tagged=319 fg_frames=n/a generated=n/a double_evaluates=0 "
                            "fg_mult=2")))
               .empty());
    // Multi frame generation: fg_mult after double_evaluates, 2, 3 or 4.
    const std::string m3NoMult =
        "captures=320 camera_fresh=319 tagged=319 fg_frames=0 generated=n/a double_evaluates=0";
    CHECK(!StatsProblem(ParseStats(M3Stats(m3NoMult))).empty());
    for (const char* bad : {"fg_mult=1", "fg_mult=5", "fg_mult=x", "fg_mult=n/a"})
        CHECK(!StatsProblem(ParseStats(M3Stats(m3NoMult + " " + bad))).empty());
    CHECK(!StatsProblem(ParseStats(M3Stats(m3NoMult, "off", "0", "48/9283 fg_mult=2"))).empty());
    CHECK(!StatsProblem(ParseStats(M3Stats(
                            "captures=320 camera_fresh=319 tagged=319 fg_frames=0 generated=n/a fg_mult=2 "
                            "double_evaluates=0")))
               .empty());
    // An M2 field missing.
    std::string noStalls = kM2Stats;
    noStalls.replace(noStalls.find(" stalls=0"), 9, "");
    CHECK(!StatsProblem(ParseStats(noStalls)).empty());
}

TEST(TestappLogs_FirstCaptureLineParses) {
    FirstCapture c;
    REQUIRE(ParseFirstCapture("20:07:40.100 [1] INFO capture: first counted evaluate: depth 39 853x480, mvec 34 "
                              "853x480, subrect 853x480, create flags 0x2, mv scale -853,-480, jitter 0,-0.166667",
                              &c));
    CHECK_EQ(c.depthFormat, 39);
    CHECK_EQ(c.depthW, 853u);
    CHECK_EQ(c.depthH, 480u);
    CHECK_EQ(c.mvecFormat, 34);
    CHECK_EQ(c.mvecW, 853u);
    CHECK_EQ(c.subrectW, 853u);
    CHECK_EQ(c.subrectH, 480u);
    CHECK_EQ(c.createFlags, 2u);
    CHECK(c.mvScaleX == -853.0f && c.mvScaleY == -480.0f);
    CHECK(c.jitterX == 0.0f && c.jitterY < -0.16f && c.jitterY > -0.17f);
    // Floats printed with decimals and a space after the comma parse too.
    REQUIRE(ParseFirstCapture("capture: first counted evaluate: depth 41 1066x600, mvec 34 1066x600, subrect "
                              "1066x600, create flags 0x2, mv scale -1066.000000, -600.000000, jitter 0.250, 0.125",
                              &c));
    CHECK_EQ(c.depthFormat, 41);
    CHECK(c.mvScaleY == -600.0f && c.jitterY == 0.125f);
    CHECK(!ParseFirstCapture("capture: first counted evaluate: depth 39 853x480", &c));
    CHECK(!ParseFirstCapture("stats: base_fps=1", &c));
}

TEST(TestappLogs_FirstCameraLineParses) {
    FirstCamera c;
    REQUIRE(ParseFirstCamera("20:07:40.101 [1] INFO camera: first fresh snapshot: pos (200.000, 1.500, 0.000) fwd "
                             "(-0.000, 0.000, 1.000) up (0.000, 1.000, 0.000) fov 56.00 near 0.100 far 20000.0 render "
                             "853x480 origin shift (0.000, 0.000, 0.000)",
                             &c));
    CHECK(c.fov == 56.0f && c.zNear == 0.1f && c.zFar == 20000.0f && c.renderW == 853.0f && c.renderH == 480.0f);
    REQUIRE(ParseFirstCamera("camera: first fresh snapshot: pos (1, 2, 3) fwd (0, 0, 1) up (0, 1, 0) fov 56 near 0.1 "
                             "far 20000 render 853.0x480.0 origin shift (0, 0, 0)",
                             &c));
    CHECK(c.renderW == 853.0f && c.renderH == 480.0f);
    CHECK(!ParseFirstCamera("camera: first fresh snapshot: pos (1, 2, 3)", &c));
}

TEST(TestappLogs_LineTime) {
    CHECK(LineTimeMs("20:07:57.031 [18708] INFO stats: x") == ((20 * 60.0 + 7) * 60 + 57) * 1000 + 31);
    CHECK(LineTimeMs("00:00:00.000 [1] WARN x") == 0);
    CHECK(LineTimeMs("no time here") == -1);
}

// At most one warning per distinct reason per period; other reasons and
// midnight do not confuse the check.
TEST(TestappLogs_FgReasonThrottle) {
    const std::vector<std::string> ok = {
        "10:00:00.000 [1] WARN fg: frame without DLSS-G: no fresh camera",
        "10:00:01.000 [1] WARN fg: frame without DLSS-G: no capture",
        "10:00:10.000 [1] WARN fg: frame without DLSS-G: no fresh camera",
        "10:00:11.000 [1] INFO stats: base_fps=1",
        "23:59:55.000 [1] WARN fg: frame without DLSS-G: paused",
        "00:00:05.000 [1] WARN fg: frame without DLSS-G: paused",
    };
    CHECK(FgReasonThrottleViolations(ok, 10000.0).empty());
    const std::vector<std::string> bad = {
        "10:00:00.000 [1] WARN fg: frame without DLSS-G: no fresh camera",
        "10:00:09.000 [1] WARN fg: frame without DLSS-G: no fresh camera",
        "23:59:59.000 [1] WARN fg: frame without DLSS-G: paused",
        "00:00:01.000 [1] WARN fg: frame without DLSS-G: paused",
    };
    const std::vector<std::string> v = FgReasonThrottleViolations(bad, 10000.0);
    REQUIRE(v.size() == 2);
    CHECK(v[0].find("no fresh camera") != std::string::npos && v[0].find("9000 ms") != std::string::npos);
    CHECK(v[1].find("paused") != std::string::npos && v[1].find("2000 ms") != std::string::npos);
}

// ------------------------------------------------------------ --expect-fg

namespace {

// A bridge log of an fg-pipeline run in the formats of the M3 contract.
std::vector<std::string> PipelineLog() {
    return {
        "20:00:00.100 [1] INFO CreateSwapChainForHwnd: hwnd 0000000000010000 (main window), 1280x720 format 28: proxy",
        "20:00:00.200 [1] INFO NGX hook: installed on 1 module(s): fake_nvngx.dll",
        "20:00:00.300 [1] INFO capture: first counted evaluate: depth 39 853x480, mvec 34 853x480, subrect 853x480, "
        "create flags 0x2, mv scale -853,-480, jitter 0,-0.166667",
        "20:00:00.301 [1] WARN fg: frame without DLSS-G: camera not fresh",
        "20:00:00.302 [1] INFO camera: first fresh snapshot: pos (200.000, 1.500, 0.000) fwd (0.000, 0.000, 1.000) up "
        "(0.000, 1.000, 0.000) fov 56.00 near 0.100 far 20000.0 render 853x480 origin shift (0.000, 0.000, 0.000)",
        "20:00:00.303 [1] INFO constants: basis det -1.000 near 0.100 far 20000.0 fovY 0.977 aspect 1.777",
        "20:00:00.304 [1] INFO fg: first tags and constants set (frame 2)",
        "20:00:01.000 [1] INFO stats: base_fps=300.0 presented_fps=300.0 skipped=0 failed=0 occluded=0 uncopied=0 "
        "max_frame_ms=4.0 max_present_ms=1.0 bridge_gpu_ms d3d11=0.100 d3d12=0.050 fg=off stalls=0 streamline=on "
        "reflex=on pcl_problems=0 captures=150 camera_fresh=100 tagged=99 fg_frames=0 generated=n/a "
        "double_evaluates=0 fg_mult=2 vram_mib=48/9283",
        "20:00:02.000 [1] INFO stats: base_fps=320.0 presented_fps=320.0 skipped=0 failed=0 occluded=0 uncopied=0 "
        "max_frame_ms=4.0 max_present_ms=1.0 bridge_gpu_ms d3d11=0.100 d3d12=0.050 fg=off stalls=0 streamline=on "
        "reflex=on pcl_problems=0 captures=320 camera_fresh=320 tagged=320 fg_frames=0 generated=n/a "
        "double_evaluates=0 fg_mult=2 vram_mib=48/9283",
        "20:00:03.000 [1] INFO stats: base_fps=318.0 presented_fps=318.0 skipped=0 failed=0 occluded=0 uncopied=0 "
        "max_frame_ms=4.0 max_present_ms=1.0 bridge_gpu_ms d3d11=0.100 d3d12=0.050 fg=off stalls=0 streamline=on "
        "reflex=on pcl_problems=0 captures=318 camera_fresh=317 tagged=317 fg_frames=0 generated=n/a "
        "double_evaluates=0 fg_mult=2 vram_mib=48/9283",
    };
}

FgExpectation Pipeline() {
    FgExpectation e;
    e.pipeline = true;
    e.renderW = 853;
    e.renderH = 480;
    e.fovVDeg = 56.0f;
    e.clipNear = 0.1f;
    e.clipFar = 20000.0f;
    return e;
}

// The same run without a camera: captured, never fresh, never tagged.
std::vector<std::string> NoCameraLog() {
    std::vector<std::string> log;
    for (const auto& l : PipelineLog()) {
        if (l.find("camera: first") != std::string::npos || l.find("constants: ") != std::string::npos ||
            l.find("fg: first tags") != std::string::npos)
            continue;
        std::string s = l;
        for (const char* k : {"camera_fresh=", "tagged="}) {
            const size_t at = s.find(k);
            if (at == std::string::npos) continue;
            const size_t end = s.find(' ', at);
            s.replace(at, end - at, std::string(k) + "0");
        }
        log.push_back(s);
    }
    return log;
}

FgExpectation NoCamera() {
    FgExpectation e = Pipeline();
    e.pipeline = false;
    return e;
}

std::vector<std::string> Replaced(std::vector<std::string> log, const std::string& from, const std::string& to) {
    for (auto& l : log) {
        const size_t at = l.find(from);
        if (at != std::string::npos) l.replace(at, from.size(), to);
    }
    return log;
}

std::vector<std::string> Without(std::vector<std::string> log, const std::string& needle) {
    std::vector<std::string> out;
    for (const auto& l : log)
        if (l.find(needle) == std::string::npos) out.push_back(l);
    return out;
}

bool HasProblem(const FgCheckResult& r, const char* needle) {
    for (const auto& p : r.problems)
        if (p.find(needle) != std::string::npos) return true;
    return false;
}

void PrintProblems(const FgCheckResult& r) {
    for (const auto& p : r.problems) std::printf("  problem: %s\n", p.c_str());
}

}  // namespace

TEST(TestappLogs_FgPipelineLogPasses) {
    const FgCheckResult r = CheckFgLines(PipelineLog(), Pipeline());
    PrintProblems(r);
    CHECK(r.problems.empty());
    CHECK(r.evidence.size() >= 5);  // the hook line and the four once-only lines
}

TEST(TestappLogs_FgPipelineProblems) {
    const FgExpectation e = Pipeline();
    CHECK(HasProblem(CheckFgLines(Without(PipelineLog(), "NGX hook: "), e), "fake_nvngx.dll"));
    CHECK(HasProblem(CheckFgLines(Replaced(PipelineLog(), "fake_nvngx.dll", "_nvngx.dll"), e), "fake_nvngx.dll"));
    CHECK(HasProblem(CheckFgLines(Without(PipelineLog(), "constants: "), e), "constants: "));
    CHECK(HasProblem(CheckFgLines(Without(PipelineLog(), "fg: first tags"), e), "first tags"));
    CHECK(HasProblem(CheckFgLines(Without(PipelineLog(), "camera: first fresh"), e), "camera: first fresh"));
    std::vector<std::string> twice = PipelineLog();
    twice.push_back(twice[2]);
    CHECK(HasProblem(CheckFgLines(twice, e), "capture: first counted evaluate"));
    CHECK(HasProblem(CheckFgLines(Replaced(PipelineLog(), "depth 39", "depth 28"), e), "first counted evaluate"));
    CHECK(HasProblem(CheckFgLines(Replaced(PipelineLog(), "mvec 34", "mvec 16"), e), "first counted evaluate"));
    CHECK(HasProblem(CheckFgLines(Replaced(PipelineLog(), "subrect 853x480", "subrect 0x0"), e), "first counted evaluate"));
    CHECK(HasProblem(CheckFgLines(Replaced(PipelineLog(), "flags 0x2", "flags 0x8"), e), "first counted evaluate"));
    CHECK(HasProblem(CheckFgLines(Replaced(PipelineLog(), "mv scale -853,-480", "mv scale 853,480"), e),
                     "first counted evaluate"));
    CHECK(HasProblem(CheckFgLines(Replaced(PipelineLog(), "fov 56.00", "fov 90.00"), e), "first fresh camera"));
    CHECK(HasProblem(CheckFgLines(Replaced(PipelineLog(), "render 853x480", "render 1280x720"), e), "first fresh camera"));
    // Statistics after the first line.
    CHECK(HasProblem(CheckFgLines(Replaced(PipelineLog(), "tagged=320", "tagged=200"), e), "tagged=200"));
    CHECK(HasProblem(CheckFgLines(Replaced(PipelineLog(), "camera_fresh=317", "camera_fresh=10"), e), "camera_fresh=10"));
    CHECK(HasProblem(CheckFgLines(Replaced(PipelineLog(), "captures=318", "captures=0"), e), "captures=0"));
    CHECK(HasProblem(CheckFgLines(Replaced(PipelineLog(), "captures=320 camera_fresh=320 tagged=320 fg_frames=0 "
                                                          "generated=n/a double_evaluates=0",
                                           "captures=320 camera_fresh=320 tagged=320 fg_frames=0 generated=n/a "
                                           "double_evaluates=3"),
                                  e),
                     "double evaluates"));
    CHECK(HasProblem(CheckFgLines(Replaced(PipelineLog(), "base_fps=318.0 presented_fps=318.0 skipped=0",
                                           "base_fps=318.0 presented_fps=318.0 skipped=4"),
                                  e),
                     "deliver every frame"));
    CHECK(HasProblem(CheckFgLines(Replaced(PipelineLog(), " captures=318 camera_fresh=317 tagged=317 fg_frames=0 "
                                                          "generated=n/a double_evaluates=0",
                                           ""),
                                  e),
                     "M3 fields"));
    // Only the warm-up line: too short.
    CHECK(HasProblem(CheckFgLines(Without(Without(PipelineLog(), "base_fps=320"), "base_fps=318"), e), "too short"));
    // The warm-up line may be low.
    CHECK(CheckFgLines(Replaced(PipelineLog(), "tagged=99", "tagged=1"), e).problems.empty());
    // A reason warned twice within 10 s.
    std::vector<std::string> spam = PipelineLog();
    spam.push_back("20:00:05.000 [1] WARN fg: frame without DLSS-G: camera not fresh");
    CHECK(HasProblem(CheckFgLines(spam, e), "camera not fresh"));
}

// --expect-fg-mult N (multi frame generation): every statistics line has
// fg_mult=N, or a lower multiplier that the log explains with Streamline's
// maximum or the video memory fallback.
TEST(TestappLogs_FgMultiplierExpectation) {
    FgExpectation e = Pipeline();
    e.multiplier = 2;
    CHECK(CheckFgLines(PipelineLog(), e).problems.empty());
    e.multiplier = 3;
    CHECK(HasProblem(CheckFgLines(PipelineLog(), e), "fg_mult=2, expected 3"));
    const std::vector<std::string> mult3 = Replaced(PipelineLog(), "fg_mult=2", "fg_mult=3");  // every line
    CHECK(CheckFgLines(mult3, e).problems.empty());
    // A lower multiplier that the log explains.
    std::vector<std::string> clamped = PipelineLog();
    clamped.insert(clamped.begin() + 1,
                   "20:00:00.150 [1] INFO fg: 3X requested, Streamline allows up to 2X; using 2X");
    PrintProblems(CheckFgLines(clamped, e));
    CHECK(CheckFgLines(clamped, e).problems.empty());
    std::vector<std::string> fellBack = PipelineLog();
    fellBack.insert(fellBack.begin() + 1,
                    "20:00:00.150 [1] INFO fg: video memory: 3X needs 700 MiB, free 500 MiB; falling back to 2X");
    CHECK(CheckFgLines(fellBack, e).problems.empty());
    // A higher one is never explained.
    e.multiplier = 2;
    CHECK(HasProblem(CheckFgLines(Replaced(clamped, "fg_mult=2", "fg_mult=4"), e), "fg_mult=4, expected 2"));
    // Unchecked by default.
    CHECK_EQ(FgExpectation().multiplier, 0u);
}

TEST(TestappLogs_FgNoCameraLogPasses) {
    std::vector<std::string> log = NoCameraLog();
    const FgCheckResult r = CheckFgLines(log, NoCamera());
    PrintProblems(r);
    CHECK(r.problems.empty());
}

TEST(TestappLogs_FgNoCameraProblems) {
    const FgExpectation e = NoCamera();
    CHECK(HasProblem(CheckFgLines(Without(NoCameraLog(), "fg: frame without DLSS-G"), e), "names the camera"));
    CHECK(HasProblem(CheckFgLines(Replaced(NoCameraLog(), "camera not fresh", "no capture"), e), "names the camera"));
    CHECK(HasProblem(CheckFgLines(PipelineLog(), e), "without a camera"));
    CHECK(HasProblem(CheckFgLines(Replaced(NoCameraLog(), "captures=320", "captures=0"), e), "no captures"));
    std::vector<std::string> fresh = NoCameraLog();
    CHECK(HasProblem(CheckFgLines(Replaced(fresh, "captures=318 camera_fresh=0", "captures=318 camera_fresh=5"), e),
                     "fresh camera or tags"));
}

// The release lines: the proxies the test app released, plus what a failed
// proxy creation releases inside itself (the presenter refused for an
// unsupported adapter, fg-unsupported-passthrough).
TEST(TestappLogs_ReleaseCounts) {
    const std::string proxy = "22:26:06.193 [1] INFO ProxySwapChain released";
    const std::string presenter = "22:26:06.192 [1] INFO presenter released";
    const std::string failed =
        "22:26:06.193 [1] ERROR proxy swap chain creation failed (0x80004005): DLSS-G is not supported on this "
        "adapter (x); passing through";
    CHECK(ReleaseProblems({}, 0).empty());
    CHECK(ReleaseProblems({presenter, proxy, presenter, proxy}, 2).empty());
    // A missing or an extra release of each kind.
    CHECK_EQ(ReleaseProblems({presenter, proxy}, 2).size(), 2u);
    CHECK_EQ(ReleaseProblems({presenter, proxy}, 0).size(), 2u);
    CHECK_EQ(ReleaseProblems({presenter, proxy, proxy}, 2).size(), 1u);
    // A failed creation releases its proxy, and its presenter when one was built.
    CHECK(ReleaseProblems({presenter, proxy, failed}, 0).empty());
    CHECK(ReleaseProblems({proxy, failed}, 0).empty());
    CHECK_EQ(ReleaseProblems({failed}, 0).size(), 1u);
    CHECK_EQ(ReleaseProblems({presenter, presenter, proxy, failed}, 0).size(), 1u);
    // One proxy the test app released and one failed creation.
    CHECK(ReleaseProblems({presenter, proxy, failed, presenter, proxy}, 1).empty());
    CHECK_EQ(ReleaseProblems({presenter, proxy, failed, presenter, proxy}, 0).size(), 2u);
}
