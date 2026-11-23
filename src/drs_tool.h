#pragma once
// ac-dlssg-drs.exe, the installer's driver-profile helper: its command line,
// its commands and its exit codes. Plain console output, no network, no
// question asked:
//
//   ac-dlssg-drs.exe smooth-motion status  [--exe <acs.exe>] [--json <file>] [--log <file>]
//   ac-dlssg-drs.exe smooth-motion off     [--exe <acs.exe>] --json <file> [--log <file>]
//   ac-dlssg-drs.exe smooth-motion restore --json <record> [--log <file>]
//
// status reads the two Smooth Motion settings (smooth_motion.h) of the
// driver profile that holds acs.exe and of the global profile, and says
// what off would do. off clears the DX11 bit of "Smooth Motion - Enabled
// APIs" in that profile (PlanSmoothMotionOff) and writes the record of the
// change into --json: before, written and the profile; the installer keeps
// it in its manifest. restore reads such a record and puts the old state
// back (PlanRestore), only while the profile still holds the value written.
// Every change is read back from a fresh load of the driver settings.
// --exe defaults to the acs.exe one folder above the helper's own (it is
// installed as <game>\ac-dlssg\ac-dlssg-drs.exe). --log also writes the
// text into a file (UTF-8): the console window of an elevated run closes
// with it. Writing the driver settings needs administrator rights; reading
// them does not.
#include <functional>
#include <string>
#include <vector>

#include "drs_store.h"

namespace acdb {

constexpr int kDrsExitOk = 0;          // done, nothing to change, or (status) read
constexpr int kDrsExitUsage = 1;       // a bad command line: nothing was read or changed
constexpr int kDrsExitNoDriver = 2;    // NVAPI or the driver settings could not be read
constexpr int kDrsExitNoProfile = 3;   // no driver profile holds acs.exe; restore: the recorded profile is gone
constexpr int kDrsExitNeedsAdmin = 4;  // saving needs administrator rights; nothing was changed
constexpr int kDrsExitFailed = 5;      // the change failed, or the driver settings read back differently
constexpr int kDrsExitChanged = 6;     // restore: changed since it was written; left as it is
constexpr int kDrsExitFile = 7;        // --json could not be written, or (restore) read as a record

enum class DrsCommand { Status, Off, Restore };

struct DrsArgs {
    DrsCommand command = DrsCommand::Status;
    std::wstring exe;   // status and off
    std::wstring json;  // status: optional output; off: output; restore: input
    std::wstring log;   // optional
};

// Pure: the arguments after the program name. defaultExe stands in for a
// missing --exe (empty when there is none: then status and off need it).
// False with *error for a command line that is not one of the three forms.
bool ParseDrsArgs(const std::vector<std::wstring>& args, const std::wstring& defaultExe, DrsArgs* out,
                  std::string* error);

// The usage text: the three forms and the exit codes.
std::string DrsUsageText();

// What a command touches besides the driver settings, so that the tests
// run it in memory: one line of text (UTF-8) for the console, and files.
struct DrsIo {
    std::function<void(const std::string& line)> print;
    std::function<bool(const std::wstring& path, std::string* utf8, std::string* error)> readFile;
    std::function<bool(const std::wstring& path, const std::string& utf8, std::string* error)> writeFile;
};

// Runs one command and returns its exit code. Its JSON result (status and
// off) is written also when the command fails, with "result" and
// "exitCode" saying how; the log, when asked for, at the end.
int RunDrs(const DrsArgs& args, DriverSettingsStore& store, const DrsIo& io);

// The program: ParseDrsArgs (on failure the reason and the usage, exit 1),
// then RunDrs.
int DrsMain(const std::vector<std::wstring>& args, const std::wstring& defaultExe, DriverSettingsStore& store,
            const DrsIo& io);

}  // namespace acdb
