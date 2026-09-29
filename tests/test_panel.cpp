// The in-game panel (spec 6.9): the status and control layouts and their Lua
// mirrors in apps/lua/AcDlssg/AcDlssg.lua, the two seqlock channels, and the
// pure rules for applying a panel request.
#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "fg_policy.h"
#include "lua_source.h"
#include "panel_control.h"
#include "panel_status.h"
#include "test_framework.h"

using namespace acdb;

namespace {

// One row per field of a Lua layout string, in order: the C type as the Lua
// ffi sees it, the element count (arrays) and the C++ offset.
struct FieldSpec {
    const char* name;
    const char* type;
    unsigned count;
    size_t offset;
};

#define STATUS_FIELD(type, name, count) {#name, type, count, offsetof(StatusLayout, name)}
const FieldSpec kStatusFields[] = {
    STATUS_FIELD("uint32_t", magic, 1),
    STATUS_FIELD("uint32_t", version, 1),
    STATUS_FIELD("uint32_t", seq, 1),
    STATUS_FIELD("uint32_t", heartbeat, 1),
    STATUS_FIELD("uint32_t", ownerPid, 1),
    STATUS_FIELD("uint32_t", bridgeState, 1),
    STATUS_FIELD("uint32_t", mode, 1),
    STATUS_FIELD("uint32_t", fgOn, 1),
    STATUS_FIELD("uint32_t", fgUserOn, 1),
    STATUS_FIELD("uint32_t", fgPaused, 1),
    STATUS_FIELD("uint32_t", spoofLoaded, 1),
    STATUS_FIELD("uint32_t", rtx30, 1),
    STATUS_FIELD("uint32_t", vsyncNote, 1),
    STATUS_FIELD("uint32_t", driverWarning, 1),
    STATUS_FIELD("uint32_t", cameraFlipHandedness, 1),
    STATUS_FIELD("uint32_t", cameraNegateSide, 1),
    STATUS_FIELD("uint32_t", startWithFg, 1),
    STATUS_FIELD("uint32_t", controlApplied, 1),
    STATUS_FIELD("uint32_t", saveCounter, 1),
    STATUS_FIELD("uint32_t", saveOk, 1),
    STATUS_FIELD("float", baseFps, 1),
    STATUS_FIELD("float", presentedFps, 1),
    STATUS_FIELD("float", bridgeGpuMs, 1),
    STATUS_FIELD("uint32_t", vramUsageMib, 1),
    STATUS_FIELD("uint32_t", vramBudgetMib, 1),
    STATUS_FIELD("float", capturesPerSec, 1),
    STATUS_FIELD("float", cameraFreshPerSec, 1),
    STATUS_FIELD("float", taggedPerSec, 1),
    STATUS_FIELD("char", reason, 160),
    STATUS_FIELD("char", stateReason, 256),
    STATUS_FIELD("char", warning, 160),
    STATUS_FIELD("char", gpuName, 64),
    STATUS_FIELD("char", hotkey, 32),
    STATUS_FIELD("char", bridgeVersion, 32),
    // Appended for the multiplier selector and the notes: the fields above
    // keep their offsets, so a window and a bridge of different builds still
    // agree on them (the new ones read as zeros from an older bridge).
    STATUS_FIELD("uint32_t", fgMultRequested, 1),
    STATUS_FIELD("uint32_t", fgMultUsed, 1),
    STATUS_FIELD("uint32_t", fgMultMax, 1),
    STATUS_FIELD("char", fgMultNote, 160),
    STATUS_FIELD("char", vramNote, 160),
    STATUS_FIELD("char", restartNote, 160),
    // Appended after those: a setting the bridge fixed by itself (spec 6.11).
    STATUS_FIELD("char", autoFixNote, 384),
};
#undef STATUS_FIELD

#define CONTROL_FIELD(type, name, count) {#name, type, count, offsetof(ControlLayout, name)}
const FieldSpec kControlFields[] = {
    CONTROL_FIELD("uint32_t", magic, 1),
    CONTROL_FIELD("uint32_t", version, 1),
    CONTROL_FIELD("uint32_t", seq, 1),
    CONTROL_FIELD("uint32_t", requestCounter, 1),
    CONTROL_FIELD("uint32_t", fgEnabled, 1),
    CONTROL_FIELD("uint32_t", cameraFlipHandedness, 1),
    CONTROL_FIELD("uint32_t", cameraNegateSide, 1),
    CONTROL_FIELD("uint32_t", saveAsDefault, 1),
    CONTROL_FIELD("uint32_t", desiredMultiplier, 1),  // appended: an older window leaves it 0 (no change)
};
#undef CONTROL_FIELD

size_t ScalarSize(const std::string& type) {
    if (type == "uint32_t" || type == "float") return 4;
    if (type == "double") return 8;
    if (type == "char") return 1;
    return 0;
}

size_t AlignUp(size_t v, size_t a) { return (v + a - 1) / a * a; }

// The layout natural C alignment gives the table (LuaJIT's ffi lays a struct
// body out the same way) must be the C++ struct's.
template <size_t N>
void CheckNaturalLayout(const FieldSpec (&fields)[N], size_t structSize, size_t structAlign) {
    size_t offset = 0;
    size_t maxAlign = 1;
    for (const auto& f : fields) {
        const size_t size = ScalarSize(f.type);
        REQUIRE(size != 0);
        offset = AlignUp(offset, size);
        if (offset != f.offset) std::printf("  field %s: expected offset %zu, struct has %zu\n", f.name, offset, f.offset);
        CHECK_EQ(offset, f.offset);
        offset += size * f.count;
        if (size > maxAlign) maxAlign = size;
    }
    CHECK_EQ(AlignUp(offset, maxAlign), structSize);
    CHECK_EQ(maxAlign, structAlign);
}

template <size_t N>
void CheckLuaLayout(const std::string& lua, const char* var, const FieldSpec (&fields)[N]) {
    const std::string body = acdb_test::LuaLayoutBody(lua, var);
    if (body.empty()) std::printf("  no 'local %s = [[ ... ]]' in AcDlssg.lua\n", var);
    REQUIRE(!body.empty());
    const std::vector<acdb_test::LuaDecl> decls = acdb_test::ParseLuaDecls(body);
    CHECK_EQ(decls.size(), N);
    for (size_t i = 0; i < decls.size() && i < N; ++i) {
        const bool same =
            decls[i].type == fields[i].type && decls[i].name == fields[i].name && decls[i].count == fields[i].count;
        if (!same)
            std::printf("  %s field %zu: Lua has '%s %s[%u]', C++ has '%s %s[%u]'\n", var, i, decls[i].type.c_str(),
                        decls[i].name.c_str(), decls[i].count, fields[i].type, fields[i].name, fields[i].count);
        CHECK(same);
    }
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(static_cast<char>(c));
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// Layouts

static_assert(kStatusMagic == 0x54534C44u);   // 'DLST'
static_assert(kControlMagic == 0x43534C44u);  // 'DLSC'
static_assert(kStatusVersion == 1u && kControlVersion == 1u);
static_assert(kPanelNotLoaded == 0u && kPanelPassThrough == 1u && kPanelProxyNoFg == 2u && kPanelFgAvailable == 3u);
static_assert(kPanelModeUnknown == 0u && kPanelModeReShade == 1u && kPanelModeStandalone == 2u);

TEST(PanelLayout_StatusOffsetsFollowNaturalAlignmentOfTheLuaTypes) {
    CheckNaturalLayout(kStatusFields, sizeof(StatusLayout), alignof(StatusLayout));
}

static_assert(sizeof(StatusLayout) <= kPanelSectionSize && sizeof(ControlLayout) <= kPanelSectionSize);

TEST(PanelLayout_ControlOffsetsFollowNaturalAlignmentOfTheLuaTypes) {
    CheckNaturalLayout(kControlFields, sizeof(ControlLayout), alignof(ControlLayout));
}

TEST(PanelLayout_MagicsReadDlstAndDlscInMemory) {
    char tag[5] = {};
    std::memcpy(tag, &kStatusMagic, 4);
    CHECK(std::strcmp(tag, "DLST") == 0);
    std::memcpy(tag, &kControlMagic, 4);
    CHECK(std::strcmp(tag, "DLSC") == 0);
}

// ---------------------------------------------------------------------------
// The Lua app's side

TEST(LuaApp_StatusLayoutStringMatchesTheStructFieldByField) {
    const std::string lua = acdb_test::ReadLuaAppFile("AcDlssg.lua");
    REQUIRE(!lua.empty());
    CheckLuaLayout(lua, "STATUS_LAYOUT", kStatusFields);
}

TEST(LuaApp_ControlLayoutStringMatchesTheStructFieldByField) {
    const std::string lua = acdb_test::ReadLuaAppFile("AcDlssg.lua");
    REQUIRE(!lua.empty());
    CheckLuaLayout(lua, "CONTROL_LAYOUT", kControlFields);
}

TEST(LuaApp_PanelConstantsAndSectionsMatchTheCppSide) {
    const std::string lua = acdb_test::ReadLuaAppFile("AcDlssg.lua");
    REQUIRE(!lua.empty());
    CHECK_EQ(acdb_test::LuaNumber(lua, "STATUS_MAGIC"), static_cast<long long>(kStatusMagic));
    CHECK_EQ(acdb_test::LuaNumber(lua, "STATUS_VERSION"), static_cast<long long>(kStatusVersion));
    CHECK_EQ(acdb_test::LuaNumber(lua, "CONTROL_MAGIC"), static_cast<long long>(kControlMagic));
    CHECK_EQ(acdb_test::LuaNumber(lua, "CONTROL_VERSION"), static_cast<long long>(kControlVersion));
    CHECK_EQ(acdb_test::LuaNumber(lua, "STATE_NOT_LOADED"), static_cast<long long>(kPanelNotLoaded));
    CHECK_EQ(acdb_test::LuaNumber(lua, "STATE_PASS_THROUGH"), static_cast<long long>(kPanelPassThrough));
    CHECK_EQ(acdb_test::LuaNumber(lua, "STATE_PROXY_NO_FG"), static_cast<long long>(kPanelProxyNoFg));
    CHECK_EQ(acdb_test::LuaNumber(lua, "STATE_FG_AVAILABLE"), static_cast<long long>(kPanelFgAvailable));
    CHECK_EQ(acdb_test::LuaNumber(lua, "MODE_RESHADE"), static_cast<long long>(kPanelModeReShade));
    CHECK_EQ(acdb_test::LuaNumber(lua, "MODE_STANDALONE"), static_cast<long long>(kPanelModeStandalone));
    // CSP's memory-mapped-file functions take the name without "Local\".
    CHECK("Local\\" + acdb_test::LuaString(lua, "STATUS_SECTION") == Narrow(kStatusSectionName));
    CHECK("Local\\" + acdb_test::LuaString(lua, "CONTROL_SECTION") == Narrow(kControlSectionName));
    // The status is only read; the control section is written, and both stay mapped.
    CHECK(lua.find("ac.readMemoryMappedFile(STATUS_SECTION, STATUS_LAYOUT, true)") != std::string::npos);
    CHECK(lua.find("ac.writeMemoryMappedFile(CONTROL_SECTION, CONTROL_LAYOUT, true)") != std::string::npos);
    // A reloaded app continues the request counter it finds, so that its
    // next request is never one the bridge has already applied.
    CHECK(lua.find("requestCounter = ctl.requestCounter") != std::string::npos);
}

// sendRequest() must keep the order the C++ reader relies on and that
// LuaStyleRequest below reproduces: seq odd, barrier, magic and version, the
// desired values, the counter, barrier, seq even.
TEST(LuaApp_SendRequestFollowsTheSeqlockWriterOrder) {
    const std::string lua = acdb_test::ReadLuaAppFile("AcDlssg.lua");
    REQUIRE(!lua.empty());
    const std::string body = acdb_test::LuaFunctionBody(lua, "sendRequest");
    REQUIRE(!body.empty());
    const char* steps[] = {
        "local s = bit.band(bit.bor(ctl.seq, 1), 0x7FFFFFFF)",
        "ctl.seq = s",
        "memoryBarrier()",
        "ctl.magic = CONTROL_MAGIC",
        "ctl.version = CONTROL_VERSION",
        "ctl.fgEnabled = ",
        "ctl.cameraFlipHandedness = ",
        "ctl.cameraNegateSide = ",
        "ctl.saveAsDefault = ",
        "ctl.desiredMultiplier = ",
        "ctl.requestCounter = requestCounter",
        "memoryBarrier()",
        "ctl.seq = s + 1",
    };
    size_t at = 0;
    for (const char* step : steps) {
        const size_t found = body.find(step, at);
        if (found == std::string::npos) std::printf("  sendRequest(): '%s' missing or out of order\n", step);
        CHECK(found != std::string::npos);
        if (found != std::string::npos) at = found + std::strlen(step);
    }
}

// readStatus() is the seqlock reader: an odd or changed seq discards the copy.
TEST(LuaApp_ReadStatusFollowsTheSeqlockReaderOrder) {
    const std::string lua = acdb_test::ReadLuaAppFile("AcDlssg.lua");
    REQUIRE(!lua.empty());
    const std::string body = acdb_test::LuaFunctionBody(lua, "readStatus");
    REQUIRE(!body.empty());
    const char* steps[] = {
        "local s1 = st.seq",
        "if s1 == statusSeq then return false end",
        "if bit.band(s1, 1) ~= 0 then return false end",
        "memoryBarrier()",
        "if st.magic ~= STATUS_MAGIC or st.version ~= STATUS_VERSION then",
        "memoryBarrier()",
        "if st.seq ~= s1 then return false end",
        "statusSeq = s1",
    };
    size_t at = 0;
    for (const char* step : steps) {
        const size_t found = body.find(step, at);
        if (found == std::string::npos) std::printf("  readStatus(): '%s' missing or out of order\n", step);
        CHECK(found != std::string::npos);
        if (found != std::string::npos) at = found + std::strlen(step);
    }
}

// readStatus() copies every field the window uses into its table, and
// newStatus() gives each a default: a field added to the layout and never
// copied would stay at its default in the window.
TEST(LuaApp_ReadStatusCopiesEveryField) {
    const std::string lua = acdb_test::ReadLuaAppFile("AcDlssg.lua");
    REQUIRE(!lua.empty());
    const std::string body = acdb_test::LuaFunctionBody(lua, "readStatus");
    const std::string defaults = acdb_test::LuaFunctionBody(lua, "newStatus");
    REQUIRE(!body.empty());
    REQUIRE(!defaults.empty());
    for (const auto& f : kStatusFields) {
        const std::string name = f.name;
        if (name == "magic" || name == "version" || name == "seq" || name == "ownerPid") continue;
        const std::string copy = std::string("c.") + name + (f.count > 1 ? " = ffi.string(st." : " = st.") + name;
        if (body.find(copy) == std::string::npos) std::printf("  readStatus(): no '%s'\n", copy.c_str());
        CHECK(body.find(copy) != std::string::npos);
        const bool hasDefault = std::regex_search(defaults, std::regex("[{,\\s]" + name + " = "));
        if (!hasDefault) std::printf("  newStatus(): no default for %s\n", name.c_str());
        CHECK(hasDefault);
    }
}

// The window's parts for the notes and the multiplier (spec 6.9): the texts
// the owner asked for, the three buttons and the hover text of a disabled one.
TEST(LuaApp_WindowShowsTheNotesAndTheMultiplierButtons) {
    const std::string lua = acdb_test::ReadLuaAppFile("AcDlssg.lua");
    REQUIRE(!lua.empty());
    CHECK(acdb_test::LuaString(lua, "TEXT_RESTART") == "Restart the game to apply");
    CHECK(acdb_test::LuaString(lua, "TEXT_MULT_UNSUPPORTED") == "not supported by this GPU/driver");
    // Disabled buttons answer the hover test only with AllowWhenDisabled.
    CHECK(lua.find("ui.itemHovered(ui.HoveredFlags.AllowWhenDisabled)") != std::string::npos);
    CHECK(lua.find("ui.setTooltip(TEXT_MULT_UNSUPPORTED)") != std::string::npos);
    for (const char* label : {"'2X###fgMult2'", "'3X###fgMult3'", "'4X###fgMult4'"}) {
        if (lua.find(label) == std::string::npos) std::printf("  no button label %s\n", label);
        CHECK(lua.find(label) != std::string::npos);
    }
    const std::string window = lua.substr(lua.find("function script.windowMain(dt)"));
    CHECK(window.find("texts.vramNote") != std::string::npos);
    CHECK(window.find("texts.restartNote") != std::string::npos);
    CHECK(window.find("multiplierButtons(") != std::string::npos);
    const std::string buttons = acdb_test::LuaFunctionBody(lua, "multiplierButtons");
    REQUIRE(!buttons.empty());
    CHECK(buttons.find("texts.multNote") != std::string::npos);
    CHECK(buttons.find("MULT_LABELS[m]") != std::string::npos);
}

// A bridge whose status record has another version (a later build with a
// changed layout, the window of an older one): the window must say that the
// versions differ, not misread the record and not "Bridge not running".
TEST(LuaApp_WindowSaysWhenBridgeAndWindowVersionsDiffer) {
    const std::string lua = acdb_test::ReadLuaAppFile("AcDlssg.lua");
    REQUIRE(!lua.empty());
    CHECK(acdb_test::LuaString(lua, "TEXT_VERSIONS_DIFFER") == "Bridge and window versions differ");
    const std::string read = acdb_test::LuaFunctionBody(lua, "readStatus");
    const size_t check = read.find("if st.magic ~= STATUS_MAGIC or st.version ~= STATUS_VERSION then");
    REQUIRE(check != std::string::npos);
    // Only the version of a record with our magic counts; nothing else is read.
    const size_t other = read.find("c.otherVersion = st.magic == STATUS_MAGIC and st.version or 0", check);
    CHECK(other != std::string::npos);
    CHECK(other < read.find("else", check));
    CHECK(read.find("c.otherVersion = 0", read.find("else", check)) != std::string::npos);
    CHECK(std::regex_search(acdb_test::LuaFunctionBody(lua, "newStatus"), std::regex("[{,\\s]otherVersion = 0")));
    CHECK(acdb_test::LuaFunctionBody(lua, "bridgeProblem").find("return status.otherVersion ~= 0 and 3 or 1") !=
          std::string::npos);
    const std::string window = lua.substr(lua.find("function script.windowMain(dt)"));
    // The window's card for it: the title in the error colour, the hint below.
    CHECK(window.find("noteCard(cards.problem, TEXT_VERSIONS_DIFFER, texts.versions, COLOR_BAD)") != std::string::npos);
    CHECK(acdb_test::LuaFunctionBody(lua, "refreshStatus").find("texts.versions = string.format(TEXT_VERSIONS_HINT") !=
          std::string::npos);
}

// A fg_vram_headroom_mib number that kept frame generation off is switched
// to auto by the bridge (spec 6.11); the window says so at its top, above the
// restart note and the big switch, whenever the status has the note.
TEST(LuaApp_WindowShowsTheAutoFixNoteAtTheTop) {
    const std::string lua = acdb_test::ReadLuaAppFile("AcDlssg.lua");
    REQUIRE(!lua.empty());
    CHECK(acdb_test::LuaFunctionBody(lua, "rebuildTexts").find("texts.autoFixNote = s.autoFixNote") !=
          std::string::npos);
    const std::string window = lua.substr(lua.find("function script.windowMain(dt)"));
    const size_t note = window.find("if texts.autoFixNote ~= '' then");
    REQUIRE(note != std::string::npos);
    // A note card in the "on" colour.
    CHECK(window.find("noteCard(cards.autoFix, nil, texts.autoFixNote, COLOR_GOOD)", note) != std::string::npos);
    CHECK(note < window.find("texts.restartNote"));
    CHECK(note < window.find("fgSwitch(available)"));
    // The card wraps its text inside its padding.
    CHECK(acdb_test::LuaFunctionBody(lua, "noteCard").find("ui.textWrapped(text, wrapX)") != std::string::npos);
    // The window's texts table has it from the start.
    CHECK(std::regex_search(lua, std::regex("local texts = \\{[^}]*[{,\\s]autoFixNote = ''")));
}

// The note of an ac-dlssg.ini that could not be saved names the file's path
// in its reason; a Steam library path fits the status field whole.
TEST(PanelLayout_TheAutoFixNoteFitsAnUnsavedIniWithItsPath) {
    const std::string path = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\assettocorsa\\ac-dlssg\\ac-dlssg.ini";
    const std::string why = "creating " + path + ".new failed: error 5";
    const std::string note = VramHeadroomSwitchNote(65536, false, why);
    CHECK(note.size() < sizeof(StatusLayout::autoFixNote));
    StatusLayout s{};
    CopyText(s.autoFixNote, note);
    CHECK(TextOf(s.autoFixNote) == note);
}

// While the guard keeps DLSS-G off with auto, the status line already says
// "Off: not enough video memory: ..."; the note under the video memory line
// does not repeat it. A different note (tight, or a number's "not enough
// video memory ..." under its "video memory: need ..." reason) stays.
TEST(LuaApp_VramNoteDoesNotRepeatTheStatusLine) {
    const std::string lua = acdb_test::ReadLuaAppFile("AcDlssg.lua");
    REQUIRE(!lua.empty());
    CHECK(acdb_test::LuaFunctionBody(lua, "rebuildTexts").find("texts.vramNote = s.vramNote ~= s.reason and s.vramNote or ''") !=
          std::string::npos);
}

// "Save as default" shows "Saved" in place of its label only for a save the
// bridge confirmed. A click clears the result of the save before it: kept,
// a second save would show the first one's "Saved" at once, and a failure
// arriving during that flash would read "Saving failed" in the success
// colour on the button (or the button would go blank when the status in
// between cleared the text).
TEST(LuaApp_SaveButtonShowsOnlyTheResultOfTheLatestSave) {
    const std::string lua = acdb_test::ReadLuaAppFile("AcDlssg.lua");
    REQUIRE(!lua.empty());
    const std::string body = acdb_test::LuaFunctionBody(lua, "saveButton");
    REQUIRE(!body.empty());
    const size_t click = body.find("lastSaveRequest = requestCounter");
    REQUIRE(click != std::string::npos);
    const size_t clickEnd = body.find("\n  end", click);
    REQUIRE(clickEnd != std::string::npos);
    const std::string onClick = body.substr(click, clickEnd - click);
    for (const char* reset : {"texts.save = ''", "a.lastSaveText = ''", "a.savedAt = -100"}) {
        if (onClick.find(reset) == std::string::npos) std::printf("  saveButton(): the click does not do '%s'\n", reset);
        CHECK(onClick.find(reset) != std::string::npos);
    }
    // The flash is drawn only while the text is a success.
    const size_t flash = body.find("local flash = ");
    REQUIRE(flash != std::string::npos);
    const size_t guard = body.find("if texts.save == '' or status.saveOk == 0 then flash = 0 end", flash);
    CHECK(guard != std::string::npos);
    CHECK(guard < body.find("if flash > 0 then"));
}

// The opened "Details" is a child window whose height eases; the mouse wheel
// over it must still scroll the app's window. CSP's SDK says of
// NoScrollWithMouse: "On child window, mouse wheel will be forwarded to the
// parent unless NoScrollbar is also set", so the child has NoScrollWithMouse
// without NoScrollbar, and its scrollbar (there while the height is below the
// content's) is made zero wide around ui.beginChild instead.
TEST(LuaApp_TheDetailsChildForwardsTheMouseWheel) {
    const std::string lua = acdb_test::ReadLuaAppFile("AcDlssg.lua");
    REQUIRE(!lua.empty());
    std::smatch m;
    REQUIRE(std::regex_search(lua, m, std::regex(R"(childFlags = ([^\n]*))")));
    const std::string flags = m[1];
    CHECK(flags.find("ui.WindowFlags.NoScrollWithMouse") != std::string::npos);
    CHECK(flags.find("ui.WindowFlags.NoScrollbar") == std::string::npos);
    const std::string body = acdb_test::LuaFunctionBody(lua, "detailsSection");
    REQUIRE(!body.empty());
    const size_t push = body.find("ui.pushStyleVar(ui.StyleVar.ScrollbarSize, 0)");
    const size_t begin = body.find("ui.beginChild(ID.detailsChild, CHILD_SIZE, false, K.childFlags)");
    const size_t pop = body.find("ui.popStyleVar(1)");
    const size_t content = body.find("pcall(detailsContent)");
    CHECK(push != std::string::npos && begin != std::string::npos && pop != std::string::npos);
    CHECK(push < begin && begin < pop && pop < content);
}

// No Lua interpreter runs in these tests; a block that is never closed (or
// closed twice) is the easiest mistake to make in the app, so the block
// keywords are counted: every function, if and do has its end, every
// repeat its until.
TEST(LuaApp_BlockKeywordsBalance) {
    const std::string lua = acdb_test::ReadLuaAppFile("AcDlssg.lua");
    REQUIRE(!lua.empty());
    const std::string code = acdb_test::LuaCodeOnly(lua);
    const size_t opened = acdb_test::LuaKeywordCount(code, "function") + acdb_test::LuaKeywordCount(code, "if") +
                          acdb_test::LuaKeywordCount(code, "do");
    const size_t closed = acdb_test::LuaKeywordCount(code, "end");
    if (opened != closed) std::printf("  function+if+do = %zu, end = %zu\n", opened, closed);
    CHECK_EQ(opened, closed);
    CHECK_EQ(acdb_test::LuaKeywordCount(code, "repeat"), acdb_test::LuaKeywordCount(code, "until"));
    // The counting itself: comments and strings do not count.
    const std::string sample = "-- if x then\nlocal s = 'end' --[[ do ]]\nif a then b() end\nlocal t = [[function]]\n";
    const std::string sampleCode = acdb_test::LuaCodeOnly(sample);
    CHECK_EQ(acdb_test::LuaKeywordCount(sampleCode, "if"), 1u);
    CHECK_EQ(acdb_test::LuaKeywordCount(sampleCode, "end"), 1u);
    CHECK_EQ(acdb_test::LuaKeywordCount(sampleCode, "do"), 0u);
    CHECK_EQ(acdb_test::LuaKeywordCount(sampleCode, "function"), 0u);
}

namespace {

// The locals the main chunk declares at its top level: LuaJIT refuses a
// function with more than 200 active locals ("too many local variables"),
// and every top-level local of the app stays active to the end of the file.
size_t TopLevelLocals(const std::string& code) {
    // Names, numbers and single symbols; a field or method name (a.b, a:b)
    // becomes "#", since it is never a keyword.
    const std::regex token(R"([A-Za-z_][A-Za-z0-9_]*|[0-9][0-9A-Za-z_.]*|\S)");
    std::vector<std::string> words;
    for (auto it = std::sregex_iterator(code.begin(), code.end(), token); it != std::sregex_iterator(); ++it) {
        const size_t at = static_cast<size_t>(it->position());
        const char before = at > 0 ? code[at - 1] : ' ';
        words.push_back(before == '.' || before == ':' ? std::string("#") : it->str());
    }
    size_t count = 0;
    int depth = 0;
    for (size_t i = 0; i < words.size(); ++i) {
        const std::string& w = words[i];
        if (w == "function" || w == "if" || w == "do" || w == "repeat") {
            ++depth;
        } else if (w == "end" || w == "until") {
            --depth;
        } else if (w == "local" && depth == 0) {
            if (i + 1 < words.size() && words[i + 1] == "function") {
                ++count;
                continue;
            }
            // local a, b, c: each name, as long as a comma follows the one before.
            for (size_t j = i + 1; j < words.size(); j += 2) {
                ++count;
                if (j + 1 >= words.size() || words[j + 1] != ",") break;
            }
        }
    }
    return count;
}

}  // namespace

// The window's constants and animation state live in tables for this.
TEST(LuaApp_TheMainChunkStaysUnderLuaJitsLocalLimit) {
    const std::string lua = acdb_test::ReadLuaAppFile("AcDlssg.lua");
    REQUIRE(!lua.empty());
    const size_t locals = TopLevelLocals(acdb_test::LuaCodeOnly(lua));
    std::printf("  %zu top-level locals\n", locals);
    CHECK(locals > 100);  // the counting finds them
    CHECK(locals <= 185);  // LuaJIT's limit is 200; keep some room
    // The counting itself.
    CHECK_EQ(TopLevelLocals("local a, b = 1, 2\nlocal function f() local x end\nif a then local y end\nlocal t = {x = 1}\n"),
             4u);
    CHECK_EQ(TopLevelLocals("function s.w(dt) local q = 1 end\nlocal c\n"), 1u);
}

namespace {

// lib.lua of CSP's Lua SDK for apps (<game>\extension\internal\lua-sdk\ac_apps),
// or an empty path. The environment variable ACDB_CSP_LUA_SDK names the
// lua-sdk folder; else the game is looked for in Steam's default folder and in
// a SteamLibrary folder at the root of every fixed drive.
std::filesystem::path CspAppsLib() {
    namespace fs = std::filesystem;
    const fs::path rel = fs::path("extension") / "internal" / "lua-sdk" / "ac_apps" / "lib.lua";
    std::vector<fs::path> candidates;
    wchar_t env[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"ACDB_CSP_LUA_SDK", env, MAX_PATH);
    if (n > 0 && n < MAX_PATH) candidates.push_back(fs::path(env) / "ac_apps" / "lib.lua");
    candidates.push_back(fs::path(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\assettocorsa") / rel);
    const DWORD drives = GetLogicalDrives();
    for (int d = 2; d < 26; ++d) {  // C: to Z:
        if (!(drives & (1u << d))) continue;
        const std::wstring root = std::wstring(1, static_cast<wchar_t>(L'A' + d)) + L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) continue;
        candidates.push_back(fs::path(root) / "SteamLibrary" / "steamapps" / "common" / "assettocorsa" / rel);
    }
    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(c, ec)) return c;
    }
    return {};
}

bool IsIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string IdentAt(const std::string& s, size_t at) {
    size_t e = at;
    while (e < s.size() && IsIdentChar(s[e])) ++e;
    return s.substr(at, e - at);
}

// How many arguments a ui function takes: at most max (-1: any number, as
// with "...") and at least min, the parameters up to the last one whose
// ---@param type is neither optional ("type?") nor nil-able (0 when an
// ---@overload may take fewer).
struct Arity {
    int min = 0;
    int max = -1;
};

// The names in a parameter list "a, b, ..." (the text between the parentheses).
std::vector<std::string> ParamNames(const std::string& list) {
    std::vector<std::string> names;
    std::string cur;
    for (char c : list + ",") {
        if (c != ',') {
            cur.push_back(c);
            continue;
        }
        const size_t b = cur.find_first_not_of(" \t");
        if (b != std::string::npos) names.push_back(cur.substr(b, cur.find_last_not_of(" \t") - b + 1));
        cur.clear();
    }
    return names;
}

// The text between the '(' at open and the next ')', or empty.
std::string ParenList(const std::string& line, size_t open) {
    if (open == std::string::npos) return {};
    const size_t close = line.find(')', open);
    return close == std::string::npos ? std::string() : line.substr(open + 1, close - open - 1);
}

// The ---@param and ---@overload lines of the doc comment above a declaration.
struct DocComment {
    std::map<std::string, bool> optional;  // parameter name -> optional
    int overloadMax = -1;                  // the most parameters an overload takes
    bool overload = false;
};

Arity ArityOf(const std::vector<std::string>& params, const DocComment& doc) {
    Arity a;
    for (const auto& p : params)
        if (p == "...") return a;
    a.max = std::max(static_cast<int>(params.size()), doc.overloadMax);
    if (doc.overload) return a;
    for (size_t i = 0; i < params.size(); ++i) {
        const auto found = doc.optional.find(params[i]);
        if (found != doc.optional.end() && !found->second) a.min = static_cast<int>(i) + 1;
    }
    return a;
}

// What lib.lua declares under ui: functions ("function ui.name(" and
// "ui.name = function (") with their arity, and enums ("ui.Name = {" with one
// member per line).
struct SdkUi {
    std::set<std::string> functions;
    std::map<std::string, Arity> arity;
    std::map<std::string, std::set<std::string>> enums;
};

SdkUi ParseSdkUi(const std::string& lib) {
    static const std::regex param(R"(^---@param\s+([A-Za-z_][A-Za-z0-9_]*)\s+(\S+))");
    SdkUi out;
    std::istringstream in(lib);
    std::string line;
    std::string openEnum;
    DocComment doc;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("---", 0) == 0) {
            std::smatch m;
            if (std::regex_search(line, m, param)) {
                const std::string type = m[2];
                doc.optional[m[1]] = type.back() == '?' || type.find("nil") != std::string::npos;
            } else if (line.rfind("---@overload fun(", 0) == 0) {
                doc.overload = true;
                doc.overloadMax =
                    std::max(doc.overloadMax, static_cast<int>(ParamNames(ParenList(line, line.find('('))).size()));
            }
            continue;
        }
        // Any other line ends the doc comment; it belongs to this line only.
        const DocComment above = std::move(doc);
        doc = DocComment();
        if (!openEnum.empty()) {
            if (line.rfind('}', 0) == 0) {
                openEnum.clear();
                continue;
            }
            const size_t b = line.find_first_not_of(" \t");
            if (b != std::string::npos && line.compare(b, 2, "--") != 0) {
                const std::string member = IdentAt(line, b);
                if (!member.empty()) out.enums[openEnum].insert(member);
            }
            continue;
        }
        if (line.rfind("function ui.", 0) == 0) {
            const std::string name = IdentAt(line, 12);
            if (!name.empty() && line.size() > 12 + name.size() && line[12 + name.size()] == '(') {
                out.functions.insert(name);
                out.arity[name] = ArityOf(ParamNames(ParenList(line, 12 + name.size())), above);
            }
        } else if (line.rfind("ui.", 0) == 0) {
            const std::string name = IdentAt(line, 3);
            size_t at = line.find_first_not_of(' ', 3 + name.size());
            if (name.empty() || at == std::string::npos || line[at] != '=') continue;
            at = line.find_first_not_of(' ', at + 1);
            if (at != std::string::npos && line[at] == '{') {
                if (line.find('}', at) == std::string::npos) openEnum = name;
                out.enums[name];
            } else {
                out.functions.insert(name);
                if (line.compare(at, 8, "function") == 0)
                    out.arity[name] = ArityOf(ParamNames(ParenList(line, line.find('(', at))), above);
                else
                    out.arity[name] = Arity();
            }
        }
    }
    return out;
}

// Each call "ui.name(...)" in code (LuaCodeOnly with a string stand-in, so a
// string argument still counts) with the number of its arguments: the commas
// outside nested (), {} and [] plus one, or 0 for "()".
std::vector<std::pair<std::string, int>> UiCallArgCounts(const std::string& code) {
    static const std::regex call(R"((?:^|[^A-Za-z0-9_.:])ui\.([A-Za-z_][A-Za-z0-9_]*)\s*\()");
    std::vector<std::pair<std::string, int>> out;
    for (auto it = std::sregex_iterator(code.begin(), code.end(), call); it != std::sregex_iterator(); ++it) {
        size_t i = static_cast<size_t>(it->position() + it->length());  // after '('
        int depth = 0;
        int commas = 0;
        bool any = false;
        for (; i < code.size(); ++i) {
            const char c = code[i];
            if (c == '(' || c == '{' || c == '[') {
                ++depth;
            } else if (c == ')' || c == '}' || c == ']') {
                if (depth == 0) break;
                --depth;
            } else if (c == ',' && depth == 0) {
                ++commas;
            }
            if (!std::isspace(static_cast<unsigned char>(c))) any = true;
        }
        out.emplace_back((*it)[1], any ? commas + 1 : 0);
    }
    return out;
}

}  // namespace

// The app runs only inside CSP, so a ui function that CSP does not have
// fails only in game. Every ui.name( the app calls must be declared in the
// SDK's lib.lua of the installed CSP, and every ui.Enum.Member it uses must be
// a member there. Skips when no CSP SDK is found (see CspAppsLib).
TEST(LuaApp_EveryUiFunctionAndEnumTheAppUsesIsInTheCspSdk) {
    const std::filesystem::path libPath = CspAppsLib();
    if (libPath.empty()) {
        std::printf("  SKIP: CSP's lua-sdk\\ac_apps\\lib.lua not found (set ACDB_CSP_LUA_SDK to the lua-sdk folder)\n");
        return;
    }
    std::printf("  %s\n", libPath.string().c_str());
    const SdkUi sdk = ParseSdkUi(acdb_test::ReadAll(libPath));
    REQUIRE(sdk.functions.count("drawRectFilled") == 1 && sdk.functions.count("DWriteFont") == 1);
    REQUIRE(sdk.enums.count("StyleColor") == 1 && sdk.enums.at("StyleColor").count("CheckMark") == 1);

    const std::string lua = acdb_test::ReadLuaAppFile("AcDlssg.lua");
    REQUIRE(!lua.empty());
    const std::string code = acdb_test::LuaCodeOnly(lua);
    std::set<std::string> called;
    const std::regex call(R"((?:^|[^A-Za-z0-9_.:])ui\.([A-Za-z_][A-Za-z0-9_]*)\s*\()");
    for (auto it = std::sregex_iterator(code.begin(), code.end(), call); it != std::sregex_iterator(); ++it)
        called.insert((*it)[1]);
    CHECK(called.size() >= 20);  // the window draws with many
    for (const auto& name : called) {
        if (sdk.functions.count(name) == 0) std::printf("  ui.%s is not in the SDK\n", name.c_str());
        CHECK(sdk.functions.count(name) == 1);
    }
    const std::regex member(R"((?:^|[^A-Za-z0-9_.:])ui\.([A-Z][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*))");
    size_t members = 0;
    for (auto it = std::sregex_iterator(code.begin(), code.end(), member); it != std::sregex_iterator(); ++it) {
        ++members;
        const std::string e = (*it)[1];
        const std::string m = (*it)[2];
        const auto found = sdk.enums.find(e);
        const bool ok = found != sdk.enums.end() && found->second.count(m) == 1;
        if (!ok) std::printf("  ui.%s.%s is not in the SDK\n", e.c_str(), m.c_str());
        CHECK(ok);
    }
    CHECK(members >= 10);

    // Every call passes no more arguments than the function declares, and at
    // least its required ones: a colour, position or flag in the wrong place
    // usually shows up as one argument too many or too few.
    const std::vector<std::pair<std::string, int>> calls = UiCallArgCounts(acdb_test::LuaCodeOnly(lua, 's'));
    CHECK(calls.size() >= 60);
    for (const auto& [name, args] : calls) {
        const auto found = sdk.arity.find(name);
        if (found == sdk.arity.end()) continue;  // reported above
        const Arity& a = found->second;
        const bool ok = args >= a.min && (a.max < 0 || args <= a.max);
        if (!ok) std::printf("  ui.%s called with %d arguments; the SDK takes %d to %d\n", name.c_str(), args, a.min, a.max);
        CHECK(ok);
    }
    CHECK(sdk.arity.at("drawRectFilled").min == 3 && sdk.arity.at("drawRectFilled").max == 5);
    CHECK(sdk.arity.at("dwriteDrawTextClipped").min == 4 && sdk.arity.at("dwriteDrawTextClipped").max == 8);

    // The parsing itself.
    const SdkUi sample = ParseSdkUi(
        "function ui.text(text) end\r\nui.Font = {\r\n  Small = 1, ---@type ui.Font\r\n}\r\n"
        "ui.DWriteFont = function (name, dir) end\r\nfunction ui.DWriteFont.Weight(x) end\r\n--ui.nope = 1\r\n"
        "---@param p1 vec2\r\n---@param color rgbm\r\n---@param rounding number? @Default value: 0.\r\n"
        "function ui.box(p1, color, rounding) end\r\n---@param a string\r\nfunction ui.any(a, ...) end\r\n"
        "---@overload fun(a: string)\r\n---@param a string\r\n---@param b number\r\nfunction ui.over(a, b) end\r\n");
    CHECK(sample.functions.count("text") == 1 && sample.functions.count("DWriteFont") == 1);
    CHECK(sample.functions.count("nope") == 0 && sample.functions.size() == 5);
    CHECK(sample.enums.count("Font") == 1 && sample.enums.at("Font").count("Small") == 1);
    CHECK(sample.arity.at("text").min == 0 && sample.arity.at("text").max == 1);  // no ---@param: optional
    CHECK(sample.arity.at("DWriteFont").max == 2);
    CHECK(sample.arity.at("box").min == 2 && sample.arity.at("box").max == 3);
    CHECK(sample.arity.at("any").max == -1);
    CHECK(sample.arity.at("over").min == 0 && sample.arity.at("over").max == 2);
    const std::vector<std::pair<std::string, int>> counted = UiCallArgCounts(acdb_test::LuaCodeOnly(
        "ui.box(P1, f(a, b), {1, 2})\nui.text('a, b')\nui.endChild()\nx.ui.no(1)\nui.box(P1, c, 2, K.corners)\n", 's'));
    REQUIRE(counted.size() == 4u);
    CHECK(counted[0] == std::make_pair(std::string("box"), 3));
    CHECK(counted[1] == std::make_pair(std::string("text"), 1));
    CHECK(counted[2] == std::make_pair(std::string("endChild"), 0));
    CHECK(counted[3] == std::make_pair(std::string("box"), 4));  // one too many for box: the check above fails it
}

// One visible CSP app window titled "AC DLSS-G"; LAZY = NONE keeps the
// camera writer running whether or not the window is open.
TEST(LuaApp_ManifestDeclaresTheSettingsWindow) {
    const std::string ini = acdb_test::ReadLuaAppFile("manifest.ini");
    REQUIRE(!ini.empty());
    const size_t window = ini.find("[WINDOW_...]");
    REQUIRE(window != std::string::npos);
    const std::string w = ini.substr(window);
    CHECK(std::regex_search(w, std::regex(R"(\n\s*NAME\s*=\s*AC DLSS-G\s*\r?\n)")));
    CHECK(std::regex_search(w, std::regex(R"(\n\s*FUNCTION_MAIN\s*=\s*windowMain\b)")));
    // A hidden window never opens from the taskbar.
    std::smatch flags;
    if (std::regex_search(w, flags, std::regex(R"(\n\s*FLAGS\s*=([^\r\n]*))")))
        CHECK(flags[1].str().find("HIDDEN") == std::string::npos);
    const std::string lua = acdb_test::ReadLuaAppFile("AcDlssg.lua");
    CHECK(lua.find("function script.windowMain(dt)") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Fixed text fields

TEST(PanelText_CopyTextTerminatesAndCutsAtACharacterBoundary) {
    char small[8];
    std::memset(small, 'x', sizeof(small));
    CopyText(small, "abc");
    CHECK(std::strcmp(small, "abc") == 0);
    CopyText(small, "0123456789");
    CHECK(std::strcmp(small, "0123456") == 0);
    // "ab" + U+00E9 (2 bytes) + U+20AC (3 bytes) is 7 bytes and fits 7 chars + NUL.
    CopyText(small, "ab\xC3\xA9\xE2\x82\xAC");
    CHECK(std::strcmp(small, "ab\xC3\xA9\xE2\x82\xAC") == 0);
    // One more leading byte: the euro sign would be cut in half, so it goes.
    CopyText(small, "abc\xC3\xA9\xE2\x82\xAC");
    CHECK(std::strcmp(small, "abc\xC3\xA9") == 0);
    CopyText(small, "");
    CHECK(small[0] == '\0');
    CHECK(TextOf(small) == "");
    char full[4] = {'a', 'b', 'c', 'd'};  // no NUL: TextOf stops at the end
    CHECK(TextOf(full) == "abcd");
}

// ---------------------------------------------------------------------------
// PanelStatusChannel

namespace {

std::wstring TestSectionName(const wchar_t* kind) {
    static std::atomic<unsigned> counter{0};
    return std::wstring(L"Local\\AcDlssg.") + kind + L".test." + std::to_wstring(GetCurrentProcessId()) + L"." +
           std::to_wstring(counter++);
}

template <class T>
T Load(const T& field) {
    return std::atomic_ref<T>(const_cast<T&>(field)).load(std::memory_order_relaxed);
}
template <class T>
void Store(T& field, T value) {
    std::atomic_ref<T>(field).store(value, std::memory_order_relaxed);
}
void Barrier() { std::atomic_thread_fence(std::memory_order_seq_cst); }

// A mapping of a named section, as CSP's ac.writeMemoryMappedFile makes it
// (PAGE_READWRITE, a view for read and write of the layout's size).
template <class T>
class Mapping {
public:
    explicit Mapping(const std::wstring& name, DWORD size = sizeof(T)) {
        handle_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size, name.c_str());
        if (handle_) view_ = static_cast<T*>(MapViewOfFile(handle_, FILE_MAP_WRITE, 0, 0, sizeof(T)));
    }
    ~Mapping() {
        if (view_) UnmapViewOfFile(view_);
        if (handle_) CloseHandle(handle_);
    }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    T* get() const { return view_; }

private:
    HANDLE handle_ = nullptr;
    T* view_ = nullptr;
};

// sendRequest() of apps/lua/AcDlssg/AcDlssg.lua, statement by statement.
void LuaStyleRequest(ControlLayout* m, uint32_t* counter, bool fg, bool flip, bool negate, bool save,
                     uint32_t multiplier = 0) {
    const uint32_t s = (Load(m->seq) | 1u) & 0x7FFFFFFFu;  // bit.band(bit.bor(ctl.seq, 1), 0x7FFFFFFF)
    Store(m->seq, s);
    Barrier();
    *counter = *counter % 0x7FFFFFFFu + 1u;
    Store(m->magic, kControlMagic);
    Store(m->version, kControlVersion);
    Store(m->fgEnabled, fg ? 1u : 0u);
    Store(m->cameraFlipHandedness, flip ? 1u : 0u);
    Store(m->cameraNegateSide, negate ? 1u : 0u);
    Store(m->saveAsDefault, save ? 1u : 0u);
    Store(m->desiredMultiplier, multiplier);
    Store(m->requestCounter, *counter);
    Barrier();
    Store(m->seq, s + 1);
}

}  // namespace

TEST(PanelStatus_DefaultSectionsAreLocalV1) {
    CHECK(PanelStatusChannel::Get().SectionName() == L"Local\\AcDlssg.Status.v1");
    CHECK(PanelControlChannel::Get().SectionName() == L"Local\\AcDlssg.Control.v1");
}

TEST(PanelStatus_ReadBeforeCreateIsNoSection) {
    PanelStatusChannel ch(TestSectionName(L"Status").c_str());
    StatusLayout s{};
    CHECK(ch.Read(&s) == PanelStatusChannel::ReadResult::NoSection);
    CHECK(!ch.Owned());
    int calls = 0;
    ch.Update([&](StatusLayout&) { ++calls; });  // nothing to publish into
    CHECK_EQ(calls, 0);
}

TEST(PanelStatus_CreatePublishesAnOwnedRecord) {
    const std::wstring name = TestSectionName(L"Status");
    PanelStatusChannel ch(name.c_str());
    std::string err;
    REQUIRE(ch.Create(&err));
    CHECK(ch.Owned());
    CHECK_EQ(ch.ForeignOwner(), 0u);
    CHECK(ch.Create(&err));  // idempotent
    StatusLayout s{};
    REQUIRE(ch.Read(&s) == PanelStatusChannel::ReadResult::Ok);
    CHECK_EQ(s.magic, kStatusMagic);
    CHECK_EQ(s.version, kStatusVersion);
    CHECK_EQ(s.ownerPid, static_cast<uint32_t>(GetCurrentProcessId()));
    CHECK_EQ(s.bridgeState, static_cast<uint32_t>(kPanelNotLoaded));
    CHECK_EQ(s.seq % 2, 0u);
    CHECK(s.heartbeat >= 1u);
    CHECK(TextOf(s.bridgeVersion) == ACDB_VERSION);
    // The Lua app maps the section with the layout's size; the bridge makes it 4096 bytes.
    Mapping<StatusLayout> lua(name);
    REQUIRE(lua.get() != nullptr);
    CHECK_EQ(Load(lua.get()->magic), kStatusMagic);
}

TEST(PanelStatus_UpdatePublishesTheChangeAndAdvancesTheHeartbeat) {
    const std::wstring name = TestSectionName(L"Status");
    PanelStatusChannel ch(name.c_str());
    std::string err;
    REQUIRE(ch.Create(&err));
    StatusLayout before{};
    REQUIRE(ch.Read(&before) == PanelStatusChannel::ReadResult::Ok);
    ch.Update([](StatusLayout& s) {
        s.bridgeState = kPanelFgAvailable;
        s.fgOn = 1;
        s.baseFps = 51.5f;
        CopyText(s.reason, "on");
        CopyText(s.gpuName, "NVIDIA GeForce RTX 3080");
    });
    ch.Update([](StatusLayout& s) { s.presentedFps = 101.0f; });  // the rest of the record stays
    StatusLayout after{};
    REQUIRE(ch.Read(&after) == PanelStatusChannel::ReadResult::Ok);
    CHECK_EQ(after.heartbeat, before.heartbeat + 2);
    CHECK_EQ(after.seq, before.seq + 4);  // two writes, odd and even each
    CHECK_EQ(after.bridgeState, static_cast<uint32_t>(kPanelFgAvailable));
    CHECK_EQ(after.fgOn, 1u);
    CHECK_EQ(after.baseFps, 51.5f);
    CHECK_EQ(after.presentedFps, 101.0f);
    CHECK(TextOf(after.reason) == "on");
    CHECK(TextOf(after.gpuName) == "NVIDIA GeForce RTX 3080");
    CHECK_EQ(after.ownerPid, static_cast<uint32_t>(GetCurrentProcessId()));
}

TEST(PanelStatus_OddSeqIsTornAndWrongMagicIsNotWritten) {
    const std::wstring name = TestSectionName(L"Status");
    PanelStatusChannel ch(name.c_str());
    std::string err;
    REQUIRE(ch.Create(&err));
    Mapping<StatusLayout> raw(name);
    REQUIRE(raw.get() != nullptr);
    const uint32_t seq = Load(raw.get()->seq);
    Store(raw.get()->seq, seq | 1u);
    StatusLayout s{};
    CHECK(ch.Read(&s) == PanelStatusChannel::ReadResult::Torn);
    Store(raw.get()->seq, seq);
    Store(raw.get()->magic, 0u);
    CHECK(ch.Read(&s) == PanelStatusChannel::ReadResult::NotWritten);
}

// A test app next to the game (or a second game on the desktop) must not
// overwrite the record of the bridge that owns the section: its channel
// stays unowned and publishes nothing.
TEST(PanelStatus_ARecordOfAnotherProcessIsNeverOverwritten) {
    const std::wstring name = TestSectionName(L"Status");
    Mapping<StatusLayout> other(name, kPanelSectionSize);
    REQUIRE(other.get() != nullptr);
    const uint32_t otherPid = GetCurrentProcessId() + 4;
    StatusLayout rec{};
    rec.magic = kStatusMagic;
    rec.version = kStatusVersion;
    rec.seq = 10;
    rec.heartbeat = 7;
    rec.ownerPid = otherPid;
    rec.bridgeState = kPanelFgAvailable;
    std::memcpy(other.get(), &rec, sizeof(rec));
    Barrier();

    PanelStatusChannel ch(name.c_str());
    std::string err;
    REQUIRE(ch.Create(&err));
    CHECK(!ch.Owned());
    CHECK_EQ(ch.ForeignOwner(), otherPid);
    ch.Update([](StatusLayout& s) { s.bridgeState = kPanelPassThrough; });
    StatusLayout s{};
    REQUIRE(ch.Read(&s) == PanelStatusChannel::ReadResult::Ok);
    CHECK_EQ(s.ownerPid, otherPid);
    CHECK_EQ(s.heartbeat, 7u);
    CHECK_EQ(s.bridgeState, static_cast<uint32_t>(kPanelFgAvailable));
}

// A section the Lua app created first holds zeros: the bridge owns it.
TEST(PanelStatus_AZeroedSectionFromTheLuaAppIsTakenOver) {
    const std::wstring name = TestSectionName(L"Status");
    Mapping<StatusLayout> lua(name);
    REQUIRE(lua.get() != nullptr);
    PanelStatusChannel ch(name.c_str());
    std::string err;
    REQUIRE(ch.Create(&err));
    CHECK(ch.Owned());
    CHECK_EQ(Load(lua.get()->magic), kStatusMagic);
}

// A reader racing the writer (as the Lua app reads while the bridge writes)
// never accepts a record whose fields come from two different writes.
TEST(PanelStatus_ConcurrentReaderNeverSeesAMixedRecord) {
    const std::wstring name = TestSectionName(L"Status");
    PanelStatusChannel ch(name.c_str());
    std::string err;
    REQUIRE(ch.Create(&err));
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (uint32_t n = 1; !stop.load(std::memory_order_relaxed); ++n) {
            ch.Update([n](StatusLayout& s) {
                s.baseFps = static_cast<float>(n % 100000);
                s.vramUsageMib = n;
                s.vramBudgetMib = n;
                char text[32];
                std::snprintf(text, sizeof(text), "%u", n);
                CopyText(s.reason, text);
            });
        }
    });
    int ok = 0;
    int mixed = 0;
    const ULONGLONG until = GetTickCount64() + 700;
    while (GetTickCount64() < until) {
        StatusLayout s{};
        if (ch.Read(&s) != PanelStatusChannel::ReadResult::Ok) continue;
        if (s.vramUsageMib == 0) continue;  // the record Create published, before the first Update
        ++ok;
        char text[32];
        std::snprintf(text, sizeof(text), "%u", s.vramUsageMib);
        if (s.vramUsageMib != s.vramBudgetMib || TextOf(s.reason) != text ||
            s.baseFps != static_cast<float>(s.vramUsageMib % 100000))
            ++mixed;
    }
    stop.store(true);
    writer.join();
    std::printf("  %d consistent reads\n", ok);
    CHECK(ok > 0);
    CHECK_EQ(mixed, 0);
}

// ---------------------------------------------------------------------------
// PanelControlChannel

TEST(PanelControl_ReadBeforeCreateIsNoSection) {
    PanelControlChannel ch(TestSectionName(L"Control").c_str());
    ControlLayout c{};
    CHECK(ch.Read(&c) == PanelControlChannel::ReadResult::NoSection);
    CHECK(!ch.Ready());
    CHECK_EQ(ch.Seq(), 0u);
}

TEST(PanelControl_LuaRequestsReadBackEveryField) {
    const std::wstring name = TestSectionName(L"Control");
    PanelControlChannel ch(name.c_str());
    std::string err;
    REQUIRE(ch.Create(&err));
    CHECK(ch.Ready());
    ControlLayout c{};
    CHECK(ch.Read(&c) == PanelControlChannel::ReadResult::NotWritten);  // zeros until the app writes
    CHECK_EQ(ch.Seq(), 0u);

    Mapping<ControlLayout> lua(name);
    REQUIRE(lua.get() != nullptr);
    uint32_t counter = 0;
    LuaStyleRequest(lua.get(), &counter, false, true, false, false);
    CHECK_EQ(ch.Seq(), 2u);
    REQUIRE(ch.Read(&c) == PanelControlChannel::ReadResult::Ok);
    CHECK_EQ(c.requestCounter, 1u);
    CHECK_EQ(c.fgEnabled, 0u);
    CHECK_EQ(c.cameraFlipHandedness, 1u);
    CHECK_EQ(c.cameraNegateSide, 0u);
    CHECK_EQ(c.saveAsDefault, 0u);
    CHECK_EQ(c.seq, 2u);

    CHECK_EQ(c.desiredMultiplier, 0u);

    LuaStyleRequest(lua.get(), &counter, true, true, true, true, 3);
    REQUIRE(ch.Read(&c) == PanelControlChannel::ReadResult::Ok);
    CHECK_EQ(c.requestCounter, 2u);
    CHECK_EQ(c.fgEnabled, 1u);
    CHECK_EQ(c.cameraNegateSide, 1u);
    CHECK_EQ(c.saveAsDefault, 1u);
    CHECK_EQ(c.desiredMultiplier, 3u);
    CHECK_EQ(ch.Seq(), 4u);

    // A write in progress (odd seq) is torn; the bridge tries again next frame.
    Store(lua.get()->seq, 5u);
    CHECK(ch.Read(&c) == PanelControlChannel::ReadResult::Torn);
    CHECK_EQ(ch.Seq(), 5u);
}

TEST(PanelControl_CounterWrapsBelowTwoToTheThirtyOneAndSkipsZero) {
    const std::wstring name = TestSectionName(L"Control");
    PanelControlChannel ch(name.c_str());
    std::string err;
    REQUIRE(ch.Create(&err));
    Mapping<ControlLayout> lua(name);
    REQUIRE(lua.get() != nullptr);
    uint32_t counter = 0x7FFFFFFEu;
    LuaStyleRequest(lua.get(), &counter, true, false, false, false);
    ControlLayout c{};
    REQUIRE(ch.Read(&c) == PanelControlChannel::ReadResult::Ok);
    CHECK_EQ(c.requestCounter, 0x7FFFFFFFu);
    LuaStyleRequest(lua.get(), &counter, true, false, false, false);
    REQUIRE(ch.Read(&c) == PanelControlChannel::ReadResult::Ok);
    CHECK_EQ(c.requestCounter, 1u);
}

// ---------------------------------------------------------------------------
// Applying a request (pure)

namespace {

ControlRequest Request(uint32_t counter, bool fg, bool flip, bool negate, bool save = false) {
    ControlRequest r;
    r.counter = counter;
    r.desired.fgUserOn = fg;
    r.desired.flipHandedness = flip;
    r.desired.negateSide = negate;
    r.saveAsDefault = save;
    return r;
}

PanelSettings Settings(bool fg, bool flip, bool negate) {
    PanelSettings s;
    s.fgUserOn = fg;
    s.flipHandedness = flip;
    s.negateSide = negate;
    return s;
}

}  // namespace

TEST(PanelApply_AnAppliedCounterIsNotAppliedAgain) {
    const ControlDecision d = DecideControl(5, Request(5, false, true, true, true), Settings(true, false, false));
    CHECK(!d.apply);
    CHECK(!d.fgChanged);
    CHECK(!d.flipChanged);
    CHECK(!d.negateChanged);
    CHECK(!d.save);
    CHECK(!d.multiplierChanged);
    // Nothing changes: the settings stay the bridge's.
    CHECK(d.next.fgUserOn);
    CHECK(!d.next.flipHandedness);
    CHECK(!d.next.negateSide);
}

TEST(PanelApply_ANewCounterSwitchesOnlyWhatDiffers) {
    ControlDecision d = DecideControl(5, Request(6, false, false, false), Settings(true, false, false));
    CHECK(d.apply);
    CHECK(d.fgChanged);
    CHECK(!d.flipChanged);
    CHECK(!d.negateChanged);
    CHECK(!d.save);
    CHECK(!d.next.fgUserOn);

    d = DecideControl(6, Request(7, false, true, false), Settings(false, false, false));
    CHECK(d.apply);
    CHECK(!d.fgChanged);
    CHECK(d.flipChanged);
    CHECK(!d.negateChanged);
    CHECK(d.next.flipHandedness);

    d = DecideControl(7, Request(8, false, true, true), Settings(false, true, false));
    CHECK(d.negateChanged);
    CHECK(!d.flipChanged);
    CHECK(d.next.negateSide);

    // Same values as the bridge has (the hotkey got there first): applied, nothing changes.
    d = DecideControl(8, Request(9, true, true, true), Settings(true, true, true));
    CHECK(d.apply);
    CHECK(!d.fgChanged && !d.flipChanged && !d.negateChanged && !d.save);
}

// Any change of the counter is a new request, including the wrap to 1 and
// a counter below the last one (a reloaded app that did not continue it).
TEST(PanelApply_AnyOtherCounterIsANewRequest) {
    CHECK(DecideControl(0x7FFFFFFFu, Request(1, true, false, false), Settings(false, false, false)).apply);
    CHECK(DecideControl(9, Request(3, true, false, false), Settings(false, false, false)).apply);
    CHECK(DecideControl(0, Request(1, true, false, false), Settings(true, false, false)).apply);
}

TEST(PanelApply_SaveAsDefaultWritesTheSettingsAfterTheRequest) {
    ControlRequest request = Request(2, false, true, false, true);
    request.desiredMultiplier = 3;
    const ControlDecision d = DecideControl(1, request, Settings(true, false, false));
    CHECK(d.apply);
    CHECK(d.save);
    const IniKeyValues keys = SavedDefaultKeys(d.next);
    REQUIRE(keys.size() == 4u);
    CHECK(keys[0].first == "start_with_fg");
    CHECK(keys[0].second == "0");
    CHECK(keys[1].first == "camera_flip_handedness");
    CHECK(keys[1].second == "1");
    CHECK(keys[2].first == "camera_negate_side");
    CHECK(keys[2].second == "0");
    CHECK(keys[3].first == "fg_multiplier");
    CHECK(keys[3].second == "3");
}

// The 2X/3X/4X buttons: desiredMultiplier 2..4 asks for that multiplier
// (D3D12Presenter::SetFgMultiplier); 0, what an older window writes, and
// anything else leave it as it is.
TEST(PanelApply_AMultiplierRequestSetsTheMultiplier) {
    PanelSettings current = Settings(true, false, false);
    CHECK_EQ(current.multiplier, 2u);
    ControlRequest request = Request(6, true, false, false);
    request.desiredMultiplier = 3;
    ControlDecision d = DecideControl(5, request, current);
    CHECK(d.apply);
    CHECK(d.multiplierChanged);
    CHECK(!d.fgChanged);
    CHECK_EQ(d.next.multiplier, 3u);
    for (uint32_t keep : {0u, 1u, 5u, 0xFFFFFFFFu}) {
        request.desiredMultiplier = keep;
        d = DecideControl(5, request, current);
        CHECK(d.apply);
        CHECK(!d.multiplierChanged);
        CHECK_EQ(d.next.multiplier, 2u);
    }
    // The multiplier the bridge already has changes nothing.
    current.multiplier = 4;
    request.desiredMultiplier = 4;
    d = DecideControl(5, request, current);
    CHECK(!d.multiplierChanged);
    CHECK_EQ(d.next.multiplier, 4u);
    // An applied counter changes nothing.
    request.desiredMultiplier = 2;
    d = DecideControl(6, request, current);
    CHECK(!d.apply);
    CHECK(!d.multiplierChanged);
    CHECK_EQ(d.next.multiplier, 4u);
}

TEST(PanelApply_RequestFromTheLayoutReadsNonZeroAsOn) {
    ControlLayout c{};
    c.requestCounter = 12;
    c.fgEnabled = 1;
    c.cameraFlipHandedness = 0;
    c.cameraNegateSide = 7;
    c.saveAsDefault = 1;
    c.desiredMultiplier = 4;
    const ControlRequest r = ControlRequestFrom(c);
    CHECK_EQ(r.counter, 12u);
    CHECK(r.desired.fgUserOn);
    CHECK(!r.desired.flipHandedness);
    CHECK(r.desired.negateSide);
    CHECK(r.saveAsDefault);
    CHECK_EQ(r.desiredMultiplier, 4u);
}

// fgMultMax: 0 while Streamline has not been asked (every button stays
// usable), else the highest multiplier numFramesToGenerateMax allows.
TEST(PanelStatus_MultiplierMaxIsZeroUntilKnown) {
    CHECK_EQ(PanelMultiplierMax(false, 0), 0u);
    CHECK_EQ(PanelMultiplierMax(false, 3), 0u);
    CHECK_EQ(PanelMultiplierMax(true, 0), 2u);  // a failed query: 2X only
    CHECK_EQ(PanelMultiplierMax(true, 1), 2u);  // RTX 40, or NGX without MultiFrameCountMax
    CHECK_EQ(PanelMultiplierMax(true, 2), 3u);
    CHECK_EQ(PanelMultiplierMax(true, 3), 4u);
    CHECK_EQ(PanelMultiplierMax(true, 5), 4u);
}

// restartNote: a successful "Save as default" sets it, and it stays for the
// rest of the session; a failed save leaves it as it was. The panel's live
// switches (DLSS-G, the multiplier, the camera switches) need no restart.
TEST(PanelStatus_RestartNoteAfterASave) {
    const std::string saved = "Saved. start_with_fg and the other saved switches apply the next time the game starts.";
    CHECK_EQ(PanelRestartNote("", true, true), saved);
    CHECK_EQ(PanelRestartNote("", true, false), std::string());
    CHECK_EQ(PanelRestartNote(saved, false, false), saved);
    CHECK_EQ(PanelRestartNote(saved, true, false), saved);
    CHECK_EQ(PanelRestartNote("", false, false), std::string());
    CHECK(saved.size() < kPanelReasonChars);
}

// The status line: on, else the user's switch (the thing the panel
// changes), else the gate's reason of the last frame.
TEST(PanelReason_OnThenTheUsersSwitchThenTheGate) {
    CHECK(PanelReason(true, true, "off by the user (panel)", "") == "on");
    CHECK(PanelReason(false, false, "off by the user (panel)", "not supported on this adapter") ==
          "off by the user (panel)");
    CHECK(PanelReason(false, true, "off by the user (panel)", "game paused") == "game paused");
    CHECK(PanelReason(false, true, "off by the user (panel)", "") == "off");
}

// Streamline pauses DLSS-G while the game window is not focused ("DLSS-G
// disabled: window not focused" in sl.log) and then generates nothing with
// the mode still eOn; the M3 run had minutes of that. The status then says
// so instead of "running": a statistics second in which DLSS-G was on for
// at least half of the Presents, slDLSSGGetState answered, and no frame was
// generated. The first second after an enable (DLSS-G on for one Present)
// is not a pause.
TEST(PanelStatus_DlssgOnWithoutGeneratedFramesIsAPause) {
    CHECK(FgPausedInSecond(52, 31, 0, true));
    CHECK(FgPausedInSecond(52, 52, 0, true));
    CHECK(!FgPausedInSecond(52, 52, 51, true));
    CHECK(!FgPausedInSecond(52, 52, 1, true));
    CHECK(!FgPausedInSecond(56, 1, 0, true));   // the first second after the enable
    CHECK(!FgPausedInSecond(52, 25, 0, true));  // on for less than half the second
    CHECK(!FgPausedInSecond(52, 0, 0, true));   // DLSS-G off
    CHECK(!FgPausedInSecond(52, 52, 0, false)); // slDLSSGGetState never answered: unknown
    CHECK(!FgPausedInSecond(0, 0, 0, true));
}

// A D3D12 stall switches DLSS-G off outside the gate (EnterStall) and skips
// every frame's decision, so the last frame's gate reason is stale, and
// empty when DLSS-G was on: the status said "off" without a reason. While
// stalled the reason is the stall, unless the adapter cannot run DLSS-G at all.
TEST(PanelReason_AStallIsTheReasonWhileItLasts) {
    CHECK(PanelGateReason(true, true, "") == "D3D12 stall");
    CHECK(PanelGateReason(true, true, "camera not fresh") == "D3D12 stall");
    CHECK(PanelGateReason(false, true, "not supported on this adapter") == "not supported on this adapter");
    CHECK(PanelGateReason(true, false, "game paused") == "game paused");
    CHECK(PanelGateReason(true, false, "") == "");
    CHECK(PanelReason(false, true, "off by the user (panel)", PanelGateReason(true, true, "")) == "D3D12 stall");
}

TEST(PanelText_HotkeyTextNamesTheChord) {
    Hotkey hk;  // ctrl+f10
    CHECK(HotkeyText(hk) == "Ctrl+F10");
    REQUIRE(ParseHotkey("ctrl+shift+g", &hk));
    CHECK(HotkeyText(hk) == "Ctrl+Shift+G");
    REQUIRE(ParseHotkey("alt+5", &hk));
    CHECK(HotkeyText(hk) == "Alt+5");
    REQUIRE(ParseHotkey("f24", &hk));
    CHECK(HotkeyText(hk) == "F24");
}

TEST(PanelText_UserOffReasonNamesTheSource) {
    CHECK(UserOffReason("panel") == "off by the user (panel)");
    CHECK(UserOffReason("hotkey") == "off by the user (hotkey)");
    CHECK(UserOffReason("") == "off by the user (start_with_fg, the hotkey or the panel)");
}
