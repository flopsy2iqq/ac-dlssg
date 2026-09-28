#pragma once
#include <cstddef>

namespace acdb {

// Replaces vtable[index] with replacement under VirtualProtect and returns the
// previous entry. Returns nullptr and changes nothing if the page cannot be
// made writable.
void* PatchVtableSlot(void** vtable, size_t index, void* replacement);

}  // namespace acdb
