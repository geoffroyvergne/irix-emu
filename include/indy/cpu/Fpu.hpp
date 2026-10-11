#pragma once

#include <cstdint>
#include <optional>

namespace indy::fpu {

// R4600 COP1 arithmetic (MIPS III): .S/.D arithmetic, conversions between
// S/D/W/L, and C.cond compares, computed with host IEEE 754 arithmetic under
// the guest's rounding mode.
//
// The R4600 hands some cases (denormal operands/results, invalid integer
// conversions) to the kernel as "unimplemented operation" traps, and the
// kernel's software FPU emulator then produces the IEEE result. Computing
// the IEEE result directly gives software the same answer without the trap.
//
// NaNs use the legacy MIPS encoding, the opposite of x86/ARM: a quiet NaN has
// the fraction's top bit CLEAR, a signalling NaN has it set. Results of
// invalid operations are the MIPS default quiet NaNs below.

// FCR31 cause bits, as a 6-bit field (FCR31 bits 17:12). Bits 4:0 also
// give the enable (bits 11:7) and flag (bits 6:2) layouts.
enum Cause : uint32_t {
    kInexact       = 0x01,
    kUnderflow     = 0x02,
    kOverflow      = 0x04,
    kDivideByZero  = 0x08,
    kInvalid       = 0x10,
    kUnimplemented = 0x20, // E: no enable bit, always traps
};

// COP1 rs field values selecting the operand format.
enum Format : uint32_t { kSingle = 0x10, kDouble = 0x11, kWord = 0x14, kLong = 0x15 };

inline constexpr uint32_t kDefaultNanS = 0x7FBF'FFFF;
inline constexpr uint64_t kDefaultNanD = 0x7FF7'FFFF'FFFF'FFFF;

inline constexpr uint32_t kFcr31FlushToZero = 0x0100'0000; // FS

struct Result {
    uint64_t value = 0;   // S/W results in bits 31:0
    uint32_t cause = 0;   // Cause bits raised by the operation
    bool wide = false;    // the result is a D/L (64-bit) value
};

// Executes COP1 function `funct` (0x00..0x2F) on operands of format `fmt`.
// `fs`/`ft` hold S/W operands in bits 31:0. `fcr31` supplies the rounding
// mode (bits 1:0) and FS. Encodings the R4600 doesn't implement come back
// with kUnimplemented as the cause.
[[nodiscard]] Result operate(uint32_t funct, uint32_t fmt, uint64_t fs, uint64_t ft, uint32_t fcr31);

struct Comparison {
    bool condition = false;
    uint32_t cause = 0;
};

// C.cond.fmt: `cond` is the low 4 bits of the function code (bit 0
// unordered, bit 1 equal, bit 2 less than, bit 3 signal on any NaN).
[[nodiscard]] Comparison compare(uint32_t cond, uint32_t fmt, uint64_t fs, uint64_t ft);

} // namespace indy::fpu
