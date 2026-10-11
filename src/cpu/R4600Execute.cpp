// R4600 instruction semantics: decode dispatch and the MIPS III integer ISA.
//
// Conventions:
//   * 32-bit operations sign-extend their result to 64 bits (MIPS III rule).
//   * Operands are read before any destination is written, so rd == rs works.
//   * Adding an instruction = one `case` in the relevant switch.

#include "indy/cpu/R4600.hpp"

#include "indy/core/Log.hpp"
#include "indy/cpu/AddressSpace.hpp"
#include "indy/cpu/Fpu.hpp"

#include <format>
#include <limits>
#include <type_traits>

namespace indy {

using mips::Instruction;

namespace {

constexpr uint64_t sx32(uint64_t value) noexcept {
    return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(value))));
}
constexpr int64_t s64(uint64_t value) noexcept { return static_cast<int64_t>(value); }
constexpr int32_t s32(uint64_t value) noexcept { return static_cast<int32_t>(static_cast<uint32_t>(value)); }
constexpr uint32_t u32(uint64_t value) noexcept { return static_cast<uint32_t>(value); }

// Signed-overflow tests for ADD/ADDI/SUB (32-bit) and DADD/DADDI/DSUB (64-bit).
constexpr bool addOverflows32(uint64_t a, uint64_t b) noexcept {
    const int64_t sum = int64_t{s32(a)} + int64_t{s32(b)};
    return sum != static_cast<int32_t>(sum);
}
constexpr bool subOverflows32(uint64_t a, uint64_t b) noexcept {
    const int64_t diff = int64_t{s32(a)} - int64_t{s32(b)};
    return diff != static_cast<int32_t>(diff);
}
constexpr bool addOverflows64(uint64_t a, uint64_t b) noexcept {
    const uint64_t sum = a + b;
    return ((a ^ sum) & (b ^ sum)) >> 63 != 0;
}
constexpr bool subOverflows64(uint64_t a, uint64_t b) noexcept {
    const uint64_t diff = a - b;
    return ((a ^ b) & (a ^ diff)) >> 63 != 0;
}

// Portable 64x64 -> 128-bit multiply (no __int128 on MSVC).
struct U128 { uint64_t hi, lo; };
constexpr U128 mulu64(uint64_t a, uint64_t b) noexcept {
    const uint64_t aLo = a & 0xFFFF'FFFF, aHi = a >> 32;
    const uint64_t bLo = b & 0xFFFF'FFFF, bHi = b >> 32;
    const uint64_t ll = aLo * bLo, lh = aLo * bHi, hl = aHi * bLo, hh = aHi * bHi;
    const uint64_t mid = (ll >> 32) + (lh & 0xFFFF'FFFF) + (hl & 0xFFFF'FFFF);
    return {hh + (lh >> 32) + (hl >> 32) + (mid >> 32), (mid << 32) | (ll & 0xFFFF'FFFF)};
}
constexpr U128 muls64(uint64_t a, uint64_t b) noexcept {
    U128 r = mulu64(a, b);
    if (s64(a) < 0) r.hi -= b;
    if (s64(b) < 0) r.hi -= a;
    return r;
}
static_assert(mulu64(~uint64_t{0}, 2).hi == 1 && mulu64(~uint64_t{0}, 2).lo == ~uint64_t{1});
static_assert(muls64(~uint64_t{0}, 2).hi == ~uint64_t{0}); // -1 * 2 = -2

} // namespace

// ---------------------------------------------------------------------------
// Control flow
// ---------------------------------------------------------------------------

void R4600::branch(bool taken, int64_t offset) noexcept {
    // Target is relative to the delay slot, which pc_ already points at. The
    // slot exists whether or not the branch is taken (it matters for Cause.BD).
    if (taken) {
        nextPc_ = pc_ + static_cast<uint64_t>(offset * 4);
    }
    branchPending_ = true;
}

void R4600::branchLikely(bool taken, int64_t offset) noexcept {
    if (taken) {
        nextPc_ = pc_ + static_cast<uint64_t>(offset * 4);
        branchPending_ = true;
    } else {
        // Not taken: the delay slot is nullified (skipped entirely).
        pc_ = nextPc_;
        nextPc_ += 4;
    }
}

void R4600::trap(bool condition) {
    if (condition) {
        raiseException(ExcCode::Tr);
    }
}

// ---------------------------------------------------------------------------
// Memory access
// ---------------------------------------------------------------------------

template <BusWord T>
bool R4600::load(VirtAddr vaddr, T& out) {
    if (vaddr % sizeof(T) != 0) [[unlikely]] {
        raiseAddressError(vaddr, Access::Load);
        return false;
    }
    const auto phys = translate(vaddr, Access::Load);
    if (!phys) [[unlikely]] {
        return false; // exception raised
    }
    if (!bus_.read(*phys, out)) [[unlikely]] {
        unmappedAccess(*phys, sizeof(T), /*isWrite=*/false);
        if (busErrorHook_) busErrorHook_(*phys, false);
        raiseException(ExcCode::DBE); // the load cannot complete
        return false;
    }
    return true;
}

template <BusWord T>
bool R4600::store(VirtAddr vaddr, T value) {
    if (vaddr % sizeof(T) != 0) [[unlikely]] {
        raiseAddressError(vaddr, Access::Store);
        return false;
    }
    const auto phys = translate(vaddr, Access::Store);
    if (!phys) [[unlikely]] {
        return false; // exception raised
    }
    if (!bus_.write(*phys, value)) [[unlikely]] {
        // Posted write: the CPU moves on; the board reports it asynchronously.
        unmappedAccess(*phys, sizeof(T), /*isWrite=*/true);
        if (busErrorHook_) busErrorHook_(*phys, true);
    }
    return true;
}

template <BusWord T, bool SignExtend>
void R4600::loadOp(Instruction insn) {
    T value;
    if (!load(effectiveAddress(insn), value)) {
        return;
    }
    if constexpr (SignExtend) {
        setGpr(insn.rt(), static_cast<uint64_t>(static_cast<int64_t>(static_cast<std::make_signed_t<T>>(value))));
    } else {
        setGpr(insn.rt(), value);
    }
}

template <BusWord T>
void R4600::storeOp(Instruction insn) {
    (void)store(effectiveAddress(insn), static_cast<T>(gpr_[insn.rt()]));
}

// Unaligned loads (big-endian). The aligned word/doubleword containing the
// address is read once; LxL merges the bytes from the address to the end of
// that unit into the high end of rt, LxR the bytes from its start up to the
// address into the low end. The other bytes of rt are kept.
template <BusWord T>
void R4600::loadUnaligned(Instruction insn, bool left) {
    constexpr unsigned kBytes = sizeof(T);
    constexpr uint64_t kAll = kBytes == 8 ? ~uint64_t{0} : 0xFFFF'FFFFu;
    const VirtAddr va = effectiveAddress(insn);
    const unsigned byte = static_cast<unsigned>(va & (kBytes - 1));
    T unit;
    if (!load(va & ~VirtAddr{kBytes - 1}, unit)) {
        return; // exception raised
    }
    const uint64_t mem = unit;
    const uint64_t old = gpr_[insn.rt()] & kAll;
    uint64_t merged;
    bool full;
    if (left) {
        const unsigned shift = byte * 8;
        merged = ((mem << shift) | (old & ((uint64_t{1} << shift) - 1))) & kAll;
        full = true; // the top byte always comes from memory
    } else {
        const unsigned shift = (kBytes - 1 - byte) * 8;
        const uint64_t keep = shift == 0 ? 0 : (kAll & ~(kAll >> shift));
        merged = ((mem >> shift) | (old & keep)) & kAll;
        full = shift == 0;
    }
    if constexpr (kBytes == 4) {
        // A fully replaced word is sign-extended; a partial LWR leaves bits 63:32.
        merged = full ? sx32(merged) : ((gpr_[insn.rt()] & 0xFFFF'FFFF'0000'0000) | merged);
    }
    setGpr(insn.rt(), merged);
}

// Unaligned stores (big-endian): SxL writes the high bytes of rt to the
// address up to the end of the unit, SxR the low bytes of rt from the start
// of the unit up to the address. Bytes are stored individually, so no
// read-modify-write ever touches neighbouring bytes or device registers.
template <BusWord T>
void R4600::storeUnaligned(Instruction insn, bool left) {
    constexpr unsigned kBytes = sizeof(T);
    const VirtAddr va = effectiveAddress(insn);
    const VirtAddr aligned = va & ~VirtAddr{kBytes - 1};
    const unsigned byte = static_cast<unsigned>(va & (kBytes - 1));
    const uint64_t value = gpr_[insn.rt()];
    const unsigned top = (kBytes - 1) * 8; // shift of rt's most significant byte
    if (left) {
        for (unsigned i = 0; i + byte < kBytes; ++i) {
            if (!store(va + i, static_cast<uint8_t>(value >> (top - 8 * i)))) return;
        }
    } else {
        for (unsigned j = 0; j <= byte; ++j) {
            if (!store(aligned + j, static_cast<uint8_t>(value >> (8 * (byte - j))))) return;
        }
    }
}

// ---------------------------------------------------------------------------
// Primary opcode dispatch
// ---------------------------------------------------------------------------

void R4600::execute(Instruction insn) {
    const uint64_t rs = gpr_[insn.rs()];
    const uint64_t rt = gpr_[insn.rt()];
    const int64_t simm = insn.simm();

    switch (insn.opcode()) {
    case mips::op::Special: executeSpecial(insn); break;
    case mips::op::RegImm:  executeRegImm(insn); break;
    case mips::op::Cop0:    executeCop0(insn); break;

    // ---- Jumps: target keeps the top 4 bits of the delay-slot address --------
    case mips::op::J:
        jump((pc_ & ~uint64_t{0x0FFF'FFFF}) | (uint64_t{insn.target()} << 2));
        break;
    case mips::op::Jal:
        link(31);
        jump((pc_ & ~uint64_t{0x0FFF'FFFF}) | (uint64_t{insn.target()} << 2));
        break;

    // ---- Branches -------------------------------------------------------------
    case mips::op::Beq:   branch(rs == rt, simm); break;
    case mips::op::Bne:   branch(rs != rt, simm); break;
    case mips::op::Blez:  branch(s64(rs) <= 0, simm); break;
    case mips::op::Bgtz:  branch(s64(rs) > 0, simm); break;
    case mips::op::Beql:  branchLikely(rs == rt, simm); break;
    case mips::op::Bnel:  branchLikely(rs != rt, simm); break;
    case mips::op::Blezl: branchLikely(s64(rs) <= 0, simm); break;
    case mips::op::Bgtzl: branchLikely(s64(rs) > 0, simm); break;

    // ---- Immediate arithmetic / logic ----------------------------------------
    case mips::op::Addi:
        if (addOverflows32(rs, static_cast<uint64_t>(simm))) {
            raiseException(ExcCode::Ov); // ADDI
        } else {
            setGpr(insn.rt(), sx32(rs + static_cast<uint64_t>(simm)));
        }
        break;
    case mips::op::Addiu: setGpr(insn.rt(), sx32(rs + static_cast<uint64_t>(simm))); break;
    case mips::op::Slti:  setGpr(insn.rt(), s64(rs) < simm ? 1 : 0); break;
    case mips::op::Sltiu: setGpr(insn.rt(), rs < static_cast<uint64_t>(simm) ? 1 : 0); break;
    case mips::op::Andi:  setGpr(insn.rt(), rs & insn.imm()); break;
    case mips::op::Ori:   setGpr(insn.rt(), rs | insn.imm()); break;
    case mips::op::Xori:  setGpr(insn.rt(), rs ^ insn.imm()); break;
    case mips::op::Lui:   setGpr(insn.rt(), sx32(uint64_t{insn.imm()} << 16)); break;
    case mips::op::Daddi:
        if (addOverflows64(rs, static_cast<uint64_t>(simm))) {
            raiseException(ExcCode::Ov); // DADDI
        } else {
            setGpr(insn.rt(), rs + static_cast<uint64_t>(simm));
        }
        break;
    case mips::op::Daddiu: setGpr(insn.rt(), rs + static_cast<uint64_t>(simm)); break;

    // ---- Loads & stores (aligned) --------------------------------------------
    case mips::op::Lb:  loadOp<uint8_t, true>(insn); break;
    case mips::op::Lbu: loadOp<uint8_t, false>(insn); break;
    case mips::op::Lh:  loadOp<uint16_t, true>(insn); break;
    case mips::op::Lhu: loadOp<uint16_t, false>(insn); break;
    case mips::op::Lw:  loadOp<uint32_t, true>(insn); break;
    case mips::op::Lwu: loadOp<uint32_t, false>(insn); break;
    case mips::op::Ld:  loadOp<uint64_t, false>(insn); break;
    case mips::op::Sb:  storeOp<uint8_t>(insn); break;
    case mips::op::Sh:  storeOp<uint16_t>(insn); break;
    case mips::op::Sw:  storeOp<uint32_t>(insn); break;
    case mips::op::Sd:  storeOp<uint64_t>(insn); break;

    // ---- Unaligned pairs -------------------------------------------------------
    case mips::op::Lwl: loadUnaligned<uint32_t>(insn, true); break;
    case mips::op::Lwr: loadUnaligned<uint32_t>(insn, false); break;
    case mips::op::Ldl: loadUnaligned<uint64_t>(insn, true); break;
    case mips::op::Ldr: loadUnaligned<uint64_t>(insn, false); break;
    case mips::op::Swl: storeUnaligned<uint32_t>(insn, true); break;
    case mips::op::Swr: storeUnaligned<uint32_t>(insn, false); break;
    case mips::op::Sdl: storeUnaligned<uint64_t>(insn, true); break;
    case mips::op::Sdr: storeUnaligned<uint64_t>(insn, false); break;

    // ---- Load-linked / store-conditional --------------------------------------
    // Single CPU, no snooping: the reservation is only broken by ERET.
    case mips::op::Ll:
    case mips::op::Lld: {
        const VirtAddr va = effectiveAddress(insn);
        if (insn.opcode() == mips::op::Ll) {
            uint32_t value;
            if (!load(va, value)) break;
            setGpr(insn.rt(), sx32(value));
        } else {
            uint64_t value;
            if (!load(va, value)) break;
            setGpr(insn.rt(), value);
        }
        llBit_ = true;
        break;
    }
    case mips::op::Sc:
    case mips::op::Scd: {
        if (!llBit_) {
            setGpr(insn.rt(), 0);
            break;
        }
        const VirtAddr va = effectiveAddress(insn);
        const bool stored = insn.opcode() == mips::op::Sc ? store(va, u32(rt)) : store(va, rt);
        if (stored) setGpr(insn.rt(), 1);
        break;
    }

    // ---- Coprocessors 1/2 --------------------------------------------------------
    case mips::op::Cop1:
    case mips::op::Cop1x:
    case mips::op::Lwc1:
    case mips::op::Ldc1:
    case mips::op::Swc1:
    case mips::op::Sdc1:
        executeCop1(insn);
        break;
    case mips::op::Cop2:
    case mips::op::Lwc2:
    case mips::op::Ldc2:
    case mips::op::Swc2:
    case mips::op::Sdc2:
        // The R4600 has no COP2: unusable unless CU2 is set, then reserved.
        if (!coprocessorUsable(2)) raiseException(ExcCode::CpU, 2);
        else unimplemented(insn);
        break;

    // Caches are not modelled: CACHE operations (index invalidate, fill, ...) are no-ops.
    case mips::op::Cache: break;

    default:
        unimplemented(insn);
        break;
    }
}

// ---------------------------------------------------------------------------
// SPECIAL (opcode 0x00): register-register ALU, shifts, jumps, HI/LO
// ---------------------------------------------------------------------------

void R4600::executeSpecial(Instruction insn) {
    const uint64_t rs = gpr_[insn.rs()];
    const uint64_t rt = gpr_[insn.rt()];
    const unsigned rd = insn.rd();
    const unsigned sa = insn.shamt();

    switch (insn.funct()) {
    // ---- Shifts (32-bit results sign-extended) --------------------------------
    case mips::fn::Sll:  setGpr(rd, sx32(u32(rt) << sa)); break; // sll $0,$0,0 == NOP
    case mips::fn::Srl:  setGpr(rd, sx32(u32(rt) >> sa)); break;
    case mips::fn::Sra:  setGpr(rd, sx32(static_cast<uint32_t>(s32(rt) >> sa))); break;
    case mips::fn::Sllv: setGpr(rd, sx32(u32(rt) << (rs & 31))); break;
    case mips::fn::Srlv: setGpr(rd, sx32(u32(rt) >> (rs & 31))); break;
    case mips::fn::Srav: setGpr(rd, sx32(static_cast<uint32_t>(s32(rt) >> (rs & 31)))); break;

    // ---- 64-bit shifts ---------------------------------------------------------
    case mips::fn::Dsll:   setGpr(rd, rt << sa); break;
    case mips::fn::Dsrl:   setGpr(rd, rt >> sa); break;
    case mips::fn::Dsra:   setGpr(rd, static_cast<uint64_t>(s64(rt) >> sa)); break;
    case mips::fn::Dsll32: setGpr(rd, rt << (sa + 32)); break;
    case mips::fn::Dsrl32: setGpr(rd, rt >> (sa + 32)); break;
    case mips::fn::Dsra32: setGpr(rd, static_cast<uint64_t>(s64(rt) >> (sa + 32))); break;
    case mips::fn::Dsllv:  setGpr(rd, rt << (rs & 63)); break;
    case mips::fn::Dsrlv:  setGpr(rd, rt >> (rs & 63)); break;
    case mips::fn::Dsrav:  setGpr(rd, static_cast<uint64_t>(s64(rt) >> (rs & 63))); break;

    // ---- Register jumps (target alignment is checked at fetch) -----------------
    case mips::fn::Jr:
        jump(rs);
        break;
    case mips::fn::Jalr:
        link(rd); // rs was read above, so jalr $ra, $ra still jumps to the old value
        jump(rs);
        break;

    case mips::fn::Sync: break; // no reordering to fence in an interpreter

    case mips::fn::Syscall: raiseException(ExcCode::Sys); break;
    case mips::fn::Break:   raiseException(ExcCode::Bp); break;

    // ---- Conditional traps ---------------------------------------------------
    case mips::fn::Tge:  trap(s64(rs) >= s64(rt)); break;
    case mips::fn::Tgeu: trap(rs >= rt); break;
    case mips::fn::Tlt:  trap(s64(rs) < s64(rt)); break;
    case mips::fn::Tltu: trap(rs < rt); break;
    case mips::fn::Teq:  trap(rs == rt); break;
    case mips::fn::Tne:  trap(rs != rt); break;

    // ---- HI/LO -----------------------------------------------------------------
    case mips::fn::Mfhi: setGpr(rd, hi_); break;
    case mips::fn::Mthi: hi_ = rs; break;
    case mips::fn::Mflo: setGpr(rd, lo_); break;
    case mips::fn::Mtlo: lo_ = rs; break;

    // ---- Multiply / divide -----------------------------------------------------
    case mips::fn::Mult: {
        const int64_t product = int64_t{s32(rs)} * int64_t{s32(rt)};
        lo_ = sx32(static_cast<uint64_t>(product));
        hi_ = sx32(static_cast<uint64_t>(product >> 32));
        break;
    }
    case mips::fn::Multu: {
        const uint64_t product = uint64_t{u32(rs)} * uint64_t{u32(rt)};
        lo_ = sx32(product);
        hi_ = sx32(product >> 32);
        break;
    }
    case mips::fn::Div: {
        const int32_t n = s32(rs), d = s32(rt);
        if (d == 0) break; // result UNPREDICTABLE; HI/LO left unchanged
        if (n == std::numeric_limits<int32_t>::min() && d == -1) {
            lo_ = sx32(static_cast<uint32_t>(n));
            hi_ = 0;
        } else {
            lo_ = sx32(static_cast<uint32_t>(n / d));
            hi_ = sx32(static_cast<uint32_t>(n % d));
        }
        break;
    }
    case mips::fn::Divu: {
        const uint32_t n = u32(rs), d = u32(rt);
        if (d == 0) break;
        lo_ = sx32(n / d);
        hi_ = sx32(n % d);
        break;
    }
    case mips::fn::Dmult: {
        const U128 product = muls64(rs, rt);
        lo_ = product.lo;
        hi_ = product.hi;
        break;
    }
    case mips::fn::Dmultu: {
        const U128 product = mulu64(rs, rt);
        lo_ = product.lo;
        hi_ = product.hi;
        break;
    }
    case mips::fn::Ddiv: {
        const int64_t n = s64(rs), d = s64(rt);
        if (d == 0) break;
        if (n == std::numeric_limits<int64_t>::min() && d == -1) {
            lo_ = static_cast<uint64_t>(n);
            hi_ = 0;
        } else {
            lo_ = static_cast<uint64_t>(n / d);
            hi_ = static_cast<uint64_t>(n % d);
        }
        break;
    }
    case mips::fn::Ddivu:
        if (rt == 0) break;
        lo_ = rs / rt;
        hi_ = rs % rt;
        break;

    // ---- Arithmetic ------------------------------------------------------------
    case mips::fn::Add:
        if (addOverflows32(rs, rt)) {
            raiseException(ExcCode::Ov); // ADD
        } else {
            setGpr(rd, sx32(rs + rt));
        }
        break;
    case mips::fn::Addu: setGpr(rd, sx32(rs + rt)); break;
    case mips::fn::Sub:
        if (subOverflows32(rs, rt)) {
            raiseException(ExcCode::Ov); // SUB
        } else {
            setGpr(rd, sx32(rs - rt));
        }
        break;
    case mips::fn::Subu: setGpr(rd, sx32(rs - rt)); break;
    case mips::fn::Dadd:
        if (addOverflows64(rs, rt)) {
            raiseException(ExcCode::Ov); // DADD
        } else {
            setGpr(rd, rs + rt);
        }
        break;
    case mips::fn::Daddu: setGpr(rd, rs + rt); break;
    case mips::fn::Dsub:
        if (subOverflows64(rs, rt)) {
            raiseException(ExcCode::Ov); // DSUB
        } else {
            setGpr(rd, rs - rt);
        }
        break;
    case mips::fn::Dsubu: setGpr(rd, rs - rt); break;

    // ---- Logic / compare -------------------------------------------------------
    case mips::fn::And:  setGpr(rd, rs & rt); break;
    case mips::fn::Or:   setGpr(rd, rs | rt); break;
    case mips::fn::Xor:  setGpr(rd, rs ^ rt); break;
    case mips::fn::Nor:  setGpr(rd, ~(rs | rt)); break;
    case mips::fn::Slt:  setGpr(rd, s64(rs) < s64(rt) ? 1 : 0); break;
    case mips::fn::Sltu: setGpr(rd, rs < rt ? 1 : 0); break;

    default:
        unimplemented(insn);
        break;
    }
}

// ---------------------------------------------------------------------------
// REGIMM (opcode 0x01): branches on sign of rs, optionally linking
// ---------------------------------------------------------------------------

void R4600::executeRegImm(Instruction insn) {
    const int64_t rs = s64(gpr_[insn.rs()]);
    const int64_t simm = insn.simm();

    // The "and link" forms write $ra whether or not the branch is taken.
    switch (insn.rt()) {
    case mips::regimm::Bltz:    branch(rs < 0, simm); break;
    case mips::regimm::Bgez:    branch(rs >= 0, simm); break;
    case mips::regimm::Bltzl:   branchLikely(rs < 0, simm); break;
    case mips::regimm::Bgezl:   branchLikely(rs >= 0, simm); break;
    case mips::regimm::Bltzal:  link(31); branch(rs < 0, simm); break;
    case mips::regimm::Bgezal:  link(31); branch(rs >= 0, simm); break; // BAL when rs = $zero
    case mips::regimm::Bltzall: link(31); branchLikely(rs < 0, simm); break;
    case mips::regimm::Bgezall: link(31); branchLikely(rs >= 0, simm); break;

    // Immediate traps (the "U" forms compare unsigned against the sign-extended immediate).
    case mips::regimm::Tgei:  trap(rs >= simm); break;
    case mips::regimm::Tgeiu: trap(static_cast<uint64_t>(rs) >= static_cast<uint64_t>(simm)); break;
    case mips::regimm::Tlti:  trap(rs < simm); break;
    case mips::regimm::Tltiu: trap(static_cast<uint64_t>(rs) < static_cast<uint64_t>(simm)); break;
    case mips::regimm::Teqi:  trap(rs == simm); break;
    case mips::regimm::Tnei:  trap(rs != simm); break;

    default:
        unimplemented(insn);
        break;
    }
}

// ---------------------------------------------------------------------------
// COP0 (opcode 0x10): system control coprocessor
// ---------------------------------------------------------------------------

void R4600::executeCop0(Instruction insn) {
    if (!coprocessorUsable(0)) {
        raiseException(ExcCode::CpU, 0);
        return;
    }
    const unsigned reg = insn.rd();

    switch (insn.rs()) {
    case mips::cop0::Mf:  setGpr(insn.rt(), sx32(cop0_[reg])); return; // MFC0 rt, rd
    case mips::cop0::Dmf: setGpr(insn.rt(), cop0_[reg]); return;
    case mips::cop0::Mt:  writeCop0(reg, sx32(gpr_[insn.rt()])); return; // MTC0 rt, rd
    case mips::cop0::Dmt: writeCop0(reg, gpr_[insn.rt()]); return;
    default: break;
    }

    if ((insn.rs() & mips::cop0::Co) != 0) {
        switch (insn.funct()) {
        case mips::cop0::co::Tlbr:  tlbRead(); return;
        case mips::cop0::co::Tlbwi: tlbWrite(static_cast<unsigned>(cop0_[kIndex] & 0x3F)); return;
        case mips::cop0::co::Tlbwr: tlbWrite(static_cast<unsigned>(cop0_[kRandom])); return;
        case mips::cop0::co::Tlbp:  tlbProbe(); return;
        case mips::cop0::co::Eret:  eret(); return;
        // R4600 low-power standby until an interrupt. The kernel only issues it
        // from its idle loop, which re-checks for work, so running on is safe.
        case mips::cop0::co::Wait:  return;
        default: break;
        }
    }
    unimplemented(insn);
}

// ---------------------------------------------------------------------------
// COP1 (FPU): register file, moves, loads/stores, branches, and the
// arithmetic/conversion/compare formats (computed by indy::fpu).
// ---------------------------------------------------------------------------

namespace {
constexpr uint64_t kStatusFR   = uint64_t{1} << 26;
constexpr uint32_t kFcr31C     = 0x0080'0000; // condition bit (compares, BC1x)
constexpr uint32_t kFcr31CE    = 0x0002'0000; // cause: unimplemented operation
constexpr uint32_t kFcr31Cause = 0x0001'F000; // cause V Z O U I
constexpr uint32_t kFcr31CauseAll = 0x0003'F000; // cause E V Z O U I
constexpr uint32_t kFcr31Enable = 0x0000'0F80; // enable V Z O U I
// Writable: FS, C, cause (incl. E), enables, flags, rounding mode. Bits 22:18 read 0.
constexpr uint32_t kFcr31WritableMask = 0x0183'FFFF;
} // namespace

bool R4600::fpr64Mode() const noexcept {
    return (cop0_[kStatus] & kStatusFR) != 0;
}

uint32_t R4600::readFpr32(unsigned reg) const noexcept {
    if (fpr64Mode() || (reg & 1) == 0) {
        return static_cast<uint32_t>(fpr_[reg]);
    }
    return static_cast<uint32_t>(fpr_[reg & ~1u] >> 32);
}

void R4600::writeFpr32(unsigned reg, uint32_t value) noexcept {
    if (fpr64Mode() || (reg & 1) == 0) {
        // FR=1 leaves the upper half undefined; keeping it is a valid choice.
        fpr_[reg] = (fpr_[reg] & 0xFFFF'FFFF'0000'0000) | value;
    } else {
        uint64_t& pair = fpr_[reg & ~1u];
        pair = (pair & 0x0000'0000'FFFF'FFFF) | (uint64_t{value} << 32);
    }
}

uint64_t R4600::readFpr64(unsigned reg) const noexcept {
    // FR=0: odd specifiers are undefined for 64-bit accesses; use the pair.
    return fpr_[fpr64Mode() ? reg : (reg & ~1u)];
}

void R4600::writeFpr64(unsigned reg, uint64_t value) noexcept {
    fpr_[fpr64Mode() ? reg : (reg & ~1u)] = value;
}

void R4600::writeFcr31(uint32_t value) {
    fcr31_ = value & kFcr31WritableMask;
    // Setting a cause bit whose exception is enabled (or the always-enabled
    // "unimplemented operation" cause) traps immediately.
    const uint32_t enabledCauses = ((fcr31_ & kFcr31Cause) >> 5) & (fcr31_ & kFcr31Enable);
    if ((fcr31_ & kFcr31CE) != 0 || enabledCauses != 0) {
        raiseException(ExcCode::FPE);
    }
}

void R4600::executeCop1(Instruction insn) {
    if (!coprocessorUsable(1)) {
        raiseException(ExcCode::CpU, 1); // Status.CU1 (bit 29) clear
        return;
    }

    switch (insn.opcode()) {
    // ---- Loads/stores: ft is the rt field ---------------------------------------
    case mips::op::Lwc1: {
        uint32_t value;
        if (load(effectiveAddress(insn), value)) writeFpr32(insn.rt(), value);
        return;
    }
    case mips::op::Ldc1: {
        uint64_t value;
        if (load(effectiveAddress(insn), value)) writeFpr64(insn.rt(), value);
        return;
    }
    case mips::op::Swc1: (void)store(effectiveAddress(insn), readFpr32(insn.rt())); return;
    case mips::op::Sdc1: (void)store(effectiveAddress(insn), readFpr64(insn.rt())); return;
    case mips::op::Cop1: break;
    default:
        unimplemented(insn); // COP1X is MIPS IV: not an R4600 instruction
        return;
    }

    const unsigned rt = insn.rt();
    const unsigned fs = insn.rd(); // fs occupies the rd field

    switch (insn.rs()) {
    case mips::cop1::Mf: setGpr(rt, sx32(readFpr32(fs))); return;      // MFC1 rt, fs
    case mips::cop1::Mt: writeFpr32(fs, u32(gpr_[rt])); return;        // MTC1 rt, fs
    case mips::cop1::Dmf:                                              // DMFC1 rt, fs
        if (fpr64Mode() || (fs & 1) == 0) setGpr(rt, readFpr64(fs));
        return;
    case mips::cop1::Dmt:                                              // DMTC1 rt, fs
        if (fpr64Mode() || (fs & 1) == 0) writeFpr64(fs, gpr_[rt]);
        return;
    case mips::cop1::Cf:                                               // CFC1 rt, fs
        setGpr(rt, fs == 0 ? sx32(kFcr0) : fs == 31 ? sx32(fcr31_) : 0);
        return;
    case mips::cop1::Ct:                                               // CTC1 rt, fs
        if (fs == 31) writeFcr31(u32(gpr_[rt]));                       // FCR0 is read-only
        return;
    case mips::cop1::Bc: {
        // rt bit 0 = branch on true, bit 1 = "likely".
        const bool condition = (fcr31_ & kFcr31C) != 0;
        const bool taken = (rt & 1) != 0 ? condition : !condition;
        if ((rt & 2) != 0) branchLikely(taken, insn.simm());
        else branch(taken, insn.simm());
        return;
    }
    case mips::cop1::S:
    case mips::cop1::D:
    case mips::cop1::W:
    case mips::cop1::L:
        executeFpuOperation(insn);
        return;
    default:
        // Reserved rs encodings trap as unimplemented operations.
        completeFpuOperation(fpu::kUnimplemented);
        return;
    }
}

bool R4600::completeFpuOperation(uint32_t cause) {
    // Every arithmetic instruction rewrites the cause field. An enabled
    // exception (or E, which has no enable) traps before anything is written
    // and leaves the sticky flags alone; otherwise the flags accumulate.
    fcr31_ = (fcr31_ & ~kFcr31CauseAll) | (cause << 12);
    const uint32_t enables = (fcr31_ & kFcr31Enable) >> 7;
    if ((cause & fpu::kUnimplemented) != 0 || (cause & enables) != 0) {
        raiseException(ExcCode::FPE);
        return false;
    }
    fcr31_ |= (cause & 0x1F) << 2;
    return true;
}

void R4600::executeFpuOperation(Instruction insn) {
    const uint32_t fmt   = insn.rs();
    const uint32_t funct = insn.funct();
    const unsigned ft = insn.rt();
    const unsigned fs = insn.rd();
    const unsigned fd = insn.shamt(); // fd occupies the shamt field
    const bool wideOperands = fmt == mips::cop1::D || fmt == mips::cop1::L;
    auto operand = [&](unsigned reg) -> uint64_t { return wideOperands ? readFpr64(reg) : readFpr32(reg); };

    if (funct >= 0x30) { // C.cond.fmt
        const fpu::Comparison c = fpu::compare(funct & 0xF, fmt, operand(fs), operand(ft));
        if (completeFpuOperation(c.cause)) {
            fcr31_ = c.condition ? (fcr31_ | kFcr31C) : (fcr31_ & ~kFcr31C);
        }
        return;
    }

    const fpu::Result r = fpu::operate(funct, fmt, operand(fs), operand(ft), fcr31_);
    // MOV.fmt is a plain copy and leaves the cause field untouched.
    const bool isMove = funct == 0x06 && r.cause == 0;
    if (!isMove && !completeFpuOperation(r.cause)) {
        return;
    }
    if (r.wide) {
        writeFpr64(fd, r.value);
    } else {
        writeFpr32(fd, static_cast<uint32_t>(r.value));
    }
}

} // namespace indy
