#include "indy/core/System.hpp"

#include "indy/core/Log.hpp"
#include "indy/cpu/R4600.hpp"
#include "indy/graphics/Newport.hpp"
#include "indy/graphics/TestPatternDisplay.hpp"
#include "indy/memory/PhysicalMap.hpp"
#include "indy/scsi/CdRomDrive.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <stdexcept>

namespace indy {

namespace {

constexpr unsigned kBusErrorLine = 4; // R4600 Int4 -> Cause.IP6

template <typename... Ts>
struct Overloaded : Ts... { using Ts::operator()...; };
} // namespace

System::System(SystemConfig config, FrameBuffer& display)
    : config_(std::move(config)),
      vblankPeriod_(static_cast<Cycles>(static_cast<double>(config_.cpuClockHz) / config_.refreshHz)),
      // Serial console: whatever the guest transmits on either Z85C30 channel
      // goes straight to the host's stdout (logs use stderr, so they stay
      // separable). Host input is sent back to the channel last written.
      ioc2_([this](Z8530::Channel channel, uint8_t byte) {
          consoleChannel_ = channel;
          std::cout << static_cast<char>(byte);
          std::cout.flush();
      }),
      cpu_(std::make_unique<R4600>(bus_, config_.traceInstructions)),
      graphics_(config_.graphics ? std::unique_ptr<GraphicsSubsystem>(std::make_unique<Newport>(display))
                                 : std::make_unique<TestPatternDisplay>(display)) {

    // ---- Physical memory map ---------------------------------------------
    // RAM storage exists from the start, but only appears in the address space
    // once the PROM programs the MC's MEMCFG registers.
    const auto banks = MemoryController::bankLayoutFor(config_.ramBytes);
    if (!banks) {
        throw std::runtime_error(banks.error());
    }
    bus_.allocateRam(config_.ramBytes);
    mc_ = std::make_unique<MemoryController>(
        bus_,
        MemoryController::Config{.bankBytes = *banks, .cpuClockHz = config_.cpuClockHz},
        [this] { return currentCycle(); });
    bus_.mapDevice(physmap::kMcBase, physmap::kMcSize, *mc_);
    mc_->setScheduler(scheduler_);

    if (!config_.promImage.empty()) {
        // A PROM the user explicitly asked for but that cannot be loaded is fatal.
        const auto loaded = bus_.loadProm(config_.promImage);
        if (!loaded) {
            throw std::runtime_error(loaded.error());
        }
        log::info("PROM: loaded {} bytes from '{}' at {:#010x}",
                  loaded->size, config_.promImage.string(), physmap::kPromBase);
        if (!loaded->orderDetected) {
            log::warn("PROM: reset vector is not a jump/branch in any known byte order; loaded verbatim");
        } else if (loaded->sourceOrder != PromByteOrder::BigEndian) {
            log::info("PROM: image was {}, normalised to big-endian", toString(loaded->sourceOrder));
        }
    } else {
        log::warn("No PROM image given: nothing is mapped at the reset vector, the CPU will halt");
    }

    hpc3_.attachPbusDevice(Hpc3::kIoc2Channel, ioc2_);
    hpc3_.attachScsi(0, scsi_);
    scsi_.attachDma(hpc3_.scsiDma(0));

    // ---- Interrupt routing -------------------------------------------------
    // INT3 drives R4600 Int0..3 (Cause.IP2..IP5); the 8254 counts against
    // emulated time and fires through the scheduler.
    ioc2_.connect([this](unsigned line, bool level) { cpu_->setInterruptLine(line, level); },
                  scheduler_, [this] { return currentCycle(); }, config_.cpuClockHz);
    if (auto* newport = dynamic_cast<Newport*>(graphics_.get())) {
        newport->setClock([this] { return currentCycle(); }, config_.cpuClockHz);
        newport->setRetraceIrqCallback(
            [this](bool level) { ioc2_.int3().setMappable(Int3::kMapVerticalRetrace, level); });
    }
    // SCSI channel 0: the WD33C93 interrupt and the HPC3 DMA interrupt share
    // INT3 local 0 bit 1.
    auto updateScsiIrq = [this] {
        ioc2_.int3().setLocal0(Int3::kL0Scsi0, scsiChipIrq_ || scsiDmaIrq_);
    };
    scsi_.setIrqCallback([this, updateScsiIrq](bool level) { scsiChipIrq_ = level; updateScsiIrq(); });
    // MC: DMA done -> INT3 local 0 bit 4; bus errors latch in the MC, writes
    // interrupt the CPU directly on Int4 (Cause.IP6).
    mc_->setDmaIrqCallback([this](bool level) { ioc2_.int3().setLocal0(Int3::kL0McDma, level); });
    mc_->setBusErrorCallback([this](bool level) { cpu_->setInterruptLine(kBusErrorLine, level); });
    static_cast<R4600&>(*cpu_).setBusErrorHook(
        [this](PhysAddr addr, bool isWrite) { mc_->reportCpuBusError(addr, isWrite); });
    hpc3_.setScsiIrqCallback([this, updateScsiIrq](unsigned channel, bool level) {
        if (channel == 0) {
            scsiDmaIrq_ = level;
            updateScsiIrq();
        }
    });
    rtc_ = std::make_unique<Ds1386>(
        Ds1386::Config{.cpuClockHz = config_.cpuClockHz, .backingFile = config_.nvramFile},
        [this] { return currentCycle(); });
    hpc3_.attachBbram(*rtc_);
    bus_.mapDevice(physmap::kHpc3Base, physmap::kHpc3Size, hpc3_);

    // Graphics: the Newport answers in the GIO graphics slot; without it the
    // PROM finds no video and uses the serial console.
    if (config_.graphics) {
        bus_.mapDevice(physmap::kNewportBase, Newport::kRegisterWindow, *graphics_);
    }

    // ---- Peripherals -----------------------------------------------------
    auto cdrom = std::make_unique<CdRomDrive>();
    cdrom_ = cdrom.get();
    scsi_.attach(config_.cdromScsiId, std::move(cdrom));
    if (!config_.cdromImage.empty()) {
        insertMedia(config_.cdromImage);
    }

    log::info("System: R4600 @ {} MHz, {} MiB RAM (bank A {} MiB, bank B {} MiB), CD-ROM at SCSI ID {}",
              config_.cpuClockHz / 1'000'000, config_.ramBytes / MiB, (*banks)[0] / MiB,
              (*banks)[1] / MiB, config_.cdromScsiId);
    reset();
}

void System::reset() {
    scheduler_.clear();
    cpu_->reset();
    mc_->reset();
    hpc3_.reset(); // also resets PBUS devices (IOC2, SCC)
    scsi_.reset();
    graphics_->reset();
    scheduleVBlank(now_ + vblankPeriod_);
}

void System::runFor(Cycles budget) {
    // CPU chunks are capped so events scheduled by MMIO writes mid-chunk (e.g.
    // an 8254 reprogram) are serviced within a few hundred cycles.
    constexpr Cycles kMaxChunk = 256;
    const Cycles target = now_ + budget;
    while (now_ < target) {
        // Never let the CPU run past the next device event.
        const Cycles stop = std::min({target, scheduler_.nextEventTime(), now_ + kMaxChunk});
        if (stop > now_) {
            const Cycles ran = cpu_->run(stop - now_);
            assert(ran > 0);
            now_ += ran;
        }
        scheduler_.runDue(now_);
    }
}

void System::scheduleVBlank(Cycles when) {
    scheduler_.scheduleAt(when, [this](Cycles at) {
        graphics_->onVBlank(at);
        scheduleVBlank(at + vblankPeriod_);
    });
}

void System::insertMedia(const std::filesystem::path& image) {
    if (auto result = cdrom_->insertMedia(image)) {
        log::info("CD-ROM: inserted '{}' ({} blocks)", image.string(), cdrom_->blockCount());
    } else {
        log::error("CD-ROM: {}", result.error());
    }
}

void System::handle(const Command& command) {
    std::visit(Overloaded{
        [this](const cmd::InsertMedia& c) { insertMedia(c.path); },
        [this](const cmd::EjectMedia&) {
            cdrom_->ejectMedia();
            log::info("CD-ROM: ejected");
        },
        [this](const cmd::ResetSystem&) {
            log::info("System: reset");
            reset();
        },
        [this](const cmd::KeyEvent& c) { ioc2_.kbc().hostKey(c.hidUsage, c.pressed); },
        [this](const cmd::MouseEvent& c) { ioc2_.kbc().hostMouse(c.dx, c.dy, c.buttons); },
        [this](const cmd::SerialInput& c) {
            // Terminals send LF for Enter; serial consoles expect CR.
            ioc2_.scc().receive(consoleChannel_, c.byte == '\n' ? uint8_t{'\r'} : c.byte);
        },
    }, command);
}

} // namespace indy
