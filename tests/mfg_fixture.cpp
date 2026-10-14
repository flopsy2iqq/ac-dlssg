// A harmless stand-in for MFG-Unlocker: it exports the function by which a
// copy of MFG-Unlocker is recognised (MfgUnlockSingleModuleQuery) and, when
// loaded, sets ACDB_MFG_FIXTURE_LOADED=1 in the process environment. It
// patches nothing. The tests copy it as version.dll into a game folder or
// into <game>\ac-dlssg\mfg; it is never the real MFG-Unlocker.
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) SetEnvironmentVariableW(L"ACDB_MFG_FIXTURE_LOADED", L"1");
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL WINAPI MfgUnlockSingleModuleQuery(void*) { return FALSE; }
