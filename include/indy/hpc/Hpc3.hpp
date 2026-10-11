#pragma once

#include "indy/core/MmioDevice.hpp"
#include "indy/core/WarnOnce.hpp"
#include "indy/memory/MemoryBus.hpp"
#include "indy/scsi/ScsiDmaPort.hpp"

#include <array>
#include <functional>
#include <vector>

namespace indy {

// SGI HPC3: the Indy's peripheral controller (physical 0x1FB80000, 512 KiB).
// It hosts the SCSI and Ethernet DMA engines, the 8 PBUS DMA channels, and 16
// PBUS PIO channels through which the 8-bit peripherals (IOC2, audio, RTC...)
// are reached.
//
// Address map (offsets from the HPC3 base), from Linux asm/sgi/hpc3.h and
// MAME src/mame/sgi/hpc3.cpp:
//   0x00000-0x0FFFF  PBUS DMA channels (0x2000 each)
//   0x10000-0x1FFFF  SCSI 0/1 and Ethernet DMA registers
//   0x20000-0x2FFFF  PBUS / SCSI / Ethernet FIFOs (PIO access)
//   0x30000-0x3000F  INTSTAT, MISC, EEPROM, INTSTAT mirror
//   0x40000-0x4FFFF  WD33C93 SCSI controllers 0/1
//   0x54000-0x544FF  SEEQ 80C03 Ethernet controller
//   0x58000-0x5BFFF  PBUS PIO: 16 channels x 0x400 (IOC2 = channel 6)
//   0x5C000-0x5CFFF  PBUS DMA configuration
//   0x5D000-0x5DFFF  PBUS PIO configuration
//   0x60000-0x7FFFF  battery-backed RAM / RTC
//
// The two SCSI DMA channels are emulated (descriptor chains in memory); the
// other DMA/config register files are plain read/write storage (enough for the
// PROM's register tests). PBUS PIO channels, the SCSI controller windows and
// the RTC/BBRAM window dispatch to attached devices. PBUS/Ethernet DMA and the
// FIFOs are not emulated yet.
class Hpc3 final : public MmioDevice {
public:
    static constexpr unsigned kPbusChannels = 16;
    static constexpr unsigned kIoc2Channel  = 6;

    enum Region : uint32_t {
        kDmaRegsBase    = 0x00000, // PBUS DMA + SCSI/Ethernet DMA register files
        kDmaRegsEnd     = 0x20000,
        kIntStat        = 0x30000,
        kMisc           = 0x30004,
        kEeprom         = 0x30008,
        kIntStatMirror  = 0x3000C,
        kPbusPioBase    = 0x58000,
        kPbusPioStride  = 0x400,
        kPbusPioEnd     = 0x5C000,
        kPbusCfgBase    = 0x5C000, // DMA config then PIO config
        kPbusCfgEnd     = 0x5E000,
        kScsiBase       = 0x40000, // WD33C93 channel 0, then channel 1
        kScsiStride     = 0x08000,
        kScsiEnd        = 0x50000,
        kBbramBase      = 0x60000, // RTC registers + battery-backed RAM
        kBbramEnd       = 0x80000,
    };

    // SCSI DMA channel registers (channel n at + n * kScsiDmaStride).
    enum ScsiDmaReg : uint32_t {
        kScsiDmaBase   = 0x10000,
        kScsiDmaStride = 0x02000,
        kScsiCbp       = 0x0000, // current buffer pointer (read-only)
        kScsiNbdp      = 0x0004, // next buffer descriptor pointer
        kScsiBc        = 0x1000, // byte count [13:0], XIE 0x4000, EOX 0x8000
        kScsiCtrl      = 0x1004,
    };
    static constexpr uint32_t kSctrlIrq    = 0x01; // read: interrupt pending (cleared by the read)
    static constexpr uint32_t kSctrlEndian = 0x02; // 1 = little-endian byte order
    static constexpr uint32_t kSctrlDir    = 0x04;
    static constexpr uint32_t kSctrlFlush  = 0x08; // drain FIFO; the channel then goes idle
    static constexpr uint32_t kSctrlActive = 0x10;
    static constexpr uint32_t kSctrlReset  = 0x40; // reset channel and external controller
    // Descriptor flags (count/info word in memory).
    static constexpr uint32_t kDescEox     = 0x8000'0000;
    static constexpr uint32_t kDescXie     = 0x2000'0000;
    static constexpr uint32_t kDescCount   = 0x0000'3FFF;

    explicit Hpc3(MemoryBus& bus);

    // Called when a SCSI DMA channel's interrupt (end of an XIE buffer) changes.
    void setScsiIrqCallback(std::function<void(unsigned channel, bool level)> cb);

    // DMA port for SCSI channel `channel`, to hand to its controller.
    [[nodiscard]] ScsiDmaPort& scsiDma(unsigned channel) { return scsiDma_.at(channel); }

    // Route PBUS PIO channel `channel` (offsets relative to its 0x400 window).
    void attachPbusDevice(unsigned channel, MmioDevice& device);
    // Route a SCSI controller window (offsets relative to its 0x8000 window).
    void attachScsi(unsigned channel, MmioDevice& device) { scsi_.at(channel) = &device; }
    // Route the RTC/BBRAM window (offsets relative to kBbramBase).
    void attachBbram(MmioDevice& device) { bbram_ = &device; }

    [[nodiscard]] std::string_view name() const noexcept override { return "HPC3"; }
    void reset() override;
    [[nodiscard]] bool read(uint32_t offset, AccessSize size, uint64_t& value) override;
    [[nodiscard]] bool write(uint32_t offset, AccessSize size, uint64_t value) override;

private:
    // One SCSI DMA channel: walks a chain of {buffer, count|flags, next}
    // descriptors in memory, one byte per controller DMA request.
    struct ScsiDma final : ScsiDmaPort {
        MemoryBus* bus = nullptr;
        uint32_t cbp = 0, nbdp = 0, count = 0, ctrl = 0;
        bool eox = false, xie = false, active = false, irq = false;
        std::function<void(bool)> notify;
        void setIrq(bool level) {
            if (level != irq) {
                irq = level;
                if (notify) notify(level);
            }
        }

        size_t toMemory(std::span<const uint8_t> bytes) override;
        size_t fromMemory(std::span<uint8_t> bytes) override;
        void reset();
        void loadDescriptor();
        void endOfBuffer();
        [[nodiscard]] PhysAddr address() const {
            return (ctrl & kSctrlEndian) != 0 ? PhysAddr{cbp ^ 3u} : PhysAddr{cbp};
        }
        [[nodiscard]] bool ready(); // active with bytes left in the current buffer
    };

    [[nodiscard]] bool scsiDmaRead(uint32_t offset, uint64_t& value);
    [[nodiscard]] bool scsiDmaWrite(uint32_t offset, uint64_t value);

    // Big-endian sub-word access into a 32-bit register file.
    [[nodiscard]] static uint64_t extract(uint32_t word, uint32_t offset, AccessSize size);
    static void merge(uint32_t& word, uint32_t offset, AccessSize size, uint64_t value);

    void unimplemented(uint32_t offset, bool isWrite);

    std::vector<uint32_t> dmaRegs_;               // 0x00000-0x1FFFF
    std::vector<uint32_t> pbusCfg_;               // 0x5C000-0x5DFFF
    uint32_t misc_ = 0;
    uint32_t eeprom_ = 0;
    std::array<MmioDevice*, kPbusChannels> pbus_{};
    MmioDevice* bbram_ = nullptr;
    std::array<MmioDevice*, 2> scsi_{};
    std::array<ScsiDma, 2> scsiDma_{};
    WarnOnce warnings_;
};

} // namespace indy
