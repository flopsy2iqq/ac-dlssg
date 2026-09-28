#include <windows.h>

#include "bootstrap.h"

// Runs under the loader lock: at process start when the bridge is the game's
// dxgi.dll (standalone), or while ReShade holds its own export-module lock
// when it loads us as its ProxyLibrary. So nothing here may load libraries or
// call DXGI (spec 6.1). The bootstrap runs later, on the first
// CreateDXGIFactory* call.
// No DisableThreadLibraryCalls: the static CRT expects thread notifications.
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) acdb::SetBridgeModule(module);
    // Nothing on DLL_PROCESS_DETACH: ReShade frees us under the loader lock,
    // and as the game's dxgi.dll we are detached at process exit.
    return TRUE;
}
