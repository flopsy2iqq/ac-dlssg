// Standalone mode: the bridge is the process's dxgi.dll, so every module that
// imports dxgi.dll, or looks its exports up by name, binds to the bridge. Each
// such name must be exported (and forwarded) by the bridge, and nothing may
// import dxgi.dll by ordinal: the bridge's ordinals are not System32's.
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "test_framework.h"

namespace {

// A PE file mapped as an image (sections at their RVAs), never executed.
class MappedImage {
public:
    explicit MappedImage(const std::wstring& path)
        : handle_(LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE)) {}
    ~MappedImage() {
        if (handle_) FreeLibrary(handle_);
    }
    MappedImage(const MappedImage&) = delete;
    MappedImage& operator=(const MappedImage&) = delete;

    bool ok() const { return handle_ != nullptr && Nt() != nullptr; }

    // RVA-to-pointer; the handle of an image resource mapping carries flag bits.
    const BYTE* At(DWORD rva) const {
        return reinterpret_cast<const BYTE*>(reinterpret_cast<uintptr_t>(handle_) & ~uintptr_t{3}) + rva;
    }

    const IMAGE_NT_HEADERS64* Nt() const {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(At(0));
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(At(static_cast<DWORD>(dos->e_lfanew)));
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            return nullptr;
        return nt;
    }

    const IMAGE_DATA_DIRECTORY& Dir(int index) const { return Nt()->OptionalHeader.DataDirectory[index]; }

private:
    HMODULE handle_;
};

struct DxgiImports {
    std::set<std::string> names;
    std::vector<unsigned> ordinals;
};

// Walks one import name table (static or delay-load) of a dxgi.dll descriptor.
void ReadThunks(const MappedImage& img, DWORD tableRva, DxgiImports* out) {
    for (const auto* t = reinterpret_cast<const IMAGE_THUNK_DATA64*>(img.At(tableRva)); t->u1.AddressOfData; ++t) {
        if (IMAGE_SNAP_BY_ORDINAL64(t->u1.Ordinal)) {
            out->ordinals.push_back(static_cast<unsigned>(IMAGE_ORDINAL64(t->u1.Ordinal)));
        } else {
            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                img.At(static_cast<DWORD>(t->u1.AddressOfData)));
            out->names.insert(reinterpret_cast<const char*>(byName->Name));
        }
    }
}

// Static and delay-load imports from dxgi.dll. found is false when the file
// cannot be mapped as a PE32+ image.
DxgiImports ReadDxgiImports(const std::wstring& path, bool* found) {
    DxgiImports out;
    MappedImage img(path);
    *found = img.ok();
    if (!*found) return out;
    const IMAGE_DATA_DIRECTORY& imp = img.Dir(IMAGE_DIRECTORY_ENTRY_IMPORT);
    if (imp.VirtualAddress) {
        for (const auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(img.At(imp.VirtualAddress)); d->Name;
             ++d) {
            if (_stricmp(reinterpret_cast<const char*>(img.At(d->Name)), "dxgi.dll") != 0) continue;
            ReadThunks(img, d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk, &out);
        }
    }
    const IMAGE_DATA_DIRECTORY& delay = img.Dir(IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT);
    if (delay.VirtualAddress) {
        for (const auto* d = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(img.At(delay.VirtualAddress));
             d->DllNameRVA; ++d) {
            // Only the RVA-based format exists in 64-bit images.
            if (!d->Attributes.RvaBased) continue;
            if (_stricmp(reinterpret_cast<const char*>(img.At(d->DllNameRVA)), "dxgi.dll") != 0) continue;
            ReadThunks(img, d->ImportNameTableRVA, &out);
        }
    }
    return out;
}

std::set<std::string> ReadExportNames(const std::wstring& path) {
    std::set<std::string> names;
    MappedImage img(path);
    if (!img.ok()) return names;
    const IMAGE_DATA_DIRECTORY& exp = img.Dir(IMAGE_DIRECTORY_ENTRY_EXPORT);
    if (!exp.VirtualAddress) return names;
    const auto* e = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(img.At(exp.VirtualAddress));
    const auto* nameRvas = reinterpret_cast<const DWORD*>(img.At(e->AddressOfNames));
    for (DWORD i = 0; i < e->NumberOfNames; ++i) names.insert(reinterpret_cast<const char*>(img.At(nameRvas[i])));
    return names;
}

std::wstring BridgePath() {
    std::wstring path = L"" ACDB_DLL_PATH;
    for (auto& c : path) {
        if (c == L'/') c = L'\\';
    }
    return path;
}

std::wstring System32(const wchar_t* file) {
    wchar_t dir[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    return std::wstring(dir, n) + L"\\" + file;
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
}

// Checks one importer against the bridge's exports; returns the number of names it imports.
size_t CheckImporter(const std::wstring& path, const std::set<std::string>& exports) {
    bool found = false;
    const DxgiImports imports = ReadDxgiImports(path, &found);
    if (!found) {
        std::printf("  note: %s is not a PE32+ image here; skipped\n", Narrow(path).c_str());
        return 0;
    }
    for (const auto& name : imports.names) {
        if (!exports.count(name)) {
            ::acdb_test::Fail(__FILE__, __LINE__, Narrow(path) + " imports dxgi.dll!" + name +
                                                      ", which the bridge does not export");
        }
    }
    for (unsigned ordinal : imports.ordinals) {
        ::acdb_test::Fail(__FILE__, __LINE__,
                          Narrow(path) + " imports dxgi.dll by ordinal " + std::to_string(ordinal));
    }
    return imports.names.size();
}

}  // namespace

TEST(DxgiExportCoverage_TheBridgeExportsWhatWindowsImportsFromDxgi) {
    const std::set<std::string> exports = ReadExportNames(BridgePath());
    REQUIRE(exports.count("CreateDXGIFactory1") == 1);
    // d3d11.dll imports CreateDXGIFactory2; D3D12Core.dll delay-loads it.
    CHECK(CheckImporter(System32(L"d3d11.dll"), exports) >= 1);
    CheckImporter(System32(L"d3d12.dll"), exports);
    if (GetFileAttributesW(System32(L"D3D12Core.dll").c_str()) != INVALID_FILE_ATTRIBUTES)
        CHECK(CheckImporter(System32(L"D3D12Core.dll"), exports) >= 1);
}

#ifdef ACDB_SL_BIN_DIR
TEST(DxgiExportCoverage_TheBridgeExportsWhatStreamlineImportsFromDxgi) {
    const std::set<std::string> exports = ReadExportNames(BridgePath());
    REQUIRE(exports.count("CreateDXGIFactory1") == 1);
    std::wstring dir = L"" ACDB_SL_BIN_DIR;
    for (auto& c : dir) {
        if (c == L'/') c = L'\\';
    }
    size_t total = 0;
    for (const wchar_t* file : {L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss_g.dll", L"sl.reflex.dll",
                                L"sl.pcl.dll", L"nvngx_dlssg.dll"}) {
        total += CheckImporter(dir + L"\\" + file, exports);
    }
    CHECK(total >= 1);  // sl.common.dll imports CreateDXGIFactory
}
#endif

// Names looked up by GetProcAddress on whatever "dxgi.dll" resolves to, found
// in the binaries' strings: d3d11.dll (CompatValue), sl.interposer.dll
// (CreateDXGIFactory, 1, 2, DXGIGetDebugInterface1,
// DXGIDeclareAdapterRemovalSupport), nvngx_dlssg.dll (CreateDXGIFactory2).
// acs.exe imports CreateDXGIFactory and CSP's dwrite.dll CreateDXGIFactory and
// CreateDXGIFactory1 (dumpbin; the game is not part of the tests).
TEST(DxgiExportCoverage_TheBridgeExportsTheNamesLookedUpAtRuntime) {
    const std::set<std::string> exports = ReadExportNames(BridgePath());
    for (const char* name : {"CompatValue", "CompatString", "CreateDXGIFactory", "CreateDXGIFactory1",
                             "CreateDXGIFactory2", "DXGIGetDebugInterface1", "DXGIDeclareAdapterRemovalSupport"}) {
        if (!exports.count(name)) ::acdb_test::Fail(__FILE__, __LINE__, std::string("missing export ") + name);
    }
}
