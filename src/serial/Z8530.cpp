#include "indy/serial/Z8530.hpp"

namespace indy {

namespace {
// WR0: bits 2:0 register select, bits 5:3 command (001 = point high).
constexpr uint8_t kWr0RegMask      = 0x07;
constexpr unsigned kWr0CmdShift    = 3;
constexpr uint8_t kWr0CmdPointHigh = 1;

// WR0 commands (bits 5:3).
constexpr uint8_t kWr0CmdResetTxIp = 5;
// WR1: interrupt enables.
constexpr uint8_t kWr1TxIntEnable  = 0x02;
constexpr uint8_t kWr1RxModeMask   = 0x18; // 00 off, 01 first char, 10 every char, 11 special only
constexpr uint8_t kWr1RxModeSpecial = 0x18;
// WR9: master interrupt enable.
constexpr uint8_t kWr9Mie          = 0x08;
// RR3 bits: channel B ext/tx/rx, then channel A ext/tx/rx.
constexpr uint8_t kRr3TxB = 0x02, kRr3RxB = 0x04, kRr3TxA = 0x10, kRr3RxA = 0x20;

// WR9 bits 7:6: reset commands.
constexpr uint8_t kWr9ResetMask    = 0xC0;
constexpr uint8_t kWr9ResetB       = 0x40;
constexpr uint8_t kWr9ResetA       = 0x80;
constexpr uint8_t kWr9ResetHw      = 0xC0;
} // namespace

void Z8530::reset() {
    wr9_ = 0;
    resetChannel(Channel::A);
    resetChannel(Channel::B);
}

void Z8530::resetChannel(Channel ch) {
    ChannelState& s = state(ch);
    s.wr.fill(0);
    s.pointer = 0;
    s.rx.clear();
    s.txPending = false;
    updateIrq();
}

uint8_t Z8530::interruptPending() const {
    auto rxPending = [&](const ChannelState& s) {
        const uint8_t mode = s.wr[1] & kWr1RxModeMask;
        return !s.rx.empty() && mode != 0 && mode != kWr1RxModeSpecial; // special conditions: none
    };
    const ChannelState& a = channels_[0];
    const ChannelState& b = channels_[1];
    uint8_t rr3 = 0;
    if (b.txPending) rr3 |= kRr3TxB;
    if (rxPending(b)) rr3 |= kRr3RxB;
    if (a.txPending) rr3 |= kRr3TxA;
    if (rxPending(a)) rr3 |= kRr3RxA;
    return rr3;
}

void Z8530::updateIrq() {
    const bool level = (wr9_ & kWr9Mie) != 0 && interruptPending() != 0;
    if (level != irqLevel_) {
        irqLevel_ = level;
        if (irq_) irq_(level);
    }
}

// ---------------------------------------------------------------------------
// Control port
// ---------------------------------------------------------------------------

uint8_t Z8530::readControl(Channel ch) {
    ChannelState& s = state(ch);
    const unsigned reg = s.pointer;
    s.pointer = 0;
    return readRegister(ch, reg);
}

void Z8530::writeControl(Channel ch, uint8_t value) {
    ChannelState& s = state(ch);
    if (s.pointer == 0) {
        // WR0: select the register for the next access, and run its command
        // ("point high", "reset Tx int pending"; the ext/status, error and IUS
        // commands act on state that is not modelled).
        s.pointer = value & kWr0RegMask;
        const uint8_t command = (value >> kWr0CmdShift) & 0x07;
        if (command == kWr0CmdPointHigh) {
            s.pointer |= 0x08;
        } else if (command == kWr0CmdResetTxIp) {
            s.txPending = false;
            updateIrq();
        }
        s.wr[0] = value;
        return;
    }
    const unsigned reg = s.pointer;
    s.pointer = 0;
    writeRegister(ch, reg, value);
}

uint8_t Z8530::readRegister(Channel ch, unsigned reg) {
    ChannelState& s = state(ch);
    // NMOS Z8530 register images: RR4-7 mirror RR0-3, RR9/13 = WR13,
    // RR11/15 = WR15, RR14 = RR10.
    switch (reg) {
    case 0: case 4: {
        uint8_t rr0 = kRr0TxEmpty | kRr0Dcd | kRr0Cts; // transmitter always ready
        if (!s.rx.empty()) {
            rr0 |= kRr0RxAvailable;
        }
        return rr0;
    }
    case 1: case 5:
        return kRr1AllSent;
    case 2: case 6:
        return state(Channel::A).wr[2]; // interrupt vector (status-modified vectors not modelled)
    case 3: case 7:
        return ch == Channel::A ? interruptPending() : 0; // RR3 exists only on channel A
    case 8:
        return readData(ch);
    case 9: case 13:
        return s.wr[13];
    case 10: case 14:
        return 0;
    case 11: case 15:
        return s.wr[15];
    case 12:
        return s.wr[12];
    default:
        return 0;
    }
}

void Z8530::writeRegister(Channel ch, unsigned reg, uint8_t value) {
    if (reg == 9) {
        // Shared between channels; bits 7:6 are reset commands.
        switch (value & kWr9ResetMask) {
        case kWr9ResetHw: reset(); break;
        case kWr9ResetA:  resetChannel(Channel::A); break;
        case kWr9ResetB:  resetChannel(Channel::B); break;
        default: break;
        }
        wr9_ = value & ~kWr9ResetMask;
        updateIrq();
        return;
    }
    if (reg == 8) {
        writeData(ch, value); // WR8 is the transmit buffer
        return;
    }
    state(ch).wr[reg] = value;
    if (reg == 1) {
        updateIrq(); // interrupt enables changed
    }
}

// ---------------------------------------------------------------------------
// Data port
// ---------------------------------------------------------------------------

uint8_t Z8530::readData(Channel ch) {
    ChannelState& s = state(ch);
    if (s.rx.empty()) {
        return 0;
    }
    const uint8_t value = s.rx.front();
    s.rx.pop_front();
    updateIrq();
    return value;
}

void Z8530::writeData(Channel ch, uint8_t value) {
    // Transmission is instantaneous: the buffer is always empty again by the
    // time the guest polls RR0. (Tx-enable in WR5 is not enforced, so nothing
    // the PROM prints is lost while bring-up details are still being worked out.)
    if (sink_) {
        sink_(ch, value);
    }
    // The buffer is empty again at once: with Tx interrupts on, that's pending
    // until the guest resets it (WR0) or writes the next byte.
    ChannelState& s = state(ch);
    s.txPending = (s.wr[1] & kWr1TxIntEnable) != 0;
    updateIrq();
}

void Z8530::receive(Channel ch, uint8_t value) {
    state(ch).rx.push_back(value);
    updateIrq();
}

} // namespace indy
