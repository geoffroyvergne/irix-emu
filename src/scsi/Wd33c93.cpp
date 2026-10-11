#include "indy/scsi/Wd33c93.hpp"

#include "indy/core/Log.hpp"

#include <algorithm>

namespace indy {

void Wd33c93::reset() {
    ScsiController::reset(); // attached targets
    regs_.fill(0);
    std::fill(regs_.begin() + kQueueTag, regs_.begin() + kAuxStatus, uint8_t{0xFF}); // 0x1A-0x1E
    address_ = 0;
    setInterrupt(false);
    pendingStatus_.clear();
    dataFifo_.clear();
    deferredStatus_.reset();
    conn_.reset();
    tiActive_ = waitingForWrite_ = pauseAfterRead_ = false;
    complete(kStatusReset); // power-on reset reports "reset" with INT
}

// ---------------------------------------------------------------------------
// Bus interface: port = (offset >> 2) & 1, byte in the low lane
// ---------------------------------------------------------------------------

bool Wd33c93::read(uint32_t offset, AccessSize size, uint64_t& value) {
    value = 0;
    if (size == AccessSize::Byte && (offset & 3) != 3) {
        return true;
    }
    value = ((offset >> 2) & 1) == 0 ? readAux() : readDataPort();
    return true;
}

bool Wd33c93::write(uint32_t offset, AccessSize size, uint64_t value) {
    if (size == AccessSize::Byte && (offset & 3) != 3) {
        return true;
    }
    if (((offset >> 2) & 1) == 0) {
        writeAddress(static_cast<uint8_t>(value));
    } else {
        writeDataPort(static_cast<uint8_t>(value));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------

uint8_t Wd33c93::readAux() const {
    uint8_t aux = interrupt_ ? kAuxINT : 0;
    if (!dataFifo_.empty() || (tiActive_ && waitingForWrite_)) {
        aux |= kAuxDBR; // polled byte waiting to be read, or wanted
    }
    if (!dataFifo_.empty() || tiActive_) {
        aux |= kAuxBSY; // command still running
    }
    return aux;
}

void Wd33c93::advanceAddress() {
    if (address_ != kCommand && address_ != kData && address_ != kAuxStatus) {
        address_ = (address_ + 1) & 0x1F;
    }
}

uint8_t Wd33c93::readDataPort() {
    uint8_t value;
    switch (address_) {
    case kAuxStatus:
        value = readAux();
        break;
    case kScsiStatus:
        value = regs_[kScsiStatus];
        popStatus(); // reading the status acknowledges the interrupt
        break;
    case kData:
        value = 0;
        if (!dataFifo_.empty()) {
            value = dataFifo_.front();
            dataFifo_.pop_front();
            if (dataFifo_.empty() && deferredStatus_) {
                complete(*deferredStatus_); // the command finishes once its data is taken
                deferredStatus_.reset();
            }
            if (dataFifo_.empty() && tiActive_) {
                if (pauseAfterRead_) {
                    pauseAfterRead_ = false;
                    finishTransfer(kStatusPausedMsgIn); // ACK stays asserted
                } else {
                    pumpTransfer();
                }
            }
        }
        break;
    default:
        value = regs_[address_];
        break;
    }
    advanceAddress();
    return value;
}

void Wd33c93::writeDataPort(uint8_t value) {
    switch (address_) {
    case kScsiStatus:
    case kAuxStatus:
    case kQueueTag:
    case 0x1B: case 0x1C: case 0x1D: case 0x1E:
        break; // read-only
    case kCommand:
        regs_[kCommand] = value;
        execute(value);
        break;
    case kData:
        if (tiActive_ && waitingForWrite_) {
            waitingForWrite_ = false;
            --tiRemaining_;
            targetReceive(value);
            pumpTransfer();
        }
        break;
    default:
        regs_[address_] = value;
        break;
    }
    advanceAddress();
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void Wd33c93::setInterrupt(bool level) {
    if (level != interrupt_) {
        interrupt_ = level;
        if (irq_) irq_(level);
    }
}

void Wd33c93::complete(uint8_t status) {
    if (interrupt_) {
        pendingStatus_.push_back(status); // the CPU has not read the previous one yet
        return;
    }
    regs_[kScsiStatus] = status;
    setInterrupt(true);
}

void Wd33c93::popStatus() {
    setInterrupt(false);
    if (!pendingStatus_.empty()) {
        regs_[kScsiStatus] = pendingStatus_.front();
        pendingStatus_.pop_front();
        setInterrupt(true);
    }
}

void Wd33c93::execute(uint8_t command) {
    switch (command & 0x7F) { // bit 7: single-byte transfer flag
    case kCmdReset:
        // Clears CONTROL..SOURCE ID and COMMAND; OWN ID (and its EAF bit) is kept.
        std::fill(regs_.begin() + kControl, regs_.begin() + kSourceId + 1, uint8_t{0});
        regs_[kCommand] = 0;
        pendingStatus_.clear();
        dataFifo_.clear();
        deferredStatus_.reset();
        conn_.reset();
        tiActive_ = waitingForWrite_ = pauseAfterRead_ = false;
        setInterrupt(false);
        complete((regs_[kOwnId] & kOwnIdEAF) != 0 ? kStatusResetEaf : kStatusReset);
        return;

    case kCmdNegateAck:
        // After COMMAND COMPLETE the target releases the bus: disconnect.
        if (conn_ && conn_->ackHeld) {
            conn_.reset();
            complete(kStatusDisconnect);
        }
        return;
    case kCmdDisconnect:
        conn_.reset();
        tiActive_ = waitingForWrite_ = pauseAfterRead_ = false;
        return; // completes without an interrupt (datasheet)
    case kCmdAssertAtn:
        return; // completes without an interrupt; targets here take no extra messages

    case kCmdTransferInfo:
        transferInfo((command & kCmdSingleByte) != 0);
        return;

    case kCmdSelectAtnXfer:
    case kCmdSelectXfer:
        selectAndTransfer();
        return;

    case kCmdSelectAtn:
    case kCmdSelect:
        select((command & 0x7F) == kCmdSelectAtn);
        return;

    default:
        if (warnings_.first(command)) {
            log::warn("WD33C93: command {:#04x} not emulated, reporting invalid command", command);
        }
        complete(kStatusInvalidCommand);
        return;
    }
}

void Wd33c93::selectAndTransfer() {
    const uint8_t id = regs_[kDestId] & 7;
    ScsiTarget* const t = target(id);
    regs_[kCommandPhase] = 0;
    if (t == nullptr || id == (regs_[kOwnId] & 7)) {
        complete(kStatusSelectTimeout); // nobody answered selection
        return;
    }

    // CDB length: OWN ID's size field with EAF, else from the group code.
    size_t cdbLength;
    if ((regs_[kOwnId] & kOwnIdEAF) != 0) {
        cdbLength = std::clamp<size_t>(regs_[kOwnId] & 0x0F, 6, 12);
    } else {
        switch (regs_[kCdb1] >> 5) {
        case 1: case 2: cdbLength = 10; break;
        case 5:         cdbLength = 12; break;
        default:        cdbLength = 6;  break;
        }
    }
    const std::span<const uint8_t> cdb(&regs_[kCdb1], cdbLength);
    regs_[kCommandPhase] = static_cast<uint8_t>(kPhaseCdbStart + cdbLength);

    uint32_t count = uint32_t{regs_[kCountMsb]} << 16 | uint32_t{regs_[kCount]} << 8 | regs_[kCountLsb];
    const bool dmaMode = (regs_[kControl] & kControlDmaMask) != 0;
    auto storeCount = [&] {
        regs_[kCountMsb] = static_cast<uint8_t>(count >> 16);
        regs_[kCount]    = static_cast<uint8_t>(count >> 8);
        regs_[kCountLsb] = static_cast<uint8_t>(count);
    };

    // DATA OUT (e.g. MODE SELECT parameters) comes from memory through DMA.
    std::vector<uint8_t> out(std::min<size_t>(t->dataOutLength(cdb), count));
    if (!out.empty()) {
        const size_t got = dmaMode && dma_ ? dma_->fromMemory(out) : 0;
        out.resize(got);
        count -= static_cast<uint32_t>(got);
    }

    ScsiCommandResult result = t->execute(cdb, out);

    // DATA IN.
    if (!result.dataIn.empty()) {
        const size_t wanted = result.dataIn.size();
        const size_t n = std::min<size_t>(wanted, count);
        const std::span<const uint8_t> chunk(result.dataIn.data(), n);
        if (dmaMode) {
            const size_t moved = dma_ ? dma_->toMemory(chunk) : 0;
            count -= static_cast<uint32_t>(moved);
        } else {
            dataFifo_.insert(dataFifo_.end(), chunk.begin(), chunk.end());
            count -= static_cast<uint32_t>(n);
        }
        if (wanted > n) {
            // The target still wants to send but the transfer count ran out.
            storeCount();
            regs_[kCommandPhase] = kPhaseUnexpected;
            complete(static_cast<uint8_t>(kStatusUnexpectedPhase | 0x01)); // phase: data in
            return;
        }
    }
    storeCount();

    // STATUS and COMMAND COMPLETE: the status byte lands in TARGET LUN.
    regs_[kTargetLun]    = result.status;
    regs_[kCommandPhase] = kPhaseComplete;
    if (!dataFifo_.empty()) {
        deferredStatus_ = kStatusSatComplete; // polled: complete when the CPU has the data
    } else {
        complete(kStatusSatComplete);
    }
}

// ---------------------------------------------------------------------------
// Phase-driven path: Select, then Transfer Info per bus phase
// ---------------------------------------------------------------------------

void Wd33c93::storeTransferCount(uint32_t count) {
    regs_[kCountMsb] = static_cast<uint8_t>(count >> 16);
    regs_[kCount]    = static_cast<uint8_t>(count >> 8);
    regs_[kCountLsb] = static_cast<uint8_t>(count);
}

void Wd33c93::select(bool withAtn) {
    const uint8_t id = regs_[kDestId] & 7;
    ScsiTarget* const t = target(id);
    if (t == nullptr || id == (regs_[kOwnId] & 7)) {
        complete(kStatusSelectTimeout);
        return;
    }
    conn_ = Connection{};
    conn_->target = t;
    conn_->phase = withAtn ? BusPhase::MsgOut : BusPhase::Command; // ATN: target wants IDENTIFY
    regs_[kCommandPhase] = 0x10;
    complete(kStatusSelectSuccess);
    complete(static_cast<uint8_t>(kStatusServiceRequired | static_cast<uint8_t>(conn_->phase)));
}

void Wd33c93::transferInfo(bool singleByte) {
    if (!conn_) {
        complete(kStatusInvalidCommand); // not connected to a target
        return;
    }
    uint32_t count = uint32_t{regs_[kCountMsb]} << 16 | uint32_t{regs_[kCount]} << 8 | regs_[kCountLsb];
    if (singleByte || count == 0) {
        count = 1;
    }
    tiActive_ = true;
    tiPhase_ = conn_->phase;
    tiRemaining_ = count;
    waitingForWrite_ = false;
    pumpTransfer();
}

void Wd33c93::finishTransfer(uint8_t status) {
    tiActive_ = false;
    waitingForWrite_ = false;
    storeTransferCount(tiRemaining_);
    complete(status);
}

void Wd33c93::pumpTransfer() {
    const bool dmaMode = (regs_[kControl] & kControlDmaMask) != 0;
    while (tiActive_) {
        const BusPhase phase = conn_->phase;
        if (phase != tiPhase_ || tiRemaining_ == 0) {
            // The target changed phase (or the count ran out): report the
            // phase it now requests.
            finishTransfer(static_cast<uint8_t>(kStatusTransferDone | static_cast<uint8_t>(phase)));
            return;
        }
        if (phase == BusPhase::DataIn && dmaMode) {
            const auto& data = conn_->result.dataIn;
            const size_t n = std::min<size_t>(tiRemaining_, data.size() - conn_->dataInPos);
            const size_t moved = dma_ ? dma_->toMemory({data.data() + conn_->dataInPos, n}) : 0;
            conn_->dataInPos += moved;
            tiRemaining_ -= static_cast<uint32_t>(moved);
            if (conn_->dataInPos == data.size()) conn_->phase = BusPhase::Status;
            if (moved == 0) return; // DMA channel not running: wait
            continue;
        }
        if (phase == BusPhase::DataOut && dmaMode) {
            std::vector<uint8_t> chunk(std::min<size_t>(tiRemaining_, conn_->dataOutWanted - conn_->dataOut.size()));
            const size_t moved = dma_ ? dma_->fromMemory(chunk) : 0;
            tiRemaining_ -= static_cast<uint32_t>(moved);
            for (size_t i = 0; i < moved; ++i) targetReceive(chunk[i]);
            if (moved == 0) return;
            continue;
        }
        if (isInputPhase(phase)) {
            if (!dataFifo_.empty()) return; // the CPU hasn't taken the last byte yet
            dataFifo_.push_back(targetSend());
            --tiRemaining_;
            if (phase == BusPhase::MsgIn) pauseAfterRead_ = true;
            return; // DBR: wait for the CPU to read DATA
        }
        waitingForWrite_ = true; // output phase: DBR asks the CPU for the next byte
        return;
    }
}

void Wd33c93::targetReceive(uint8_t byte) {
    Connection& c = *conn_;
    switch (c.phase) {
    case BusPhase::MsgOut:
        // IDENTIFY (and any further messages) are accepted; the target moves
        // on to COMMAND once the initiator stops sending (ATN dropped).
        if (tiRemaining_ == 0) c.phase = BusPhase::Command;
        break;
    case BusPhase::Command: {
        c.cdb.push_back(byte);
        size_t length = 6;
        switch (c.cdb[0] >> 5) {
        case 1: case 2: length = 10; break;
        case 5:         length = 12; break;
        default:        break;
        }
        if (c.cdb.size() == length) {
            c.dataOutWanted = c.target->dataOutLength(c.cdb);
            if (c.dataOutWanted != 0) {
                c.phase = BusPhase::DataOut;
            } else {
                runCommand();
            }
        }
        break;
    }
    case BusPhase::DataOut:
        c.dataOut.push_back(byte);
        if (c.dataOut.size() == c.dataOutWanted) runCommand();
        break;
    default:
        break;
    }
}

void Wd33c93::runCommand() {
    Connection& c = *conn_;
    c.result = c.target->execute(c.cdb, c.dataOut);
    c.dataInPos = 0;
    c.phase = c.result.dataIn.empty() ? BusPhase::Status : BusPhase::DataIn;
}

uint8_t Wd33c93::targetSend() {
    Connection& c = *conn_;
    switch (c.phase) {
    case BusPhase::DataIn: {
        const uint8_t b = c.result.dataIn[c.dataInPos++];
        if (c.dataInPos == c.result.dataIn.size()) c.phase = BusPhase::Status;
        return b;
    }
    case BusPhase::Status:
        regs_[kTargetLun] = c.result.status;
        c.phase = BusPhase::MsgIn;
        return c.result.status;
    case BusPhase::MsgIn:
        c.ackHeld = true;
        return 0x00; // COMMAND COMPLETE
    default:
        return 0;
    }
}

} // namespace indy
