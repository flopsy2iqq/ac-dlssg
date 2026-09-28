// A harmless unsigned DLL for the Streamline signature tests. Loading it sets
// ACDB_UNSIGNED_FIXTURE_LOADED=1 in the process environment, so a test can
// tell whether anything ever loaded it.
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) SetEnvironmentVariableW(L"ACDB_UNSIGNED_FIXTURE_LOADED", L"1");
    return TRUE;
}

extern "C" __declspec(dllexport) int AcdbUnsignedFixture() { return 42; }
