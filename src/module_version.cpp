#include "module_version.h"

// VS_FIXEDFILEINFO and VS_FFI_SIGNATURE come with windows.h (winver.h); only
// the declarations are used, nothing from VERSION.dll.
#include <cstdio>
#include <cstring>

namespace acdb {
namespace {

// One node of the VS_VERSIONINFO tree: wLength, wValueLength, wType, a
// NUL-terminated key, padding to 32 bits, the value, padding, the children.
struct Block {
    size_t end = 0;       // offset just past the block
    std::wstring key;
    size_t value = 0;     // offset of the value
    WORD value_len = 0;   // in bytes for binary values, in characters for text
    size_t children = 0;  // offset of the first child
};

size_t Align4(size_t v) { return (v + 3) & ~static_cast<size_t>(3); }

WORD ReadWord(const BYTE* base, size_t at) {
    WORD w = 0;
    std::memcpy(&w, base + at, sizeof(w));
    return w;
}

bool ReadBlock(const BYTE* base, size_t limit, size_t at, Block* b) {
    if (at + 6 > limit) return false;
    const WORD len = ReadWord(base, at);
    if (len < 6 || at + len > limit) return false;
    b->end = at + len;
    b->value_len = ReadWord(base, at + 2);
    const WORD type = ReadWord(base, at + 4);
    b->key.clear();
    size_t p = at + 6;
    for (;;) {
        if (p + 2 > b->end) return false;
        const wchar_t c = static_cast<wchar_t>(ReadWord(base, p));
        p += 2;
        if (c == 0) break;
        b->key.push_back(c);
    }
    b->value = Align4(p);
    const size_t valueBytes = type == 1 ? static_cast<size_t>(b->value_len) * 2 : b->value_len;
    b->children = Align4(b->value + valueBytes);
    if (b->value > b->end) b->value = b->end;
    if (b->children > b->end) b->children = b->end;
    return true;
}

std::string TextAt(const BYTE* base, size_t from, size_t end) {
    std::wstring w;
    for (size_t p = from; p + 2 <= end; p += 2) {
        const wchar_t c = static_cast<wchar_t>(ReadWord(base, p));
        if (c == 0) break;
        w.push_back(c);
    }
    while (!w.empty() && (w.back() == L' ' || w.back() == L'\t')) w.pop_back();
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// The first "ProductVersion" string of any StringTable under StringFileInfo.
std::string ProductVersion(const BYTE* base, const Block& root) {
    Block info;
    for (size_t a = root.children; ReadBlock(base, root.end, a, &info); a = Align4(info.end)) {
        if (info.key != L"StringFileInfo") continue;
        Block table;
        for (size_t b = info.children; ReadBlock(base, info.end, b, &table); b = Align4(table.end)) {
            Block str;
            for (size_t c = table.children; ReadBlock(base, table.end, c, &str); c = Align4(str.end)) {
                if (str.key == L"ProductVersion") return TextAt(base, str.value, str.end);
            }
        }
    }
    return {};
}

}  // namespace

ModuleVersion ReadModuleVersion(HMODULE module) {
    ModuleVersion v;
    try {
        if (!module) return v;
        const HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(VS_VERSION_INFO), RT_VERSION);
        if (!res) return v;
        const DWORD size = SizeofResource(module, res);
        const HGLOBAL loaded = LoadResource(module, res);
        const auto* base = loaded ? static_cast<const BYTE*>(LockResource(loaded)) : nullptr;
        if (!base || size == 0) return v;
        Block root;
        if (!ReadBlock(base, size, 0, &root) || root.key != L"VS_VERSION_INFO") return v;
        VS_FIXEDFILEINFO fixed{};
        if (root.value_len < sizeof(fixed) || root.value + sizeof(fixed) > root.end) return v;
        std::memcpy(&fixed, base + root.value, sizeof(fixed));
        if (fixed.dwSignature != VS_FFI_SIGNATURE) return v;
        v.file[0] = HIWORD(fixed.dwFileVersionMS);
        v.file[1] = LOWORD(fixed.dwFileVersionMS);
        v.file[2] = HIWORD(fixed.dwFileVersionLS);
        v.file[3] = LOWORD(fixed.dwFileVersionLS);
        v.found = true;
        v.product = ProductVersion(base, root);
    } catch (...) {
        v = ModuleVersion();
    }
    return v;
}

std::string ModuleVersionText(HMODULE module) {
    const ModuleVersion v = ReadModuleVersion(module);
    if (!v.found) return "no version resource";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", v.file[0], v.file[1], v.file[2], v.file[3]);
    std::string text = buf;
    if (!v.product.empty() && v.product != text) text += " (product " + v.product + ")";
    return text;
}

}  // namespace acdb
