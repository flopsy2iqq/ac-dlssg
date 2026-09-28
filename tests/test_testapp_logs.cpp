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

const char kM3Fields[] = "captures=320 camera_fresh=319 tagged=319 fg_frames=0 generated=n/a double_evaluates=0";

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
    CHECK_EQ(StatsProblem(ParseStats(M3Stats(
                 "captures=320 camera_fresh=319 tagged=319 fg_frames=319 generated=318 double_evaluates=0", "on"))),
             std::string());
}

TEST(TestappLogs_StatisticsLineProblems) {
    // Some M3 fields but not all, a wrong order, or after vram_mib.
    CHECK(!StatsProblem(ParseStats(M3Stats("captures=320 camera_fresh=319 tagged=319"))).empty());
    CHECK(!StatsProblem(ParseStats(M3Stats(
                            "camera_fresh=319 captures=320 tagged=319 fg_frames=0 generated=n/a double_evaluates=0")))
               .empty());
    CHECK(!StatsProblem(ParseStats(std::string(kM2Stats) + " " + kM3Fields)).empty());
    // Values.
    CHECK(!StatsProblem(ParseStats(M3Stats(kM3Fields, "maybe"))).empty());
    CHECK(!StatsProblem(ParseStats(M3Stats(kM3Fields, "off", "2"))).empty());
    CHECK(!StatsProblem(ParseStats(M3Stats(kM3Fields, "off", "0", "48/0"))).empty());
    CHECK(!StatsProblem(ParseStats(M3Stats(kM3Fields, "off", "0", "n/a"))).empty());
    CHECK(!StatsProblem(ParseStats(M3Stats(
                            "captures=x camera_fresh=319 tagged=319 fg_frames=0 generated=n/a double_evaluates=0")))
               .empty());
    CHECK(!StatsProblem(ParseStats(M3Stats(
                            "captures=320 camera_fresh=319 tagged=319 fg_frames=n/a generated=n/a double_evaluates=0")))
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
