#include "drs_tool.h"

#include <windows.h>

#include <cstdio>
#include <optional>

#include "drs_json.h"

namespace acdb {
namespace {

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(v));
    return buf;
}

std::string Quoted(const std::wstring& name) { return "\"" + Utf8(name) + "\""; }

// "5 (DX12, Vulkan)", "not set"; NVIDIA's own value says so.
std::string ApisText(const ProfileValue& v) {
    if (!v.present) return "not set";
    return std::to_string(v.value) + " (" + SmoothMotionApisText(v.value) + ")" +
           (v.predefined ? ", NVIDIA's predefined value" : "");
}

std::string EnableText(const ProfileValue& v) {
    if (!v.present) return "not set";
    return std::to_string(v.value) + (v.value == 0 ? " (off)" : v.value == 1 ? " (on)" : "") +
           (v.predefined ? ", NVIDIA's predefined value" : "");
}

std::string ValueJson(const ProfileValue& v) {
    return std::string("{\"present\": ") + (v.present ? "true" : "false") +
           ", \"value\": " + (v.present ? std::to_string(v.value) : std::string("null")) +
           ", \"predefined\": " + (v.predefined ? "true" : "false") + "}";
}

std::string OptionalJson(const std::optional<uint32_t>& v) { return v ? std::to_string(*v) : std::string("null"); }

// The console text, kept for --log.
class Out {
public:
    explicit Out(const DrsIo& io) : io_(io) {}
    void Line(const std::string& s) {
        lines_.push_back(s);
        if (io_.print) io_.print(s);
    }
    std::string Joined() const {
        std::string all;
        for (const std::string& l : lines_) all += l + "\r\n";
        return all;
    }

private:
    const DrsIo& io_;
    std::vector<std::string> lines_;
};

// What status and off write into --json.
struct Report {
    const char* command = "status";
    std::string result;
    int exitCode = kDrsExitOk;
    std::wstring exe;
    bool haveProfile = false;
    SmoothMotionProfile profile;  // as read before any change
    SmoothMotionOffPlan plan;
    std::optional<uint32_t> written;
    std::optional<ProfileValue> after;
    std::string message;
};

std::string ReportJson(const Report& r) {
    const bool off = std::string(r.command) == "off";
    std::string j = "{\r\n";
    auto add = [&j](const char* key, const std::string& value, bool last = false) {
        j += std::string("  \"") + key + "\": " + value + (last ? "\r\n" : ",\r\n");
    };
    add("tool", JsonQuote(std::string("ac-dlssg-drs ") + ACDB_VERSION));
    add("command", JsonQuote(r.command));
    add("result", JsonQuote(r.result));
    add("exitCode", std::to_string(r.exitCode));
    add("exe", JsonQuote(Utf8(r.exe)));
    add("profile", r.haveProfile ? JsonQuote(Utf8(r.profile.name)) : std::string("null"));
    add("profilePredefined", r.haveProfile ? (r.profile.predefined ? "true" : "false") : std::string("null"));
    add("settingId", JsonQuote(Hex(kDrsSmoothMotionApis)));
    add(off ? "before" : "app", ValueJson(r.profile.apis));
    add("global", ValueJson(r.profile.globalApis));
    add("effective", r.haveProfile ? std::to_string(r.plan.effective) : std::string("null"));
    add("source", r.haveProfile ? JsonQuote(r.plan.source) : std::string("null"));
    const bool write = r.haveProfile && r.plan.action == SmoothMotionAction::Write;
    add("plan", std::string("{\"action\": ") + (r.haveProfile ? (write ? "\"write\"" : "\"none\"") : "null") +
                    ", \"value\": " + (write ? std::to_string(r.plan.value) : std::string("null")) + "}");
    if (off) {
        add("written", OptionalJson(r.written));
        add("after", r.after ? ValueJson(*r.after) : std::string("null"));
    }
    add("enable", ValueJson(r.profile.enable));
    add("globalEnable", ValueJson(r.profile.globalEnable));
    add("message", JsonQuote(r.message), true);
    return j + "}\r\n";
}

// The lines that describe a profile's two settings.
void PrintProfile(Out& out, const SmoothMotionProfile& p) {
    out.Line("driver profile: " + Quoted(p.name) +
             (p.predefined ? " (predefined by NVIDIA)" : " (user-defined)"));
    const SmoothMotionOffPlan plan = PlanSmoothMotionOff(p.apis, p.globalApis);
    std::string effective = std::to_string(plan.effective) + " (" + SmoothMotionApisText(plan.effective) + ")";
    if (std::string(plan.source) == "default") effective += ", the driver's default";
    else effective += std::string(", from the ") + plan.source + " profile";
    out.Line(Hex(kDrsSmoothMotionApis) + " Smooth Motion - Enabled APIs: in the profile: " + ApisText(p.apis) +
             "; in the global profile: " + ApisText(p.globalApis) + "; in effect: " + effective);
    out.Line(Hex(kDrsSmoothMotionEnable) + " Smooth Motion - Enable: in the profile: " + EnableText(p.enable) +
             "; in the global profile: " + EnableText(p.globalEnable));
}

// Review finding A-1: why off leaves another game's predefined profile
// alone, and the one manual route.
std::string ForeignProfileText(const std::wstring& name, uint32_t value) {
    return "the driver gives acs.exe NVIDIA's predefined profile " + Quoted(name) +
           ", which belongs to another game. NVIDIA's own " + Quoted(kNvidiaAcProfileName) +
           " profile only matches an acs.exe in a folder named assettocorsa, as in every Steam install. To turn "
           "Smooth Motion off by hand: in NVIDIA Profile Inspector, open the profile " + Quoted(name) +
           ", set \"Smooth Motion - Enabled APIs\" (" + Hex(kDrsSmoothMotionApis) + ") to " + std::to_string(value) +
           " (" + SmoothMotionApisText(value) + ") and click Apply changes; this applies to the other game of that "
           "profile too";
}

std::string ReadFailure(DrsStatus st, const std::wstring& what, const std::string& error, Report* r) {
    if (st == DrsStatus::NoProfile) {
        r->result = "no-profile";
        r->exitCode = kDrsExitNoProfile;
        return "no NVIDIA driver profile holds " + Utf8(what) + " (" + error + "); nothing was changed";
    }
    r->result = "no-driver";
    r->exitCode = kDrsExitNoDriver;
    return "the NVIDIA driver settings could not be read: " + error + "; nothing was changed";
}

// Writes --json (when given); false, with the reason printed, when it cannot.
bool WriteReport(const DrsArgs& args, const DrsIo& io, Out& out, const Report& r) {
    if (args.json.empty()) return true;
    std::string error;
    if (io.writeFile && io.writeFile(args.json, ReportJson(r), &error)) return true;
    out.Line("the result could not be written to " + Utf8(args.json) + ": " + error);
    return false;
}

int Finish(const DrsArgs& args, const DrsIo& io, Out& out, Report& r) {
    out.Line(r.message);
    if (!WriteReport(args, io, out, r)) return kDrsExitFile;
    return r.exitCode;
}

int RunStatusOrOff(const DrsArgs& args, DriverSettingsStore& store, const DrsIo& io, Out& out, bool off) {
    Report r;
    r.command = off ? "off" : "status";
    r.exe = args.exe;
    out.Line(std::string("ac-dlssg-drs ") + ACDB_VERSION + ": NVIDIA Smooth Motion " + r.command + " for " +
             Utf8(args.exe));
    std::string error;
    DrsStatus st = store.Read(args.exe, L"", &r.profile, &error);
    if (st != DrsStatus::Ok) {
        r.profile = SmoothMotionProfile();
        r.message = ReadFailure(st, args.exe, error, &r);
        return Finish(args, io, out, r);
    }
    r.haveProfile = true;
    PrintProfile(out, r.profile);
    r.plan = PlanSmoothMotionOff(r.profile.apis, r.profile.globalApis);
    const std::string where = Quoted(r.profile.name);
    const bool write = r.plan.action == SmoothMotionAction::Write;
    if (!write) {
        r.result = off ? "unchanged" : "read";
        r.message = "Smooth Motion is kept away from Assetto Corsa already (DX11 is not among its APIs): nothing to change";
        return Finish(args, io, out, r);
    }
    const std::string newValue = std::to_string(r.plan.value) + " (" + SmoothMotionApisText(r.plan.value) + ")";
    if (IsForeignPredefinedProfile(r.profile.name, r.profile.predefined)) {
        r.result = "foreign-profile";
        r.exitCode = kDrsExitForeignProfile;
        r.message = (off ? std::string("not changed: ")
                         : std::string("Smooth Motion may take over Assetto Corsa (DX11 is among its APIs), and "
                                       "smooth-motion off would not change it: ")) +
                    ForeignProfileText(r.profile.name, r.plan.value);
        return Finish(args, io, out, r);
    }
    if (!off) {
        r.result = "read";
        r.message = "Smooth Motion may take over Assetto Corsa (DX11 is among its APIs): smooth-motion off would write " +
                    newValue + " into " + where;
        return Finish(args, io, out, r);
    }

    // The record goes out before the change, so that a change is never
    // made without one: a --json that cannot be written changes nothing.
    r.result = "planned";
    r.exitCode = kDrsExitFailed;
    r.message = "about to write " + newValue + " into " + where;
    if (!WriteReport(args, io, out, r)) return kDrsExitFile;

    st = store.Write(kDrsSmoothMotionApis, r.plan.value, &error);
    if (st != DrsStatus::Ok) {
        if (st == DrsStatus::NeedsAdmin) {
            r.result = "needs-admin";
            r.exitCode = kDrsExitNeedsAdmin;
            r.message = "not changed: saving the driver settings needs administrator rights (" + error +
                        "); run ac-dlssg-drs.exe as administrator";
        } else {
            r.result = "failed";
            r.exitCode = kDrsExitFailed;
            r.message = "failed: " + error + "; the driver settings may be unchanged";
        }
        return Finish(args, io, out, r);
    }
    r.written = r.plan.value;
    SmoothMotionProfile after;
    st = store.Read(args.exe, r.profile.name, &after, &error);
    if (st != DrsStatus::Ok) {
        r.result = "failed";
        r.exitCode = kDrsExitFailed;
        r.message = "failed: wrote " + newValue + " into " + where +
                    ", but the driver settings could not be read back: " + error;
        return Finish(args, io, out, r);
    }
    r.after = after.apis;
    if (!after.apis.present || after.apis.value != r.plan.value) {
        r.result = "failed";
        r.exitCode = kDrsExitFailed;
        r.message = "failed: wrote " + newValue + " into " + where + ", but the driver settings read back " +
                    ApisText(after.apis);
        return Finish(args, io, out, r);
    }
    r.result = "written";
    r.exitCode = kDrsExitOk;
    r.message = "wrote " + newValue + " into " + where + " and saved the driver settings; read back " +
                std::to_string(after.apis.value);
    return Finish(args, io, out, r);
}

// The record restore takes: the members off wrote that it needs.
struct RestoreRecord {
    std::wstring exe, profile;
    SmoothMotionRecord record;
};

bool Bool(const JsonValue* v, bool* out) {
    if (!v || v->type != JsonValue::Type::Bool) return false;
    *out = v->boolean;
    return true;
}

// Empty when j is such a record (then *out holds it), else what is wrong.
std::string ParseRecord(const JsonValue& j, RestoreRecord* out) {
    if (j.type != JsonValue::Type::Object) return "not a JSON object";
    const JsonValue* id = j.Find("settingId");
    if (!id || id->type != JsonValue::Type::String || id->text != Hex(kDrsSmoothMotionApis))
        return "settingId is not \"" + Hex(kDrsSmoothMotionApis) + "\"";
    const JsonValue* profile = j.Find("profile");
    if (!profile || profile->type != JsonValue::Type::String || profile->text.empty()) return "no profile";
    out->profile = Wide(profile->text);
    const JsonValue* exe = j.Find("exe");
    if (exe && exe->type == JsonValue::Type::String) out->exe = Wide(exe->text);
    const JsonValue* before = j.Find("before");
    if (!before || before->type != JsonValue::Type::Object || !Bool(before->Find("present"), &out->record.before.present))
        return "no before.present";
    if (out->record.before.present) {
        const JsonValue* value = before->Find("value");
        if (!value || !JsonUint32(*value, &out->record.before.value)) return "no before.value";
        bool predefined = false;
        if (Bool(before->Find("predefined"), &predefined)) out->record.before.predefined = predefined;
    }
    const JsonValue* written = j.Find("written");
    if (!written || !JsonUint32(*written, &out->record.written)) return "no written value";
    return {};
}

int RunRestore(const DrsArgs& args, DriverSettingsStore& store, const DrsIo& io, Out& out) {
    out.Line(std::string("ac-dlssg-drs ") + ACDB_VERSION + ": NVIDIA Smooth Motion restore from " + Utf8(args.json));
    std::string text, error;
    if (!io.readFile || !io.readFile(args.json, &text, &error)) {
        out.Line("the record " + Utf8(args.json) + " could not be read: " + error + "; nothing was changed");
        return kDrsExitFile;
    }
    JsonValue json;
    RestoreRecord rec;
    if (!ParseJson(text, &json, &error)) {
        out.Line("the record " + Utf8(args.json) + " is not JSON (" + error + "); nothing was changed");
        return kDrsExitFile;
    }
    const std::string why = ParseRecord(json, &rec);
    if (!why.empty()) {
        out.Line("the record " + Utf8(args.json) + " is not one of smooth-motion off (" + why +
                 "); nothing was changed");
        return kDrsExitFile;
    }
    const std::string where = Quoted(rec.profile);
    const std::string wrote = std::to_string(rec.record.written);
    out.Line("record: profile " + where + ", before the install: " + ApisText(rec.record.before) + ", written: " + wrote);
    SmoothMotionProfile p;
    DrsStatus st = store.Read(rec.exe, rec.profile, &p, &error);
    if (st == DrsStatus::NoProfile) {
        out.Line("nothing to restore: the driver profile " + where + " is gone (" + error + ")");
        return kDrsExitNoProfile;
    }
    if (st != DrsStatus::Ok) {
        out.Line("the NVIDIA driver settings could not be read: " + error + "; nothing was changed");
        return kDrsExitNoDriver;
    }
    PrintProfile(out, p);
    const RestorePlan plan = PlanRestore(rec.record, p.apis);
    if (plan.action == RestoreAction::Leave) {
        if (!p.apis.present) {
            out.Line("left as it is: " + Hex(kDrsSmoothMotionApis) + " is not set in " + where +
                     " any more (the install wrote " + wrote + ")");
        } else {
            out.Line("left as it is: " + Hex(kDrsSmoothMotionApis) + " in " + where + " is " + ApisText(p.apis) +
                     " now, not the " + wrote + " the install wrote");
        }
        return kDrsExitChanged;
    }
    const bool del = plan.action == RestoreAction::Delete;
    st = store.Write(kDrsSmoothMotionApis, del ? std::nullopt : std::optional<uint32_t>(plan.value), &error);
    if (st != DrsStatus::Ok) {
        if (st == DrsStatus::NeedsAdmin) {
            out.Line("not restored: saving the driver settings needs administrator rights (" + error +
                     "); run ac-dlssg-drs.exe as administrator");
            return kDrsExitNeedsAdmin;
        }
        out.Line("failed: " + error + "; the driver settings may be unchanged");
        return kDrsExitFailed;
    }
    SmoothMotionProfile after;
    st = store.Read(rec.exe, p.name, &after, &error);
    const bool back = st == DrsStatus::Ok &&
                      (del ? (!after.apis.present || after.apis.predefined)
                           : (after.apis.present && after.apis.value == plan.value));
    const std::string what =
        del ? "deleted " + Hex(kDrsSmoothMotionApis) + " from " + where +
                  (rec.record.before.present ? " (NVIDIA's predefined value is back)"
                                             : " (it had no value of its own before the install)")
            : "wrote " + ApisText(ProfileValue{true, plan.value, false}) + " back into " + where;
    if (!back) {
        out.Line("failed: " + what + ", but the driver settings read back " +
                 (st == DrsStatus::Ok ? ApisText(after.apis) : "nothing (" + error + ")"));
        return kDrsExitFailed;
    }
    out.Line("restored: " + what + " and saved the driver settings; read back " + ApisText(after.apis));
    return kDrsExitOk;
}

}  // namespace

bool ParseDrsArgs(const std::vector<std::wstring>& args, const std::wstring& defaultExe, DrsArgs* out,
                  std::string* error) {
    *out = DrsArgs();
    if (args.size() < 2 || args[0] != L"smooth-motion") {
        *error = "expected smooth-motion status, off or restore";
        return false;
    }
    if (args[1] == L"status") {
        out->command = DrsCommand::Status;
    } else if (args[1] == L"off") {
        out->command = DrsCommand::Off;
    } else if (args[1] == L"restore") {
        out->command = DrsCommand::Restore;
    } else {
        *error = "unknown command smooth-motion " + Utf8(args[1]);
        return false;
    }
    bool haveExe = false, haveJson = false, haveLog = false;
    for (size_t i = 2; i < args.size(); i += 2) {
        const std::wstring& opt = args[i];
        std::wstring* target = nullptr;
        bool* seen = nullptr;
        if (opt == L"--exe") {
            target = &out->exe;
            seen = &haveExe;
        } else if (opt == L"--json") {
            target = &out->json;
            seen = &haveJson;
        } else if (opt == L"--log") {
            target = &out->log;
            seen = &haveLog;
        } else {
            *error = "unknown argument " + Utf8(opt);
            return false;
        }
        if (*seen) {
            *error = Utf8(opt) + " is given twice";
            return false;
        }
        if (i + 1 >= args.size() || args[i + 1].empty()) {
            *error = Utf8(opt) + " needs a value";
            return false;
        }
        *seen = true;
        *target = args[i + 1];
    }
    if (out->command == DrsCommand::Restore) {
        if (haveExe) {
            *error = "smooth-motion restore takes acs.exe from the record; it has no --exe";
            return false;
        }
        if (!haveJson) {
            *error = "smooth-motion restore needs --json <record>";
            return false;
        }
        return true;
    }
    if (out->command == DrsCommand::Off && !haveJson) {
        *error = "smooth-motion off needs --json <file> for the record of the change";
        return false;
    }
    if (!haveExe) out->exe = defaultExe;
    if (out->exe.empty()) {
        *error = "acs.exe is not one folder above ac-dlssg-drs.exe; pass --exe <path of acs.exe>";
        return false;
    }
    return true;
}

std::string DrsUsageText() {
    return "usage:\r\n"
           "  ac-dlssg-drs.exe smooth-motion status  [--exe <acs.exe>] [--json <file>] [--log <file>]\r\n"
           "  ac-dlssg-drs.exe smooth-motion off     [--exe <acs.exe>] --json <file> [--log <file>]\r\n"
           "  ac-dlssg-drs.exe smooth-motion restore --json <record> [--log <file>]\r\n"
           "NVIDIA Smooth Motion in the driver profile that holds acs.exe (\"Assetto Corsa\"): status reads it;\r\n"
           "off removes DX11 from \"Smooth Motion - Enabled APIs\" (0xB0CC0875) in that profile, never in the\r\n"
           "global one, and writes the record of the change into --json; restore puts the recorded state back\r\n"
           "while the profile still holds the value written. off and restore need administrator rights.\r\n"
           "--exe defaults to the acs.exe one folder above this program; --log also writes the text to a file.\r\n"
           "exit codes: 0 done or nothing to change, 1 bad command line, 2 the driver settings could not be\r\n"
           "read, 3 no driver profile holds acs.exe, 4 administrator rights needed, 5 the change failed or\r\n"
           "reads back differently, 6 restore: changed since, left as it is, 7 --json could not be written\r\n"
           "or read as a record, 8 another game's predefined driver profile holds acs.exe: not changed";
}

int RunDrs(const DrsArgs& args, DriverSettingsStore& store, const DrsIo& io) {
    Out out(io);
    int code = kDrsExitUsage;
    try {
        switch (args.command) {
            case DrsCommand::Status: code = RunStatusOrOff(args, store, io, out, false); break;
            case DrsCommand::Off: code = RunStatusOrOff(args, store, io, out, true); break;
            case DrsCommand::Restore: code = RunRestore(args, store, io, out); break;
        }
    } catch (...) {
        out.Line("failed: an unexpected exception");
        code = kDrsExitFailed;
    }
    if (!args.log.empty()) {
        std::string error;
        if (!io.writeFile || !io.writeFile(args.log, out.Joined(), &error) ) {
            if (io.print) io.print("the log could not be written to " + Utf8(args.log) + ": " + error);
        }
    }
    return code;
}

int DrsMain(const std::vector<std::wstring>& args, const std::wstring& defaultExe, DriverSettingsStore& store,
            const DrsIo& io) {
    DrsArgs parsed;
    std::string error;
    if (!ParseDrsArgs(args, defaultExe, &parsed, &error)) {
        if (io.print) {
            io.print("ac-dlssg-drs: " + error);
            const std::string usage = DrsUsageText();
            size_t start = 0;
            while (start <= usage.size()) {
                size_t end = usage.find("\r\n", start);
                if (end == std::string::npos) end = usage.size();
                io.print(usage.substr(start, end - start));
                start = end + 2;
            }
        }
        return kDrsExitUsage;
    }
    return RunDrs(parsed, store, io);
}

}  // namespace acdb
