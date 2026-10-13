#pragma once

#include "indy/core/Types.hpp"

namespace indy {

// Abstract CPU core. Keeping this an interface lets us swap the interpreter for
// a cached interpreter or a JIT later without touching System or the devices.
// Processor fitted to the Indy. The R4600 implements MIPS III; the R5000
// (shipped as R5000PC/SC Indys) adds MIPS IV: conditional moves, PREF,
// indexed FP loads/stores, FP multiply-add, RECIP/RSQRT and eight FP
// condition codes. Some IRIX software (e.g. parts of Maya 6.5) is MIPS IV only.
enum class CpuModel : uint8_t { R4600, R5000 };

class Cpu {
public:
    Cpu() = default;
    virtual ~Cpu() = default;
    Cpu(const Cpu&) = delete;
    Cpu& operator=(const Cpu&) = delete;

    virtual void reset() = 0;

    // Execute for roughly `budget` cycles and return the cycles actually
    // consumed. May overshoot by one instruction; must return > 0 when budget > 0.
    [[nodiscard]] virtual Cycles run(Cycles budget) = 0;

    // Cycles consumed so far inside the current run() call (0 outside it).
    // Lets devices see time advance mid-slice: System::currentCycle().
    [[nodiscard]] virtual Cycles elapsedInRun() const noexcept = 0;

    // Drive one of the hardware interrupt inputs Int[5:0] (Cause.IP2..IP7).
    virtual void setInterruptLine(unsigned line, bool asserted) = 0;

    [[nodiscard]] virtual uint64_t pc() const noexcept = 0;
};

} // namespace indy
