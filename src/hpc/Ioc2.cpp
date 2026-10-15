#include "indy/hpc/Ioc2.hpp"

#include "indy/core/Log.hpp"

namespace indy {

void Ioc2::connect(Int3::CpuLine cpu, Scheduler& scheduler, Pit8254::Clock clock, uint64_t cpuClockHz) {
    int3_.connect(std::move(cpu));
    pit_.connect(scheduler, std::move(clock), cpuClockHz, [this](unsigned counter) { int3_.timerFired(counter); });
    // The 8042 and the Z85C30 are "mappable" INT3 sources (Linux SGI_KEYBD_IRQ /
    // SGI_SERIAL_IRQ = LOCAL2 + 4 / + 5).
    kbc_.setIrqCallback([this](bool level) { int3_.setMappable(Int3::kMapKeyboardMouse, level); });
    scc_.setIrqCallback([this](bool level) { int3_.setMappable(Int3::kMapSerial, level); });
}

void Ioc2::reset() {
    regs_.fill(0);
    regs_[kPanel / 4] = kPanelResetValue;
    regs_[kSysId / 4] = kGuinnessSysId;
    scc_.reset();
    kbc_.reset();
    int3_.reset();
    pit_.reset();
}

// ---------------------------------------------------------------------------
// Bus interface: 8-bit registers in the low byte lane of 32-bit slots
// ---------------------------------------------------------------------------

bool Ioc2::read(uint32_t offset, AccessSize size, uint64_t& value) {
    const uint32_t slot = offset & ~3u;
    // A byte access must hit the register's lane (slot + 3); other lanes float.
    if (size == AccessSize::Byte && (offset & 3) != 3) {
        value = 0;
        return true;
    }
    value = slot < kSlots * 4 ? readRegister(slot) : 0;
    if (slot >= kSlots * 4) {
        unimplemented(slot, false, 0);
    }
    return true;
}

bool Ioc2::write(uint32_t offset, AccessSize size, uint64_t value) {
    const uint32_t slot = offset & ~3u;
    if (size == AccessSize::Byte && (offset & 3) != 3) {
        return true;
    }
    if (slot < kSlots * 4) {
        writeRegister(slot, static_cast<uint8_t>(value));
    } else {
        unimplemented(slot, true, static_cast<uint8_t>(value));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Register semantics
// ---------------------------------------------------------------------------

uint8_t Ioc2::readRegister(uint32_t slot) {
    switch (slot) {
    case kSerialBCtrl: return scc_.readControl(Z8530::Channel::B);
    case kSerialBData: return scc_.readData(Z8530::Channel::B);
    case kSerialACtrl: return scc_.readControl(Z8530::Channel::A);
    case kSerialAData: return scc_.readData(Z8530::Channel::A);

    case kKbdData:    return kbc_.readData();
    case kKbdCommand: return kbc_.readStatus();

    case kPitCounter0:
    case kPitCounter1:
    case kPitCounter2:
    case kPitControl:
        return pit_.read((slot - kPitCounter0) / 4);

    default:
        if (slot <= kParallelLast) {
            return 0; // parallel port not emulated: idle
        }
        if (slot >= kLocal0Status && slot <= kErrorStatus) {
            return int3_.read(slot);
        }
        return regs_[slot / 4];
    }
}

void Ioc2::writeRegister(uint32_t slot, uint8_t value) {
    switch (slot) {
    case kSerialBCtrl: scc_.writeControl(Z8530::Channel::B, value); return;
    case kSerialBData: scc_.writeData(Z8530::Channel::B, value); return;
    case kSerialACtrl: scc_.writeControl(Z8530::Channel::A, value); return;
    case kSerialAData: scc_.writeData(Z8530::Channel::A, value); return;

    // Read-only registers.
    case kSysId:
    case kRead:
        return;

    case kPanel:
        // Writing 1s acknowledges the corresponding button "interrupt" bits.
        regs_[slot / 4] &= static_cast<uint8_t>(~(value & kPanelIntMask));
        return;


    case kKbdData:    kbc_.writeData(value); return;
    case kKbdCommand: kbc_.writeCommand(value); return;

    case kPitCounter0:
    case kPitCounter1:
    case kPitCounter2:
    case kPitControl:
        pit_.write((slot - kPitCounter0) / 4, value);
        return;

    default:
        if (slot <= kParallelLast) {
            return; // parallel port not emulated
        }
        if (slot >= kLocal0Status && slot <= kErrorStatus) {
            int3_.write(slot, value);
            return;
        }
        regs_[slot / 4] = value; // plain storage: control, DMA select, LEDs, ...
        return;
    }
}

void Ioc2::unimplemented(uint32_t slot, bool isWrite, uint8_t value) {
    if (log::verbose() && warnings_.first(slot)) {
        if (isWrite) {
            log::warn("IOC2: write {:#04x} to unimplemented register +{:#04x}", value, slot + 3);
        } else {
            log::warn("IOC2: read from unimplemented register +{:#04x}, returning 0", slot + 3);
        }
    }
}

} // namespace indy
