#include <windows.h>

#include <regex>
#include <sstream>
#include <thread>
#include <vector>

#include "config.h"
#include "ini.h"
#include "log.h"
#include "temp_dir.h"
#include "test_framework.h"

using namespace acdb;
using acdb_test::ReadAll;
using acdb_test::TempDir;

// ---------------------------------------------------------------- IniFile

TEST(Ini_SectionsKeysAndSpaces) {
    auto ini = IniFile::Parse("[VIDEO]\nWIDTH=1920\n  HEIGHT = 1080  \n[CAMERA]\nMODE=DEFAULT\n");
    CHECK(ini.Get("VIDEO", "WIDTH") == std::optional<std::string>("1920"));
    CHECK(ini.Get("VIDEO", "HEIGHT") == std::optional<std::string>("1080"));
    CHECK(ini.Get("CAMERA", "MODE") == std::optional<std::string>("DEFAULT"));
    CHECK(!ini.Get("CAMERA", "WIDTH").has_value());
    CHECK(!ini.Get("NOPE", "WIDTH").has_value());
}

TEST(Ini_CommentLinesAreIgnored) {
    auto ini = IniFile::Parse("; top comment\n# hash comment\n[A]\n;K1=1\n#K2=2\n  ; indented\nK3=3\n");
    CHECK(!ini.Get("A", "K1").has_value());
    CHECK(!ini.Get("A", ";K1").has_value());
    CHECK(!ini.Get("A", "K2").has_value());
    CHECK(ini.Get("A", "K3") == std::optional<std::string>("3"));
}

TEST(Ini_InlineSemicolonEndsValue) {
    auto ini = IniFile::Parse("[FSR]\nACTIVE=0 ; Active; 1 or 0\nOLD_IMPLEMENTATION=3; DLSS\nNAME=a b c\n");
    CHECK(ini.Get("FSR", "ACTIVE") == std::optional<std::string>("0"));
    CHECK(ini.Get("FSR", "OLD_IMPLEMENTATION") == std::optional<std::string>("3"));
    CHECK(ini.Get("FSR", "NAME") == std::optional<std::string>("a b c"));
}

TEST(Ini_NamesAreCaseInsensitive) {
    auto ini = IniFile::Parse("[Compatibility]\nOld_SwapChain=1\n");
    CHECK(ini.Get("COMPATIBILITY", "OLD_SWAPCHAIN") == std::optional<std::string>("1"));
    CHECK(ini.Get("compatibility", "old_swapchain") == std::optional<std::string>("1"));
}

TEST(Ini_LastDuplicateWins) {
    auto ini = IniFile::Parse("[A]\nK=1\nK=2\n[B]\nX=0\n[a]\nk=3\n");
    CHECK(ini.Get("A", "K") == std::optional<std::string>("3"));
    CHECK(ini.Get("B", "X") == std::optional<std::string>("0"));
}

TEST(Ini_BomAndCrlf) {
    auto ini = IniFile::Parse("\xEF\xBB\xBF[A]\r\nK=1\r\nL = two \r\n");
    CHECK(ini.Get("A", "K") == std::optional<std::string>("1"));
    CHECK(ini.Get("A", "L") == std::optional<std::string>("two"));
}

TEST(Ini_EmptyValueIsPresent) {
    auto ini = IniFile::Parse("[A]\nK=\nJ= ; only a comment\nNOEQUALS\n");
    CHECK(ini.Get("A", "K") == std::optional<std::string>(""));
    CHECK(ini.Get("A", "J") == std::optional<std::string>(""));
    CHECK(!ini.Get("A", "NOEQUALS").has_value());
}

TEST(Ini_LoadMissingAndExisting) {
    TempDir dir(L"ini");
    CHECK(!IniFile::Load(dir.Str() + L"\\missing.ini").has_value());
    auto p = dir.Write(L"cfg\\x.ini", "[S]\nK=5\n");
    auto ini = IniFile::Load(p.wstring());
    REQUIRE(ini.has_value());
    CHECK(ini->Get("S", "K") == std::optional<std::string>("5"));
}

TEST(Ini_LayeredGet) {
    std::optional<IniFile> base = IniFile::Parse("[S]\nA=1\nB=2\n");
    std::optional<IniFile> over = IniFile::Parse("[S]\nB=20\nC=30\n");
    CHECK(LayeredGet(base, over, "S", "A") == std::optional<std::string>("1"));
    CHECK(LayeredGet(base, over, "S", "B") == std::optional<std::string>("20"));
    CHECK(LayeredGet(base, over, "S", "C") == std::optional<std::string>("30"));
    CHECK(!LayeredGet(base, over, "S", "D").has_value());
    CHECK(LayeredGet(std::nullopt, over, "S", "B") == std::optional<std::string>("20"));
    CHECK(LayeredGet(base, std::nullopt, "S", "B") == std::optional<std::string>("2"));
    CHECK(!LayeredGet(std::nullopt, std::nullopt, "S", "A").has_value());
}

TEST(Ini_ToInt) {
    CHECK(ToInt(std::string("42")) == std::optional<long long>(42));
    CHECK(ToInt(std::string("  7  ")) == std::optional<long long>(7));
    CHECK(ToInt(std::string("\t-3 ")) == std::optional<long long>(-3));
    CHECK(ToInt(std::string("+5")) == std::optional<long long>(5));
    CHECK(ToInt(std::string("0")) == std::optional<long long>(0));
    CHECK(!ToInt(std::string("abc")).has_value());
    CHECK(!ToInt(std::string("12abc")).has_value());
    CHECK(!ToInt(std::string("1 2")).has_value());
    CHECK(!ToInt(std::string("1.5")).has_value());
    CHECK(!ToInt(std::string("")).has_value());
    CHECK(!ToInt(std::string("   ")).has_value());
    CHECK(!ToInt(std::string("-")).has_value());
    CHECK(!ToInt(std::string("+-5")).has_value());
    CHECK(!ToInt(std::string("--5")).has_value());
    CHECK(!ToInt(std::string("99999999999999999999999")).has_value());
    CHECK(!ToInt(std::nullopt).has_value());
}

// ---------------------------------------------------------------- Hotkey

TEST(Hotkey_CtrlF10) {
    Hotkey h{};
    h.ctrl = false;
    REQUIRE(ParseHotkey("ctrl+f10", &h));
    CHECK_EQ(h.vk, 0x79u);
    CHECK(h.ctrl);
    CHECK(!h.shift);
    CHECK(!h.alt);
}

TEST(Hotkey_CtrlShiftLetterMixedCase) {
    Hotkey h;
    REQUIRE(ParseHotkey("Ctrl+Shift+G", &h));
    CHECK_EQ(h.vk, static_cast<unsigned>('G'));
    CHECK(h.ctrl);
    CHECK(h.shift);
    CHECK(!h.alt);
}

TEST(Hotkey_AltDigit) {
    Hotkey h;
    REQUIRE(ParseHotkey("alt+5", &h));
    CHECK_EQ(h.vk, static_cast<unsigned>('5'));
    CHECK(!h.ctrl);
    CHECK(!h.shift);
    CHECK(h.alt);
}

TEST(Hotkey_BareFunctionKeys) {
    Hotkey h;
    REQUIRE(ParseHotkey("f9", &h));
    CHECK_EQ(h.vk, 0x78u);
    CHECK(!h.ctrl);
    CHECK(!h.shift);
    CHECK(!h.alt);
    REQUIRE(ParseHotkey("F1", &h));
    CHECK_EQ(h.vk, 0x70u);
    REQUIRE(ParseHotkey("F24", &h));
    CHECK_EQ(h.vk, 0x87u);
    REQUIRE(ParseHotkey("z", &h));
    CHECK_EQ(h.vk, static_cast<unsigned>('Z'));
    REQUIRE(ParseHotkey("0", &h));
    CHECK_EQ(h.vk, static_cast<unsigned>('0'));
    REQUIRE(ParseHotkey("shift+ctrl+alt+x", &h));
    CHECK(h.ctrl && h.shift && h.alt);
    CHECK_EQ(h.vk, static_cast<unsigned>('X'));
}

TEST(Hotkey_InvalidLeavesOutputUntouched) {
    const char* bad[] = {"ctrl+", "hyper+x", "", "+", "f0", "f25", "ctrl+shift", "ctrl + f10",
                         "ab", "10", "ctrl++x", "x+ctrl", "ctrl+f10+", "é"};
    for (const char* text : bad) {
        Hotkey h;
        h.vk = 0x41;
        h.ctrl = false;
        h.shift = true;
        h.alt = true;
        const bool ok = ParseHotkey(text, &h);
        if (ok) std::printf("  unexpectedly accepted '%s'\n", text);
        CHECK(!ok);
        CHECK_EQ(h.vk, 0x41u);
        CHECK(!h.ctrl);
        CHECK(h.shift);
        CHECK(h.alt);
    }
}

// ---------------------------------------------------------------- Config

TEST(Config_DefaultsWhenEmpty) {
    Config c = ParseConfig(IniFile::Parse(""));
    CHECK(c.enabled);
    CHECK(c.start_with_fg);
    CHECK_EQ(c.hotkey.vk, 0x79u);
    CHECK(c.hotkey.ctrl);
    CHECK(!c.hotkey.shift);
    CHECK(!c.hotkey.alt);
    CHECK_EQ(c.max_frame_latency, 0u);
    CHECK(c.log_level == LogLevel::Info);
    CHECK(c.warnings.empty());
}

TEST(Config_EveryKey) {
    Config c = ParseConfig(IniFile::Parse(
        "[Bridge]\nenabled=0\nstart_with_fg=0\nhotkey=alt+5\nmax_frame_latency=3\nlog_level=DEBUG\n"));
    CHECK(!c.enabled);
    CHECK(!c.start_with_fg);
    CHECK_EQ(c.hotkey.vk, static_cast<unsigned>('5'));
    CHECK(c.hotkey.alt);
    CHECK(!c.hotkey.ctrl);
    CHECK_EQ(c.max_frame_latency, 3u);
    CHECK(c.log_level == LogLevel::Debug);
    CHECK(c.warnings.empty());

    c = ParseConfig(IniFile::Parse("[bridge]\nenabled=1\nstart_with_fg=1\nlog_level=warn\nmax_frame_latency=16\n"));
    CHECK(c.enabled);
    CHECK(c.start_with_fg);
    CHECK(c.log_level == LogLevel::Warn);
    CHECK_EQ(c.max_frame_latency, 16u);
    CHECK(c.warnings.empty());

    c = ParseConfig(IniFile::Parse("[bridge]\nlog_level=error\nmax_frame_latency=1\n"));
    CHECK(c.log_level == LogLevel::Error);
    CHECK_EQ(c.max_frame_latency, 1u);
    c = ParseConfig(IniFile::Parse("[bridge]\nlog_level=info\nmax_frame_latency=0\n"));
    CHECK(c.log_level == LogLevel::Info);
    CHECK_EQ(c.max_frame_latency, 0u);
    CHECK(c.warnings.empty());
}

TEST(Config_KeysOutsideBridgeSectionAreIgnored) {
    Config c = ParseConfig(IniFile::Parse("[other]\nenabled=0\n"));
    CHECK(c.enabled);
}

TEST(Config_InvalidValuesKeepDefaultsAndWarn) {
    Config c = ParseConfig(IniFile::Parse(
        "[bridge]\nenabled=2\nstart_with_fg=yes\nhotkey=hyper+x\nmax_frame_latency=17\nlog_level=verbose\n"));
    CHECK(c.enabled);
    CHECK(c.start_with_fg);
    CHECK_EQ(c.hotkey.vk, 0x79u);
    CHECK(c.hotkey.ctrl);
    CHECK_EQ(c.max_frame_latency, 0u);
    CHECK(c.log_level == LogLevel::Info);
    REQUIRE(c.warnings.size() == 5);
    const char* keys[] = {"enabled", "start_with_fg", "hotkey", "max_frame_latency", "log_level"};
    for (const char* k : keys) {
        bool found = false;
        for (const auto& w : c.warnings) found = found || w.find(k) != std::string::npos;
        if (!found) std::printf("  no warning mentions %s\n", k);
        CHECK(found);
    }
}

TEST(Config_MaxFrameLatencyRange) {
    CHECK_EQ(ParseConfig(IniFile::Parse("[bridge]\nmax_frame_latency=-1\n")).warnings.size(), 1u);
    CHECK_EQ(ParseConfig(IniFile::Parse("[bridge]\nmax_frame_latency=abc\n")).warnings.size(), 1u);
    Config c = ParseConfig(IniFile::Parse("[bridge]\nmax_frame_latency=\n"));
    CHECK_EQ(c.max_frame_latency, 0u);
    CHECK(c.warnings.empty());
}

// The M3 keys (camera A/B switches, the DLSS-G-unsupported policy, the video
// memory guard's headroom).
TEST(Config_M3KeyDefaults) {
    Config c = ParseConfig(IniFile::Parse(""));
    CHECK(!c.camera_flip_handedness);
    CHECK(!c.camera_negate_side);
    CHECK(!c.proxy_without_fg);
    CHECK(!c.tag_without_fg);
    CHECK_EQ(c.fg_vram_headroom_mib, 0u);
    CHECK(c.warnings.empty());
}

TEST(Config_M3KeysParsed) {
    Config c = ParseConfig(IniFile::Parse("[bridge]\ncamera_flip_handedness=1\ncamera_negate_side=1\n"
                                          "proxy_without_fg=1\ntag_without_fg=1\nfg_vram_headroom_mib=0\n"));
    CHECK(c.camera_flip_handedness);
    CHECK(c.camera_negate_side);
    CHECK(c.proxy_without_fg);
    CHECK(c.tag_without_fg);
    CHECK_EQ(c.fg_vram_headroom_mib, 0u);
    CHECK(c.warnings.empty());
    c = ParseConfig(IniFile::Parse("[bridge]\ncamera_flip_handedness=0\nproxy_without_fg=0\nfg_vram_headroom_mib=2048\n"));
    CHECK(!c.camera_flip_handedness);
    CHECK(!c.proxy_without_fg);
    CHECK_EQ(c.fg_vram_headroom_mib, 2048u);
    CHECK(c.warnings.empty());
}

// Windows 11 25H2 binds VERSION.dll to System32 before acs.exe's own import;
// spoof_load_any=1 lets the bridge load a version.dll that is not the pinned
// dlssg_for_sm86 release.
TEST(Config_SpoofLoadAny) {
    CHECK(!ParseConfig(IniFile::Parse("")).spoof_load_any);
    CHECK(ParseConfig(IniFile::Parse("[bridge]\nspoof_load_any=1\n")).spoof_load_any);
    Config c = ParseConfig(IniFile::Parse("[bridge]\nspoof_load_any=yes\n"));
    CHECK(!c.spoof_load_any);
    REQUIRE(c.warnings.size() == 1);
    CHECK(c.warnings[0].find("spoof_load_any") != std::string::npos);
}

// Multi frame generation (spec 3, 6.8): fg_multiplier=2|3|4, 2X by default.
// The key asks for a multiplier; Streamline's numFramesToGenerateMax and the
// video memory guard decide what DLSS-G gets (fg_policy.h).
TEST(Config_FgMultiplierDefaultsTo2X) {
    const Config c = ParseConfig(IniFile::Parse(""));
    CHECK_EQ(c.fg_multiplier, 2u);
    CHECK(c.warnings.empty());
}

TEST(Config_FgMultiplierAccepts2To4) {
    for (unsigned m : {2u, 3u, 4u}) {
        const Config c = ParseConfig(IniFile::Parse("[bridge]\nfg_multiplier=" + std::to_string(m) + "\n"));
        CHECK_EQ(c.fg_multiplier, m);
        CHECK(c.warnings.empty());
    }
    // Spaces around the number, as for the other number keys.
    CHECK_EQ(ParseConfig(IniFile::Parse("[bridge]\nfg_multiplier = 3 \n")).fg_multiplier, 3u);
}

TEST(Config_FgMultiplierInvalidKeeps2XAndWarns) {
    for (const char* v : {"1", "5", "0", "-3", "6", "4x", "x", "3.0", "on"}) {
        const Config c = ParseConfig(IniFile::Parse(std::string("[bridge]\nfg_multiplier=") + v + "\n"));
        CHECK_EQ(c.fg_multiplier, 2u);
        REQUIRE(c.warnings.size() == 1);
        CHECK(c.warnings[0].find("fg_multiplier: invalid value '") != std::string::npos);
        CHECK(c.warnings[0].find("(expected 2, 3 or 4); using 2") != std::string::npos);
    }
    // An empty value keeps the default quietly, like the other number keys.
    const Config c = ParseConfig(IniFile::Parse("[bridge]\nfg_multiplier=\n"));
    CHECK_EQ(c.fg_multiplier, 2u);
    CHECK(c.warnings.empty());
}

TEST(Config_M3InvalidValuesKeepDefaultsAndWarn) {
    Config c = ParseConfig(IniFile::Parse("[bridge]\ncamera_flip_handedness=2\ncamera_negate_side=on\n"
                                          "proxy_without_fg=-1\ntag_without_fg=x\nfg_vram_headroom_mib=-5\n"));
    CHECK(!c.camera_flip_handedness);
    CHECK(!c.camera_negate_side);
    CHECK(!c.proxy_without_fg);
    CHECK(!c.tag_without_fg);
    CHECK_EQ(c.fg_vram_headroom_mib, 0u);
    REQUIRE(c.warnings.size() == 5);
    const char* keys[] = {"camera_flip_handedness", "camera_negate_side", "proxy_without_fg", "tag_without_fg",
                          "fg_vram_headroom_mib"};
    for (const char* k : keys) {
        bool found = false;
        for (const auto& w : c.warnings) found = found || w.find(k) != std::string::npos;
        if (!found) std::printf("  no warning mentions %s\n", k);
        CHECK(found);
    }
    // Above 64 GiB is refused as a typo; an empty value keeps the default quietly.
    c = ParseConfig(IniFile::Parse("[bridge]\nfg_vram_headroom_mib=65537\n"));
    CHECK_EQ(c.fg_vram_headroom_mib, 0u);
    CHECK_EQ(c.warnings.size(), 1u);
    c = ParseConfig(IniFile::Parse("[bridge]\nfg_vram_headroom_mib=\n"));
    CHECK_EQ(c.fg_vram_headroom_mib, 0u);
    CHECK(c.warnings.empty());
}

TEST(Config_LoadMissingFileGivesDefaults) {
    TempDir dir(L"cfg");
    Config c = LoadConfig(dir.Str() + L"\\ac-dlssg.ini");
    CHECK(c.enabled);
    CHECK(c.warnings.empty());
    auto p = dir.Write(L"ac-dlssg.ini", "[bridge]\r\nenabled=0 ; off\r\nhotkey=f9\r\n");
    c = LoadConfig(p.wstring());
    CHECK(!c.enabled);
    CHECK_EQ(c.hotkey.vk, 0x78u);
    CHECK(!c.hotkey.ctrl);
}

// ---------------------------------------------------------------- Log

namespace {
std::vector<std::string> Lines(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) out.push_back(line);
    return out;
}
}  // namespace

TEST(Log_OpenCreatesDirsAndTruncates) {
    TempDir dir(L"log");
    const auto path = dir.Path() / L"a" / L"b" / L"bridge.log";
    REQUIRE(LogOpen(path.wstring(), LogLevel::Info));
    CHECK(std::filesystem::exists(path));
    LOGI("first");
    LogClose();
    CHECK(ReadAll(path).find("first") != std::string::npos);

    REQUIRE(LogOpen(path.wstring(), LogLevel::Info));
    CHECK(ReadAll(path).empty());
    LogClose();
}

TEST(Log_LineFormat) {
    TempDir dir(L"log");
    const auto path = dir.Path() / L"bridge.log";
    REQUIRE(LogOpen(path.wstring(), LogLevel::Debug));
    LOGE("err %d", 1);
    LOGW("warn %s", "two");
    LOGI("info %u", 3u);
    LOGD("debug %.1f", 4.5);
    LogClose();
    const auto lines = Lines(ReadAll(path));
    REQUIRE(lines.size() == 4);
    const std::regex re(R"(^\d\d:\d\d:\d\d\.\d\d\d \[\d+\] (ERROR|WARN|INFO|DEBUG) .*$)");
    for (const auto& l : lines) {
        if (!std::regex_match(l, re)) std::printf("  bad line: %s\n", l.c_str());
        CHECK(std::regex_match(l, re));
    }
    const std::string tid = "[" + std::to_string(GetCurrentThreadId()) + "]";
    CHECK(lines[0].find(tid + " ERROR err 1") != std::string::npos);
    CHECK(lines[1].find(tid + " WARN warn two") != std::string::npos);
    CHECK(lines[2].find(tid + " INFO info 3") != std::string::npos);
    CHECK(lines[3].find(tid + " DEBUG debug 4.5") != std::string::npos);
    CHECK(ReadAll(path).find("\r") == std::string::npos);
}

TEST(Log_LevelFilteringAndClosedNoop) {
    TempDir dir(L"log");
    const auto path = dir.Path() / L"bridge.log";
    REQUIRE(LogOpen(path.wstring(), LogLevel::Warn));
    CHECK(LogGetLevel() == LogLevel::Warn);
    LOGI("hidden-info");
    LOGD("hidden-debug");
    LOGW("shown-warn");
    LOGE("shown-error");
    LogSetLevel(LogLevel::Debug);
    CHECK(LogGetLevel() == LogLevel::Debug);
    LOGD("shown-debug");
    LogClose();
    LOGE("after-close");
    const auto text = ReadAll(path);
    CHECK(text.find("hidden") == std::string::npos);
    CHECK(text.find("shown-warn") != std::string::npos);
    CHECK(text.find("shown-error") != std::string::npos);
    CHECK(text.find("shown-debug") != std::string::npos);
    CHECK(text.find("after-close") == std::string::npos);
    LogSetLevel(LogLevel::Info);
}

TEST(Log_ReopenMovesToNewPathAndLongLines) {
    TempDir dir(L"log");
    const auto p1 = dir.Path() / L"one.log";
    const auto p2 = dir.Path() / L"two.log";
    REQUIRE(LogOpen(p1.wstring(), LogLevel::Info));
    LOGI("to-one");
    REQUIRE(LogOpen(p2.wstring(), LogLevel::Info));
    const std::string big(5000, 'x');
    LOGI("to-two %s end", big.c_str());
    LogClose();
    const auto t1 = ReadAll(p1);
    const auto t2 = ReadAll(p2);
    CHECK(t1.find("to-one") != std::string::npos);
    CHECK(t1.find("to-two") == std::string::npos);
    CHECK(t2.find("to-two " + big + " end\n") != std::string::npos);
}

TEST(Log_ThreadsDoNotInterleave) {
    TempDir dir(L"log");
    const auto path = dir.Path() / L"bridge.log";
    REQUIRE(LogOpen(path.wstring(), LogLevel::Info));
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < 200; ++i) LOGI("thread %d line %d payload-payload-payload", t, i);
        });
    }
    for (auto& th : threads) th.join();
    LogClose();
    const auto lines = Lines(ReadAll(path));
    CHECK_EQ(lines.size(), 800u);
    const std::regex re(R"(^\d\d:\d\d:\d\d\.\d\d\d \[\d+\] INFO thread \d line \d+ payload-payload-payload$)");
    size_t bad = 0;
    for (const auto& l : lines) bad += std::regex_match(l, re) ? 0 : 1;
    CHECK_EQ(bad, 0u);
}
