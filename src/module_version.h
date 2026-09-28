#pragma once
// Version of a loaded module, read from its RT_VERSION resource without
// VERSION.dll, which the bridge must never import (spec 6.1).
#include <windows.h>

#include <string>

namespace acdb {

struct ModuleVersion {
    bool found = false;           // the module has a VS_VERSIONINFO with a VS_FIXEDFILEINFO
    unsigned file[4] = {};        // FILEVERSION
    std::string product;          // the first StringFileInfo "ProductVersion" value (UTF-8), may be empty
};

// Not found for a null module or one without a version resource.
ModuleVersion ReadModuleVersion(HMODULE module);

// "6.8.0.2155 (product 6.8.0)", "6.8.0.2155", or "no version resource".
std::string ModuleVersionText(HMODULE module);

}  // namespace acdb
