// StreamlineRuntime without sl.interposer.dll: the slInit preferences (spec
// 6.8), Init's failure paths and the calls that must refuse before Init. The
// runtime keeps process-wide state, so those tests run in child processes.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>

#include "child_process.h"
#include "streamline_runtime.h"
#include "temp_dir.h"
#include "test_framework.h"

using namespace acdb;

namespace {

void TestLogCallback(sl::LogType, const char*) {}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
}

std::wstring FixturePath() {
    std::wstring p = L"" ACDB_UNSIGNED_FIXTURE_PATH;
    for (auto& c : p) {
        if (c == L'/') c = L'\\';
    }
    return p;
}

constexpr wchar_t kFixtureMark[] = L"ACDB_UNSIGNED_FIXTURE_LOADED";

bool FixtureMarkSet() {
    wchar_t buf[8] = {};
    return GetEnvironmentVariableW(kFixtureMark, buf, 8) > 0;
}

uint64_t Bits(sl::PreferenceFlags f) { return static_cast<uint64_t>(f); }

}  // namespace

// --- BuildPreferences ------------------------------------------------------

TEST(SlPrefs_FlagsAreExactlyManualHookingFrameTaggingAndNoClStateTracking) {
    SlPreferencesStorage st;
    const sl::Preferences p = BuildPreferences(L"C:\\g\\ac-dlssg\\sl", L"C:\\g\\ac-dlssg\\logs", "0.1.0",
                                               &TestLogCallback, &st);
    const uint64_t expected = Bits(sl::PreferenceFlags::eUseManualHooking) |
                              Bits(sl::PreferenceFlags::eUseFrameBasedResourceTagging) |
                              Bits(sl::PreferenceFlags::eDisableCLStateTracking);
    CHECK_EQ(Bits(p.flags), expected);
    CHECK_EQ(Bits(p.flags) & Bits(sl::PreferenceFlags::eAllowOTA), 0u);
    CHECK_EQ(Bits(p.flags) & Bits(sl::PreferenceFlags::eLoadDownloadedPlugins), 0u);
}

TEST(SlPrefs_LoadsDlssgReflexAndPcl) {
    SlPreferencesStorage st;
    const sl::Preferences p = BuildPreferences(L"C:\\sl", L"C:\\logs", "0.1.0", nullptr, &st);
    REQUIRE(p.featuresToLoad != nullptr);
    REQUIRE(p.numFeaturesToLoad == 3u);
    CHECK_EQ(p.featuresToLoad[0], sl::kFeatureDLSS_G);
    CHECK_EQ(p.featuresToLoad[1], sl::kFeatureReflex);
    CHECK_EQ(p.featuresToLoad[2], sl::kFeaturePCL);
    CHECK(p.featuresToLoad == st.features);
}

TEST(SlPrefs_PathsPointAtOurFoldersAndOutliveTheCall) {
    SlPreferencesStorage st;
    sl::Preferences p;
    {
        const std::wstring plugins = L"D:\\Games\\assettocorsa\\ac-dlssg\\sl";
        const std::wstring logs = L"D:\\Games\\assettocorsa\\ac-dlssg\\logs";
        p = BuildPreferences(plugins, logs, "0.1.0", nullptr, &st);
    }
    // The arguments are gone; the storage holds the strings.
    REQUIRE(p.pathsToPlugins != nullptr);
    REQUIRE(p.numPathsToPlugins == 1u);
    REQUIRE(p.pathsToPlugins[0] != nullptr);
    CHECK(std::wstring(p.pathsToPlugins[0]) == L"D:\\Games\\assettocorsa\\ac-dlssg\\sl");
    CHECK(p.pathsToPlugins == st.plugin_paths);
    REQUIRE(p.pathToLogsAndData != nullptr);
    CHECK(std::wstring(p.pathToLogsAndData) == L"D:\\Games\\assettocorsa\\ac-dlssg\\logs");
}

TEST(SlPrefs_IdentifiesACustomD3D12EngineWithProjectAndVersion) {
    SlPreferencesStorage st;
    const sl::Preferences p = BuildPreferences(L"C:\\sl", L"C:\\logs", ACDB_VERSION, &TestLogCallback, &st);
    CHECK(p.renderAPI == sl::RenderAPI::eD3D12);
    CHECK(p.engine == sl::EngineType::eCustom);
    CHECK_EQ(p.applicationId, 0u);
    // Production Streamline disables NGX features unless both are non-empty.
    REQUIRE(p.engineVersion != nullptr);
    CHECK(std::strcmp(p.engineVersion, ACDB_VERSION) == 0);
    CHECK(std::strlen(p.engineVersion) > 0);
    REQUIRE(p.projectId != nullptr);
    CHECK(std::strcmp(p.projectId, kSlProjectId) == 0);
    // A GUID in the 8-4-4-4-12 form Streamline documents.
    const std::string id = p.projectId;
    REQUIRE(id.size() == 36u);
    for (size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            CHECK_EQ(id[i], '-');
        } else {
            CHECK(std::iswxdigit(static_cast<wint_t>(id[i])) != 0);
        }
    }
}

TEST(SlPrefs_LogsThroughTheCallbackWithoutConsole) {
    SlPreferencesStorage st;
    const sl::Preferences p = BuildPreferences(L"C:\\sl", L"C:\\logs", "0.1.0", &TestLogCallback, &st);
    CHECK(p.logMessageCallback == &TestLogCallback);
    CHECK(p.logLevel == sl::LogLevel::eDefault);
    CHECK(!p.showConsole);
    CHECK(p.allocateCallback == nullptr);
    CHECK(p.releaseCallback == nullptr);
    const sl::Preferences noCallback = BuildPreferences(L"C:\\sl", L"C:\\logs", "0.1.0", nullptr, &st);
    CHECK(noCallback.logMessageCallback == nullptr);
}

// --- Init failure paths (child processes) ------------------------------------

TEST(SlInit_MissingPluginDirFailsNamingTheFile) {
    CHECK_EQ(acdb_test::RunChildTest("Child_SlInit_MissingPluginDir"), 0);
}

TEST(Child_SlInit_MissingPluginDir) {
    acdb_test::TempDir tmp(L"sl_missing");
    const std::wstring dir = tmp.Str() + L"\\no-such-dir";
    StreamlineRuntime& rt = StreamlineRuntime::Get();
    std::string err;
    CHECK(!rt.Init(dir, tmp.Str() + L"\\logs", &err));
    std::printf("  error: %s\n", err.c_str());
    CHECK(err.find(Narrow(dir) + "\\sl.interposer.dll") != std::string::npos);
    CHECK(!rt.Initialized());
    CHECK(!rt.IsShutDown());
    // Idempotent: a second call returns the first result, whatever its arguments.
    std::string again;
    CHECK(!rt.Init(tmp.Str(), tmp.Str(), &again));
    CHECK(again == err);
    CHECK(GetModuleHandleW(L"sl.interposer.dll") == nullptr);
}

TEST(SlInit_UnsignedInterposerIsNeverLoaded) {
    CHECK_EQ(acdb_test::RunChildTest("Child_SlInit_UnsignedInterposer"), 0);
}

TEST(Child_SlInit_UnsignedInterposer) {
    acdb_test::TempDir tmp(L"sl_unsigned");
    const std::wstring dir = tmp.Str() + L"\\sl";
    REQUIRE(CreateDirectoryW(dir.c_str(), nullptr));
    const std::wstring dll = dir + L"\\sl.interposer.dll";
    REQUIRE(CopyFileW(FixturePath().c_str(), dll.c_str(), FALSE));
    SetEnvironmentVariableW(kFixtureMark, nullptr);

    std::string err;
    CHECK(!StreamlineRuntime::Get().Init(dir, tmp.Str() + L"\\logs", &err));
    std::printf("  error: %s\n", err.c_str());
    CHECK(err.find(Narrow(dll)) != std::string::npos);
    CHECK(err.find("signature") != std::string::npos);
    CHECK(!StreamlineRuntime::Get().Initialized());
    CHECK(GetModuleHandleW(dll.c_str()) == nullptr);
    CHECK(GetModuleHandleW(L"sl.interposer.dll") == nullptr);
    CHECK(!FixtureMarkSet());

    // Control: the copy is a loadable DLL whose DllMain leaves the mark.
    const HMODULE loaded = LoadLibraryW(dll.c_str());
    REQUIRE(loaded != nullptr);
    CHECK(FixtureMarkSet());
    FreeLibrary(loaded);
}

#ifdef ACDB_SL_BIN_DIR
// Only built when the Streamline DLLs are staged (CMakeLists.txt).
TEST(SlInit_SignedDllWithoutSlExportsIsVerifiedLoadedAndUnloaded) {
    CHECK_EQ(acdb_test::RunChildTest("Child_SlInit_SignedNonInterposer"), 0);
}

TEST(Child_SlInit_SignedNonInterposer) {
    // sl.common.dll carries Streamline's signatures but no sl* exports: Init
    // verifies it while the file is pinned, loads it, finds no slInit and
    // unloads it again, so slInit is never reached.
    std::wstring source = L"" ACDB_SL_BIN_DIR L"/sl.common.dll";
    for (auto& c : source) {
        if (c == L'/') c = L'\\';
    }
    acdb_test::TempDir tmp(L"sl_signed");
    const std::wstring dir = tmp.Str() + L"\\sl";
    REQUIRE(CreateDirectoryW(dir.c_str(), nullptr));
    const std::wstring dll = dir + L"\\sl.interposer.dll";
    REQUIRE(CopyFileW(source.c_str(), dll.c_str(), FALSE));

    std::string err;
    CHECK(!StreamlineRuntime::Get().Init(dir, tmp.Str() + L"\\logs", &err));
    std::printf("  error: %s\n", err.c_str());
    CHECK(err.find("has no export slInit") != std::string::npos);
    CHECK(err.find("signature") == std::string::npos);
    CHECK(!StreamlineRuntime::Get().Initialized());
    CHECK(GetModuleHandleW(dll.c_str()) == nullptr);
    // The pin is gone: the file can be replaced again.
    CHECK(DeleteFileW(dll.c_str()));
}
#endif

TEST(SlRuntime_CallsBeforeInitAreRefused) {
    CHECK_EQ(acdb_test::RunChildTest("Child_SlRuntime_CallsBeforeInit"), 0);
}

TEST(Child_SlRuntime_CallsBeforeInit) {
    StreamlineRuntime& rt = StreamlineRuntime::Get();
    CHECK(!rt.Initialized());
    std::string err;
    CHECK(!rt.SetDevice(nullptr, &err));
    CHECK(!err.empty());
    std::string why;
    CHECK(!rt.DlssgSupported(LUID{}, &why));
    CHECK(!why.empty());
    void* iface = nullptr;
    CHECK(!rt.Upgrade(&iface));
    CHECK(!rt.IsProxied(nullptr));
    CHECK(!rt.EnableReflexLowLatency());
    CHECK(!rt.ReflexLowLatencyAvailable());
    CHECK(rt.NewFrameToken(1) == nullptr);
    CHECK_EQ(rt.ErrorsLogged(), 0u);
    CHECK_EQ(rt.WarningsLogged(), 0u);

    // Shutdown closes the runtime for the rest of the process, Init included.
    rt.Shutdown();
    CHECK(rt.IsShutDown());
    CHECK(!rt.Initialized());
    rt.Shutdown();
    CHECK(rt.IsShutDown());
    acdb_test::TempDir tmp(L"sl_closed");
    CHECK(!rt.Init(tmp.Str(), tmp.Str(), &err));
    std::printf("  error after Shutdown: %s\n", err.c_str());
    CHECK(err.find("shut down") != std::string::npos);
    CHECK(!rt.Initialized());
    CHECK(GetModuleHandleW(L"sl.interposer.dll") == nullptr);
}

// The harness itself: a child started for a test that does not exist must
// fail, or a renamed Child_ test would pass without running.
TEST(ChildProcess_UnknownChildTestFails) {
    CHECK(acdb_test::RunChildTest("Child_NoTestHasThisName") != 0);
}

// --- LogLoadedModules --------------------------------------------------------

TEST(SlRuntime_LogLoadedModulesCountsCopiesOutsideThePluginDir) {
    CHECK_EQ(acdb_test::RunChildTest("Child_SlRuntime_LogLoadedModules"), 0);
}

TEST(Child_SlRuntime_LogLoadedModules) {
    acdb_test::TempDir tmp(L"sl_modules");
    const std::wstring dir = tmp.Str() + L"\\sl";
    const std::wstring other = tmp.Str() + L"\\other";
    REQUIRE(CreateDirectoryW(dir.c_str(), nullptr));
    REQUIRE(CreateDirectoryW(other.c_str(), nullptr));
    StreamlineRuntime& rt = StreamlineRuntime::Get();
    // Init fails (no interposer) but records the plugin directory; the
    // comparison ignores case and a trailing separator.
    std::wstring upper = dir + L"\\";
    for (auto& c : upper) c = static_cast<wchar_t>(std::towupper(c));
    std::string err;
    CHECK(!rt.Init(upper, tmp.Str() + L"\\logs", &err));
    CHECK_EQ(rt.LogLoadedModules(), 0);

    const std::wstring inside = dir + L"\\sl.fixture.dll";
    const std::wstring outside = other + L"\\nvngx_dlssg.dll";
    const std::wstring unrelated = other + L"\\unrelated.dll";
    for (const auto* p : {&inside, &outside, &unrelated}) REQUIRE(CopyFileW(FixturePath().c_str(), p->c_str(), FALSE));
    const HMODULE a = LoadLibraryW(inside.c_str());
    const HMODULE b = LoadLibraryW(outside.c_str());
    const HMODULE c = LoadLibraryW(unrelated.c_str());
    REQUIRE(a && b && c);
    CHECK_EQ(rt.LogLoadedModules(), 1);
    FreeLibrary(c);
    FreeLibrary(b);
    FreeLibrary(a);
    CHECK_EQ(rt.LogLoadedModules(), 0);
}
