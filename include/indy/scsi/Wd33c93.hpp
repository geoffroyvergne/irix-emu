#pragma once

#include "indy/core/MmioDevice.hpp"
#include "indy/core/WarnOnce.hpp"
#include "indy/scsi/ScsiController.hpp"
#include "indy/scsi/ScsiDmaPort.hpp"

#include <array>
#include <deque>
#include <functional>
#include <optional>
#include <span>

namespace indy {

// Western Digital WD33C93 SCSI bus interface controller, behind HPC3 at
// 0x1FBC0000 (SCSI channel 0). Two byte-wide ports in the low lane of 4-byte
// slots: +0x3 address register (write) / auxiliary status (read), +0x7 data
// for the register selected by the address register.
//
// Register model per the WD33C93 datasheet, as cross-checked with MAME
// src/devices/machine/wd33c9x.cpp: 32 registers, auto-incrementing address
// (except COMMAND, DATA, AUX STATUS), status + INT on command completion,
// reading SCSI STATUS clears INT.
//
// Two ways to drive the bus are supported:
//  * Select-and-Transfer (0x08/0x09) runs the whole conversation synchronously
//    and reports 0x16 with the target status in TARGET LUN.
//  * Select (0x06 with ATN / 0x07) then Transfer Info (0x20) steps through the
//    bus phases one at a time: MSG OUT, COMMAND, DATA IN/OUT, STATUS, MSG IN.
//    Each Transfer Info ends with 0x18 | new phase; after the message byte the
//    chip pauses with ACK held (0x27) and Negate ACK lets the target go bus
//    free (0x85).
// Data phases use the DMA port in DMA mode; everything else (and polled mode)
// moves through the DATA register, paced by DBR. Targets complete commands
// immediately, so they never disconnect mid-command (no reselection needed).
class Wd33c93 final : public ScsiController, public MmioDevice {
public:
    enum Reg : uint8_t {
        kOwnId        = 0x00, // [2:0] own SCSI ID, [3] EAF, [7:6] clock divisor
        kControl      = 0x01,
        kTimeout      = 0x02,
        kCdb1         = 0x03, // ... 0x0E
        kTargetLun    = 0x0F,
        kCommandPhase = 0x10,
        kSyncTransfer = 0x11,
        kCountMsb     = 0x12,
        kCount        = 0x13,
        kCountLsb     = 0x14,
        kDestId       = 0x15,
        kSourceId     = 0x16,
        kScsiStatus   = 0x17, // read-only; reading clears INT
        kCommand      = 0x18,
        kData         = 0x19,
        kQueueTag     = 0x1A,
        kAuxStatus    = 0x1F, // read-only; also returned by reading the address port
    };

    static constexpr uint8_t kAuxDBR = 0x01;
    static constexpr uint8_t kAuxBSY = 0x20;
    static constexpr uint8_t kAuxINT = 0x80;
    static constexpr uint8_t kOwnIdEAF = 0x08;

    enum Status : uint8_t {
        kStatusReset          = 0x00,
        kStatusResetEaf       = 0x01,
        kStatusSelectSuccess  = 0x11,
        kStatusSatComplete    = 0x16, // select-and-transfer completed
        kStatusInvalidCommand = 0x40,
        kStatusSelectTimeout  = 0x42,
        kStatusUnexpectedPhase = 0x48, // | bus phase (data in = 1)
        kStatusTransferDone   = 0x18, // | new bus phase (Transfer Info finished)
        kStatusPausedMsgIn    = 0x27, // message byte received, ACK still asserted
        kStatusServiceRequired = 0x88, // | bus phase the target is requesting
        kStatusDisconnect     = 0x85,
    };

    // SCSI bus phases (MSG, C/D, I/O encoding).
    enum class BusPhase : uint8_t { DataOut = 0, DataIn = 1, Command = 2, Status = 3, MsgOut = 6, MsgIn = 7 };

    // COMMAND PHASE register values for select-and-transfer.
    static constexpr uint8_t kPhaseCdbStart     = 0x30; // + CDB bytes sent
    static constexpr uint8_t kPhaseUnexpected   = 0x46;
    static constexpr uint8_t kPhaseComplete     = 0x60;
    static constexpr uint8_t kControlDmaMask    = 0xE0; // 0 = polled

    enum Command : uint8_t {
        kCmdReset           = 0x00,
        kCmdAbort           = 0x01,
        kCmdAssertAtn       = 0x02,
        kCmdNegateAck       = 0x03,
        kCmdDisconnect      = 0x04,
        kCmdSelectAtn       = 0x06,
        kCmdSelect          = 0x07,
        kCmdSelectAtnXfer   = 0x08,
        kCmdSelectXfer      = 0x09,
        kCmdTransferInfo    = 0x20,
    };
    static constexpr uint8_t kCmdSingleByte = 0x80; // SBT: transfer exactly one byte

    Wd33c93() { reset(); }

    void attachDma(ScsiDmaPort& port) { dma_ = &port; }
    // INTRQ output (level): follows the INT bit of the auxiliary status.
    void setIrqCallback(std::function<void(bool)> irq) { irq_ = std::move(irq); if (irq_) irq_(interrupt_); }

    [[nodiscard]] std::string_view name() const noexcept override { return "WD33C93"; }
    void reset() override; // hardware reset (overrides both bases)
    [[nodiscard]] bool read(uint32_t offset, AccessSize size, uint64_t& value) override;
    [[nodiscard]] bool write(uint32_t offset, AccessSize size, uint64_t value) override;

    // Byte-level port access (tests, future DMA glue).
    [[nodiscard]] uint8_t readAux() const;
    void writeAddress(uint8_t value) { address_ = value & 0x1F; }
    [[nodiscard]] uint8_t readDataPort();
    void writeDataPort(uint8_t value);

private:
    void advanceAddress();
    void execute(uint8_t command);
    void selectAndTransfer();

    // ---- Phase-driven path (select + Transfer Info) -----------------------
    struct Connection {
        ScsiTarget* target = nullptr;
        BusPhase phase = BusPhase::Command;
        std::vector<uint8_t> cdb;
        std::vector<uint8_t> dataOut;
        size_t dataOutWanted = 0;
        ScsiCommandResult result;
        size_t dataInPos = 0;
        bool ackHeld = false; // COMMAND COMPLETE received, waiting for Negate ACK
    };
    void select(bool withAtn);
    void transferInfo(bool singleByte);
    void pumpTransfer();
    void finishTransfer(uint8_t status);
    void targetReceive(uint8_t byte);       // initiator -> target (output phases)
    [[nodiscard]] uint8_t targetSend();     // target -> initiator (input phases)
    void runCommand();
    void storeTransferCount(uint32_t count);
    [[nodiscard]] static bool isInputPhase(BusPhase p) {
        return p == BusPhase::DataIn || p == BusPhase::Status || p == BusPhase::MsgIn;
    }

    std::optional<Connection> conn_;
    bool tiActive_ = false;         // a Transfer Info is in progress
    BusPhase tiPhase_ = BusPhase::Command;
    uint32_t tiRemaining_ = 0;
    bool waitingForWrite_ = false;  // polled output: DBR asks the CPU for a byte
    bool pauseAfterRead_ = false;   // message byte in FIFO: pause once the CPU takes it
    void complete(uint8_t status); // queue a completion status and raise INT
    void setInterrupt(bool level);
    void popStatus();

    std::array<uint8_t, 32> regs_{};
    uint8_t address_ = 0;
    bool interrupt_ = false;
    std::deque<uint8_t> pendingStatus_; // statuses behind the current one
    std::deque<uint8_t> dataFifo_;      // polled-mode DATA IN bytes awaiting the CPU
    std::optional<uint8_t> deferredStatus_; // raised once the polled FIFO drains
    ScsiDmaPort* dma_ = nullptr;
    std::function<void(bool)> irq_;
    WarnOnce warnings_;
};

} // namespace indy
