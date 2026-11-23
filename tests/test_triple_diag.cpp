// The triple-screen diagnostics of 1.3.0 (triple_diag.h): when they run,
// what they read from CSP's DLSS evaluate, the log lines, and the windows of
// logged frames.
#include <windows.h>

#include <optional>
#include <string>
#include <vector>

#include "fake_nvngx/fake_nvngx.h"
#include "ngx_hook.h"
#include "test_framework.h"
#include "triple_diag.h"

using namespace acdb;

namespace {

struct Lines {
    std::vector<std::string> all;
    std::function<void(const std::string&)> Sink() {
        return [this](const std::string& l) { all.push_back(l); };
    }
    size_t Count(const char* piece) const {
        size_t n = 0;
        for (const std::string& l : all)
            if (l.find(piece) != std::string::npos) ++n;
        return n;
    }
    void Print() const {
        for (const std::string& l : all) std::printf("    | %s\n", l.c_str());
    }
};

TripleDiagEvaluate Evaluate(uint64_t key) {
    TripleDiagEvaluate e;
    e.featureKey = key;
    return e;
}

bool Has(const std::string& s, const char* piece) { return s.find(piece) != std::string::npos; }
bool Has(const std::string& s, const std::string& piece) { return Has(s, piece.c_str()); }

std::string Ptr(const void* p) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%p", p);
    return buf;
}

}  // namespace

static_assert(kTripleDiagFrames == 300 && kTripleDiagWindows == 4);

TEST(TripleDiag_RunsOnlyWithTheSwitchInTripleScreenMode) {
    CHECK(TripleDiagActive(true, std::string("TRIPLE")));
    CHECK(TripleDiagActive(true, std::string("triple")));
    CHECK(!TripleDiagActive(false, std::string("TRIPLE")));
    CHECK(!TripleDiagActive(true, std::string("DEFAULT")));
    CHECK(!TripleDiagActive(true, std::string("__EXT_360")));
    CHECK(!TripleDiagActive(true, std::nullopt));
}

TEST(TripleDiagLog_NothingBeforeTheFirstEvaluate) {
    Lines lines;
    TripleDiagLog log(lines.Sink(), 3, 2);
    for (int i = 0; i < 5; ++i) log.EndFrame(5760, 1080, 28);
    CHECK(lines.all.empty());
    CHECK(log.WantsEvaluates());
}

// From the first evaluate on, every evaluate and every frame for the
// window's frames, then nothing until a new DLSS feature.
TEST(TripleDiagLog_LogsEveryEvaluateOfTheWindow) {
    Lines lines;
    TripleDiagLog log(lines.Sink(), 3, 2);
    log.OnEvaluate(Evaluate(0xA));
    log.OnEvaluate(Evaluate(0xB));
    log.OnEvaluate(Evaluate(0xC));
    log.EndFrame(5760, 1080, 28);
    log.EndFrame(5760, 1080, 28);
    log.OnEvaluate(Evaluate(0xA));
    log.EndFrame(5760, 1080, 28);
    log.OnEvaluate(Evaluate(0xA));
    log.EndFrame(5760, 1080, 28);
    lines.Print();
    CHECK_EQ(lines.Count("triple-diag: window 1 of 2"), 1u);
    CHECK_EQ(lines.Count("frame 1 evaluate "), 3u);
    CHECK_EQ(lines.Count("frame 1 evaluate 3: feature 0xC"), 1u);
    CHECK_EQ(lines.Count("triple-diag: frame 1: 3 DLSS evaluates; swap chain 5760x1080 format 28"), 1u);
    CHECK_EQ(lines.Count("triple-diag: frame 2: 0 DLSS evaluates"), 1u);
    CHECK_EQ(lines.Count("frame 3 evaluate 1: feature 0xA"), 1u);
    CHECK_EQ(lines.Count("triple-diag: window 1 done after 3 frames"), 1u);
    CHECK_EQ(lines.Count("frame 4"), 0u);
    CHECK(!log.WantsEvaluates());
}

// Review finding B-4: a window whose frames never end (no Present ends them
// once the presenter stopped) still stops, at the cap of evaluate lines per
// window; and every window together stops at the cap for the session.
TEST(TripleDiagLog_EvaluatesWithoutAnEndStopAtTheCaps) {
    static_assert(kTripleDiagMaxEvaluateLinesPerWindow == 3000 && kTripleDiagMaxEvaluateLines == 6000);
    Lines lines;
    TripleDiagLog log(lines.Sink(), 300, 4, 10, 15);
    for (int i = 0; i < 50; ++i) log.OnEvaluate(Evaluate(1));
    CHECK_EQ(lines.Count(": feature 0x1;"), 10u);
    CHECK_EQ(lines.Count("triple-diag: window 1 stopped at frame 1 after 10 evaluate lines, the most for one window; "
                         "a DLSS feature created later starts window 2"),
             1u);
    CHECK(!log.WantsEvaluates());
    log.OnCreate(2, 1920, 1080, 5760, 1080);
    CHECK(log.WantsEvaluates());
    for (int i = 0; i < 50; ++i) log.OnEvaluate(Evaluate(2));
    lines.Print();
    CHECK_EQ(lines.Count(": feature 0x2;"), 5u);
    CHECK_EQ(lines.Count("triple-diag: 15 evaluate lines in this session, the most; nothing more is logged"), 1u);
    CHECK(!log.WantsEvaluates());
    log.OnCreate(3, 1920, 1080, 5760, 1080);
    CHECK(!log.WantsEvaluates());
    log.OnEvaluate(Evaluate(3));
    log.EndFrame(1920, 1080, 28);
    CHECK_EQ(lines.Count(": feature 0x3;"), 0u);
    CHECK_EQ(lines.Count("triple-diag: frame "), 15u);  // the evaluate lines only, no frame line after the stop
}

// Review finding B-5: while a window is open, the frame line counts the
// evaluates the hook does not count, on a deferred context and failed ones
// with the last result.
TEST(TripleDiagLog_TheFrameLineCountsTheUncountedEvaluates) {
    Lines lines;
    TripleDiagLog log(lines.Sink(), 2, 1);
    log.OnUncountedEvaluate(true, kNgxSuccess);  // before the window: in no frame line
    log.OnEvaluate(Evaluate(1));
    log.OnUncountedEvaluate(true, kNgxSuccess);
    log.OnUncountedEvaluate(true, kNgxSuccess);
    log.OnUncountedEvaluate(false, static_cast<NgxResult>(0xBAD00001u));
    log.OnUncountedEvaluate(false, static_cast<NgxResult>(0xBAD00005u));
    log.EndFrame(5760, 1080, 28);
    log.EndFrame(5760, 1080, 28);
    lines.Print();
    CHECK_EQ(lines.Count("triple-diag: frame 1: 1 DLSS evaluates; not counted: 2 on a deferred context, 2 failed "
                         "(last result 0xBAD00005); swap chain 5760x1080 format 28"),
             1u);
    CHECK_EQ(lines.Count("triple-diag: frame 2: 0 DLSS evaluates; swap chain 5760x1080 format 28"), 1u);
}

TEST(TripleDiagLog_ANewFeatureStartsAnotherWindowUpToTheLimit) {
    Lines lines;
    TripleDiagLog log(lines.Sink(), 1, 2);
    log.OnEvaluate(Evaluate(1));
    log.EndFrame(1920, 1080, 28);
    log.OnCreate(2, 1920, 1080, 5760, 1080);
    CHECK(log.WantsEvaluates());
    log.OnEvaluate(Evaluate(2));
    log.EndFrame(1920, 1080, 28);
    log.OnCreate(3, 1920, 1080, 5760, 1080);
    log.OnEvaluate(Evaluate(3));
    log.EndFrame(1920, 1080, 28);
    lines.Print();
    CHECK_EQ(lines.Count("triple-diag: window 2 of 2"), 1u);
    CHECK_EQ(lines.Count("DLSS feature 0x2 created: render 1920x1080, output 5760x1080"), 1u);
    CHECK_EQ(lines.Count("evaluate 1: feature 0x2"), 1u);
    CHECK_EQ(lines.Count("feature 0x3 created"), 1u);
    CHECK_EQ(lines.Count("evaluate 1: feature 0x3"), 0u);
    CHECK_EQ(lines.Count("that was the last window"), 1u);
    CHECK(!log.WantsEvaluates());
}

TEST(TripleDiagLines_NameTheSizesTheSubrectsAndTheTextures) {
    TripleDiagEvaluate e = Evaluate(0x1F2E);
    e.createObserved = true;
    e.createWidth = 1920;
    e.createHeight = 360;
    e.createOutWidth = 5760;
    e.createOutHeight = 1080;
    e.createFlags = 2;
    e.width = 1920;
    e.outWidth = 5760;
    e.subrectWidth = 1920;
    e.subrectHeight = 360;
    e.colorBaseX = 1920;
    e.colorBaseY = 0;
    e.outputSubrects = 1;
    e.jitterX = -0.25f;
    e.jitterY = 0.125f;
    e.mvScaleX = 1920.0f;
    e.mvScaleY = 360.0f;
    e.color = TripleDiagTexture{true, true, 1920, 360, 10, 1, 1, 1};
    e.output = TripleDiagTexture{true, true, 5760, 1080, 28, 1, 1, 1};
    e.depth = TripleDiagTexture{true, false, 0, 0, 0, 0, 0, 0};
    const std::string l = TripleDiagEvaluateLine(7, 2, e);
    std::printf("    | %s\n", l.c_str());
    CHECK(Has(l, "triple-diag: frame 7 evaluate 2: feature 0x1F2E"));
    CHECK(Has(l, "create render 1920x360 output 5760x1080 flags 0x2"));
    CHECK(Has(l, "Width=1920 Height=unset OutWidth=5760 OutHeight=unset"));
    CHECK(Has(l, "subrect 1920x360"));
    CHECK(Has(l, "bases color 1920,0 depth unset mv unset output unset"));
    CHECK(Has(l, "output subrects 1"));
    CHECK(Has(l, "jitter -0.2500,0.1250 mv scale 1920.0,360.0 reset 0"));
    CHECK(Has(l, "Color " + Ptr(nullptr) + " 1920x360 format 10 samples 1"));
    CHECK(Has(l, "Output " + Ptr(nullptr) + " 5760x1080 format 28 samples 1"));
    CHECK(Has(l, "Depth " + Ptr(nullptr) + " not a 2D texture"));
    CHECK(Has(l, "MotionVectors none"));
    TripleDiagEvaluate bare = Evaluate(9);
    CHECK(Has(TripleDiagEvaluateLine(1, 1, bare), "create not seen"));
    CHECK_EQ(TripleDiagFrameLine({7, 3}, 5760, 1080, 28),
             std::string("triple-diag: frame 7: 3 DLSS evaluates; swap chain 5760x1080 format 28"));
    // Review finding B-5: the evaluates the hook does not count.
    CHECK_EQ(TripleDiagFrameLine({7, 0, 0, 2, 0xBAD00005u}, 5760, 1080, 28),
             std::string("triple-diag: frame 7: 0 DLSS evaluates; not counted: 2 failed (last result 0xBAD00005); swap "
                         "chain 5760x1080 format 28"));
    CHECK_EQ(TripleDiagFrameLine({7, 1, 3, 0, 0}, 5760, 1080, 28),
             std::string("triple-diag: frame 7: 1 DLSS evaluates; not counted: 3 on a deferred context; swap chain "
                         "5760x1080 format 28"));
}

// Review finding B-2: the line identifies each texture by its pointer, and
// says on which context the evaluate ran and whether that context belongs
// to the game's D3D11 device (the device of the proxied swap chain).
TEST(TripleDiagLines_IdentifyTheResourcesAndTheDevice) {
    TripleDiagEvaluate e = Evaluate(0x10);
    int ctx = 0, color = 0, depth = 0;
    e.context = &ctx;
    e.immediate = true;
    e.gameDevice = true;
    e.color = TripleDiagTexture{true, true, 1920, 360, 10, 1, 1, 1, &color};
    e.depth = TripleDiagTexture{true, false, 0, 0, 0, 0, 0, 0, &depth};
    std::string l = TripleDiagEvaluateLine(1, 1, e);
    std::printf("    | %s\n", l.c_str());
    CHECK(Has(l, "feature 0x10; context " + Ptr(&ctx) + " immediate, on the game's device; "));
    CHECK(Has(l, "Color " + Ptr(&color) + " 1920x360 format 10 samples 1"));
    CHECK(Has(l, "Depth " + Ptr(&depth) + " not a 2D texture"));
    CHECK(Has(l, "Output none"));
    e.immediate = false;
    e.gameDevice = false;
    CHECK(Has(TripleDiagEvaluateLine(1, 1, e), "context " + Ptr(&ctx) + " deferred, not on the game's device; "));
    e.gameDevice.reset();
    CHECK(Has(TripleDiagEvaluateLine(1, 1, e), " deferred, device unknown; "));
    e.context = nullptr;
    CHECK(Has(TripleDiagEvaluateLine(1, 1, e), "feature 0x10; context none; "));
}

// What NgxEvaluateInputs carries plus CSP's block, read during the evaluate.
TEST(TripleDiag_ReadsTheEvaluateBlock) {
    FakeNgxParam p;
    p.SetU("Width", 1920);
    p.SetU("Height", 360);
    p.SetU("OutWidth", 5760);
    p.SetU("OutHeight", 1080);
    p.SetU("DLSS.Render.Subrect.Dimensions.Width", 1920);
    p.SetU("DLSS.Render.Subrect.Dimensions.Height", 360);
    p.SetU("DLSS.Input.Color.Subrect.Base.X", 1920);
    p.SetU("DLSS.Input.Color.Subrect.Base.Y", 0);
    p.SetU("DLSS.Output.Subrect.Base.X", 1920);
    p.SetI("DLSS.Enable.Output.Subrects", 1);
    NgxEvaluateInputs in;
    in.params = &p;
    in.featureKey = 0x42;
    in.createObserved = true;
    in.jitterX = 0.5f;
    in.mvScaleX = 1920.0f;
    in.reset = true;
    NgxCreateInfo create;
    create.width = 1920;
    create.height = 360;
    create.outWidth = 5760;
    create.outHeight = 1080;
    create.createFlags = 2;
    const TripleDiagEvaluate e = ReadTripleDiagEvaluate(in, &create);
    CHECK_EQ(e.featureKey, 0x42ull);
    CHECK(e.createObserved && e.createWidth == 1920 && e.createOutWidth == 5760 && e.createFlags == 2u);
    CHECK(e.width == 1920u && e.height == 360u && e.outWidth == 5760u && e.outHeight == 1080u);
    CHECK(e.subrectWidth == 1920u && e.subrectHeight == 360u);
    CHECK(e.colorBaseX == 1920u && e.colorBaseY == 0u && e.outputBaseX == 1920u);
    CHECK(!e.outputBaseY && !e.depthBaseX && !e.mvBaseY);
    CHECK(e.outputSubrects == 1);
    CHECK(e.jitterX == 0.5f && e.mvScaleX == 1920.0f && e.reset);
    CHECK(!e.color.present && !e.output.present && !e.depth.present && !e.mvec.present);
    // Without a block and a create, only what NgxEvaluateInputs has.
    NgxEvaluateInputs none;
    none.featureKey = 7;
    const TripleDiagEvaluate n = ReadTripleDiagEvaluate(none, nullptr);
    CHECK(n.featureKey == 7u && !n.createObserved && !n.width && !n.subrectWidth);
    CHECK(n.context == nullptr && !n.gameDevice);
}
