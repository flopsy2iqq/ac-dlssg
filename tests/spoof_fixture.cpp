// A harmless DLL that the spoof-loader tests copy as version.dll. Loading it
// sets ACDB_SPOOF_FIXTURE_LOADED=1 in the process environment.
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) SetEnvironmentVariableW(L"ACDB_SPOOF_FIXTURE_LOADED", L"1");
    return TRUE;
}

extern "C" __declspec(dllexport) int AcdbSpoofFixture() { return 7; }
