// Runtime mode (standalone or proxy) from the bridge's own module path, and
// the banner line about the game folder's dxgi.dll.
#include <string>

#include "bootstrap.h"
#include "test_framework.h"

using namespace acdb;

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
