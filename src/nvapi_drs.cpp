#include "nvapi_drs.h"

#include <cstdio>
#include <cstring>
#include <memory>

namespace acdb::nvdrs {

QueryInterfaceFn LoadQueryInterface(std::string* error) {
    wchar_t sysDir[MAX_PATH] = {};
    const UINT len = GetSystemDirectoryW(sysDir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        *error = "GetSystemDirectoryW failed";
        return nullptr;
    }
    // Absolute System32 path: never a nvapi64.dll from the game folder.
    const std::wstring path = std::wstring(sysDir) + L"\\nvapi64.dll";
    const HMODULE nvapi = LoadLibraryW(path.c_str());
    if (!nvapi) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "System32\\nvapi64.dll not loaded (error %lu); no NVIDIA driver",
                      static_cast<unsigned long>(GetLastError()));
        *error = buf;
        return nullptr;
    }
    const auto query = reinterpret_cast<QueryInterfaceFn>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (!query) *error = "nvapi64.dll has no nvapi_QueryInterface export";
    return query;
}

bool ResolveApi(QueryInterfaceFn query, Api* api, std::string* error) {
    Resolve(query, kIdGetErrorMessage, &api->GetErrorMessage);  // optional, for error texts
    const char* missing = nullptr;
    if (!Resolve(query, kIdInitialize, &api->Initialize)) missing = "NvAPI_Initialize";
    else if (!Resolve(query, kIdUnload, &api->Unload)) missing = "NvAPI_Unload";
    else if (!Resolve(query, kIdDrsCreateSession, &api->CreateSession)) missing = "NvAPI_DRS_CreateSession";
    else if (!Resolve(query, kIdDrsDestroySession, &api->DestroySession)) missing = "NvAPI_DRS_DestroySession";
    else if (!Resolve(query, kIdDrsLoadSettings, &api->LoadSettings)) missing = "NvAPI_DRS_LoadSettings";
    else if (!Resolve(query, kIdDrsFindApplicationByName, &api->FindApplicationByName))
        missing = "NvAPI_DRS_FindApplicationByName";
    else if (!Resolve(query, kIdDrsGetProfileInfo, &api->GetProfileInfo)) missing = "NvAPI_DRS_GetProfileInfo";
    else if (!Resolve(query, kIdDrsGetBaseProfile, &api->GetBaseProfile)) missing = "NvAPI_DRS_GetBaseProfile";
    else if (!Resolve(query, kIdDrsGetSetting, &api->GetSetting)) missing = "NvAPI_DRS_GetSetting";
    if (!missing) return true;
    *error = std::string("nvapi_QueryInterface has no ") + missing;
    return false;
}

std::string Failed(const Api& api, const char* what, NvAPI_Status status) {
    NvAPI_ShortString text = {};
    const bool haveText = api.GetErrorMessage && api.GetErrorMessage(status, text) == NVAPI_OK;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s failed: %d%s%s%s", what, static_cast<int>(status), haveText ? " (" : "",
                  haveText ? text : "", haveText ? ")" : "");
    return buf;
}

void ToUnicodeString(const std::wstring& s, NvAPI_UnicodeString* out) {
    std::memset(*out, 0, sizeof(*out));
    const size_t n = s.size() < NVAPI_UNICODE_STRING_MAX - 1 ? s.size() : NVAPI_UNICODE_STRING_MAX - 1;
    for (size_t i = 0; i < n; ++i) (*out)[i] = static_cast<NvU16>(s[i]);
}

std::wstring FromUnicodeString(const NvU16* s) {
    std::wstring w;
    for (size_t i = 0; i < NVAPI_UNICODE_STRING_MAX && s[i]; ++i) w.push_back(static_cast<wchar_t>(s[i]));
    return w;
}

bool FindAppProfile(const Api& api, NvDRSSessionHandle session, const std::wstring& exePath,
                    NvDRSProfileHandle* profile, std::string* error) {
    *profile = nullptr;
    NvAPI_UnicodeString name;
    ToUnicodeString(exePath, &name);
    auto app = std::make_unique<NVDRS_APPLICATION>();
    app->version = NVDRS_APPLICATION_VER;
    NvDRSProfileHandle found = nullptr;
    const NvAPI_Status status = api.FindApplicationByName(session, name, &found, app.get());
    if (status == NVAPI_OK && found) {
        *profile = found;
        return true;
    }
    // Not found or ambiguous: no app profile applies to that name.
    if (status == NVAPI_OK || status == NVAPI_EXECUTABLE_NOT_FOUND || status == NVAPI_PROFILE_NOT_FOUND ||
        status == NVAPI_EXECUTABLE_PATH_IS_AMBIGUOUS)
        return true;
    *error = Failed(api, "NvAPI_DRS_FindApplicationByName", status);
    return false;
}

std::wstring ProfileName(const Api& api, NvDRSSessionHandle session, NvDRSProfileHandle profile) {
    auto info = std::make_unique<NVDRS_PROFILE>();
    info->version = NVDRS_PROFILE_VER;
    if (api.GetProfileInfo(session, profile, info.get()) != NVAPI_OK) return {};
    return FromUnicodeString(info->profileName);
}

}  // namespace acdb::nvdrs
