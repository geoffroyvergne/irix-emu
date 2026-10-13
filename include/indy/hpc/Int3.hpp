#pragma once

#include <cstdint>
#include <functional>

namespace indy {

// SGI INT3 interrupt controller (inside the Indy's IOC2, registers at
// 0x1FBD9880..0x1FBD98A7). Aggregates level-sensitive device interrupts into
// the R5000's hardware interrupt inputs:
//
//   local 0 (status & mask)  -> Int0 = Cause.IP2
//   local 1 (status & mask)  -> Int1 = Cause.IP3
//   8254 counter 0 / 1       -> Int2 / Int3 = Cause.IP4 / IP5 (latched until cleared)
//   (bus error -> IP6, R5000 Count/Compare -> IP7 are elsewhere)
//
// "Mappable" sources cascade through map mask 0 into local 0 bit 7 and through
// map mask 1 into local 1 bit 3. Refs: Linux arch/mips/sgi-ip22/ip22-int.c,
// asm/sgi/ioc.h; MAME src/mame/sgi/ioc2.cpp/.h.
class Int3 {
public:
    // Local 0 sources.
    static constexpr uint8_t kL0Fifo     = 0x01;
    static constexpr uint8_t kL0Scsi0    = 0x02;
    static constexpr uint8_t kL0Scsi1    = 0x04;
    static constexpr uint8_t kL0Ethernet = 0x08;
    static constexpr uint8_t kL0McDma    = 0x10;
    static constexpr uint8_t kL0Parallel = 0x20;
    static constexpr uint8_t kL0Graphics = 0x40;
    static constexpr uint8_t kL0Mappable = 0x80; // cascade from map status & map mask 0
    // Local 1 sources.
    static constexpr uint8_t kL1Gp0      = 0x01;
    static constexpr uint8_t kL1Panel    = 0x02;
    static constexpr uint8_t kL1Gp2      = 0x04;
    static constexpr uint8_t kL1Mappable = 0x08; // cascade from map status & map mask 1
    static constexpr uint8_t kL1HpcDma   = 0x10;
    static constexpr uint8_t kL1AcFail   = 0x20;
    static constexpr uint8_t kL1Video    = 0x40;
    static constexpr uint8_t kL1Retrace  = 0x80;
    // Mappable sources.
    static constexpr uint8_t kMapVerticalRetrace = 0x01; // Newport (Linux SGI_VERT_IRQ)
    static constexpr uint8_t kMapKeyboardMouse = 0x10;
    static constexpr uint8_t kMapSerial        = 0x20;

    // R5000 hardware interrupt inputs Int[n] (Cause.IP(n+2)).
    static constexpr unsigned kCpuLocal0 = 0;
    static constexpr unsigned kCpuLocal1 = 1;
    static constexpr unsigned kCpuTimer0 = 2;
    static constexpr unsigned kCpuTimer1 = 3;

    // Register offsets within the IOC2 (32-bit slots, byte in the low lane).
    enum Reg : uint32_t {
        kLocal0Status = 0x80, kLocal0Mask = 0x84, kLocal1Status = 0x88, kLocal1Mask = 0x8C,
        kMapStatus = 0x90, kMapMask0 = 0x94, kMapMask1 = 0x98, kMapPolarity = 0x9C,
        kTimerClear = 0xA0, kErrorStatus = 0xA4,
    };

    using CpuLine = std::function<void(unsigned line, bool asserted)>;

    void connect(CpuLine cpu) { cpu_ = std::move(cpu); update(); }
    void reset();

    // Device inputs: level-sensitive, held until the device deasserts.
    void setLocal0(uint8_t source, bool asserted) { setBits(local0_, source, asserted); }
    void setLocal1(uint8_t source, bool asserted) { setBits(local1_, source, asserted); }
    void setMappable(uint8_t source, bool asserted) { setBits(map_, source, asserted); }

    // 8254 counter 0/1 output rising edge: latched until cleared via kTimerClear.
    void timerFired(unsigned counter);

    [[nodiscard]] uint8_t read(uint32_t slot) const;
    void write(uint32_t slot, uint8_t value);

    [[nodiscard]] uint8_t local0Status() const;
    [[nodiscard]] uint8_t local1Status() const;

private:
    void setBits(uint8_t& reg, uint8_t bits, bool asserted);
    void update();

    CpuLine cpu_;
    uint8_t local0_ = 0, local1_ = 0, map_ = 0;
    uint8_t mask0_ = 0, mask1_ = 0, mapMask0_ = 0, mapMask1_ = 0, mapPolarity_ = 0;
    bool timer0_ = false, timer1_ = false;
    // Last level driven onto each CPU line, to only report changes.
    bool lineState_[4] = {false, false, false, false};
};

} // namespace indy
