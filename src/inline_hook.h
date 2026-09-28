#pragma once
// x64 inline detour with a trampoline (spec 6.5). A 14-byte absolute jump is
// written over the target's prologue; the overwritten instructions are relocated
// into an allocated trampoline that ends with a jump back, so Original() reaches
// the untouched function. A small length decoder measures the prologue and
// relocates the two RIP-relative forms the NGX entry points actually use
// (mov reg,[rip+disp32] and E8/E9 rel32); anything it does not understand makes
// Install fail so the caller skips that module rather than corrupt it.
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace acdb {

// One decoded x64 instruction. length == 0 (or understood == false) means the
// decoder could not size it and the prologue must not be copied.
struct DecodedInsn {
    size_t length = 0;
    bool understood = false;
    bool ripRelative = false;  // has a RIP-relative memory operand (needs disp fix-up)
    int dispOffset = 0;        // byte offset of that disp32 inside the instruction
    bool relBranch = false;    // E8/E9/EB/7x/0F8x relative branch
    int relSize = 0;           // 1 or 4
    int relOffset = 0;         // byte offset of the rel operand inside the instruction
};

// Decodes the single instruction at p, reading at most `avail` bytes.
DecodedInsn DecodeInsn(const uint8_t* p, size_t avail);

// Total length of whole instructions covering at least `need` bytes, or 0 if the
// decoder meets something it does not understand before reaching `need`. A rel8
// branch is allowed here; Install widens it to rel32 when it copies the prologue.
size_t PrologueLength(const uint8_t* code, size_t need, std::string* why);

class InlineHook {
public:
    InlineHook() = default;
    ~InlineHook();
    InlineHook(const InlineHook&) = delete;
    InlineHook& operator=(const InlineHook&) = delete;

    // Patches `target` to jump to `detour` and builds a trampoline to the
    // original. On failure returns false, sets *error and changes nothing.
    bool Install(void* target, void* detour, std::string* error);
    // Restores the original prologue bytes (target must still be mapped).
    void Remove();
    // Frees the trampoline and forgets the hook without touching the target's
    // memory -- for a module that has been unloaded (spec 6.5).
    void Detach();

    bool Active() const { return active_; }
    // Call through this to reach the original function.
    void* Original() const { return trampoline_; }
    void* Target() const { return target_; }

private:
    void* target_ = nullptr;
    void* trampoline_ = nullptr;
    uint8_t saved_[32] = {};
    size_t savedLen_ = 0;
    bool active_ = false;
};

}  // namespace acdb
