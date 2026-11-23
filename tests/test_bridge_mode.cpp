// Runtime mode (standalone or proxy) from the bridge's own module path, the
// banner line about the game folder's dxgi.dll, and the banner's first line.
#include <regex>
#include <string>

#include "bootstrap.h"
#include "test_framework.h"

using namespace acdb;

// The version comes from CMakeLists.txt; the panel's bridgeVersion holds 31
// characters, and the release workflow expects the tag v<major.minor.patch>.
TEST(Banner_VersionIsMajorMinorPatch) {
    CHECK(std::regex_match(std::string(ACDB_VERSION), std::regex(R"(\d+\.\d+\.\d+)")));
}

TEST(Banner_NamesTheVersionAndWhatTheBridgeDoes) {
    const std::string line = BannerLine();
    std::printf("  %s\n", line.c_str());
    const std::string head = std::string("ac-dlssg ") + ACDB_VERSION + " (";
    CHECK(line.compare(0, head.size(), head) == 0);
    CHECK(!line.empty() && line.back() == ')');
    CHECK(line.find("DLSS Frame Generation 2X/3X/4X") != std::string::npos);
    CHECK(line.find("Reflex") != std::string::npos);
    CHECK(line.find("Streamline") != std::string::npos);
    // A release banner names no development milestone.
    CHECK(!std::regex_search(line, std::regex(R"(\bM\d\b)")));
}

TEST(BridgeMode_DxgiDllIsStandalone) {
    CHECK(BridgeModeFromPath(L"C:\\Games\\assettocorsa\\dxgi.dll") == BridgeMode::Standalone);
    CHECK(BridgeModeFromPath(L"C:\\Games\\assettocorsa\\DXGI.DLL") == BridgeMode::Standalone);
    CHECK(BridgeModeFromPath(L"C:/Games/assettocorsa/Dxgi.dll") == BridgeMode::Standalone);
    CHECK(BridgeModeFromPath(L"dxgi.dll") == BridgeMode::Standalone);
    CHECK(BridgeModeFromPath(L"\\\\?\\C:\\Program Files (x86)\\Steam\\steamapps\\common\\assettocorsa\\dxgi.dll") ==
          BridgeMode::Standalone);
}

TEST(BridgeMode_AnyOtherNameIsProxy) {
    CHECK(BridgeModeFromPath(L"C:\\Games\\assettocorsa\\ac-dlssg.dll") == BridgeMode::Proxy);
    CHECK(BridgeModeFromPath(L"C:\\dxgi.dll\\ac-dlssg.dll") == BridgeMode::Proxy);
    CHECK(BridgeModeFromPath(L"C:\\Games\\assettocorsa\\dxgi.dll.bak") == BridgeMode::Proxy);
    CHECK(BridgeModeFromPath(L"C:\\Games\\assettocorsa\\mydxgi.dll") == BridgeMode::Proxy);
    CHECK(BridgeModeFromPath(L"C:\\Games\\assettocorsa\\dxgi") == BridgeMode::Proxy);
    CHECK(BridgeModeFromPath(L"") == BridgeMode::Proxy);
}

TEST(BridgeMode_Names) {
    CHECK(std::string(BridgeModeName(BridgeMode::Standalone)) == "standalone");
    CHECK(std::string(BridgeModeName(BridgeMode::Proxy)) == "proxy");
}

TEST(BridgeMode_StandaloneBannerNeverCallsTheBridgeReShade) {
    const std::string line = GameFolderDxgiLine(true, true, "C:\\AC\\dxgi.dll", "no version resource");
    CHECK(line == "dxgi.dll: C:\\AC\\dxgi.dll is this bridge (standalone), not ReShade");
    CHECK(line.find("ReShade:") == std::string::npos);
}

TEST(BridgeMode_ProxyBannerNamesReShade) {
    CHECK(GameFolderDxgiLine(true, false, "C:\\AC\\dxgi.dll", "6.8.0.2155") ==
          "ReShade: C:\\AC\\dxgi.dll, version 6.8.0.2155");
    CHECK(GameFolderDxgiLine(false, false, "", "") == "ReShade: dxgi.dll is not loaded from the game folder");
}
