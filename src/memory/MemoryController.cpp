#include "indy/memory/MemoryController.hpp"

#include "indy/core/Log.hpp"
#include "indy/memory/PhysicalMap.hpp"

#include <bit>
#include <format>
#include <numeric>
#include <vector>

namespace indy {

namespace {

// MEMCFG: each register holds two 16-bit bank descriptors (upper half = even bank).
constexpr uint32_t kCfgBaseMask  = 0x00FF; // base address in 4 MiB units
constexpr uint32_t kCfgSizeShift = 8;      // [12:8] size: (n + 1) * 4 MiB
constexpr uint32_t kCfgSizeMask  = 0x1F;
constexpr uint32_t kCfgValid     = 0x2000;
constexpr uint32_t kCfgSubbanks  = 0x4000; // two ranks: window split in half per rank
constexpr unsigned kCfgUnitShift = 22;     // 4 MiB

constexpr uint32_t kCpuCtrl0WarmReset = 0x0001'0000 | 0x0004'0000; // WRESETPMEM | WRESETDMEM
constexpr uint32_t kEepromDataIn      = 0x10;
constexpr uint32_t kRpssResetDivider  = 0x0104;

// DMA engine.
constexpr uint32_t kDmaCtrlIntEnable = 0x10;
constexpr uint32_t kDmaIntrDone      = 0x08;
constexpr uint32_t kDmaTlbRegionMask = 0xFFC0'0000; // each entry maps a 4 MiB region
constexpr uint32_t kDmaPteFrameMask  = 0x03FF'FFC0;
constexpr uint32_t kDmaPtBaseMask    = 0x003F'FFC0;
// Defaults loaded by kDmaDefsStart.
constexpr uint32_t kDmaDefaultSize   = 0x0001'000C;
constexpr uint32_t kDmaDefaultStride = 0x0001'0000;
constexpr uint32_t kDmaDefaultCount  = 0x0001'000C;
constexpr uint32_t kDmaDefaultMode   = 0x0000'0028;
constexpr unsigned kDmaLogLimit      = 64;
// Start latency (MAME's DMA timer): 4 ticks after a GIO-address start, 1 after
// the other start registers, at 33.33 MHz.
constexpr uint64_t kDmaTimerHz       = 33'333'333;
constexpr unsigned kDmaTicksGioStart = 4;
constexpr unsigned kDmaTicksStart    = 1;

constexpr uint64_t kMinBank = 4 * MiB;
constexpr uint64_t kMaxBank = 128 * MiB;

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

std::expected<std::array<uint64_t, MemoryController::kBanks>, std::string>
MemoryController::bankLayoutFor(uint64_t totalBytes) {
    std::array<uint64_t, kBanks> banks{};
    uint64_t remaining = totalBytes;
    for (unsigned bank = 0; bank < 2 && remaining != 0; ++bank) { // Indy: banks A and B
        const uint64_t size = std::bit_floor(std::min(remaining, kMaxBank));
        if (size < kMinBank) {
            break;
        }
        banks[bank] = size;
        remaining -= size;
    }
    if (remaining != 0 || totalBytes == 0) {
        return std::unexpected(std::format(
            "{} MiB cannot be built from two SIMM banks of 4..128 MiB each "
            "(try 16, 32, 64, 96, 128, 192 or 256)", totalBytes / MiB));
    }
    return banks;
}

MemoryController::MemoryController(MemoryBus& bus, Config config, Clock clock)
    : bus_(bus), config_(config), clock_(std::move(clock)) {
    uint64_t offset = 0;
    for (unsigned bank = 0; bank < kBanks; ++bank) {
        bankOffset_[bank] = offset;
        offset += config_.bankBytes[bank];
    }
    if (offset > bus_.ram().size()) {
        throw std::invalid_argument("MemoryController: banks exceed allocated RAM");
    }
    reset();
}

void MemoryController::reset() {
    // All registers clear on reset except the system ID and RPSS divider. In
    // particular no bank is valid: RAM is invisible until the PROM programs MEMCFG.
    regs_.fill(0);
    semaphores_.fill(0);
    regs_[kSysId / 8]       = config_.sysId;
    regs_[kRpssDivider / 8] = kRpssResetDivider;
    rpssLastUpdate_ = clock_();
    rpssRemainder_  = 0;
    dmaLogged_      = 0;
    dmaActive_      = false;
    dmaDoneAt_      = 0;
    ++dmaGeneration_;
    busErrorFromWrite_ = false;
    updateDmaIrq();
    updateBusErrorIrq();
    remapRam();
}

uint32_t MemoryController::peek(uint32_t slot) const {
    if (slot >= kSemaphoreFirst) {
        return semaphores_[((slot - kSemaphoreFirst) >> 12) & 15];
    }
    return slot / 8 < kRegSlots ? regs_[slot / 8] : 0;
}

// ---------------------------------------------------------------------------
// Bus interface
// ---------------------------------------------------------------------------

bool MemoryController::isKnownSlot(uint32_t slot) noexcept {
    switch (slot) {
    case kCpuCtrl0: case kCpuCtrl1: case kWatchdog: case kSysId: case kRpssDivider:
    case kEeprom: case kRefreshPreload: case kRefreshCount: case kGioParams:
    case kCpuArbTime: case kLongBurstTime: case kMemCfg0: case kMemCfg1:
    case kCpuMemAccess: case kGioMemAccess: case kCpuErrorAddr: case kCpuErrorStatus:
    case kGioErrorAddr: case kGioErrorStatus: case kSysSemaphore: case kGioLock:
    case kEisaLock: case kGioDmaTransMask: case kGioDmaSubstBits: case kDmaIntrCause:
    case kDmaCtrl: case kRpssCounter: case kDmaMemAddr: case kDmaMemAddrDefs:
    case kDmaSize: case kDmaStride: case kDmaGioAddr: case kDmaGioAddrStart:
    case kDmaMode: case kDmaCount: case kDmaStart: case kDmaRun: case kDmaDefsStart:
        return true;
    default:
        return (slot >= kDmaTlbFirst && slot <= kDmaTlbLast) ||
               (slot >= kSemaphoreFirst && slot <= kSemaphoreLast && (slot & 0xFFF) == 0);
    }
}

bool MemoryController::read(uint32_t offset, AccessSize size, uint64_t& value) {
    const uint32_t slot = offset & ~7u;
    if (!isKnownSlot(slot)) {
        if (warnings_.first(offset)) {
            log::warn("MC: read from unknown register {:#07x}, returning 0", offset);
        }
        value = 0;
        return true;
    }
    const uint32_t word = readRegister(slot);
    switch (size) {
    case AccessSize::Word:
    case AccessSize::Dword: value = word; break;
    case AccessSize::Half:  value = (word >> ((2 - (offset & 2)) * 8)) & 0xFFFF; break;
    case AccessSize::Byte:  value = (word >> ((3 - (offset & 3)) * 8)) & 0xFF; break;
    }
    return true;
}

bool MemoryController::write(uint32_t offset, AccessSize size, uint64_t value) {
    const uint32_t slot = offset & ~7u;
    if (!isKnownSlot(slot)) {
        if (warnings_.first(offset)) {
            log::warn("MC: write {:#010x} to unknown register {:#07x} ignored", value, offset);
        }
        return true;
    }
    if (size == AccessSize::Byte || size == AccessSize::Half) {
        if (warnings_.first(offset | 0x8000'0000u)) {
            log::warn("MC: {}-byte write to register {:#07x} ignored (32-bit only)", byteCount(size), offset);
        }
        return true;
    }
    writeRegister(slot, static_cast<uint32_t>(value));
    return true;
}

// ---------------------------------------------------------------------------
// Register semantics
// ---------------------------------------------------------------------------

uint32_t MemoryController::readRegister(uint32_t slot) {
    if (slot >= kSemaphoreFirst) {
        // Test-and-set: returns the old value and takes the semaphore.
        uint8_t& sem = semaphores_[(slot - kSemaphoreFirst) >> 12];
        const uint32_t old = sem;
        sem = 1;
        return old;
    }
    switch (slot) {
    case kRpssCounter:
        updateRpss();
        return regs_[slot / 8];
    case kEeprom:
        return regs_[slot / 8] & ~kEepromDataIn; // no EEPROM attached yet: DI reads 0
    case kDmaStart:
        return 0;
    case kDmaRun:
    case kDmaIntrCause:
        updateDmaStatus();
        return regs_[slot / 8];
    default:
        return regs_[slot / 8];
    }
}

void MemoryController::writeRegister(uint32_t slot, uint32_t value) {
    if (slot >= kSemaphoreFirst) {
        semaphores_[(slot - kSemaphoreFirst) >> 12] = static_cast<uint8_t>(value & 1);
        return;
    }
    uint32_t& reg = regs_[slot / 8];
    switch (slot) {
    // Read-only registers.
    case kSysId:
    case kRefreshCount:
    case kRpssCounter:
    case kCpuErrorAddr:
    case kGioErrorAddr:
    case kDmaRun:
        return;

    // Any write clears.
    case kWatchdog:
        reg = 0;
        return;
    case kCpuErrorStatus:
    case kGioErrorStatus:
        reg = 0;
        if (regs_[kCpuErrorStatus / 8] == 0 && regs_[kGioErrorStatus / 8] == 0) {
            busErrorFromWrite_ = false; // acknowledged: drop IP6
        }
        updateBusErrorIrq();
        return;

    case kCpuCtrl0:
        reg = value;
        if ((value & kCpuCtrl0WarmReset) != 0 && warnings_.first(slot)) {
            log::warn("MC: warm reset requested via CPUCTRL0 ({:#010x}); not implemented", value);
        }
        return;

    case kRpssDivider:
        updateRpss(); // account for elapsed time at the old rate first
        reg = value;
        return;

    case kMemCfg0:
    case kMemCfg1:
        reg = value;
        remapRam();
        return;

    // ---- Virtual DMA --------------------------------------------------------
    case kDmaMemAddrDefs:
        regs_[kDmaMemAddr / 8] = value; // same register as kDmaMemAddr
        return;
    case kDmaSize:
        reg = value;
        regs_[kDmaCount / 8] = (regs_[kDmaCount / 8] & ~0xFFFFu) | (value & 0xFFFF); // byte count
        return;
    case kDmaStride:
        reg = value;
        regs_[kDmaCount / 8] = (regs_[kDmaCount / 8] & ~0x03FF'0000u) | (value & 0x03FF'0000); // zoom
        return;
    case kDmaGioAddrStart:
        regs_[kDmaGioAddr / 8] = value;
        startDma(kDmaTicksGioStart);
        return;
    case kDmaStart:
        if ((value & 1) != 0) {
            startDma(kDmaTicksStart);
        }
        return;
    case kDmaDefsStart:
        regs_[kDmaGioAddr / 8] = value;
        regs_[kDmaSize / 8]    = kDmaDefaultSize;
        regs_[kDmaStride / 8]  = kDmaDefaultStride;
        regs_[kDmaCount / 8]   = kDmaDefaultCount;
        regs_[kDmaMode / 8]    = kDmaDefaultMode;
        startDma(kDmaTicksStart);
        return;
    case kDmaIntrCause:
        reg = value; // plain store (writing 0 acknowledges)
        updateDmaIrq();
        return;

    default:
        reg = value;
        return;
    }
}

// ---------------------------------------------------------------------------
// RPSS counter: ticks at (50 MHz / (divider[7:0] + 1)), adding divider[15:8]
// per tick. Computed lazily from elapsed CPU cycles.
// ---------------------------------------------------------------------------

void MemoryController::updateRpss() {
    const Cycles now = clock_();
    const Cycles elapsed = now - rpssLastUpdate_;
    rpssLastUpdate_ = now;

    const uint32_t divider   = regs_[kRpssDivider / 8];
    const uint64_t divide    = (divider & 0xFF) + 1;
    const uint64_t increment = (divider >> 8) & 0xFF;

    // ticks = elapsed * mcClock / (cpuClock * divide), reduced to avoid overflow.
    const uint64_t g   = std::gcd(kClockHz, config_.cpuClockHz);
    const uint64_t num = kClockHz / g;
    const uint64_t den = (config_.cpuClockHz / g) * divide;
    const uint64_t scaled = elapsed * num + rpssRemainder_;
    const uint64_t ticks  = scaled / den;
    rpssRemainder_ = scaled % den;

    uint32_t& counter = regs_[kRpssCounter / 8];
    counter = static_cast<uint32_t>(counter + ticks * increment);
}

// ---------------------------------------------------------------------------
// Virtual DMA engine
//
// Moves data between memory (virtual addresses, translated through the 4-entry
// DMA TLB) and one fixed GIO-bus address, line by line:
//   for each of `lines`:
//     repeat `zoom` times:  move `width` bytes (memory address steps +/-)
//                           and rewind for the next repeat
//     memory address += signed `stride`
// Fill mode writes the 32-bit GIO address value to memory instead.
// ---------------------------------------------------------------------------

std::optional<PhysAddr> MemoryController::dmaTranslate(uint32_t vaddr) {
    // Entries match on the top 10 address bits (4 MiB regions).
    for (unsigned entry = 0; entry < 4; ++entry) {
        const uint32_t hi = regs_[(kDmaTlbFirst + entry * 16) / 8];
        const uint32_t lo = regs_[(kDmaTlbFirst + entry * 16 + 8) / 8];
        if (((vaddr ^ hi) & kDmaTlbRegionMask) != 0) {
            continue;
        }
        // Two-level: the entry points at a page table of 1024 32-bit PTEs, one
        // per 4 KiB page of the 4 MiB region.
        const PhysAddr pteAddr = (PhysAddr{lo & kDmaPtBaseMask} << 6) + ((vaddr >> 12) & 0x3FF) * 4;
        const auto pte = bus_.read32(pteAddr);
        if (!pte) {
            return std::nullopt;
        }
        return (PhysAddr{*pte & kDmaPteFrameMask} << 6) | (vaddr & 0xFFF);
    }
    // No entry matched. ASSUMPTION (undocumented in available references): the
    // address is physical. The PROM's "VDMA Clear" programs no DMA TLB entry
    // and passes physical RAM addresses, so real hardware must behave this way.
    if (warnings_.first(0xD3A0'0000u)) {
        log::tagged("MC DMA", "no DMA TLB entry covers {:#010x}; treating untranslated DMA addresses as physical",
                    vaddr);
    }
    return PhysAddr{vaddr};
}

bool MemoryController::dmaWriteByte(uint32_t vaddr, uint8_t value) {
    const auto phys = dmaTranslate(vaddr);
    return phys && bus_.write<uint8_t>(*phys, value);
}

bool MemoryController::dmaReadByte(uint32_t vaddr, uint8_t& value) {
    const auto phys = dmaTranslate(vaddr);
    return phys && bus_.read<uint8_t>(*phys, value);
}

void MemoryController::updateDmaStatus() {
    if (!dmaActive_ || clock_() < dmaDoneAt_) {
        return;
    }
    dmaActive_ = false;
    regs_[kDmaRun / 8] = kDmaRunDone;
    if ((regs_[kDmaCtrl / 8] & kDmaCtrlIntEnable) != 0) {
        regs_[kDmaIntrCause / 8] |= kDmaIntrDone;
        updateDmaIrq();
    }
}

void MemoryController::updateDmaIrq() {
    const bool level = (regs_[kDmaIntrCause / 8] & kDmaIntrDone) != 0;
    if (level != dmaIrqLevel_) {
        dmaIrqLevel_ = level;
        if (dmaIrq_) dmaIrq_(level);
    }
}

void MemoryController::updateBusErrorIrq() {
    const bool level = busErrorFromWrite_;
    if (level != busErrorLevel_) {
        busErrorLevel_ = level;
        if (busErrorIrq_) busErrorIrq_(level);
    }
}

void MemoryController::reportCpuBusError(PhysAddr addr, bool isWrite) {
    // The GIO bus (graphics and expansion slots) times out; anything else is
    // a CPU-side bad address.
    const bool gio = addr >= 0x1F00'0000 && addr < 0x1FA0'0000;
    if (gio) {
        regs_[kGioErrorAddr / 8] = static_cast<uint32_t>(addr);
        regs_[kGioErrorStatus / 8] |= kGioErrStatusTime;
    } else {
        regs_[kCpuErrorAddr / 8] = static_cast<uint32_t>(addr);
        regs_[kCpuErrorStatus / 8] |= kCpuErrStatusAddr;
    }
    if (isWrite) {
        busErrorFromWrite_ = true;
        updateBusErrorIrq();
    }
}

void MemoryController::startDma(unsigned startTicks) {
    const uint32_t mode   = regs_[kDmaMode / 8];
    const uint32_t size   = regs_[kDmaSize / 8];
    const uint32_t stride = regs_[kDmaStride / 8];
    const uint32_t gio    = regs_[kDmaGioAddr / 8];
    const uint32_t start  = regs_[kDmaMemAddr / 8];

    const uint32_t lines = size >> 16;
    const uint32_t width = size & 0xFFFF;
    const uint32_t zoom  = std::max<uint32_t>(1, (regs_[kDmaCount / 8] >> 16) & 0x3FF);
    const auto lineStep  = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(stride & 0xFFFF)));
    const bool toHost    = (mode & kDmaModeToHost) != 0;
    const bool fill      = toHost && (mode & kDmaModeFill) != 0;
    const bool increment = (mode & kDmaModeIncr) != 0;

    regs_[kDmaRun / 8] = kDmaRunActive;

    uint32_t addr = start;
    uint64_t moved = 0;
    bool ok = true;
    auto advance = [&](uint32_t n) { addr = increment ? addr + n : addr - n; };

    for (uint32_t line = 0; ok && line < lines; ++line) {
        for (uint32_t pass = 0; ok && pass < zoom; ++pass) {
            uint32_t remaining = width;
            while (ok && remaining > 0) {
                const uint32_t n = std::min<uint32_t>(fill ? 4 : 8, remaining);
                if (fill) {
                    // The 32-bit pattern, big-endian, byte by byte.
                    for (uint32_t i = 0; ok && i < 4; ++i) {
                        ok = dmaWriteByte(addr + i, static_cast<uint8_t>(gio >> (24 - 8 * i)));
                    }
                } else if (toHost) {
                    uint64_t qword = 0;
                    (void)bus_.read<uint64_t>(gio & ~7u, qword); // nothing on GIO yet reads 0
                    for (uint32_t i = 0; ok && i < n; ++i) {
                        ok = dmaWriteByte(addr + i, static_cast<uint8_t>(qword >> (56 - 8 * i)));
                    }
                } else {
                    uint64_t qword = 0;
                    for (uint32_t i = 0; ok && i < n; ++i) {
                        uint8_t b = 0;
                        ok = dmaReadByte(addr + i, b);
                        qword |= uint64_t{b} << (56 - 8 * i);
                    }
                    if (ok) {
                        (void)bus_.write<uint64_t>(gio & ~7u, qword);
                    }
                }
                advance(n);
                remaining -= n;
                moved += n;
            }
            if (pass + 1 < zoom) {
                addr = increment ? addr - width : addr + width; // repeat the same line
            }
        }
        addr += lineStep;
    }

    regs_[kDmaMemAddr / 8] = addr;
    regs_[kDmaCount / 8]   = 0;

    // Report "active" until the start latency has elapsed (rounded up), then
    // complete on schedule so the DMA-done interrupt arrives on time.
    dmaActive_ = true;
    dmaDoneAt_ = clock_() + (startTicks * config_.cpuClockHz + kDmaTimerHz - 1) / kDmaTimerHz;
    if (scheduler_ != nullptr) {
        const uint64_t generation = ++dmaGeneration_;
        scheduler_->scheduleAt(dmaDoneAt_, [this, generation](Cycles) {
            if (generation == dmaGeneration_) {
                updateDmaStatus();
            }
        });
    }

    if (!ok) {
        log::tagged("MC DMA", "aborted after {} bytes: bus error near DMA address {:#010x}", moved, addr);
    } else if (dmaLogged_ < kDmaLogLimit) {
        ++dmaLogged_;
        if (fill) {
            log::tagged("MC DMA", "Filled {} bytes at virtual {:#010x} with {:#010x} ({} lines x {} bytes)",
                        moved, start, gio, lines, width);
        } else {
            log::tagged("MC DMA", "Transferred {} bytes from {:#010x} to {:#010x} ({} lines x {} bytes)",
                        moved, toHost ? gio : start, toHost ? start : gio, lines, width);
        }
        if (dmaLogged_ == kDmaLogLimit) {
            log::tagged("MC DMA", "further transfer logs suppressed");
        }
    }
}

// ---------------------------------------------------------------------------
// RAM decode
// ---------------------------------------------------------------------------

void MemoryController::remapRam() {
    // Segments first (floating), banks on top: later windows win.
    std::vector<MemoryBus::RamWindow> windows = {
        {physmap::kMemSeg0Base, physmap::kMemSeg0Size, 0, 0, /*floating=*/true},
        {physmap::kMemSeg1Base, physmap::kMemSeg1Size, 0, 0, /*floating=*/true},
    };
    for (unsigned bank = 0; bank < kBanks; ++bank) {
        const uint32_t cfgReg = regs_[(bank < 2 ? kMemCfg0 : kMemCfg1) / 8];
        const uint32_t desc   = (bank % 2 == 0) ? (cfgReg >> 16) : (cfgReg & 0xFFFF);
        const uint64_t installed = config_.bankBytes[bank];
        if ((desc & kCfgValid) == 0) {
            continue;
        }
        const PhysAddr base = PhysAddr{desc & kCfgBaseMask} << kCfgUnitShift;
        uint64_t window = uint64_t{((desc >> kCfgSizeShift) & kCfgSizeMask) + 1} << kCfgUnitShift;
        if ((desc & kCfgSubbanks) != 0) {
            window /= 2; // single-rank SIMMs only populate the first half
        }
        if (installed == 0) {
            // Configured but no SIMMs: the MC still decodes the range, so the
            // PROM's memory probe reads floating data instead of bus-erroring.
            windows.push_back({base, window, 0, 0, /*floating=*/true});
            continue;
        }
        // A bank smaller than its window repeats (mirrors) across it; the PROM
        // relies on that to size the SIMMs.
        windows.push_back({base, window, bankOffset_[bank], installed});

        if (base == physmap::kRamBase) {
            windows.push_back({physmap::kLowAliasBase, physmap::kLowAliasSize, bankOffset_[bank],
                               physmap::kLowAliasSize});
        }
        log::debug("MC: bank {} -> {:#010x}..{:#010x} ({} MiB installed)",
                   static_cast<char>('A' + bank), base, base + window - 1, installed / MiB);
    }
    bus_.setRamWindows(windows);
}

} // namespace indy
