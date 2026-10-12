#pragma once

#include "indy/core/Commands.hpp"
#include "indy/core/FrameBuffer.hpp"
#include "indy/core/Scheduler.hpp"
#include "indy/core/Types.hpp"
#include "indy/cpu/Cpu.hpp"
#include "indy/graphics/GraphicsSubsystem.hpp"
#include "indy/hpc/Ds1386.hpp"
#include "indy/hpc/Hpc3.hpp"
#include "indy/hpc/Ioc2.hpp"
#include "indy/memory/MemoryBus.hpp"
#include "indy/memory/MemoryController.hpp"
#include "indy/scsi/Wd33c93.hpp"

#include <array>
#include <filesystem>
#include <vector>
#include <optional>
#include <memory>

namespace indy {

class CdRomDrive;

struct SystemConfig {
    uint64_t cpuClockHz  = 133'000'000; // R4600 pipeline clock (Indy shipped 100/133 MHz)
    uint64_t ramBytes    = 64 * MiB;
    double   refreshHz   = 60.0;
    uint32_t displayWidth  = 1280;      // Newport XL scan-out
    uint32_t displayHeight = 1024;
    bool     graphics = true;           // Newport XL8 in the GIO graphics slot (false: serial console)
    uint8_t  cdromScsiId = 4;           // conventional Indy CD-ROM ID; configurable
    // Hard disk images, attached at SCSI IDs 1, 2, 3, 5, 6, 7 in order (ID 1 is
    // the conventional system disk, dks0d1; the CD-ROM's ID is skipped).
    std::vector<std::filesystem::path> diskImages;
    uint64_t diskCreateBytes = 0;       // create a blank image of this size if missing
    uint64_t traceInstructions = 0;     // log the first N executed instructions
    std::filesystem::path promImage;    // raw PROM dump; without it the CPU halts at reset
    std::filesystem::path cdromImage;   // optional initial disc
    std::filesystem::path nvramFile = "indy_nvram.bin"; // DS1386 SRAM backing store
    std::optional<std::array<uint8_t, 6>> macAddress; // Ethernet address; default: keep NVRAM's, else SGI default
};

// The machine. Owns every component and the master clock, wires them to the
// bus, and advances time. Not thread-safe by design: after construction it is
// owned and driven exclusively by the EmulationThread.
class System {
public:
    System(SystemConfig config, FrameBuffer& display);

    void reset();

    // Advance emulated time by `budget` cycles, interleaving CPU execution
    // with scheduled device events.
    void runFor(Cycles budget);

    // Apply a UI request (called between slices on the emulation thread).
    void handle(const Command& command);

    [[nodiscard]] Cycles now() const noexcept { return now_; }
    // Exact current cycle, including progress inside the running CPU slice.
    [[nodiscard]] Cycles currentCycle() const noexcept { return now_ + cpu_->elapsedInRun(); }
    [[nodiscard]] const SystemConfig& config() const noexcept { return config_; }

    [[nodiscard]] MemoryBus& bus() noexcept { return bus_; }
    [[nodiscard]] MemoryController& mc() noexcept { return *mc_; }
    [[nodiscard]] Hpc3& hpc3() noexcept { return hpc3_; }
    [[nodiscard]] Ioc2& ioc2() noexcept { return ioc2_; }
    [[nodiscard]] Ds1386& rtc() noexcept { return *rtc_; }
    [[nodiscard]] Cpu& cpu() noexcept { return *cpu_; }
    [[nodiscard]] ScsiController& scsi() noexcept { return scsi_; }
    [[nodiscard]] GraphicsSubsystem& graphics() noexcept { return *graphics_; }
    [[nodiscard]] Scheduler& scheduler() noexcept { return scheduler_; }

private:
    void scheduleVBlank(Cycles when);
    void insertMedia(const std::filesystem::path& image);

    // Declaration order == construction order: the bus must exist before the
    // CPU and devices that hold references to it.
    SystemConfig config_;
    Cycles now_ = 0;
    Cycles vblankPeriod_;

    Scheduler scheduler_;
    MemoryBus bus_; // owns RAM and PROM
    std::unique_ptr<MemoryController> mc_;
    Z8530::Channel consoleChannel_ = Z8530::Channel::A; // last channel the guest wrote to
    bool scsiChipIrq_ = false; // WD33C93 INTRQ
    bool scsiDmaIrq_ = false;  // HPC3 SCSI channel 0 DMA interrupt
    Ioc2 ioc2_; // before hpc3_, which routes PBUS channel 6 to it
    std::unique_ptr<Ds1386> rtc_;
    Hpc3 hpc3_{bus_};

    std::unique_ptr<Cpu> cpu_;
    Wd33c93 scsi_; // HPC3 SCSI channel 0, owns the targets
    CdRomDrive* cdrom_ = nullptr; // non-owning; owned by scsi_
    std::unique_ptr<GraphicsSubsystem> graphics_;
};

} // namespace indy
