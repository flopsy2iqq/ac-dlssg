// Length decoder and x64 inline detour (src/inline_hook.*).
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "inline_hook.h"
#include "test_framework.h"

using namespace acdb;

namespace {

// Real NGX export prologues captured on this machine (radare2/capstone,
// 2026-09-28): each must decode to whole instructions and be safe to copy.
struct Prologue {
    const char* name;
    std::vector<uint8_t> bytes;
    bool hasRip;  // contains a RIP-relative mov we must relocate
};

std::vector<Prologue> RealPrologues() {
    return {
        // _nvngx.dll (32.0.16.1664) NVSDK_NGX_D3D11_CreateFeature
        {"nvngx_create", {0x48, 0x89, 0x6c, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xec, 0x40, 0x48, 0x8b, 0x01}, false},
        // _nvngx.dll NVSDK_NGX_D3D11_EvaluateFeature
        {"nvngx_eval", {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57}, false},
        // nvngx_dlss.dll (310.8) CreateFeature
        {"dlss_create", {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xec, 0x50}, false},
        // nvngx_dlssnr.dll CreateFeature: push run then sub rsp,imm32 then mov rax,[rip+disp32]
        {"dlssnr_create", {0x40, 0x53, 0x55, 0x56, 0x48, 0x81, 0xec, 0x80, 0x02, 0x00, 0x00, 0x48, 0x8b, 0x05, 0x7e, 0xcc, 0x12, 0x01}, true},
        // nvngx_dlssg.dll EvaluateFeature: push run, sub rsp,imm32, mov rax,[rip+disp32]
        {"dlssg_eval", {0x40, 0x53, 0x56, 0x57, 0x48, 0x81, 0xec, 0x60, 0x02, 0x00, 0x00, 0x48, 0x8b, 0x05, 0x1e, 0xcb, 0x6f, 0x00}, true},
        // old _nvngx.dll (30.0) CreateFeature: sub rsp,imm8; mov [rsp+0x20],rcx; call rel32
        {"old_nvngx_create", {0x48, 0x83, 0xec, 0x38, 0x48, 0x89, 0x4c, 0x24, 0x20, 0xe8, 0x42, 0x07, 0x00, 0x00, 0x48, 0x83, 0xc4, 0x38}, false},
        // nvngx_dlss.dll CreateFeature with lea rbp,[rsp-x] (dlssg create form)
        {"lea_rbp", {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xac, 0x24, 0x48, 0xfe, 0xff, 0xff}, false},
    };
}

}  // namespace

TEST(Decoder_SizesSingleInstructions) {
    struct C {
        std::vector<uint8_t> b;
        size_t len;
        bool rip;
    };
    const C cases[] = {
        {{0x56}, 1, false},                                            // push rsi
        {{0x41, 0x56}, 2, false},                                      // push r14
        {{0x40, 0x53}, 2, false},                                      // push rbx (REX)
        {{0x48, 0x89, 0x6c, 0x24, 0x18}, 5, false},                    // mov [rsp+0x18], rbp
        {{0x48, 0x83, 0xec, 0x40}, 4, false},                          // sub rsp, 0x40
        {{0x48, 0x81, 0xec, 0x80, 0x02, 0x00, 0x00}, 7, false},        // sub rsp, 0x280
        {{0x48, 0x8b, 0x01}, 3, false},                                // mov rax, [rcx]
        {{0x48, 0x8b, 0x05, 0x7e, 0xcc, 0x12, 0x01}, 7, true},         // mov rax, [rip+disp32]
        {{0x48, 0x8d, 0xac, 0x24, 0x48, 0xfe, 0xff, 0xff}, 8, false},  // lea rbp, [rsp-0x1b8]
        {{0xe8, 0x42, 0x07, 0x00, 0x00}, 5, false},                    // call rel32
        {{0xe9, 0x00, 0x10, 0x00, 0x00}, 5, false},                    // jmp rel32
        {{0xc3}, 1, false},                                            // ret
        {{0x90}, 1, false},                                            // nop
        {{0x48, 0xb8, 1, 2, 3, 4, 5, 6, 7, 8}, 10, false},            // mov rax, imm64
    };
    for (const auto& c : cases) {
        DecodedInsn d = DecodeInsn(c.b.data(), c.b.size());
        CHECK(d.understood);
        CHECK_EQ(d.length, c.len);
        CHECK_EQ(d.ripRelative, c.rip);
    }
}

TEST(Decoder_FlagsRelBranches) {
    const uint8_t call[] = {0xe8, 0x11, 0x22, 0x33, 0x44};
    DecodedInsn d = DecodeInsn(call, sizeof(call));
    CHECK(d.understood && d.relBranch);
    CHECK_EQ(d.relSize, 4);
    CHECK_EQ(d.relOffset, 1);
}

TEST(Decoder_RejectsUnknown) {
    const uint8_t junk[] = {0x0f, 0x0b};  // ud2: not a prologue instruction
    DecodedInsn d = DecodeInsn(junk, sizeof(junk));
    CHECK(!d.understood);
}

TEST(Prologue_CoversRealNgxEntryPoints) {
    for (const auto& p : RealPrologues()) {
        std::string why;
        const size_t n = PrologueLength(p.bytes.data(), 14, &why);
        if (n == 0) std::printf("  %s not covered: %s\n", p.name, why.c_str());
        CHECK(n >= 14);
        CHECK(n <= p.bytes.size());
    }
}

// An ordinary function, detoured through the trampoline, keeps working: the
// detour runs, Original() reaches the untouched body, Remove() restores it.
namespace {
using AddFn = int (*)(int, int);
InlineHook* g_addHook = nullptr;
volatile int g_detourCalls = 0;

__declspec(noinline) int Add(int a, int b) {
    // A body long enough that a 14-byte patch stays inside it.
    volatile int x = a;
    x += b;
    x ^= 0x55;
    x += a * 3;
    x -= b;
    return x - (a * 3) - 0x55 + (a + b) - (a + b) + (a + b) - a - b + a + b;
}

int DetourAdd(int a, int b) {
    ++g_detourCalls;
    AddFn orig = reinterpret_cast<AddFn>(g_addHook->Original());
    return orig(a, b) + 1000;
}
}  // namespace

TEST(InlineHook_DetoursAndRestores) {
    const int base = Add(2, 3);
    InlineHook hook;
    g_addHook = &hook;
    g_detourCalls = 0;
    std::string err;
    REQUIRE(hook.Install(reinterpret_cast<void*>(&Add), reinterpret_cast<void*>(&DetourAdd), &err));
    CHECK(hook.Active());

    const int hooked = Add(2, 3);
    CHECK_EQ(g_detourCalls, 1);
    CHECK_EQ(hooked, base + 1000);
    // Original() bypasses the detour.
    CHECK_EQ(reinterpret_cast<AddFn>(hook.Original())(2, 3), base);
    CHECK_EQ(g_detourCalls, 1);

    hook.Remove();
    CHECK(!hook.Active());
    CHECK_EQ(Add(2, 3), base);
    CHECK_EQ(g_detourCalls, 1);
    g_addHook = nullptr;
}

// A prologue with an early short conditional branch is relocated by widening the
// rel8 to rel32 in the trampoline (the case the optimized NGX prologues hit).
namespace {
using IntFn = int (*)(int);
InlineHook* g_branchHook = nullptr;

// A hand-assembled function whose first bytes contain a `je rel8` whose target
// lies past the copied region:
//   cmp ecx,0 ; je L ; mov eax,1 ; ret ; nops... ; L: mov eax,2 ; ret
// Returns 2 when the argument is 0, else 1.
uint8_t* MakeBranchingFn() {
    static const uint8_t code[] = {
        0x83, 0xF9, 0x00,              // cmp ecx, 0
        0x74, 0x0F,                    // je +0x0F -> offset 0x14
        0xB8, 0x01, 0x00, 0x00, 0x00,  // mov eax, 1
        0xC3,                          // ret
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,  // padding to offset 0x14
        0xB8, 0x02, 0x00, 0x00, 0x00,  // (0x14) mov eax, 2
        0xC3,                          // ret
    };
    auto* mem = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, sizeof(code), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (mem) {
        memcpy(mem, code, sizeof(code));
        FlushInstructionCache(GetCurrentProcess(), mem, sizeof(code));
    }
    return mem;
}

int DetourBranch(int x) {
    IntFn orig = reinterpret_cast<IntFn>(g_branchHook->Original());
    return orig(x) + 100;
}
}  // namespace

TEST(InlineHook_WidensShortBranch) {
    uint8_t* fn = MakeBranchingFn();
    REQUIRE(fn != nullptr);
    IntFn f = reinterpret_cast<IntFn>(fn);
    CHECK_EQ(f(0), 2);
    CHECK_EQ(f(1), 1);

    InlineHook hook;
    g_branchHook = &hook;
    std::string err;
    REQUIRE(hook.Install(fn, reinterpret_cast<void*>(&DetourBranch), &err));
    // Both branches survive relocation into the trampoline.
    CHECK_EQ(f(0), 102);
    CHECK_EQ(f(1), 101);
    CHECK_EQ(reinterpret_cast<IntFn>(hook.Original())(0), 2);
    CHECK_EQ(reinterpret_cast<IntFn>(hook.Original())(1), 1);
    hook.Remove();
    CHECK_EQ(f(0), 2);
    g_branchHook = nullptr;
    VirtualFree(fn, 0, MEM_RELEASE);
}

// Remove() writes the saved prologue back only over its own patch. When the
// target no longer holds our jump (another module now occupies that address),
// it only forgets the hook and leaves the foreign bytes alone (ngx review F6).
TEST(InlineHook_RemoveLeavesForeignBytesAlone) {
    uint8_t* fn = MakeBranchingFn();
    REQUIRE(fn != nullptr);
    InlineHook hook;
    g_branchHook = &hook;
    std::string err;
    REQUIRE(hook.Install(fn, reinterpret_cast<void*>(&DetourBranch), &err));
    CHECK(hook.PatchIntact());

    uint8_t foreign[20];
    for (size_t i = 0; i < sizeof(foreign); ++i) foreign[i] = static_cast<uint8_t>(0xA0 + i);
    memcpy(fn, foreign, sizeof(foreign));  // the buffer is RWX
    CHECK(!hook.PatchIntact());

    hook.Remove();
    CHECK(!hook.Active());
    CHECK(hook.Original() == nullptr);
    CHECK(memcmp(fn, foreign, sizeof(foreign)) == 0);
    g_branchHook = nullptr;
    VirtualFree(fn, 0, MEM_RELEASE);
}

// PatchIntact on memory that is gone reports false instead of faulting.
TEST(InlineHook_PatchIntactOnReleasedMemory) {
    uint8_t* fn = MakeBranchingFn();
    REQUIRE(fn != nullptr);
    InlineHook hook;
    g_branchHook = &hook;
    std::string err;
    REQUIRE(hook.Install(fn, reinterpret_cast<void*>(&DetourBranch), &err));
    VirtualFree(fn, 0, MEM_RELEASE);  // "the module unloaded"
    CHECK(!hook.PatchIntact());
    hook.Detach();
    g_branchHook = nullptr;
}

// A pointer taken BEFORE Install still reaches the detour: the patch is at the
// function body, not in an import or export table (spec 6.5 test list).
TEST(InlineHook_PreTakenPointerReachesDetour) {
    AddFn before = &Add;
    InlineHook hook;
    g_addHook = &hook;
    g_detourCalls = 0;
    std::string err;
    REQUIRE(hook.Install(reinterpret_cast<void*>(&Add), reinterpret_cast<void*>(&DetourAdd), &err));
    const int r = before(4, 5);
    CHECK_EQ(g_detourCalls, 1);
    CHECK_EQ(r, Add(4, 5));  // both go through the detour now
    hook.Remove();
    g_addHook = nullptr;
}

// ---------------------------------------------------------------------------
// Another mod's hook on an NGX entry point (GitHub issue 12, Nexus crash
// reports): MinHook writes a 5-byte E9 jump, dlss5-bridge 1.4.12 a 14-byte
// FF 25 absolute jump. A patch of ours over either breaks it (MinHook's
// trampoline returns into our patch; the relocated FF 25 reads our own
// detour address), so such a prologue is recognised by its jump and left
// alone. The fake prologues sit in one buffer that plays the module: its
// first half is the module's range, its second half lies outside it.

namespace {

struct FakeModule {
    std::vector<uint8_t> mem = std::vector<uint8_t>(0x200, 0xCC);
    uint8_t* base() { return mem.data(); }
    size_t size() const { return 0x100; }       // the "module": mem[0, 0x100)
    uint8_t* inside(size_t at) { return mem.data() + at; }
    uint8_t* outside() { return mem.data() + 0x180; }  // past the module's end
};

void PutE9(uint8_t* at, const void* dest) {
    at[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(static_cast<const uint8_t*>(dest) - (at + 5));
    std::memcpy(at + 1, &rel, sizeof(rel));
}

// FF 25 disp32: jmp qword ptr [rip + disp32], the pointer at at + 6 + disp32.
void PutFf25(uint8_t* at, int32_t disp, uint8_t* slot, const void* dest) {
    at[0] = 0xFF;
    at[1] = 0x25;
    std::memcpy(at + 2, &disp, sizeof(disp));
    const uint64_t d = reinterpret_cast<uint64_t>(dest);
    std::memcpy(slot, &d, sizeof(d));
}

}  // namespace

TEST(ForeignHook_AnE9JumpOutOfTheModuleIsAForeignHook) {
    FakeModule m;
    uint8_t* fn = m.inside(0x10);
    PutE9(fn, m.outside());
    const void* dest = nullptr;
    CHECK(IsForeignHook(fn, m.base(), m.size(), &dest));
    CHECK(dest == m.outside());
    CHECK(LeadingJumpDestination(fn) == m.outside());
}

TEST(ForeignHook_AnE9JumpInsideTheModuleIsAThunkNotAHook) {
    FakeModule m;
    uint8_t* fn = m.inside(0x10);
    PutE9(fn, m.inside(0x80));
    const void* dest = nullptr;
    CHECK(!IsForeignHook(fn, m.base(), m.size(), &dest));
    CHECK(LeadingJumpDestination(fn) == m.inside(0x80));
}

TEST(ForeignHook_AnAbsoluteFf25JumpOutOfTheModuleIsAForeignHook) {
    FakeModule m;
    // dlss5-bridge 1.4.12's form: FF 25 00000000 <absolute address>.
    uint8_t* fn = m.inside(0x10);
    PutFf25(fn, 0, fn + 6, m.outside());
    const void* dest = nullptr;
    CHECK(IsForeignHook(fn, m.base(), m.size(), &dest));
    CHECK(dest == m.outside());
    // The pointer elsewhere: FF 25 disp32 with the slot 0x40 bytes on.
    uint8_t* fn2 = m.inside(0x40);
    PutFf25(fn2, 0x40, fn2 + 6 + 0x40, m.outside() + 8);
    CHECK(IsForeignHook(fn2, m.base(), m.size(), &dest));
    CHECK(dest == m.outside() + 8);
    // Into the module itself: an import thunk, not a hook.
    PutFf25(fn, 0, fn + 6, m.inside(0xA0));
    CHECK(!IsForeignHook(fn, m.base(), m.size(), &dest));
}

TEST(ForeignHook_ARealPrologueOrAnotherInstructionIsNoHook) {
    for (const Prologue& p : RealPrologues()) {
        std::vector<uint8_t> code = p.bytes;
        code.resize(32, 0xCC);
        const void* dest = reinterpret_cast<const void*>(1);
        if (IsForeignHook(code.data(), code.data(), code.size(), &dest)) std::printf("  %s taken for a hook\n", p.name);
        CHECK(!IsForeignHook(code.data(), code.data(), code.size(), &dest));
        CHECK(LeadingJumpDestination(code.data()) == nullptr);
    }
    // FF 15 is an indirect call, FF 24 a jump through a SIB operand, EB a short jump.
    std::vector<uint8_t> call = {0xFF, 0x15, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(LeadingJumpDestination(call.data()) == nullptr);
    std::vector<uint8_t> sib = {0xFF, 0x24, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(LeadingJumpDestination(sib.data()) == nullptr);
    std::vector<uint8_t> shortJmp = {0xEB, 0x10, 0xCC, 0xCC, 0xCC, 0xCC};
    CHECK(LeadingJumpDestination(shortJmp.data()) == nullptr);
    // Unreadable memory is no jump.
    const void* dest = nullptr;
    CHECK(LeadingJumpDestination(nullptr) == nullptr);
    CHECK(!IsForeignHook(nullptr, nullptr, 0, &dest));
}

// Our own patch over a function of a module is a jump into another module
// too (the bridge's detour): a second patch over it would chain into itself,
// so it is refused like any other.
TEST(ForeignHook_OurOwnPatchCountsAsAJumpOutOfTheModule) {
    std::vector<uint8_t> code(64, 0x90);
    code[0] = 0x48;  // a plain prologue to patch: sub rsp, 0x28; then nops
    code[1] = 0x83;
    code[2] = 0xEC;
    code[3] = 0x28;
    void* exec = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    REQUIRE(exec != nullptr);
    std::memcpy(exec, code.data(), code.size());
    {
        InlineHook hook;
        std::string err;
        static uint8_t detour[16] = {0xC3};
        REQUIRE(hook.Install(exec, detour, &err));
        const void* dest = nullptr;
        CHECK(IsForeignHook(exec, exec, 0x1000, &dest));
        CHECK(dest == detour);
        hook.Remove();
        CHECK(!IsForeignHook(exec, exec, 0x1000, &dest));
    }
    VirtualFree(exec, 0, MEM_RELEASE);
}
