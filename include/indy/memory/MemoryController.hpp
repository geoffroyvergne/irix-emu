#pragma once

#include "indy/core/MmioDevice.hpp"
#include "indy/core/Scheduler.hpp"
#include "indy/core/Types.hpp"
#include "indy/core/WarnOnce.hpp"
#include "indy/memory/MemoryBus.hpp"

#include <array>
#include <expected>
#include <functional>
#include <optional>
#include <string>

namespace indy {

// SGI MC: the IP22 memory controller. Decodes the SIMM banks into the physical
// address space, provides the RPSS free-running counter, error latches, bus
// arbitration parameters, semaphores and the "virtual DMA" engine.
//
// Register map from Linux arch/mips/include/asm/sgi/mc.h and MAME's
// src/mame/sgi/mc.cpp, cross-checked against the accesses the Indy PROM makes.
// Every register is a 32-bit value in an 8-byte slot. Software uses the
// big-endian low word (slot + 4), but the whole slot decodes to the register,
// so slot + 0 aliases it.
class MemoryController final : public MmioDevice {
public:
    static constexpr unsigned kBanks = 4; // MC supports 4 banks; the Indy board has 2

    // Slot offsets from physmap::kMcBase.
    enum Reg : uint32_t {
        kCpuCtrl0        = 0x0000, // MC_CPUCTRL0: refresh, parity, watchdog, endianness
        kCpuCtrl1        = 0x0008, // MC_CPUCTRL1: GIO timeout, HPC/EXP endianness
        kWatchdog        = 0x0010, // MC_WATCHDOG: read-only count, any write clears
        kSysId           = 0x0018, // MC_SYSID: [3:0] MC revision, [4] EISA present
        kRpssDivider     = 0x0028, // [7:0] divide-1, [15:8] increment
        kEeprom          = 0x0030, // serial EEPROM bit-bang for the R4k
        kRefreshPreload  = 0x0040,
        kRefreshCount    = 0x0048, // read-only
        kGioParams       = 0x0080, // GIO64 arbitration parameters
        kCpuArbTime      = 0x0088,
        kLongBurstTime   = 0x0098,
        kMemCfg0         = 0x00C0, // MC_MEMCFG0: banks A (upper half) and B (lower half)
        kMemCfg1         = 0x00C8, // MC_MEMCFG1: banks C and D
        kCpuMemAccess    = 0x00D0, // CPU memory access timing
        kGioMemAccess    = 0x00D8, // GIO memory access timing
        kCpuErrorAddr    = 0x00E0, // read-only
        kCpuErrorStatus  = 0x00E8, // any write clears
        kGioErrorAddr    = 0x00F0, // read-only
        kGioErrorStatus  = 0x00F8, // any write clears
        kSysSemaphore    = 0x0100,
        kGioLock         = 0x0108,
        kEisaLock        = 0x0110,
        kGioDmaTransMask = 0x0150,
        kGioDmaSubstBits = 0x0158,
        kDmaIntrCause    = 0x0160, // bit 3: DMA done (when enabled in kDmaCtrl)
        kDmaCtrl         = 0x0168, // bit 4: interrupt on completion
        kDmaTlbFirst     = 0x0180, // 4 entries x (hi, lo) up to 0x01B8
        kDmaTlbLast      = 0x01B8,
        kRpssCounter     = 0x1000, // read-only, ticks every 100 ns with the reset divider
        // Virtual DMA engine ("MC_DMA_*").
        kDmaMemAddr      = 0x2000, // MC_DMA_MEMAD: virtual memory address
        kDmaMemAddrDefs  = 0x2008, // same register (alternate decode)
        kDmaSize         = 0x2010, // MC_DMA_LINE_CNT: [31:16] lines, [15:0] line width (bytes)
        kDmaStride       = 0x2018, // [25:16] zoom, [15:0] signed stride (bytes)
        kDmaGioAddr      = 0x2020, // GIO-side address (fill: the 32-bit fill pattern)
        kDmaGioAddrStart = 0x2028, // GIO address + start
        kDmaMode         = 0x2030, // see kDmaMode* bits
        kDmaCount        = 0x2038, // [25:16] zoom count, [15:0] byte count
        kDmaStart        = 0x2040, // write bit 0 = start
        kDmaRun          = 0x2048, // status: 0x40 running, 0x08 done (read-only)
        kDmaDefsStart    = 0x2070, // GIO address + load defaults + start
        kSemaphoreFirst  = 0x10000, // 16 semaphores, 0x1000 apart
        kSemaphoreLast   = 0x1F000,
    };

    // DMA mode bits.
    static constexpr uint32_t kDmaModeToHost = 0x02; // GIO -> memory (else memory -> GIO)
    static constexpr uint32_t kDmaModeSync   = 0x04;
    static constexpr uint32_t kDmaModeFill   = 0x08; // with ToHost: fill memory with the GIO address value
    static constexpr uint32_t kDmaModeIncr   = 0x10; // memory address increments (else decrements)
    static constexpr uint32_t kDmaModeSnoop  = 0x20;
    // DMA_RUN status.
    static constexpr uint32_t kDmaRunActive  = 0x40;
    static constexpr uint32_t kDmaRunDone    = 0x08;

    // MC revision C, no EISA bus (the Indigo2 sets bit 4).
    static constexpr uint32_t kIndySysId = 0x03;

    // The MC runs at 50 MHz: with the reset divider 0x0104 (divide by 5) the
    // RPSS counter ticks at 10 MHz, matching mc.h's "chirps at 100ns".
    static constexpr uint64_t kClockHz = 50'000'000;

    using Clock = std::function<Cycles()>; // current CPU cycle, for the RPSS counter

    struct Config {
        std::array<uint64_t, kBanks> bankBytes{}; // installed RAM per bank (0 = empty)
        uint64_t cpuClockHz = 133'000'000;
        uint32_t sysId = kIndySysId;
    };

    // Split a total RAM size into SIMM banks: the largest power-of-two bank
    // (4..128 MiB) first, the remainder in bank B. E.g. 64 -> A:64, 96 -> A:64 B:32.
    [[nodiscard]] static std::expected<std::array<uint64_t, kBanks>, std::string> bankLayoutFor(uint64_t totalBytes);

    // RAM storage must already be allocated on `bus` (sum of all banks).
    MemoryController(MemoryBus& bus, Config config, Clock clock);

    [[nodiscard]] std::string_view name() const noexcept override { return "MC"; }
    void reset() override;
    [[nodiscard]] bool read(uint32_t offset, AccessSize size, uint64_t& value) override;
    [[nodiscard]] bool write(uint32_t offset, AccessSize size, uint64_t value) override;

    // Register value by slot offset (inspection/tests; no read side effects).
    [[nodiscard]] uint32_t peek(uint32_t slot) const;

    // ---- Interrupt outputs ------------------------------------------------
    using Line = std::function<void(bool)>;
    // DMA done (INT3 local 0 bit 4): DMA_INTR_CAUSE bit 3, set at completion
    // when DMA_CTRL bit 4 enables it, cleared by writing the cause register.
    void setDmaIrqCallback(Line line) { dmaIrq_ = std::move(line); updateDmaIrq(); }
    // Bus error (R5000 Int4 = Cause.IP6): asserted while a write error is latched.
    void setBusErrorCallback(Line line) { busErrorIrq_ = std::move(line); updateBusErrorIrq(); }
    // Lets DMA completion happen on time rather than on the next status read.
    void setScheduler(Scheduler& scheduler) { scheduler_ = &scheduler; }

    // The CPU touched an address nothing decodes. Latches the address and
    // status (CPU or GIO error registers); posted writes also raise IP6, reads
    // are reported synchronously by the CPU as a bus error exception.
    void reportCpuBusError(PhysAddr addr, bool isWrite);

    static constexpr uint32_t kCpuErrStatusAddr = 0x0400; // memory bus error, bad address
    static constexpr uint32_t kGioErrStatusTime = 0x0400; // GIO bus timed out

private:
    static constexpr size_t kRegSlots = 0x2080 / 8;

    [[nodiscard]] static bool isKnownSlot(uint32_t slot) noexcept;
    [[nodiscard]] uint32_t readRegister(uint32_t slot);
    void writeRegister(uint32_t slot, uint32_t value);

    void updateRpss();
    void remapRam();

    // Virtual DMA. The data moves synchronously at start; DMA_RUN keeps reading
    // "active" until `startTicks` 33.33 MHz ticks have elapsed, because the
    // PROM checks that a transfer is busy right after starting it.
    void startDma(unsigned startTicks);
    void updateDmaStatus();
    [[nodiscard]] std::optional<PhysAddr> dmaTranslate(uint32_t vaddr);
    [[nodiscard]] bool dmaWriteByte(uint32_t vaddr, uint8_t value);
    [[nodiscard]] bool dmaReadByte(uint32_t vaddr, uint8_t& value);
    unsigned dmaLogged_ = 0;
    bool dmaActive_ = false;
    Cycles dmaDoneAt_ = 0;
    uint64_t dmaGeneration_ = 0; // ignores completion events of superseded transfers

    void updateDmaIrq();
    void updateBusErrorIrq();
    Line dmaIrq_, busErrorIrq_;
    bool dmaIrqLevel_ = false, busErrorLevel_ = false, busErrorFromWrite_ = false;
    Scheduler* scheduler_ = nullptr;

    MemoryBus& bus_;
    Config config_;
    Clock clock_;
    std::array<uint64_t, kBanks> bankOffset_{}; // where each bank lives in RAM storage

    std::array<uint32_t, kRegSlots> regs_{};
    std::array<uint8_t, 16> semaphores_{};

    // RPSS bookkeeping: CPU cycles -> MC ticks, with the fraction carried over.
    Cycles rpssLastUpdate_ = 0;
    uint64_t rpssRemainder_ = 0;

    WarnOnce warnings_;
};

} // namespace indy
