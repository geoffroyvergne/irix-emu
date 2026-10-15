#include "indy/hpc/Hpc3.hpp"

#include "indy/core/Log.hpp"

#include <algorithm>

namespace indy {

namespace {
constexpr uint32_t kEepromDataIn = 0x10; // serial EEPROM DATI: no EEPROM attached yet
} // namespace

Hpc3::Hpc3(MemoryBus& bus)
    : dmaRegs_((kDmaRegsEnd - kDmaRegsBase) / 4, 0),
      pbusCfg_((kPbusCfgEnd - kPbusCfgBase) / 4, 0) {
    for (ScsiDma& channel : scsiDma_) {
        channel.bus = &bus;
    }
}

// ---------------------------------------------------------------------------
// SCSI DMA channels
// ---------------------------------------------------------------------------

void Hpc3::setScsiIrqCallback(std::function<void(unsigned, bool)> cb) {
    for (unsigned i = 0; i < scsiDma_.size(); ++i) {
        scsiDma_[i].notify = [cb, i](bool level) { cb(i, level); };
    }
}

void Hpc3::ScsiDma::reset() {
    cbp = nbdp = count = ctrl = 0;
    eox = xie = active = false;
    setIrq(false);
}

void Hpc3::ScsiDma::loadDescriptor() {
    const auto word = [&](uint32_t at) { return bus->read32(at).value_or(0); };
    cbp   = word(nbdp);
    const uint32_t info = word(nbdp + 4);
    const uint32_t next = word(nbdp + 8);
    count = info & kDescCount;
    eox   = (info & kDescEox) != 0;
    xie   = (info & kDescXie) != 0;
    nbdp  = next;
}

void Hpc3::ScsiDma::endOfBuffer() {
    if (xie) {
        setIrq(true);
    }
    if (eox) {
        active = false;
    } else {
        loadDescriptor();
    }
}

bool Hpc3::ScsiDma::ready() {
    // Skip empty descriptors, but never loop forever on a broken chain.
    for (int guard = 0; active && count == 0 && guard < 4096; ++guard) {
        endOfBuffer();
    }
    if (active && count == 0) {
        log::warn("HPC3: SCSI DMA descriptor chain has no data; stopping channel");
        active = false;
    }
    return active;
}

size_t Hpc3::ScsiDma::toMemory(std::span<const uint8_t> bytes) {
    size_t done = 0;
    while (done < bytes.size() && ready()) {
        (void)bus->write<uint8_t>(address(), bytes[done]);
        ++cbp;
        ++done;
        if (--count == 0) {
            endOfBuffer();
        }
    }
    return done;
}

size_t Hpc3::ScsiDma::fromMemory(std::span<uint8_t> bytes) {
    size_t done = 0;
    while (done < bytes.size() && ready()) {
        uint8_t b = 0;
        (void)bus->read<uint8_t>(address(), b);
        bytes[done] = b;
        ++cbp;
        ++done;
        if (--count == 0) {
            endOfBuffer();
        }
    }
    return done;
}

bool Hpc3::scsiDmaRead(uint32_t offset, uint64_t& value) {
    ScsiDma& ch = scsiDma_[(offset - kScsiDmaBase) / kScsiDmaStride];
    switch ((offset - kScsiDmaBase) % kScsiDmaStride) {
    case kScsiCbp:  value = ch.cbp; return true;
    case kScsiNbdp: value = ch.nbdp; return true;
    case kScsiBc:   value = ch.count | (ch.xie ? 0x4000u : 0u) | (ch.eox ? 0x8000u : 0u); return true;
    case kScsiCtrl:
        value = ch.ctrl | (ch.active ? kSctrlActive : 0u) | (ch.irq ? kSctrlIrq : 0u);
        ch.setIrq(false); // reading acknowledges the interrupt
        return true;
    default:
        return false; // not a channel register: plain storage
    }
}

bool Hpc3::scsiDmaWrite(uint32_t offset, uint64_t value) {
    const unsigned channel = (offset - kScsiDmaBase) / kScsiDmaStride;
    ScsiDma& ch = scsiDma_[channel];
    const auto v = static_cast<uint32_t>(value);
    switch ((offset - kScsiDmaBase) % kScsiDmaStride) {
    case kScsiCbp:  return true; // read-only
    case kScsiNbdp: ch.nbdp = v; return true;
    case kScsiBc:
        ch.count = v & kDescCount;
        ch.xie = (v & 0x4000) != 0;
        ch.eox = (v & 0x8000) != 0;
        return true;
    case kScsiCtrl:
        if ((v & kSctrlReset) != 0) {
            ch.reset();
            if (scsi_[channel] != nullptr) {
                scsi_[channel]->reset(); // CRESET also resets the external controller
            }
            ch.ctrl = v & ~(kSctrlActive | kSctrlIrq | kSctrlFlush);
            return true;
        }
        if ((v & kSctrlFlush) != 0) {
            ch.active = false; // nothing is buffered: the flush completes at once
        } else if ((v & kSctrlActive) != 0) {
            if (!ch.active) {
                ch.active = true;
                ch.loadDescriptor(); // the chain starts at NBDP
            }
        } else {
            ch.active = false;
        }
        ch.ctrl = v & ~(kSctrlActive | kSctrlIrq | kSctrlFlush);
        return true;
    default:
        return false;
    }
}

void Hpc3::attachPbusDevice(unsigned channel, MmioDevice& device) {
    pbus_.at(channel) = &device;
}

void Hpc3::reset() {
    std::ranges::fill(dmaRegs_, 0u);
    std::ranges::fill(pbusCfg_, 0u);
    misc_   = 0;
    eeprom_ = 0;
    for (ScsiDma& channel : scsiDma_) {
        channel.reset();
    }
    for (MmioDevice* device : scsi_) {
        if (device != nullptr) {
            device->reset();
        }
    }
    for (MmioDevice* device : pbus_) {
        if (device != nullptr) {
            device->reset();
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

uint64_t Hpc3::extract(uint32_t word, uint32_t offset, AccessSize size) {
    switch (size) {
    case AccessSize::Byte:  return (word >> ((3 - (offset & 3)) * 8)) & 0xFF;
    case AccessSize::Half:  return (word >> ((2 - (offset & 2)) * 8)) & 0xFFFF;
    case AccessSize::Word:
    case AccessSize::Dword: return word;
    }
    return 0;
}

void Hpc3::merge(uint32_t& word, uint32_t offset, AccessSize size, uint64_t value) {
    switch (size) {
    case AccessSize::Byte: {
        const unsigned shift = (3 - (offset & 3)) * 8;
        word = (word & ~(0xFFu << shift)) | (static_cast<uint32_t>(value & 0xFF) << shift);
        return;
    }
    case AccessSize::Half: {
        const unsigned shift = (2 - (offset & 2)) * 8;
        word = (word & ~(0xFFFFu << shift)) | (static_cast<uint32_t>(value & 0xFFFF) << shift);
        return;
    }
    case AccessSize::Word:
    case AccessSize::Dword:
        word = static_cast<uint32_t>(value);
        return;
    }
}

void Hpc3::unimplemented(uint32_t offset, bool isWrite) {
    // One warning per 4 KiB block keeps the log readable during bring-up.
    if (log::verbose() && warnings_.first(offset >> 12)) {
        log::warn("HPC3: {} at +{:#07x} (physical {:#010x}) not emulated yet{}",
                  isWrite ? "write" : "read", offset, 0x1FB8'0000u + offset,
                  isWrite ? "" : ", returning 0");
    }
}

// ---------------------------------------------------------------------------
// Decode
// ---------------------------------------------------------------------------

bool Hpc3::read(uint32_t offset, AccessSize size, uint64_t& value) {
    value = 0;
    if (offset >= kScsiDmaBase && offset < kScsiDmaBase + 2 * kScsiDmaStride &&
        size == AccessSize::Word && scsiDmaRead(offset, value)) {
        return true;
    }
    if (offset < kDmaRegsEnd) {
        value = extract(dmaRegs_[offset / 4], offset, size);
        return true;
    }
    if (offset >= kPbusPioBase && offset < kPbusPioEnd) {
        const uint32_t rel = offset - kPbusPioBase;
        if (MmioDevice* device = pbus_[rel / kPbusPioStride]) {
            return device->read(rel % kPbusPioStride, size, value);
        }
        unimplemented(offset, false);
        return true;
    }
    if (offset >= kPbusCfgBase && offset < kPbusCfgEnd) {
        value = extract(pbusCfg_[(offset - kPbusCfgBase) / 4], offset, size);
        return true;
    }
    if (offset >= kBbramBase && offset < kBbramEnd && bbram_ != nullptr) {
        return bbram_->read(offset - kBbramBase, size, value);
    }
    if (offset >= kScsiBase && offset < kScsiEnd) {
        if (MmioDevice* device = scsi_[(offset - kScsiBase) / kScsiStride]) {
            return device->read((offset - kScsiBase) % kScsiStride, size, value);
        }
    }
    switch (offset & ~3u) {
    case kIntStat:
    case kIntStatMirror:
        // Bits 8/9: SCSI DMA channel 0/1 interrupt pending.
        value = (scsiDma_[0].irq ? 0x100u : 0u) | (scsiDma_[1].irq ? 0x200u : 0u);
        return true;
    case kMisc:
        value = extract(misc_, offset, size);
        return true;
    case kEeprom:
        value = extract(eeprom_ & ~kEepromDataIn, offset, size);
        return true;
    default:
        unimplemented(offset, false);
        return true;
    }
}

bool Hpc3::write(uint32_t offset, AccessSize size, uint64_t value) {
    if (offset >= kScsiDmaBase && offset < kScsiDmaBase + 2 * kScsiDmaStride &&
        size == AccessSize::Word && scsiDmaWrite(offset, value)) {
        return true;
    }
    if (offset < kDmaRegsEnd) {
        merge(dmaRegs_[offset / 4], offset, size, value);
        return true;
    }
    if (offset >= kPbusPioBase && offset < kPbusPioEnd) {
        const uint32_t rel = offset - kPbusPioBase;
        if (MmioDevice* device = pbus_[rel / kPbusPioStride]) {
            return device->write(rel % kPbusPioStride, size, value);
        }
        unimplemented(offset, true);
        return true;
    }
    if (offset >= kPbusCfgBase && offset < kPbusCfgEnd) {
        merge(pbusCfg_[(offset - kPbusCfgBase) / 4], offset, size, value);
        return true;
    }
    if (offset >= kBbramBase && offset < kBbramEnd && bbram_ != nullptr) {
        return bbram_->write(offset - kBbramBase, size, value);
    }
    if (offset >= kScsiBase && offset < kScsiEnd) {
        if (MmioDevice* device = scsi_[(offset - kScsiBase) / kScsiStride]) {
            return device->write((offset - kScsiBase) % kScsiStride, size, value);
        }
    }
    switch (offset & ~3u) {
    case kIntStat:
    case kIntStatMirror:
        return true; // read-only
    case kMisc:
        merge(misc_, offset, size, value);
        return true;
    case kEeprom:
        merge(eeprom_, offset, size, value);
        return true;
    default:
        unimplemented(offset, true);
        return true;
    }
}

} // namespace indy
