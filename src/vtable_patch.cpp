#include "vtable_patch.h"

#include <windows.h>

#include <mutex>

namespace acdb {
namespace {

// Two patches on the same page must not interleave: the first restore would
// make the page read-only under the second write.
std::mutex g_patch_mu;

bool IsExecutable(DWORD protect) {
    return (protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

}  // namespace

void* PatchVtableSlot(void** vtable, size_t index, void* replacement) {
    if (!vtable) return nullptr;
    void** slot = vtable + index;
    std::lock_guard<std::mutex> lock(g_patch_mu);

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(slot, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT) return nullptr;
    // Keep execute rights if the page has them: code sharing the page may run
    // on another thread while it is unprotected.
    const DWORD writable = IsExecutable(mbi.Protect) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), writable, &old)) return nullptr;
    void* previous = InterlockedExchangePointer(slot, replacement);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    return previous;
}

}  // namespace acdb
