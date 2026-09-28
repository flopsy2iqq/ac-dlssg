#include "inline_hook.h"

#include <cstring>

#include "log.h"

namespace acdb {
namespace {

// The absolute jump written over the prologue and appended to the trampoline:
//   FF 25 00 00 00 00        jmp qword ptr [rip+0]
//   <8-byte absolute target>
constexpr size_t kJumpLen = 14;

// Reserve+commit RWX memory within ±2GB of `target`, so a copied RIP-relative
// operand or rel32 branch in the trampoline still reaches its original target
// with a 32-bit displacement. Some NGX prologues (nvngx_dlssnr, nvngx_dlssg)
// begin with `mov rax,[rip+disp32]`, so a far trampoline would fail to relocate.
void* AllocNear(const void* target, size_t size) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    const uintptr_t t = reinterpret_cast<uintptr_t>(target);
    const uintptr_t kSpan = 0x7FFF0000;  // a little under 2GB, for safety
    const uintptr_t lo = t > kSpan ? t - kSpan : 0x10000;
    const uintptr_t hi = (t + kSpan < t) ? UINTPTR_MAX : t + kSpan;
    auto tryAt = [&](uintptr_t a) -> void* {
        return VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_COMMIT | MEM_RESERVE,
                            PAGE_EXECUTE_READWRITE);
    };
    // Search upward from the target, then downward, probing free regions.
    for (uintptr_t addr = (t + gran - 1) & ~(gran - 1); addr < hi;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        if (mbi.State == MEM_FREE) {
            if (void* p = tryAt(addr)) return p;
        }
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        addr = (next + gran - 1) & ~(gran - 1);
        if (addr <= reinterpret_cast<uintptr_t>(mbi.BaseAddress)) break;  // no progress
    }
    for (uintptr_t addr = (t & ~(gran - 1)); addr >= lo;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        if (mbi.State == MEM_FREE) {
            if (void* p = tryAt(addr)) return p;
        }
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.AllocationBase ? mbi.AllocationBase
                                                                              : mbi.BaseAddress);
        if (base < gran) break;
        addr = (base - gran) & ~(gran - 1);
    }
    return nullptr;
}

void WriteAbsJump(uint8_t* at, const void* dest) {
    at[0] = 0xFF;
    at[1] = 0x25;
    at[2] = at[3] = at[4] = at[5] = 0x00;
    uint64_t d = reinterpret_cast<uint64_t>(dest);
    std::memcpy(at + 6, &d, sizeof(d));
}

// Reads bytes that may belong to an unloaded module. No unwinding objects here,
// so the SEH frame is allowed.
bool ReadCodeSafe(const void* p, uint8_t* out, size_t n) {
    __try {
        std::memcpy(out, p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ModRM + optional SIB + displacement, for the default 64-bit address size (no
// 0x67). Sets *ripRel when the operand is [rip+disp32] (mod=00, rm=101, no SIB).
// Returns bytes consumed, or 0 if the bytes run out.
size_t ModRMLen(const uint8_t* m, size_t avail, bool* ripRel, int* dispOff, int* dispSize) {
    *ripRel = false;
    *dispOff = 0;
    *dispSize = 0;
    if (avail < 1) return 0;
    const uint8_t modrm = m[0];
    const int mod = modrm >> 6;
    const int rm = modrm & 7;
    size_t len = 1;
    int disp = 0;
    bool rip = false;
    if (mod != 3) {
        const bool sib = (rm == 4);
        uint8_t base = 0;
        if (sib) {
            if (avail < 2) return 0;
            base = m[1] & 7;
            len += 1;
        }
        if (mod == 0) {
            if (!sib && rm == 5) {
                disp = 4;
                rip = true;
            } else if (sib && base == 5) {
                disp = 4;
            }
        } else if (mod == 1) {
            disp = 1;
        } else {  // mod == 2
            disp = 4;
        }
    }
    *dispOff = static_cast<int>(len);
    *dispSize = disp;
    *ripRel = rip;
    return len + disp;
}

// Immediate size for a one-byte opcode that has one, given the operand-size
// prefix and REX.W. Returns -1 for opcodes with no immediate handled elsewhere.
int ImmSizeOneByte(uint8_t op, bool opsize, bool rexW) {
    switch (op) {
        case 0x04: case 0x0C: case 0x14: case 0x1C:
        case 0x24: case 0x2C: case 0x34: case 0x3C:  // alu al, imm8
        case 0x6A:                                    // push imm8
        case 0x80: case 0x83:                         // grp1 r/m, imm8
        case 0xA8:                                    // test al, imm8
        case 0xC0: case 0xC1:                         // shift r/m, imm8
        case 0xC6:                                    // mov r/m8, imm8
        case 0x6B:                                    // imul r, r/m, imm8
            return 1;
        case 0x05: case 0x0D: case 0x15: case 0x1D:
        case 0x25: case 0x2D: case 0x35: case 0x3D:  // alu eax, imm(16/32)
        case 0x68:                                    // push imm32
        case 0x69:                                    // imul r, r/m, imm(16/32)
        case 0x81:                                    // grp1 r/m, imm(16/32)
        case 0xA9:                                    // test eax, imm(16/32)
        case 0xC7:                                    // mov r/m, imm(16/32)
            return opsize ? 2 : 4;
        case 0xC2:  // ret imm16
            return 2;
        default:
            (void)rexW;
            return 0;
    }
}

// True for one-byte opcodes that carry a ModRM byte.
bool HasModRMOneByte(uint8_t op) {
    if (op <= 0x3F) {
        const uint8_t low = op & 7;
        if (low <= 3) return true;  // alu r/m forms
    }
    switch (op) {
        case 0x62: case 0x63:
        case 0x69: case 0x6B:
        case 0x80: case 0x81: case 0x82: case 0x83:
        case 0x84: case 0x85: case 0x86: case 0x87:
        case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F:
        case 0xC0: case 0xC1: case 0xC6: case 0xC7:
        case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        case 0xF6: case 0xF7:
        case 0xFE: case 0xFF:
            return true;
        default:
            return false;
    }
}

}  // namespace

DecodedInsn DecodeInsn(const uint8_t* p, size_t avail) {
    DecodedInsn d;
    if (avail == 0) return d;
    size_t i = 0;
    bool opsize = false;
    // Legacy prefixes and REX. REX must be the last prefix, so keep the newest.
    for (; i < avail; ++i) {
        const uint8_t b = p[i];
        if (b == 0x66) {
            opsize = true;
        } else if (b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 || b == 0x2E || b == 0x36 ||
                   b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65) {
            // accepted prefix
        } else if (b >= 0x40 && b <= 0x4F) {
            // REX; a following opcode byte ends the prefix run
            ++i;
            break;
        } else {
            break;
        }
    }
    // Recover REX.W by scanning the prefix run (last REX wins).
    bool rexW = false;
    for (size_t k = 0; k < i; ++k) {
        if (p[k] >= 0x40 && p[k] <= 0x4F) rexW = (p[k] & 0x08) != 0;
    }
    if (i >= avail) return d;

    const uint8_t op = p[i];
    size_t opBytes = 1;

    // Two-byte opcode map (0x0F ...).
    if (op == 0x0F) {
        if (i + 1 >= avail) return d;
        const uint8_t op2 = p[i + 1];
        opBytes = 2;
        // Jcc rel32.
        if (op2 >= 0x80 && op2 <= 0x8F) {
            const size_t len = i + 2 + 4;
            if (len > avail) return d;
            d.length = len;
            d.understood = true;
            d.relBranch = true;
            d.relSize = 4;
            d.relOffset = static_cast<int>(i + 2);
            return d;
        }
        // Groups with a ModRM byte and no immediate we rely on.
        const bool modrm2 =
            (op2 >= 0x10 && op2 <= 0x17) ||  // SSE moves (movups/movss/movsd/...)
            (op2 >= 0x28 && op2 <= 0x2F) ||  // movaps and friends
            (op2 >= 0x40 && op2 <= 0x4F) ||  // cmovcc
            (op2 == 0x1E || op2 == 0x1F) ||  // nop/endbr r/m
            (op2 == 0x57) ||                 // xorps
            (op2 == 0x54) || (op2 == 0x55) || (op2 == 0x56) ||
            (op2 == 0xAF) ||                 // imul
            (op2 >= 0x90 && op2 <= 0x9F) ||  // setcc
            (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF) ||  // movzx/movsx
            (op2 == 0xD6) || (op2 == 0x6E) || (op2 == 0x7E) || (op2 == 0x6F) || (op2 == 0x7F);
        if (!modrm2) return d;
        bool rip = false;
        int dispOff = 0, dispSize = 0;
        const size_t mlen = ModRMLen(p + i + 2, avail - (i + 2), &rip, &dispOff, &dispSize);
        if (mlen == 0) return d;
        const size_t len = i + 2 + mlen;
        if (len > avail) return d;
        d.length = len;
        d.understood = true;
        d.ripRelative = rip;
        d.dispOffset = rip ? static_cast<int>(i + 2 + dispOff) : 0;
        return d;
    }

    // push/pop r64 (with optional REX already consumed).
    if (op >= 0x50 && op <= 0x5F) {
        d.length = i + 1;
        d.understood = i + 1 <= avail;
        return d;
    }
    // Simple no-operand opcodes.
    if (op == 0x90 || op == 0xC3 || op == 0xCC || op == 0xC9 || op == 0x98 || op == 0x99 ||
        op == 0xF4) {
        d.length = i + 1;
        d.understood = true;
        return d;
    }
    if (op == 0xC2) {  // ret imm16
        const size_t len = i + 1 + 2;
        if (len > avail) return d;
        d.length = len;
        d.understood = true;
        return d;
    }
    // mov r64, imm (B8-BF): imm64 when REX.W, else imm32; B0-B7: imm8.
    if (op >= 0xB8 && op <= 0xBF) {
        const size_t imm = rexW ? 8 : (opsize ? 2 : 4);
        const size_t len = i + 1 + imm;
        if (len > avail) return d;
        d.length = len;
        d.understood = true;
        return d;
    }
    if (op >= 0xB0 && op <= 0xB7) {
        const size_t len = i + 1 + 1;
        if (len > avail) return d;
        d.length = len;
        d.understood = true;
        return d;
    }
    // Relative branches.
    if (op == 0xE8 || op == 0xE9) {  // call/jmp rel32
        const size_t len = i + 1 + 4;
        if (len > avail) return d;
        d.length = len;
        d.understood = true;
        d.relBranch = true;
        d.relSize = 4;
        d.relOffset = static_cast<int>(i + 1);
        return d;
    }
    if (op == 0xEB || (op >= 0x70 && op <= 0x7F)) {  // jmp/Jcc rel8
        const size_t len = i + 1 + 1;
        if (len > avail) return d;
        d.length = len;
        d.understood = true;
        d.relBranch = true;
        d.relSize = 1;
        d.relOffset = static_cast<int>(i + 1);
        return d;
    }

    // Everything else: ModRM (if any) + immediate.
    if (HasModRMOneByte(op)) {
        bool rip = false;
        int dispOff = 0, dispSize = 0;
        const size_t mlen = ModRMLen(p + i + 1, avail - (i + 1), &rip, &dispOff, &dispSize);
        if (mlen == 0) return d;
        // Group 3 (F6/F7): only /0 and /1 (TEST) carry an immediate.
        int imm = ImmSizeOneByte(op, opsize, rexW);
        if (op == 0xF6 || op == 0xF7) {
            const int reg = (p[i + 1] >> 3) & 7;
            imm = (reg <= 1) ? (op == 0xF6 ? 1 : (opsize ? 2 : 4)) : 0;
        }
        const size_t len = i + 1 + mlen + static_cast<size_t>(imm);
        if (len > avail) return d;
        d.length = len;
        d.understood = true;
        d.ripRelative = rip;
        d.dispOffset = rip ? static_cast<int>(i + 1 + dispOff) : 0;
        return d;
    }
    // One-byte opcode with only an immediate (push imm, alu eAX/AL, test).
    const int imm = ImmSizeOneByte(op, opsize, rexW);
    if (imm > 0) {
        const size_t len = i + 1 + static_cast<size_t>(imm);
        if (len > avail) return d;
        d.length = len;
        d.understood = true;
        return d;
    }
    return d;  // not understood
}

size_t PrologueLength(const uint8_t* code, size_t need, std::string* why) {
    size_t total = 0;
    // A generous window; NGX prologues are short and this never runs off code.
    while (total < need) {
        const DecodedInsn d = DecodeInsn(code + total, 32);
        if (!d.understood || d.length == 0) {
            if (why) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "unrecognized instruction at +%zu (byte 0x%02X)", total,
                              code[total]);
                *why = buf;
            }
            return 0;
        }
        // A rel8 branch is fine: Install widens it to rel32 in the trampoline.
        total += d.length;
    }
    return total;
}

InlineHook::~InlineHook() {
    if (trampoline_) VirtualFree(trampoline_, 0, MEM_RELEASE);
}

bool InlineHook::Install(void* target, void* detour, std::string* error) {
    if (active_) {
        if (error) *error = "already installed";
        return false;
    }
    if (!target || !detour) {
        if (error) *error = "null target or detour";
        return false;
    }
    const uint8_t* code = static_cast<const uint8_t*>(target);
    std::string why;
    const size_t copyLen = PrologueLength(code, kJumpLen, &why);
    if (copyLen == 0) {
        if (error) *error = "prologue not relocatable: " + why;
        return false;
    }
    if (copyLen > sizeof(saved_)) {
        if (error) *error = "prologue too long to save";
        return false;
    }

    // Trampoline: the relocated prologue plus a jump back to target+copyLen.
    // Widening a short branch to its rel32 form can grow an instruction by up to
    // 4 bytes, so allow room for that.
    const size_t trampCap = copyLen * 2 + kJumpLen;
    auto* tramp = static_cast<uint8_t*>(AllocNear(target, trampCap));
    if (!tramp) {
        if (error) *error = "no free memory within 2GB of the target for a trampoline";
        return false;
    }

    // Emit instruction by instruction. RIP-relative operands and rel32 branches
    // are re-based; a rel8 branch is rewritten to its rel32 form so its target
    // survives the move far from the original.
    size_t src = 0, dst = 0;
    bool ok = true;
    while (src < copyLen) {
        const DecodedInsn d = DecodeInsn(code + src, copyLen - src + 32);
        if (!d.understood) {
            ok = false;
            break;
        }
        const uint8_t* si = code + src;
        uint8_t* di = tramp + dst;
        const uintptr_t srcAddr = reinterpret_cast<uintptr_t>(si);
        const uintptr_t dstAddr = reinterpret_cast<uintptr_t>(di);

        if (d.relBranch && d.relSize == 1) {
            int8_t rel8 = 0;
            std::memcpy(&rel8, si + d.relOffset, 1);
            const uintptr_t targetAddr = srcAddr + d.length + rel8;
            const uint8_t op = si[d.relOffset - 1];  // the branch opcode (no prefixes kept)
            if (op == 0xEB) {  // jmp rel8 -> jmp rel32
                di[0] = 0xE9;
                const int64_t r = static_cast<int64_t>(targetAddr) - static_cast<int64_t>(dstAddr + 5);
                if (r > INT32_MAX || r < INT32_MIN) { ok = false; break; }
                const int32_t r32 = static_cast<int32_t>(r);
                std::memcpy(di + 1, &r32, 4);
                dst += 5;
            } else {  // Jcc rel8 (0x70-0x7F) -> Jcc rel32 (0F 8x)
                di[0] = 0x0F;
                di[1] = static_cast<uint8_t>(0x80 + (op - 0x70));
                const int64_t r = static_cast<int64_t>(targetAddr) - static_cast<int64_t>(dstAddr + 6);
                if (r > INT32_MAX || r < INT32_MIN) { ok = false; break; }
                const int32_t r32 = static_cast<int32_t>(r);
                std::memcpy(di + 2, &r32, 4);
                dst += 6;
            }
            src += d.length;
            continue;
        }

        std::memcpy(di, si, d.length);
        const int64_t delta = static_cast<int64_t>(srcAddr) - static_cast<int64_t>(dstAddr);
        if (d.ripRelative) {
            int32_t disp;
            std::memcpy(&disp, di + d.dispOffset, sizeof(disp));
            const int64_t fixed = static_cast<int64_t>(disp) + delta;
            if (fixed > INT32_MAX || fixed < INT32_MIN) { ok = false; break; }
            const int32_t nd = static_cast<int32_t>(fixed);
            std::memcpy(di + d.dispOffset, &nd, sizeof(nd));
        }
        if (d.relBranch && d.relSize == 4) {
            int32_t rel;
            std::memcpy(&rel, di + d.relOffset, sizeof(rel));
            const int64_t fixed = static_cast<int64_t>(rel) + delta;
            if (fixed > INT32_MAX || fixed < INT32_MIN) { ok = false; break; }
            const int32_t nr = static_cast<int32_t>(fixed);
            std::memcpy(di + d.relOffset, &nr, sizeof(nr));
        }
        dst += d.length;
        src += d.length;
    }
    if (!ok) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        if (error) *error = "prologue relocation failed";
        return false;
    }
    WriteAbsJump(tramp + dst, code + copyLen);

    // Patch the target under VirtualProtect, keeping execute rights.
    DWORD oldProtect = 0;
    if (!VirtualProtect(target, kJumpLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        if (error) *error = "VirtualProtect on target failed";
        return false;
    }
    std::memcpy(saved_, code, copyLen);
    savedLen_ = copyLen;
    WriteAbsJump(static_cast<uint8_t*>(target), detour);
    DWORD ignored = 0;
    VirtualProtect(target, kJumpLen, oldProtect, &ignored);

    FlushInstructionCache(GetCurrentProcess(), target, kJumpLen);
    FlushInstructionCache(GetCurrentProcess(), tramp, dst + kJumpLen);

    target_ = target;
    detour_ = detour;
    trampoline_ = tramp;
    active_ = true;
    return true;
}

bool InlineHook::PatchIntact() const {
    if (!active_ || !target_) return false;
    uint8_t expected[kJumpLen];
    WriteAbsJump(expected, detour_);
    uint8_t now[kJumpLen];
    if (!ReadCodeSafe(target_, now, sizeof(now))) return false;
    return std::memcmp(now, expected, sizeof(now)) == 0;
}

void InlineHook::Remove() {
    if (!active_) return;
    if (!PatchIntact()) {
        // Not our bytes any more: never write the saved prologue into them.
        Detach();
        return;
    }
    DWORD oldProtect = 0;
    if (VirtualProtect(target_, savedLen_, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        std::memcpy(target_, saved_, savedLen_);
        DWORD ignored = 0;
        VirtualProtect(target_, savedLen_, oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), target_, savedLen_);
    }
    if (trampoline_) VirtualFree(trampoline_, 0, MEM_RELEASE);
    trampoline_ = nullptr;
    target_ = nullptr;
    detour_ = nullptr;
    savedLen_ = 0;
    active_ = false;
}

void InlineHook::Detach() {
    // The target's memory is gone (module unloaded): free only the trampoline
    // and forget, never write back (spec 6.5).
    if (trampoline_) VirtualFree(trampoline_, 0, MEM_RELEASE);
    trampoline_ = nullptr;
    target_ = nullptr;
    detour_ = nullptr;
    savedLen_ = 0;
    active_ = false;
}

}  // namespace acdb
