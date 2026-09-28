#pragma once
// A fake NGX module and parameter object for the NgxHook tests. It exports the
// three D3D11 entry points with NVIDIA's exact signatures and records what its
// "original" was called with, so a test can prove the hook forwarded first and
// returned the result unchanged. The parameter object is header-only so a test
// can build one, set keys and pass it through the entry points; the hook reads
// it through acdb::NgxParameter.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ngx_params.h"

// State the test configures and inspects. One instance per process.
struct FakeNgxState {
    volatile long createCalls = 0;
    volatile long evalCalls = 0;
    volatile long evalCCalls = 0;
    int createResult = acdb::kNgxSuccess;  // what the original create returns
    int evalResult = acdb::kNgxSuccess;    // what the original evaluate returns
    void* nextHandle = nullptr;            // *outHandle the create hands back
    int nestReentryDepth = 0;              // >0: EvaluateFeature reenters _C that deep
    // The maximum nesting depth the fake's own body observed (to prove the hook
    // saw the reentry).
    volatile long maxReentryObserved = 0;
    volatile long reentryCounter = 0;
};

extern "C" {
__declspec(dllexport) FakeNgxState* FakeNgxGetState();
// Signatures mirror nvsdk_ngx.h. featureId is the NVSDK_NGX_Feature enum (int).
__declspec(dllexport) int __cdecl NVSDK_NGX_D3D11_CreateFeature(void* ctx, unsigned int featureId,
                                                                void* params, void** outHandle);
__declspec(dllexport) int __cdecl NVSDK_NGX_D3D11_EvaluateFeature(void* ctx, const void* handle,
                                                                  const void* params, void* callback);
__declspec(dllexport) int __cdecl NVSDK_NGX_D3D11_EvaluateFeature_C(void* ctx, const void* handle,
                                                                    const void* params, void* callback);
}

// Header-only fake NVSDK_NGX_Parameter. Values are stored with the type they were
// set under; Get returns Success only for the matching type, which mirrors NGX
// answering a non-Success code for a key that is absent or of another type.
class FakeNgxParam : public acdb::NgxParameter {
public:
    void SetU(const char* name, unsigned int v) { Put(name, Entry{Kind::U32, v, 0, 0.0f, nullptr}); }
    void SetI(const char* name, int v) { Put(name, Entry{Kind::I32, 0, v, 0.0f, nullptr}); }
    void SetF(const char* name, float v) { Put(name, Entry{Kind::F32, 0, 0, v, nullptr}); }
    void SetRes(const char* name, ID3D11Resource* v) { Put(name, Entry{Kind::Res, 0, 0, 0.0f, v}); }

    // acdb::NgxParameter -- only the accessors the hook uses do anything.
    void Set(const char* n, unsigned long long v) override { SetU(n, static_cast<unsigned int>(v)); }
    void Set(const char* n, float v) override { SetF(n, v); }
    void Set(const char* n, double v) override { SetF(n, static_cast<float>(v)); }
    void Set(const char* n, unsigned int v) override { SetU(n, v); }
    void Set(const char* n, int v) override { SetI(n, v); }
    void Set(const char* n, ID3D11Resource* v) override { SetRes(n, v); }
    void Set(const char*, ID3D12Resource*) override {}
    void Set(const char*, void*) override {}

    acdb::NgxResult Get(const char* n, unsigned long long* o) const override {
        const Entry* e = Find(n, Kind::U32);
        if (!e) return kFail;
        *o = e->u;
        return acdb::kNgxSuccess;
    }
    acdb::NgxResult Get(const char* n, float* o) const override {
        const Entry* e = Find(n, Kind::F32);
        if (!e) return kFail;
        *o = e->f;
        return acdb::kNgxSuccess;
    }
    acdb::NgxResult Get(const char*, double*) const override { return kFail; }
    acdb::NgxResult Get(const char* n, unsigned int* o) const override {
        const Entry* e = Find(n, Kind::U32);
        if (!e) return kFail;
        *o = e->u;
        return acdb::kNgxSuccess;
    }
    acdb::NgxResult Get(const char* n, int* o) const override {
        const Entry* e = Find(n, Kind::I32);
        if (!e) return kFail;
        *o = e->i;
        return acdb::kNgxSuccess;
    }
    acdb::NgxResult Get(const char* n, ID3D11Resource** o) const override {
        const Entry* e = Find(n, Kind::Res);
        if (!e) return kFail;
        *o = e->res;
        return acdb::kNgxSuccess;
    }
    acdb::NgxResult Get(const char*, ID3D12Resource**) const override { return kFail; }
    acdb::NgxResult Get(const char*, void**) const override { return kFail; }
    void Reset() override { entries_.clear(); }

private:
    static constexpr acdb::NgxResult kFail = static_cast<acdb::NgxResult>(0xBAD00010);  // UnsupportedParameter
    enum class Kind { U32, I32, F32, Res };
    struct Entry {
        Kind kind;
        unsigned int u;
        int i;
        float f;
        ID3D11Resource* res;
        std::string name;
    };
    void Put(const char* name, Entry e) {
        e.name = name;
        for (auto& x : entries_)
            if (x.name == e.name) {
                x = e;
                return;
            }
        entries_.push_back(std::move(e));
    }
    const Entry* Find(const char* name, Kind k) const {
        for (const auto& x : entries_)
            if (x.kind == k && x.name == name) return &x;
        return nullptr;
    }
    std::vector<Entry> entries_;
};
