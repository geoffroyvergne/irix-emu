#pragma once

#include "indy/core/MmioDevice.hpp"
#include "indy/core/WarnOnce.hpp"
#include "indy/hpc/Int3.hpp"
#include "indy/hpc/Pit8254.hpp"
#include "indy/input/Kbc8042.hpp"
#include "indy/serial/Z8530.hpp"

#include <array>

namespace indy {

// SGI IOC2, Guinness (Indy) variant: the I/O controller on HPC3 PBUS channel 6
// (physical 0x1FBD9800). It decodes the Z85C30 serial controller, the 8042
// keyboard/mouse controller, the parallel port, the front panel, board ID and
// the INT3 interrupt controller with its 8254 timer.
//
// Refs: Linux arch/mips/include/asm/sgi/ioc.h (struct sgioc_regs), MAME
// src/mame/sgi/ioc2.cpp/.h. Every register is 8 bits wide in the low byte of
// a 32-bit slot (byte address slot + 3); 32-bit accesses use bits 7:0.
class Ioc2 final : public MmioDevice {
public:
    // Byte offsets of each register's 32-bit slot.
    enum Reg : uint32_t {
        kParallelFirst = 0x00, // PI1 parallel port, 0x00..0x27
        kParallelLast  = 0x27,
        kSerialBCtrl   = 0x30, // Z85C30 channel B control
        kSerialBData   = 0x34, // Z85C30 channel B data
        kSerialACtrl   = 0x38, // Z85C30 channel A control
        kSerialAData   = 0x3C, // Z85C30 channel A data
        kKbdData       = 0x40, // 8042 data
        kKbdCommand    = 0x44, // 8042 status (read) / command (write)
        kGcSelect      = 0x48,
        kGenCtrl       = 0x4C,
        kPanel         = 0x50, // front panel: power/volume buttons
        kSysId         = 0x58, // board ID (read-only)
        kRead          = 0x60,
        kDmaSel        = 0x68,
        kReset         = 0x70, // active-low resets, LED control on Guinness
        kWrite         = 0x78,
        // INT3 interrupt controller (Guinness).
        kLocal0Status  = 0x80,
        kLocal0Mask    = 0x84,
        kLocal1Status  = 0x88,
        kLocal1Mask    = 0x8C,
        kMapStatus     = 0x90,
        kMapMask0      = 0x94,
        kMapMask1      = 0x98,
        kMapPolarity   = 0x9C,
        kTimerClear    = 0xA0,
        kErrorStatus   = 0xA4,
        // 8254 PIT.
        kPitCounter0   = 0xB0,
        kPitCounter1   = 0xB4,
        kPitCounter2   = 0xB8,
        kPitControl    = 0xBC,
    };

    // Board ID: bit 0 = Full House (Indigo2), [4:1] board rev, [7:5] chip rev.
    // Indy: not Full House, board rev 3, chip rev 1.
    static constexpr uint8_t kGuinnessSysId = 0x26;
    // Front panel at reset: both volume buttons released, power on.
    static constexpr uint8_t kPanelResetValue = 0xA1;
    static constexpr uint8_t kPanelIntMask    = 0x52; // power, vol-down, vol-up "interrupt" bits

    explicit Ioc2(Z8530::TxSink serialSink) : scc_(std::move(serialSink)) { reset(); }

    [[nodiscard]] std::string_view name() const noexcept override { return "IOC2"; }
    void reset() override;
    [[nodiscard]] bool read(uint32_t offset, AccessSize size, uint64_t& value) override;
    [[nodiscard]] bool write(uint32_t offset, AccessSize size, uint64_t value) override;

    [[nodiscard]] Z8530& scc() noexcept { return scc_; }
    [[nodiscard]] Kbc8042& kbc() noexcept { return kbc_; }
    [[nodiscard]] Int3& int3() noexcept { return int3_; }
    [[nodiscard]] Pit8254& pit() noexcept { return pit_; }

    // Wire the interrupt outputs to the CPU and give the 8254 a time base.
    void connect(Int3::CpuLine cpu, Scheduler& scheduler, Pit8254::Clock clock, uint64_t cpuClockHz);

private:
    static constexpr size_t kSlots = 0xC0 / 4;

    [[nodiscard]] uint8_t readRegister(uint32_t slot);
    void writeRegister(uint32_t slot, uint8_t value);
    void unimplemented(uint32_t slot, bool isWrite, uint8_t value);

    Z8530 scc_;
    Kbc8042 kbc_;
    Int3 int3_;
    Pit8254 pit_;
    std::array<uint8_t, kSlots> regs_{};
    WarnOnce warnings_;
};

} // namespace indy
