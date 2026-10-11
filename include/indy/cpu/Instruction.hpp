#pragma once

#include <cstdint>

// MIPS III instruction encoding.
namespace indy::mips {

// A raw 32-bit instruction word with field accessors. Plain shifts and masks
// are used instead of C++ bit-fields because bit-field layout is
// implementation-defined; these compile to the same one or two instructions.
//
//   R-type: | opcode:6 | rs:5 | rt:5 | rd:5 | shamt:5 | funct:6 |
//   I-type: | opcode:6 | rs:5 | rt:5 |        immediate:16      |
//   J-type: | opcode:6 |              target:26                 |
struct Instruction {
    uint32_t raw = 0;

    [[nodiscard]] constexpr uint32_t opcode() const noexcept { return raw >> 26; }
    [[nodiscard]] constexpr uint32_t rs() const noexcept { return (raw >> 21) & 0x1F; }
    [[nodiscard]] constexpr uint32_t rt() const noexcept { return (raw >> 16) & 0x1F; }
    [[nodiscard]] constexpr uint32_t rd() const noexcept { return (raw >> 11) & 0x1F; }
    [[nodiscard]] constexpr uint32_t shamt() const noexcept { return (raw >> 6) & 0x1F; }
    [[nodiscard]] constexpr uint32_t funct() const noexcept { return raw & 0x3F; }
    [[nodiscard]] constexpr uint16_t imm() const noexcept { return static_cast<uint16_t>(raw); }
    // Immediate/offset sign-extended to 64 bits.
    [[nodiscard]] constexpr int64_t simm() const noexcept { return static_cast<int16_t>(raw); }
    [[nodiscard]] constexpr uint32_t target() const noexcept { return raw & 0x03FF'FFFF; }
};

static_assert(Instruction{0x3C02'3041}.opcode() == 0x0F);      // lui $2, 0x3041
static_assert(Instruction{0x3C02'3041}.rt() == 2);
static_assert(Instruction{0x3C02'3041}.imm() == 0x3041);
static_assert(Instruction{0x1420'FFFC}.simm() == -4);           // bnez $1, -4
static_assert(Instruction{0x0005'2D82}.shamt() == 0x16);        // srl $5, $5, 22

// Primary opcode field (bits 31..26).
namespace op {
enum : uint32_t {
    Special = 0x00, RegImm = 0x01, J     = 0x02, Jal   = 0x03,
    Beq     = 0x04, Bne    = 0x05, Blez  = 0x06, Bgtz  = 0x07,
    Addi    = 0x08, Addiu  = 0x09, Slti  = 0x0A, Sltiu = 0x0B,
    Andi    = 0x0C, Ori    = 0x0D, Xori  = 0x0E, Lui   = 0x0F,
    Cop0    = 0x10, Cop1   = 0x11, Cop2  = 0x12, Cop1x = 0x13,
    Beql    = 0x14, Bnel   = 0x15, Blezl = 0x16, Bgtzl = 0x17,
    Daddi   = 0x18, Daddiu = 0x19, Ldl   = 0x1A, Ldr   = 0x1B,
    Lb      = 0x20, Lh     = 0x21, Lwl   = 0x22, Lw    = 0x23,
    Lbu     = 0x24, Lhu    = 0x25, Lwr   = 0x26, Lwu   = 0x27,
    Sb      = 0x28, Sh     = 0x29, Swl   = 0x2A, Sw    = 0x2B,
    Sdl     = 0x2C, Sdr    = 0x2D, Swr   = 0x2E, Cache = 0x2F,
    Ll      = 0x30, Lwc1   = 0x31, Lwc2  = 0x32, Pref  = 0x33,
    Lld     = 0x34, Ldc1   = 0x35, Ldc2  = 0x36, Ld    = 0x37,
    Sc      = 0x38, Swc1   = 0x39, Swc2  = 0x3A,
    Scd     = 0x3C, Sdc1   = 0x3D, Sdc2  = 0x3E, Sd    = 0x3F,
};
} // namespace op

// SPECIAL function field (opcode 0x00, bits 5..0).
namespace fn {
enum : uint32_t {
    Sll    = 0x00, Srl    = 0x02, Sra    = 0x03,
    Sllv   = 0x04, Srlv   = 0x06, Srav   = 0x07,
    Jr     = 0x08, Jalr   = 0x09, Syscall = 0x0C, Break = 0x0D, Sync = 0x0F,
    Mfhi   = 0x10, Mthi   = 0x11, Mflo   = 0x12, Mtlo   = 0x13,
    Dsllv  = 0x14, Dsrlv  = 0x16, Dsrav  = 0x17,
    Mult   = 0x18, Multu  = 0x19, Div    = 0x1A, Divu   = 0x1B,
    Dmult  = 0x1C, Dmultu = 0x1D, Ddiv   = 0x1E, Ddivu  = 0x1F,
    Add    = 0x20, Addu   = 0x21, Sub    = 0x22, Subu   = 0x23,
    And    = 0x24, Or     = 0x25, Xor    = 0x26, Nor    = 0x27,
    Slt    = 0x2A, Sltu   = 0x2B,
    Dadd   = 0x2C, Daddu  = 0x2D, Dsub   = 0x2E, Dsubu  = 0x2F,
    Tge    = 0x30, Tgeu   = 0x31, Tlt    = 0x32, Tltu   = 0x33, Teq = 0x34, Tne = 0x36,
    Dsll   = 0x38, Dsrl   = 0x3A, Dsra   = 0x3B,
    Dsll32 = 0x3C, Dsrl32 = 0x3E, Dsra32 = 0x3F,
};
} // namespace fn

// REGIMM rt field (opcode 0x01, bits 20..16).
namespace regimm {
enum : uint32_t {
    Bltz   = 0x00, Bgez   = 0x01, Bltzl   = 0x02, Bgezl   = 0x03,
    Tgei   = 0x08, Tgeiu  = 0x09, Tlti    = 0x0A, Tltiu   = 0x0B, Teqi = 0x0C, Tnei = 0x0E,
    Bltzal = 0x10, Bgezal = 0x11, Bltzall = 0x12, Bgezall = 0x13, // BAL = BGEZAL $zero
};
} // namespace regimm

// COP0 rs field (opcode 0x10, bits 25..21), and function field when rs.CO is set.
namespace cop0 {
enum : uint32_t {
    Mf = 0x00, Dmf = 0x01, Mt = 0x04, Dmt = 0x05,
    Co = 0x10, // rs bit 4: "coprocessor operation", function in funct
};
namespace co {
enum : uint32_t { Tlbr = 0x01, Tlbwi = 0x02, Tlbwr = 0x06, Tlbp = 0x08, Eret = 0x18 };
} // namespace co
} // namespace cop0

// COP1 rs field (opcode 0x11, bits 25..21).
namespace cop1 {
enum : uint32_t {
    Mf = 0x00, Dmf = 0x01, Cf = 0x02, // MFC1, DMFC1, CFC1
    Mt = 0x04, Dmt = 0x05, Ct = 0x06, // MTC1, DMTC1, CTC1
    Bc = 0x08,                        // BC1F/BC1T/BC1FL/BC1TL (rt selects)
    S  = 0x10, D = 0x11, W = 0x14, L = 0x15, // arithmetic formats
};
} // namespace cop1

} // namespace indy::mips
