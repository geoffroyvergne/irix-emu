#pragma once

#include "indy/core/WarnOnce.hpp"

#include <array>
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <functional>

namespace indy {

// Intel 8042-compatible keyboard/mouse controller with an attached PS/2
// keyboard and PS/2 mouse, as found in the Indy's IOC2 (data at IOC2 +0x43,
// status/command at +0x47). Implements the standard PC/AT controller command
// set and the PS/2 device command protocols; every command completes
// instantly, so the input buffer (IBF) never reads full.
//
// Host input arrives as USB HID keyboard usage codes (what SDL scancodes are)
// and is translated to the scan code set the guest selected: set 2 by default,
// set 3 once the PROM/IRIX switches to it (the Indy does, with F0 03).
// Not yet: controller interrupts (to the IOC2 mappable interrupt), set 1.
class Kbc8042 {
public:
    // Status register.
    static constexpr uint8_t kStatusOBF  = 0x01; // output buffer full (data for the CPU)
    static constexpr uint8_t kStatusIBF  = 0x02; // input buffer full (never, commands are instant)
    static constexpr uint8_t kStatusSYS  = 0x04; // system flag, set by self-test
    static constexpr uint8_t kStatusA2   = 0x08; // last write was to the command port
    static constexpr uint8_t kStatusINH  = 0x10; // 1 = keyboard not inhibited
    static constexpr uint8_t kStatusAUXB = 0x20; // output buffer holds mouse data

    Kbc8042() { reset(); }

    // Interrupt request (level): data waiting for the CPU and that source's
    // interrupt enabled in the command byte (bit 0 keyboard, bit 1 mouse).
    void setIrqCallback(std::function<void(bool)> irq) { irq_ = std::move(irq); updateIrq(); }
    void reset();

    [[nodiscard]] uint8_t readData();
    [[nodiscard]] uint8_t readStatus() const;
    void writeData(uint8_t value);
    void writeCommand(uint8_t value);

    // Host keyboard event (USB HID usage page 0x07). Unknown usages are ignored.
    void hostKey(uint16_t hidUsage, bool pressed);

    // Host mouse movement / buttons, in PS/2 orientation: dx positive = right,
    // dy positive = up; buttons bit 0 = left, 1 = right, 2 = middle. Becomes
    // 3-byte stream packets while the guest has data reporting enabled (F4),
    // stream mode and the mouse port enabled (A8 / command byte bit 5 clear).
    void hostMouse(int dx, int dy, uint8_t buttons);

    static constexpr uint8_t kMouseLeft = 0x01, kMouseRight = 0x02, kMouseMiddle = 0x04;

private:
    enum class Source : uint8_t { Keyboard, Mouse };
    struct Byte {
        uint8_t value;
        Source source;
    };

    void queue(uint8_t value, Source source) {
        output_.push_back({value, source});
        updateIrq();
    }
    void updateIrq();
    void keyboardCommand(uint8_t value);
    void mouseCommand(uint8_t value);

    std::deque<Byte> output_;
    uint8_t commandByte_ = 0;
    uint8_t outputPort_ = 0;
    bool system_ = false;
    bool lastWasCommand_ = false;
    uint8_t pendingCommand_ = 0;      // controller command awaiting its data byte
    std::array<uint8_t, 32> ram_{};   // controller RAM (0x20-0x3F / 0x60-0x7F)

    // PS/2 keyboard.
    bool kbdEnabled_ = true;
    uint8_t kbdScanSet_ = 2;
    uint8_t kbdPendingArg_ = 0;       // command awaiting an argument (ED, F0, F3)
    bool kbdKeyList_ = false;         // after set-3 FB/FC/FD: ACK key codes until a command
    uint8_t kbdLastSent_ = 0xAA;

    // PS/2 mouse.
    bool mouseEnabled_ = false;
    uint8_t mousePendingArg_ = 0;     // F3 (rate) or E8 (resolution)
    uint8_t mouseRate_ = 100;
    uint8_t mouseResolution_ = 2;
    bool mouseRemote_ = false;   // remote mode: packets only on request (EB)
    bool mouseScale21_ = false;  // 2:1 scaling (stored; not applied)
    uint8_t mouseButtons_ = 0;

    std::function<void(bool)> irq_;
    bool irqLevel_ = false;
    WarnOnce warnings_{64};
    bool trace_ = std::getenv("INDY_TRACE_KBC") != nullptr; // log CPU <-> 8042 traffic and host keys
};

} // namespace indy
