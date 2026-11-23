#include "drs_store.h"

#include <cstdio>
#include <memory>

#include "nvapi_drs.h"

namespace acdb {
namespace {

// nvapi_QueryInterface ids of the functions only the helper needs, from
// nvapi_interface.h of the pinned NVAPI commit.
constexpr unsigned kIdDrsFindProfileByName = 0x7e4a9a0b;
constexpr unsigned kIdDrsSetSetting = 0x577dd202;
constexpr unsigned kIdDrsDeleteProfileSetting = 0xe4a26362;
constexpr unsigned kIdDrsSaveSettings = 0xfcbc7e14;

struct WriteApi : nvdrs::Api {
    decltype(&NvAPI_DRS_FindProfileByName) FindProfileByName = nullptr;
    decltype(&NvAPI_DRS_SetSetting) SetSetting = nullptr;
    decltype(&NvAPI_DRS_DeleteProfileSetting) DeleteProfileSetting = nullptr;
    decltype(&NvAPI_DRS_SaveSettings) SaveSettings = nullptr;
};

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string WithId(const char* what, NvU32 id) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s(0x%08X)", what, static_cast<unsigned>(id));
    return buf;
}

// One session at a time: Read loads the settings afresh into a new one, and
// Write changes and saves the profile that Read found in it.
class NvapiStore final : public DriverSettingsStore {
public:
    ~NvapiStore() override {
        CloseSession();
        if (initialized_) api_.Unload();
    }

    DrsStatus Read(const std::wstring& exePath, const std::wstring& profileName, SmoothMotionProfile* out,
                   std::string* error) override {
        try {
            return ReadImpl(exePath, profileName, out, error);
        } catch (...) {
            CloseSession();
            *error = "unexpected exception while reading the driver settings";
            return DrsStatus::NoDriver;
        }
    }

    DrsStatus Write(uint32_t settingId, std::optional<uint32_t> value, std::string* error) override {
        try {
            return WriteImpl(settingId, value, error);
        } catch (...) {
            *error = "unexpected exception while writing the driver settings";
            return DrsStatus::Failed;
        }
    }

private:
    bool Load(std::string* error) {
        if (initialized_) return true;
        if (load_error_.empty()) {
            const nvdrs::QueryInterfaceFn query = nvdrs::LoadQueryInterface(&load_error_);
            if (query && nvdrs::ResolveApi(query, &api_, &load_error_)) {
                const char* missing = nullptr;
                if (!nvdrs::Resolve(query, kIdDrsFindProfileByName, &api_.FindProfileByName))
                    missing = "NvAPI_DRS_FindProfileByName";
                else if (!nvdrs::Resolve(query, kIdDrsSetSetting, &api_.SetSetting)) missing = "NvAPI_DRS_SetSetting";
                else if (!nvdrs::Resolve(query, kIdDrsDeleteProfileSetting, &api_.DeleteProfileSetting))
                    missing = "NvAPI_DRS_DeleteProfileSetting";
                else if (!nvdrs::Resolve(query, kIdDrsSaveSettings, &api_.SaveSettings))
                    missing = "NvAPI_DRS_SaveSettings";
                if (missing) {
                    load_error_ = std::string("nvapi_QueryInterface has no ") + missing;
                } else {
                    const NvAPI_Status status = api_.Initialize();
                    if (status == NVAPI_OK) {
                        initialized_ = true;
                        return true;
                    }
                    load_error_ = nvdrs::Failed(api_, "NvAPI_Initialize", status);
                }
            }
        }
        *error = load_error_;
        return false;
    }

    void CloseSession() {
        if (session_) api_.DestroySession(session_);
        session_ = nullptr;
        profile_ = nullptr;
    }

    // ownOnly: the profile's own value (a value it only inherits is not
    // present); else any value that is not the driver's default.
    bool ReadValue(NvDRSProfileHandle profile, NvU32 id, bool ownOnly, ProfileValue* out, std::string* error) {
        *out = ProfileValue();
        auto setting = std::make_unique<NVDRS_SETTING>();
        setting->version = NVDRS_SETTING_VER;
        const NvAPI_Status status = api_.GetSetting(session_, profile, id, setting.get());
        if (status == NVAPI_SETTING_NOT_FOUND) return true;
        if (status != NVAPI_OK) {
            *error = nvdrs::Failed(api_, WithId("NvAPI_DRS_GetSetting", id).c_str(), status);
            return false;
        }
        if (setting->settingType != NVDRS_DWORD_TYPE) return true;
        const NVDRS_SETTING_LOCATION loc = setting->settingLocation;
        if (ownOnly && loc != NVDRS_CURRENT_PROFILE_LOCATION) return true;
        if (!ownOnly && loc == NVDRS_DEFAULT_PROFILE_LOCATION) return true;
        out->present = true;
        out->value = setting->u32CurrentValue;
        out->predefined = setting->isCurrentPredefined != 0;
        return true;
    }

    DrsStatus ReadImpl(const std::wstring& exePath, const std::wstring& profileName, SmoothMotionProfile* out,
                       std::string* error) {
        *out = SmoothMotionProfile();
        CloseSession();
        if (!Load(error)) return DrsStatus::NoDriver;
        NvDRSSessionHandle session = nullptr;
        NvAPI_Status status = api_.CreateSession(&session);
        if (status != NVAPI_OK) {
            *error = nvdrs::Failed(api_, "NvAPI_DRS_CreateSession", status);
            return DrsStatus::NoDriver;
        }
        session_ = session;
        status = api_.LoadSettings(session_);
        if (status != NVAPI_OK) {
            *error = nvdrs::Failed(api_, "NvAPI_DRS_LoadSettings", status);
            CloseSession();
            return DrsStatus::NoDriver;
        }
        NvDRSProfileHandle profile = nullptr;
        if (profileName.empty()) {
            if (!nvdrs::FindAppProfile(api_, session_, exePath, &profile, error)) {
                CloseSession();
                return DrsStatus::NoDriver;
            }
            if (!profile) {
                *error = "NvAPI_DRS_FindApplicationByName found no profile for " + Utf8(exePath);
                CloseSession();
                return DrsStatus::NoProfile;
            }
        } else {
            NvAPI_UnicodeString name;
            nvdrs::ToUnicodeString(profileName, &name);
            status = api_.FindProfileByName(session_, name, &profile);
            if (status == NVAPI_PROFILE_NOT_FOUND || (status == NVAPI_OK && !profile)) {
                *error = "no driver profile is named \"" + Utf8(profileName) + "\"";
                CloseSession();
                return DrsStatus::NoProfile;
            }
            if (status != NVAPI_OK) {
                *error = nvdrs::Failed(api_, "NvAPI_DRS_FindProfileByName", status);
                CloseSession();
                return DrsStatus::NoDriver;
            }
        }
        const nvdrs::ProfileInfo info = nvdrs::ReadProfileInfo(api_, session_, profile);
        out->name = info.name.empty() ? profileName : info.name;
        out->predefined = info.predefined;
        NvDRSProfileHandle base = nullptr;
        status = api_.GetBaseProfile(session_, &base);
        if (status != NVAPI_OK) {
            *error = nvdrs::Failed(api_, "NvAPI_DRS_GetBaseProfile", status);
            CloseSession();
            return DrsStatus::NoDriver;
        }
        if (!ReadValue(profile, kDrsSmoothMotionApis, true, &out->apis, error) ||
            !ReadValue(profile, kDrsSmoothMotionEnable, true, &out->enable, error) ||
            !ReadValue(base, kDrsSmoothMotionApis, false, &out->globalApis, error) ||
            !ReadValue(base, kDrsSmoothMotionEnable, false, &out->globalEnable, error)) {
            CloseSession();
            return DrsStatus::NoDriver;
        }
        profile_ = profile;
        return DrsStatus::Ok;
    }

    static DrsStatus StatusOf(NvAPI_Status status) {
        return status == NVAPI_INVALID_USER_PRIVILEGE ? DrsStatus::NeedsAdmin : DrsStatus::Failed;
    }

    DrsStatus WriteImpl(uint32_t settingId, std::optional<uint32_t> value, std::string* error) {
        if (!session_ || !profile_) {
            *error = "no driver profile was read before the write";
            return DrsStatus::Failed;
        }
        NvAPI_Status status;
        if (value) {
            auto setting = std::make_unique<NVDRS_SETTING>();
            setting->version = NVDRS_SETTING_VER;
            setting->settingId = settingId;
            setting->settingType = NVDRS_DWORD_TYPE;
            setting->u32CurrentValue = *value;
            status = api_.SetSetting(session_, profile_, setting.get());
            if (status != NVAPI_OK) {
                *error = nvdrs::Failed(api_, WithId("NvAPI_DRS_SetSetting", settingId).c_str(), status);
                return StatusOf(status);
            }
        } else {
            status = api_.DeleteProfileSetting(session_, profile_, settingId);
            // NVAPI_SETTING_NOT_FOUND: the profile has no value of its own to delete.
            if (status != NVAPI_OK && status != NVAPI_SETTING_NOT_FOUND) {
                *error = nvdrs::Failed(api_, WithId("NvAPI_DRS_DeleteProfileSetting", settingId).c_str(), status);
                return StatusOf(status);
            }
        }
        status = api_.SaveSettings(session_);
        if (status != NVAPI_OK) {
            *error = nvdrs::Failed(api_, "NvAPI_DRS_SaveSettings", status);
            return StatusOf(status);
        }
        return DrsStatus::Ok;
    }

    WriteApi api_;
    bool initialized_ = false;
    std::string load_error_;
    NvDRSSessionHandle session_ = nullptr;
    NvDRSProfileHandle profile_ = nullptr;
};

}  // namespace

std::unique_ptr<DriverSettingsStore> MakeNvapiStore() { return std::make_unique<NvapiStore>(); }

}  // namespace acdb
