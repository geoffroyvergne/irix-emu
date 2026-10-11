#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>

namespace indy {

// Zilog Z85C30 SCC (serial communications controller), as attached to the
// Indy's IOC2: two channels, each with a control port and a data port.
//
// Programming model (Zilog Z8530 datasheet): every control-port access goes to
// the register selected by the pointer, which is 0 by default. Writing WR0
// sets the pointer (bits 2:0, plus 8 with the "point high" command); the next
// control-port access uses it and the pointer returns to 0.
//
// Implemented: register file, pointer protocol, transmit to a host sink,
// receive queue, resets, and the interrupt logic for receive and transmit:
// WR1 enables, WR9 MIE, RR3 pending bits, WR0 "reset Tx int pending".
// Not yet: external/status interrupts, interrupt-under-service ordering,
// status-modified vectors (WR9 VIS), baud timing, modem lines.
class Z8530 {
public:
    enum class Channel : unsigned { A = 0, B = 1 };

    // Called for every byte the guest transmits.
    using TxSink = std::function<void(Channel, uint8_t)>;

    explicit Z8530(TxSink sink) : sink_(std::move(sink)) { reset(); }

    void reset();

    [[nodiscard]] uint8_t readControl(Channel ch);
    void writeControl(Channel ch, uint8_t value);
    [[nodiscard]] uint8_t readData(Channel ch);
    void writeData(Channel ch, uint8_t value);

    // Host -> guest: queue a received byte (emulation thread only).
    void receive(Channel ch, uint8_t value);

    // INT output (level): any enabled interrupt pending while WR9 MIE is set.
    void setIrqCallback(std::function<void(bool)> irq) { irq_ = std::move(irq); updateIrq(); }
    // RR3 (interrupt pending bits, read through channel A).
    [[nodiscard]] uint8_t interruptPending() const;

    // RR0 bits.
    static constexpr uint8_t kRr0RxAvailable = 0x01;
    static constexpr uint8_t kRr0TxEmpty     = 0x04;
    static constexpr uint8_t kRr0Dcd         = 0x08;
    static constexpr uint8_t kRr0Cts         = 0x20;
    // RR1 bits.
    static constexpr uint8_t kRr1AllSent     = 0x01;

private:
    struct ChannelState {
        std::array<uint8_t, 16> wr{};
        uint8_t pointer = 0;
        std::deque<uint8_t> rx;
        bool txPending = false; // transmit buffer emptied with Tx interrupts enabled
    };
    void updateIrq();

    [[nodiscard]] ChannelState& state(Channel ch) { return channels_[static_cast<unsigned>(ch)]; }
    [[nodiscard]] uint8_t readRegister(Channel ch, unsigned reg);
    void writeRegister(Channel ch, unsigned reg, uint8_t value);
    void resetChannel(Channel ch);

    TxSink sink_;
    std::array<ChannelState, 2> channels_;
    uint8_t wr9_ = 0; // master interrupt control: one register shared by both channels
    std::function<void(bool)> irq_;
    bool irqLevel_ = false;
};

} // namespace indy
