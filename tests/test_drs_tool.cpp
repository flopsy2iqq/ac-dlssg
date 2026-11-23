// ac-dlssg-drs.exe's command line and commands (drs_tool.h) against a fake
// of the driver settings: nothing here touches NVAPI.
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "drs_json.h"
#include "drs_tool.h"
#include "smooth_motion.h"
#include "test_framework.h"

using namespace acdb;

namespace {

const wchar_t kExe[] = L"D:\\Games\\assettocorsa\\acs.exe";

ProfileValue Own(uint32_t v) {
    ProfileValue p;
    p.present = true;
    p.value = v;
    return p;
}

// The driver settings: one application profile, "Assetto Corsa", that
// holds kExe.
class FakeStore final : public DriverSettingsStore {
public:
    FakeStore() { profile.name = L"Assetto Corsa"; }

    DrsStatus Read(const std::wstring& exePath, const std::wstring& profileName, SmoothMotionProfile* out,
                   std::string* error) override {
        calls.push_back("read");
        readExe = exePath;
        readName = profileName;
        if (readStatus != DrsStatus::Ok) {
            *error = "the fake's read failure";
            return readStatus;
        }
        const bool found = profileName.empty() ? (hasProfile && exePath == kExe) : (hasProfile && profileName == profile.name);
        if (!found) {
            *error = "no such profile";
            return DrsStatus::NoProfile;
        }
        *out = profile;
        return DrsStatus::Ok;
    }

    DrsStatus Write(uint32_t settingId, std::optional<uint32_t> value, std::string* error) override {
        char buf[64];
        if (value) {
            std::snprintf(buf, sizeof(buf), "write 0x%08X=%u", settingId, *value);
        } else {
            std::snprintf(buf, sizeof(buf), "delete 0x%08X", settingId);
        }
        calls.push_back(buf);
        if (writeStatus != DrsStatus::Ok) {
            *error = "the fake's write failure";
            return writeStatus;
        }
        if (!ignoreWrites && settingId == kDrsSmoothMotionApis) profile.apis = value ? Own(*value) : ProfileValue();
        return DrsStatus::Ok;
    }

    SmoothMotionProfile profile;
    bool hasProfile = true;
    DrsStatus readStatus = DrsStatus::Ok;
    DrsStatus writeStatus = DrsStatus::Ok;
    bool ignoreWrites = false;  // Write says Ok, and nothing changes
    std::vector<std::string> calls;
    std::wstring readExe, readName;
};

// Console lines and files in memory.
struct Memory {
    std::vector<std::string> lines;
    std::map<std::wstring, std::string> files;
    DrsIo Io() {
        DrsIo io;
        io.print = [this](const std::string& line) { lines.push_back(line); };
        io.readFile = [this](const std::wstring& path, std::string* text, std::string* error) {
            const auto it = files.find(path);
            if (it == files.end()) {
                *error = "not found";
                return false;
            }
            *text = it->second;
            return true;
        };
        io.writeFile = [this](const std::wstring& path, const std::string& text, std::string*) {
            files[path] = text;
            return true;
        };
        return io;
    }
    std::string Text(const char* eol = "\n") const {
        std::string all;
        for (const std::string& l : lines) all += l + eol;
        return all;
    }
    bool Says(const std::string& part) const { return Text().find(part) != std::string::npos; }
    JsonValue Json(const std::wstring& path) const {
        JsonValue v;
        const auto it = files.find(path);
        if (it == files.end()) return v;
        std::string error;
        if (!ParseJson(it->second, &v, &error)) std::printf("  JSON error: %s\n", error.c_str());
        return v;
    }
};

int Run(const std::vector<std::wstring>& args, FakeStore& store, Memory& memory) {
    const int code = DrsMain(args, kExe, store, memory.Io());
    for (const std::string& l : memory.lines) std::printf("    | %s\n", l.c_str());
    return code;
}

std::string Str(const JsonValue& v, const char* key) {
    const JsonValue* m = v.Find(key);
    return m && m->type == JsonValue::Type::String ? m->text : std::string("<none>");
}

// "absent", or the value: a ProfileValue member of the JSON.
std::string Own(const JsonValue& v, const char* key) {
    const JsonValue* m = v.Find(key);
    if (!m || m->type != JsonValue::Type::Object) return "<none>";
    const JsonValue* present = m->Find("present");
    if (!present || present->type != JsonValue::Type::Bool) return "<bad>";
    if (!present->boolean) return "absent";
    uint32_t n = 0;
    return m->Find("value") && JsonUint32(*m->Find("value"), &n) ? std::to_string(n) : std::string("<bad>");
}

std::string Num(const JsonValue& v, const char* key) {
    const JsonValue* m = v.Find(key);
    if (!m) return "<none>";
    if (m->type == JsonValue::Type::Null) return "null";
    uint32_t n = 0;
    return JsonUint32(*m, &n) ? std::to_string(n) : std::string("<bad>");
}

std::string Flag(const JsonValue& v, const char* key) {
    const JsonValue* m = v.Find(key);
    if (!m) return "<none>";
    if (m->type != JsonValue::Type::Bool) return "<bad>";
    return m->boolean ? "true" : "false";
}

const std::vector<std::string> kReadOnly = {"read"};

// NVIDIA's predefined profile of another game that holds a bare acs.exe
// (review finding A-1).
void MakeForeign(FakeStore& store) {
    store.profile.name = L"ACS";
    store.profile.predefined = true;
}

}  // namespace

// ---------------------------------------------------------------- command line

TEST(DrsArgs_TakesTheThreeForms) {
    DrsArgs a;
    std::string error;
    CHECK(ParseDrsArgs({L"smooth-motion", L"status"}, kExe, &a, &error));
    CHECK(a.command == DrsCommand::Status && a.exe == kExe && a.json.empty() && a.log.empty());
    CHECK(ParseDrsArgs({L"smooth-motion", L"off", L"--exe", L"C:\\AC\\acs.exe", L"--json", L"r.json", L"--log", L"l.txt"},
                       kExe, &a, &error));
    CHECK(a.command == DrsCommand::Off && a.exe == L"C:\\AC\\acs.exe" && a.json == L"r.json" && a.log == L"l.txt");
    CHECK(ParseDrsArgs({L"smooth-motion", L"restore", L"--json", L"r.json"}, L"", &a, &error));
    CHECK(a.command == DrsCommand::Restore && a.json == L"r.json");
}

TEST(DrsArgs_RefusesAnythingElse) {
    const std::vector<std::vector<std::wstring>> bad = {
        {},
        {L"status"},
        {L"smooth-motion"},
        {L"smooth-motion", L"on"},
        {L"smooth-motion", L"off"},                         // no --json
        {L"smooth-motion", L"restore"},                     // no --json
        {L"smooth-motion", L"restore", L"--json", L"r.json", L"--exe", L"x"},  // restore takes the record's exe
        {L"smooth-motion", L"status", L"--json"},           // no value
        {L"smooth-motion", L"status", L"--force"},
        {L"smooth-motion", L"status", L"--json", L"a", L"--json", L"b"},
        {L"smooth-motion", L"status", L"extra"},
    };
    for (const auto& args : bad) {
        DrsArgs a;
        std::string error;
        CHECK(!ParseDrsArgs(args, kExe, &a, &error));
        CHECK(!error.empty());
    }
    // Without a default, status and off need --exe.
    DrsArgs a;
    std::string error;
    CHECK(!ParseDrsArgs({L"smooth-motion", L"status"}, L"", &a, &error));
    CHECK(error.find("--exe") != std::string::npos);
}

TEST(DrsMain_ABadCommandLineChangesNothingAndShowsTheUsage) {
    FakeStore store;
    Memory memory;
    CHECK_EQ(Run({L"smooth-motion", L"on"}, store, memory), kDrsExitUsage);
    CHECK(store.calls.empty());
    CHECK(memory.Says("smooth-motion status") && memory.Says("smooth-motion off") && memory.Says("smooth-motion restore"));
    CHECK(DrsUsageText().find("exit code") != std::string::npos);
    CHECK(DrsUsageText().find("8 another game's predefined driver profile holds acs.exe") != std::string::npos);
}

// ---------------------------------------------------------------- status

TEST(DrsStatus_ReadsAndSaysWhatOffWouldDo) {
    FakeStore store;
    Memory memory;
    CHECK_EQ(Run({L"smooth-motion", L"status", L"--json", L"s.json"}, store, memory), kDrsExitOk);
    CHECK(store.calls == kReadOnly);
    CHECK(store.readExe == kExe && store.readName.empty());
    CHECK(memory.Says("\"Assetto Corsa\""));
    CHECK(memory.Says("0xB0CC0875") && memory.Says("0xB0D384C0"));
    CHECK(memory.Says("would write 5 (DX12, Vulkan)"));
    const JsonValue j = memory.Json(L"s.json");
    CHECK_EQ(Str(j, "command"), std::string("status"));
    CHECK_EQ(Str(j, "result"), std::string("read"));
    CHECK_EQ(Num(j, "exitCode"), std::string("0"));
    CHECK_EQ(Str(j, "profile"), std::string("Assetto Corsa"));
    CHECK_EQ(Str(j, "settingId"), std::string("0xB0CC0875"));
    CHECK_EQ(Own(j, "app"), std::string("absent"));
    CHECK_EQ(Own(j, "global"), std::string("absent"));
    CHECK_EQ(Num(j, "effective"), std::string("7"));
    CHECK_EQ(Str(j, "source"), std::string("default"));
    REQUIRE(j.Find("plan") != nullptr);
    CHECK_EQ(Str(*j.Find("plan"), "action"), std::string("write"));
    CHECK_EQ(Num(*j.Find("plan"), "value"), std::string("5"));
}

TEST(DrsStatus_SaysWhenDx11IsAlreadyExcluded) {
    FakeStore store;
    store.profile.globalApis = Own(5);
    Memory memory;
    CHECK_EQ(Run({L"smooth-motion", L"status", L"--json", L"s.json"}, store, memory), kDrsExitOk);
    CHECK(memory.Says("nothing to change"));
    const JsonValue j = memory.Json(L"s.json");
    CHECK_EQ(Str(j, "source"), std::string("global"));
    REQUIRE(j.Find("plan") != nullptr);
    CHECK_EQ(Str(*j.Find("plan"), "action"), std::string("none"));
    CHECK_EQ(Num(*j.Find("plan"), "value"), std::string("null"));
}

TEST(DrsStatus_NoDriverAndNoProfile) {
    {
        FakeStore store;
        store.readStatus = DrsStatus::NoDriver;
        Memory memory;
        CHECK_EQ(Run({L"smooth-motion", L"status", L"--json", L"s.json"}, store, memory), kDrsExitNoDriver);
        CHECK_EQ(Str(memory.Json(L"s.json"), "result"), std::string("no-driver"));
        CHECK(memory.Says("the fake's read failure"));
    }
    {
        FakeStore store;
        store.hasProfile = false;
        Memory memory;
        CHECK_EQ(Run({L"smooth-motion", L"status", L"--json", L"s.json"}, store, memory), kDrsExitNoProfile);
        CHECK_EQ(Str(memory.Json(L"s.json"), "result"), std::string("no-profile"));
    }
}

// ---------------------------------------------------------------- off

TEST(DrsOff_WritesFiveAndRecordsTheOldState) {
    FakeStore store;
    Memory memory;
    CHECK_EQ(Run({L"smooth-motion", L"off", L"--json", L"r.json"}, store, memory), kDrsExitOk);
    const std::vector<std::string> calls = {"read", "write 0xB0CC0875=5", "read"};
    CHECK(store.calls == calls);
    // The read-back finds the profile it wrote into by its name.
    CHECK(store.readName == L"Assetto Corsa");
    CHECK(store.profile.apis == Own(5));
    CHECK(memory.Says("wrote 5 (DX12, Vulkan)"));
    const JsonValue j = memory.Json(L"r.json");
    CHECK_EQ(Str(j, "command"), std::string("off"));
    CHECK_EQ(Str(j, "result"), std::string("written"));
    CHECK_EQ(Num(j, "exitCode"), std::string("0"));
    CHECK_EQ(Str(j, "exe"), std::string("D:\\Games\\assettocorsa\\acs.exe"));
    CHECK_EQ(Str(j, "profile"), std::string("Assetto Corsa"));
    CHECK_EQ(Str(j, "settingId"), std::string("0xB0CC0875"));
    CHECK_EQ(Own(j, "before"), std::string("absent"));
    CHECK_EQ(Num(j, "written"), std::string("5"));
    CHECK_EQ(Own(j, "after"), std::string("5"));
}

TEST(DrsOff_KeepsTheOtherBitsOfTheAppProfilesValue) {
    FakeStore store;
    store.profile.apis = Own(3);
    store.profile.globalApis = Own(5);
    Memory memory;
    CHECK_EQ(Run({L"smooth-motion", L"off", L"--json", L"r.json"}, store, memory), kDrsExitOk);
    CHECK(store.profile.apis == Own(1));
    const JsonValue j = memory.Json(L"r.json");
    CHECK_EQ(Own(j, "before"), std::string("3"));
    CHECK_EQ(Num(j, "written"), std::string("1"));
}

TEST(DrsOff_ChangesNothingWhenDx11IsAlreadyExcluded) {
    FakeStore store;
    store.profile.apis = Own(5);
    Memory memory;
    CHECK_EQ(Run({L"smooth-motion", L"off", L"--json", L"r.json"}, store, memory), kDrsExitOk);
    CHECK(store.calls == kReadOnly);
    CHECK(memory.Says("nothing to change"));
    const JsonValue j = memory.Json(L"r.json");
    CHECK_EQ(Str(j, "result"), std::string("unchanged"));
    CHECK_EQ(Own(j, "before"), std::string("5"));
    CHECK_EQ(Num(j, "written"), std::string("null"));
}

TEST(DrsOff_WithoutAdministratorRightsChangesNothingAndSaysSo) {
    FakeStore store;
    store.writeStatus = DrsStatus::NeedsAdmin;
    Memory memory;
    CHECK_EQ(Run({L"smooth-motion", L"off", L"--json", L"r.json"}, store, memory), kDrsExitNeedsAdmin);
    CHECK(memory.Says("administrator"));
    const JsonValue j = memory.Json(L"r.json");
    CHECK_EQ(Str(j, "result"), std::string("needs-admin"));
    CHECK_EQ(Num(j, "exitCode"), std::string("4"));
    CHECK_EQ(Num(j, "written"), std::string("null"));
}

TEST(DrsOff_AWriteThatDoesNotReadBackFails) {
    FakeStore store;
    store.ignoreWrites = true;
    Memory memory;
    CHECK_EQ(Run({L"smooth-motion", L"off", L"--json", L"r.json"}, store, memory), kDrsExitFailed);
    const JsonValue j = memory.Json(L"r.json");
    CHECK_EQ(Str(j, "result"), std::string("failed"));
    CHECK_EQ(Own(j, "after"), std::string("absent"));
    CHECK(memory.Says("read back"));
}

TEST(DrsOff_AFailedWriteFails) {
    FakeStore store;
    store.writeStatus = DrsStatus::Failed;
    Memory memory;
    CHECK_EQ(Run({L"smooth-motion", L"off", L"--json", L"r.json"}, store, memory), kDrsExitFailed);
    CHECK_EQ(Str(memory.Json(L"r.json"), "result"), std::string("failed"));
    CHECK(memory.Says("the fake's write failure"));
}

TEST(DrsOff_NoProfileChangesNothing) {
    FakeStore store;
    store.hasProfile = false;
    Memory memory;
    CHECK_EQ(Run({L"smooth-motion", L"off", L"--json", L"r.json"}, store, memory), kDrsExitNoProfile);
    CHECK(store.calls == kReadOnly);
    CHECK_EQ(Str(memory.Json(L"r.json"), "result"), std::string("no-profile"));
}

// ---------------------------------------------------------------- another game's profile

// Review finding A-1: an acs.exe that is not in a folder named assettocorsa
// gets NVIDIA's predefined "ACS" profile of another game. off changes
// nothing there, since the change would apply to that game too, and says
// which profile it is, why, and how to do it by hand.
TEST(DrsOff_LeavesAnotherGamesPredefinedProfileAlone) {
    FakeStore store;
    MakeForeign(store);
    Memory memory;
    CHECK_EQ(Run({L"smooth-motion", L"off", L"--json", L"r.json"}, store, memory), kDrsExitForeignProfile);
    CHECK(store.calls == kReadOnly);
    CHECK(memory.Says("NVIDIA's predefined profile \"ACS\""));
    CHECK(memory.Says("only matches an acs.exe in a folder named assettocorsa"));
    CHECK(memory.Says("NVIDIA Profile Inspector"));
    CHECK(memory.Says("to 5 (DX12, Vulkan)"));
    CHECK(memory.Says("the other game"));
    const JsonValue j = memory.Json(L"r.json");
    CHECK_EQ(Str(j, "result"), std::string("foreign-profile"));
    CHECK_EQ(Num(j, "exitCode"), std::string("8"));
    CHECK_EQ(Str(j, "profile"), std::string("ACS"));
    CHECK_EQ(Flag(j, "profilePredefined"), std::string("true"));
    CHECK_EQ(Num(j, "written"), std::string("null"));
}

TEST(DrsStatus_SaysThatOffLeavesAnotherGamesPredefinedProfileAlone) {
    FakeStore store;
    MakeForeign(store);
    Memory memory;
    CHECK_EQ(Run({L"smooth-motion", L"status", L"--json", L"s.json"}, store, memory), kDrsExitForeignProfile);
    CHECK(store.calls == kReadOnly);
    CHECK(memory.Says("NVIDIA's predefined profile \"ACS\""));
    const JsonValue j = memory.Json(L"s.json");
    CHECK_EQ(Str(j, "result"), std::string("foreign-profile"));
    CHECK_EQ(Num(j, "exitCode"), std::string("8"));
    REQUIRE(j.Find("plan") != nullptr);
    CHECK_EQ(Str(*j.Find("plan"), "action"), std::string("write"));
}

// Without DX11 in effect there is nothing to change, in any profile.
TEST(DrsOff_AnotherGamesProfileWithoutDx11IsNothingToChange) {
    FakeStore store;
    MakeForeign(store);
    store.profile.apis = Own(5);
    Memory memory;
    CHECK_EQ(Run({L"smooth-motion", L"off", L"--json", L"r.json"}, store, memory), kDrsExitOk);
    CHECK(store.calls == kReadOnly);
    CHECK(memory.Says("nothing to change"));
}

// NVIDIA's own "Assetto Corsa" profile is predefined too, and a profile the
// user made may have any name: off writes into both.
TEST(DrsOff_WritesIntoNvidiasAcProfileAndIntoProfilesTheUserMade) {
    {
        FakeStore store;
        store.profile.predefined = true;
        Memory memory;
        CHECK_EQ(Run({L"smooth-motion", L"off", L"--json", L"r.json"}, store, memory), kDrsExitOk);
        CHECK(store.profile.apis == Own(5));
        CHECK_EQ(Flag(memory.Json(L"r.json"), "profilePredefined"), std::string("true"));
    }
    {
        FakeStore store;
        store.profile.name = L"ACS";
        Memory memory;
        CHECK_EQ(Run({L"smooth-motion", L"off", L"--json", L"r.json"}, store, memory), kDrsExitOk);
        CHECK(store.profile.apis == Own(5));
        CHECK_EQ(Flag(memory.Json(L"r.json"), "profilePredefined"), std::string("false"));
    }
}

// ---------------------------------------------------------------- restore

namespace {

// The record off writes, as the installer keeps it.
std::string Record(const char* before, uint32_t written, const char* profile = "Assetto Corsa",
                   const char* settingId = "0xB0CC0875") {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "{\"exe\": \"D:\\\\Games\\\\assettocorsa\\\\acs.exe\", \"profile\": \"%s\", \"settingId\": \"%s\", "
                  "\"before\": %s, \"written\": %u}",
                  profile, settingId, before, written);
    return buf;
}

const char kAbsent[] = "{\"present\": false, \"value\": null, \"predefined\": false}";
const char kSeven[] = "{\"present\": true, \"value\": 7, \"predefined\": false}";

}  // namespace

TEST(DrsRestore_DeletesTheSettingTheInstallCreated) {
    FakeStore store;
    store.profile.apis = Own(5);
    Memory memory;
    memory.files[L"r.json"] = Record(kAbsent, 5);
    CHECK_EQ(Run({L"smooth-motion", L"restore", L"--json", L"r.json"}, store, memory), kDrsExitOk);
    const std::vector<std::string> calls = {"read", "delete 0xB0CC0875", "read"};
    CHECK(store.calls == calls);
    CHECK(store.readName == L"Assetto Corsa");
    CHECK(!store.profile.apis.present);
    CHECK(memory.Says("restored"));
}

TEST(DrsRestore_WritesTheOldValueBack) {
    FakeStore store;
    store.profile.apis = Own(5);
    Memory memory;
    memory.files[L"r.json"] = Record(kSeven, 5);
    CHECK_EQ(Run({L"smooth-motion", L"restore", L"--json", L"r.json"}, store, memory), kDrsExitOk);
    const std::vector<std::string> calls = {"read", "write 0xB0CC0875=7", "read"};
    CHECK(store.calls == calls);
    CHECK(store.profile.apis == Own(7));
}

TEST(DrsRestore_LeavesAValueChangedSinceTheInstall) {
    FakeStore store;
    store.profile.apis = Own(7);
    Memory memory;
    memory.files[L"r.json"] = Record(kAbsent, 5);
    CHECK_EQ(Run({L"smooth-motion", L"restore", L"--json", L"r.json"}, store, memory), kDrsExitChanged);
    CHECK(store.calls == kReadOnly);
    CHECK(store.profile.apis == Own(7));
    CHECK(memory.Says("left as it is"));
}

TEST(DrsRestore_AProfileThatIsGoneIsNothingToRestore) {
    FakeStore store;
    store.hasProfile = false;
    Memory memory;
    memory.files[L"r.json"] = Record(kAbsent, 5);
    CHECK_EQ(Run({L"smooth-motion", L"restore", L"--json", L"r.json"}, store, memory), kDrsExitNoProfile);
    CHECK(store.calls == kReadOnly);
}

TEST(DrsRestore_WithoutAdministratorRightsSaysSo) {
    FakeStore store;
    store.profile.apis = Own(5);
    store.writeStatus = DrsStatus::NeedsAdmin;
    Memory memory;
    memory.files[L"r.json"] = Record(kAbsent, 5);
    CHECK_EQ(Run({L"smooth-motion", L"restore", L"--json", L"r.json"}, store, memory), kDrsExitNeedsAdmin);
    CHECK(memory.Says("administrator"));
}

TEST(DrsRestore_RefusesWhatIsNotARecord) {
    const std::vector<std::string> bad = {
        "not json",
        "{}",
        Record(kAbsent, 5, "Assetto Corsa", "0x10E41E06"),  // not the Smooth Motion setting
        Record(kAbsent, 5, ""),                             // no profile
        Record("{\"present\": true, \"value\": null}", 5),   // present without a value
        "{\"profile\": \"Assetto Corsa\", \"settingId\": \"0xB0CC0875\", \"before\": {\"present\": false}, "
        "\"written\": null}",
    };
    for (const std::string& text : bad) {
        FakeStore store;
        store.profile.apis = Own(5);
        Memory memory;
        memory.files[L"r.json"] = text;
        CHECK_EQ(Run({L"smooth-motion", L"restore", L"--json", L"r.json"}, store, memory), kDrsExitFile);
        CHECK(store.calls.empty());
    }
    FakeStore store;
    Memory memory;
    CHECK_EQ(Run({L"smooth-motion", L"restore", L"--json", L"missing.json"}, store, memory), kDrsExitFile);
    CHECK(store.calls.empty());
}

// off's own output is a record restore takes.
TEST(DrsRestore_TakesTheRecordOffWrote) {
    FakeStore store;
    store.profile.apis = Own(7);
    Memory memory;
    CHECK_EQ(Run({L"smooth-motion", L"off", L"--json", L"r.json"}, store, memory), kDrsExitOk);
    CHECK(store.profile.apis == Own(5));
    CHECK_EQ(Run({L"smooth-motion", L"restore", L"--json", L"r.json"}, store, memory), kDrsExitOk);
    CHECK(store.profile.apis == Own(7));
}

// ---------------------------------------------------------------- the log

TEST(DrsLog_GetsTheConsoleText) {
    FakeStore store;
    Memory memory;
    CHECK_EQ(Run({L"smooth-motion", L"off", L"--json", L"r.json", L"--log", L"l.txt"}, store, memory), kDrsExitOk);
    REQUIRE(memory.files.count(L"l.txt") == 1);
    CHECK_EQ(memory.files[L"l.txt"], memory.Text("\r\n"));
}
