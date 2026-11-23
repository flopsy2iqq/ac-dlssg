#pragma once
// NVAPI's driver settings functions (DRS) through nvapi_QueryInterface of
// System32\nvapi64.dll, loaded by absolute path, never a copy from the game
// folder (spec 6.8). Shared by driver_profile.cpp, which the bridge uses and
// which only reads, and drs_store.cpp of ac-dlssg-drs.exe (acdb_drs), which
// also resolves the functions that write. It needs the NVAPI headers, so only
// those two files include it.
#include <windows.h>

#include <nvapi.h>

#include <string>

namespace acdb::nvdrs {

// nvapi_QueryInterface ids, from nvapi_interface.h of the pinned NVAPI commit.
constexpr unsigned kIdInitialize = 0x0150e828;
constexpr unsigned kIdUnload = 0xd22bdd7e;
constexpr unsigned kIdGetErrorMessage = 0x6c2d048c;
constexpr unsigned kIdDrsCreateSession = 0x0694d52e;
constexpr unsigned kIdDrsDestroySession = 0xdad9cff8;
constexpr unsigned kIdDrsLoadSettings = 0x375dbd6b;
constexpr unsigned kIdDrsFindApplicationByName = 0xeee566b2;
constexpr unsigned kIdDrsGetProfileInfo = 0x61cd6fd6;
constexpr unsigned kIdDrsGetBaseProfile = 0xda8466a0;
constexpr unsigned kIdDrsGetSetting = 0x73bf8338;

using QueryInterfaceFn = void*(__cdecl*)(unsigned int id);

template <typename Fn>
bool Resolve(QueryInterfaceFn query, unsigned id, Fn* out) {
    *out = reinterpret_cast<Fn>(query(id));
    return *out != nullptr;
}

// The functions every user of DRS needs: a session, the settings, the
// profile of an exe and the global (base) profile, and a setting's value.
struct Api {
    decltype(&NvAPI_Initialize) Initialize = nullptr;
    decltype(&NvAPI_Unload) Unload = nullptr;
    decltype(&NvAPI_GetErrorMessage) GetErrorMessage = nullptr;  // optional, for error texts
    decltype(&NvAPI_DRS_CreateSession) CreateSession = nullptr;
    decltype(&NvAPI_DRS_DestroySession) DestroySession = nullptr;
    decltype(&NvAPI_DRS_LoadSettings) LoadSettings = nullptr;
    decltype(&NvAPI_DRS_FindApplicationByName) FindApplicationByName = nullptr;
    decltype(&NvAPI_DRS_GetProfileInfo) GetProfileInfo = nullptr;
    decltype(&NvAPI_DRS_GetBaseProfile) GetBaseProfile = nullptr;
    decltype(&NvAPI_DRS_GetSetting) GetSetting = nullptr;
};

// System32\nvapi64.dll's nvapi_QueryInterface, or null with *error. The
// DLL stays loaded: the driver, CSP and Streamline use it too.
QueryInterfaceFn LoadQueryInterface(std::string* error);

// Resolves *api; false with *error "nvapi_QueryInterface has no <name>"
// for the first function that is missing (GetErrorMessage may be).
bool ResolveApi(QueryInterfaceFn query, Api* api, std::string* error);

// "NvAPI_DRS_LoadSettings failed: -3 (NVAPI_NO_IMPLEMENTATION)".
std::string Failed(const Api& api, const char* what, NvAPI_Status status);

// s as NVAPI's NvAPI_UnicodeString, cut to fit; and back.
void ToUnicodeString(const std::wstring& s, NvAPI_UnicodeString* out);
std::wstring FromUnicodeString(const NvU16* s);

// The application profile that holds exePath (a full path: NVAPI then
// returns the profile the driver applies to that very file, while a bare
// "acs.exe" can match another one). *profile is null when none does (not
// found, or the name is ambiguous); false with *error on another failure.
bool FindAppProfile(const Api& api, NvDRSSessionHandle session, const std::wstring& exePath,
                    NvDRSProfileHandle* profile, std::string* error);

// The profile's name; empty when NvAPI_DRS_GetProfileInfo fails.
std::wstring ProfileName(const Api& api, NvDRSSessionHandle session, NvDRSProfileHandle profile);

// Destroys the DRS session on every exit, a C++ exception included.
struct SessionGuard {
    const Api& api;
    NvDRSSessionHandle session;
    ~SessionGuard() { api.DestroySession(session); }
};

// Balances NvAPI_Initialize's reference count on every exit.
struct UnloadGuard {
    const Api& api;
    ~UnloadGuard() { api.Unload(); }
};

}  // namespace acdb::nvdrs
