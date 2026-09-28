// Length decoder and x64 inline detour (src/inline_hook.*).
#include <windows.h>

#include <cstdint>
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
