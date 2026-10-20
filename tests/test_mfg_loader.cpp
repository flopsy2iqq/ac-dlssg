#include <windows.h>

#include <string>

#include "gpu_info.h"
#include "mfg_loader.h"
#include "spoof_loader.h"
#include "test_framework.h"

using namespace acdb;

namespace {

std::wstring TempDir(const wchar_t* leaf) {
    wchar_t buf[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, buf);
    std::wstring dir = std::wstring(buf) + leaf;
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

// Everything that lets the bootstrap load the pinned copy.
MfgInputs Ready() {
    MfgInputs in;
    in.enabled = true;
    in.configured = true;
    in.file_exists = true;
    in.file_sha256 = kMfgPin.dllSha256;
    in.render_adapter_known = true;
    in.render_arch = GpuArch::Ada;
    in.render_adapter = "NVIDIA GeForce RTX 4070 Ti";
    return in;
}

const std::string kOtherSha = "2222222222222222222222222222222222222222222222222222222222222222";

}  // namespace

TEST(Mfg_TheCopyLivesInTheMfgFolderOfTheDataDir) {
    CHECK(MfgPath(L"D:\\Games\\assettocorsa\\ac-dlssg") == L"D:\\Games\\assettocorsa\\ac-dlssg\\mfg\\version.dll");
}

TEST(Mfg_ThePinnedCopyIsLoadedForAnRtx40) {
    const MfgDecision d = DecideMfg(Ready());
    CHECK(d.action == MfgAction::Load);
    CHECK(!d.warn);
    CHECK(d.reason.find(kMfgPin.version) != std::string::npos);
}

// v1.5.0-hotfix.1 is release 1.5.0 plus only the fix for the video memory
// leak (upstream PR #80); the DLL hash is the one of its SHA256SUMS.txt.
TEST(Mfg_ThePinIsV150Hotfix1) {
    CHECK_EQ(std::string(kMfgPin.version), "1.5.0-hotfix.1");
    CHECK_EQ(std::string(kMfgPin.dllSha256), "4ac118f5cbb13a556c84979c2a2d2210a756dce172a9425ff62d870694747a2e");
    const MfgDecision d = DecideMfg(Ready());
    CHECK(d.reason.find("MFG-Unlocker 1.5.0-hotfix.1") != std::string::npos);
    // A copy of the earlier pin (1.5.0) is not loaded: install.bat replaces it.
    MfgInputs in = Ready();
    in.file_sha256 = "97c6b90cc443b3a5d2151fe992e57e37717071a11eaea6054f2474e883de75d7";
    const MfgDecision old = DecideMfg(in);
    CHECK(old.action == MfgAction::Skip);
    CHECK(old.warn);
    CHECK(old.reason.find("install.bat") != std::string::npos);
}

TEST(Mfg_NothingHappensWithoutTheFile) {
    MfgInputs in = Ready();
    in.file_exists = false;
    in.file_sha256.clear();
    const MfgDecision d = DecideMfg(in);
    CHECK(d.action == MfgAction::NotInstalled);
    CHECK(!d.warn);
}

TEST(Mfg_NotLoadedWhenTheBridgeIsDisabled) {
    MfgInputs in = Ready();
    in.enabled = false;
    const MfgDecision d = DecideMfg(in);
    CHECK(d.action == MfgAction::Skip);
    CHECK(d.reason.find("disabled") != std::string::npos);
}

TEST(Mfg_NotLoadedWithMfgUnlockerZero) {
    MfgInputs in = Ready();
    in.configured = false;
    const MfgDecision d = DecideMfg(in);
    CHECK(d.action == MfgAction::Skip);
    CHECK(d.reason.find("mfg_unlocker=0") != std::string::npos);
}

TEST(Mfg_NeverLoadedNextToTheSpoof) {
    MfgInputs in = Ready();
    in.spoof_loaded = true;
    const MfgDecision d = DecideMfg(in);
    CHECK(d.action == MfgAction::Skip);
    CHECK(d.reason.find("dlssg_for_sm86") != std::string::npos);
}

// A game-folder version.dll that is neither the pinned spoof nor
// MFG-Unlocker counts as the spoof (another dlssg_for_sm86 build or another
// mod): MFG-Unlocker stays out, and the log does not call that file
// dlssg_for_sm86 (issue 2).
TEST(Mfg_AnUnidentifiedVersionDllKeepsItOutAndIsNotCalledTheSpoof) {
    MfgInputs in = Ready();
    in.spoof_loaded = true;
    in.spoof_unidentified = true;
    const MfgDecision d = DecideMfg(in);
    CHECK(d.action == MfgAction::Skip);
    CHECK(!d.warn);
    CHECK(d.reason.find("unidentified version.dll") != std::string::npos);
    CHECK(d.reason.find("not the pinned dlssg_for_sm86") != std::string::npos);
    CHECK(d.reason.find("dlssg_for_sm86 (version.dll in the game folder) is loaded") == std::string::npos);
    CHECK(d.reason.find("remove") != std::string::npos);
}

TEST(Mfg_NotLoadedForARenderAdapterThatIsNotAnRtx40) {
    for (GpuArch arch : {GpuArch::Ampere, GpuArch::Blackwell, GpuArch::Turing, GpuArch::NonNvidia, GpuArch::Unknown}) {
        MfgInputs in = Ready();
        in.render_arch = arch;
        const MfgDecision d = DecideMfg(in);
        CHECK(d.action == MfgAction::Skip);
        CHECK(d.reason.find(ArchName(arch)) != std::string::npos);
    }
    MfgInputs in = Ready();
    in.render_adapter_known = false;
    CHECK(DecideMfg(in).action == MfgAction::Skip);
}

// Only the pinned release: another MFG-Unlocker build, dlssg_for_sm86 put
// there by hand, or a file whose hash could not be computed.
TEST(Mfg_AFileThatIsNotThePinIsRefusedWithAWarning) {
    for (const std::string& sha : {kOtherSha, std::string(kPinnedSpoofSha256), std::string()}) {
        MfgInputs in = Ready();
        in.file_sha256 = sha;
        const MfgDecision d = DecideMfg(in);
        CHECK(d.action == MfgAction::Skip);
        CHECK(d.warn);
        CHECK(d.reason.find(kMfgPin.version) != std::string::npos);
        if (!sha.empty()) CHECK(d.reason.find(sha) != std::string::npos);
    }
}

// MFG-Unlocker's own mutex would keep a second copy idle; the one already
// in the process (the user's) is what runs.
TEST(Mfg_AUsersOwnLoadedCopyIsNotJoinedByOurs) {
    MfgInputs in = Ready();
    in.other_copy_loaded = true;
    in.other_copy_path = L"D:\\Games\\assettocorsa\\winmm.dll";
    const MfgDecision d = DecideMfg(in);
    CHECK(d.action == MfgAction::AlreadyLoaded);
    CHECK(d.reason.find("winmm.dll") != std::string::npos);
}

TEST(Mfg_LoadsTheCopyByFullPathAndTimesIt) {
    const HMODULE system = GetModuleHandleW(L"version.dll");
    REQUIRE(system != nullptr);
    const std::wstring data = TempDir(L"acdb_mfg_load");
    CreateDirectoryW((data + L"\\mfg").c_str(), nullptr);
    const std::wstring path = MfgPath(data);
    REQUIRE(CopyFileW(L"" ACDB_MFG_FIXTURE_PATH, path.c_str(), FALSE));
    SetEnvironmentVariableW(L"ACDB_MFG_FIXTURE_LOADED", nullptr);
    std::wstring found;
    CHECK(!FindLoadedMfgCopy(&found));

    double ms = -1;
    std::string err;
    const HMODULE loaded = LoadMfgModule(path, &ms, &err);
    REQUIRE(loaded != nullptr);
    CHECK(loaded != system);
    CHECK(ms >= 0);
    wchar_t mark[4] = {};
    CHECK(GetEnvironmentVariableW(L"ACDB_MFG_FIXTURE_LOADED", mark, 4) == 1);
    CHECK(GetModuleHandleW(L"version.dll") == system);  // the process's binding is unchanged
    CHECK(FindLoadedMfgCopy(&found));
    CHECK(_wcsicmp(found.c_str(), path.c_str()) == 0);
    FreeLibrary(loaded);
    CHECK(!FindLoadedMfgCopy(&found));

    CHECK(LoadMfgModule(data + L"\\mfg\\missing.dll", &ms, &err) == nullptr);
    CHECK(!err.empty());
    DeleteFileW(path.c_str());
    RemoveDirectoryW((data + L"\\mfg").c_str());
    RemoveDirectoryW(data.c_str());
}

TEST(Mfg_TheLoadLineNamesThePathTheFileVersionAndTheTime) {
    CHECK_EQ(MfgLoadedLine("D:/AC/ac-dlssg/mfg/version.dll", 1234.56, "1.5.0.932", true),
             std::string("loaded D:/AC/ac-dlssg/mfg/version.dll (file version 1.5.0.932) in 1235 ms"));
    const std::string odd = MfgLoadedLine("D:/AC/ac-dlssg/mfg/version.dll", 0.4, "", false);
    CHECK(odd.find("in 0 ms") != std::string::npos);
    CHECK(odd.find("file version unknown") != std::string::npos);
    CHECK(odd.find("does not export MfgUnlockSingleModuleQuery") != std::string::npos);
}

// MFG-Unlocker was loaded for the adapter DXGI enumerates first; CSP may
// still render on another one (an Optimus laptop without a GPU preference).
TEST(Mfg_ARenderAdapterThatIsNotAnRtx40IsReportedWhenItIsLoaded) {
    CHECK(MfgRenderAdapterNote(false, false, "Intel(R) Iris(R) Xe Graphics").empty());
    CHECK(MfgRenderAdapterNote(true, true, "NVIDIA GeForce RTX 4070 Laptop GPU").empty());
    const std::string n = MfgRenderAdapterNote(true, false, "Intel(R) Iris(R) Xe Graphics");
    CHECK(n.find("Intel(R) Iris(R) Xe Graphics") != std::string::npos);
    CHECK(n.find("not an RTX 40") != std::string::npos);
}

// The adapter the bootstrap expects the game to render on, before CSP made
// its device: DXGI's first hardware adapter when it is NVIDIA, else the only
// NVIDIA adapter (an Optimus laptop without a GPU preference, where CSP's
// SELECT_ADAPTER may still pick it); the bridge runs only on NVIDIA.
TEST(Mfg_TheRenderAdapterIsPredictedFromTheAdapterList) {
    const AdapterId intel{0x8086, 0xA7A0, false, "Intel(R) Iris(R) Xe Graphics"};
    const AdapterId rtx4070m{0x10DE, 0x2860, false, "NVIDIA GeForce RTX 4070 Laptop GPU"};
    const AdapterId rtx3080{0x10DE, 0x2206, false, "NVIDIA GeForce RTX 3080"};
    const AdapterId warp{0x1414, 0x008C, true, "Microsoft Basic Render Driver"};
    CHECK(!PredictRenderAdapter({}).known);
    CHECK(!PredictRenderAdapter({warp}).known);
    PredictedAdapter p = PredictRenderAdapter({rtx4070m, intel, warp});
    CHECK(p.known && p.name == rtx4070m.name);
    p = PredictRenderAdapter({intel, rtx4070m, warp});
    CHECK(p.known && p.name == rtx4070m.name && p.device == 0x2860u);
    p = PredictRenderAdapter({intel, rtx4070m, rtx3080});  // two NVIDIA adapters: no guess, the first one
    CHECK(p.known && p.name == intel.name);
    p = PredictRenderAdapter({intel});
    CHECK(p.known && p.vendor == 0x8086u);
    p = PredictRenderAdapter({warp, rtx3080});
    CHECK(p.known && p.name == rtx3080.name);
}
