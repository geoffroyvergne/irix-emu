#include "indy/input/Kbc8042.hpp"

#include "indy/core/Log.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace indy {

namespace {

// Command byte.
constexpr uint8_t kCmdByteSys         = 0x04;
constexpr uint8_t kCmdByteKbdDisabled = 0x10;
constexpr uint8_t kCmdByteAuxDisabled = 0x20;

// PS/2 device responses.
constexpr uint8_t kAck       = 0xFA;
constexpr uint8_t kResend    = 0xFE;
constexpr uint8_t kBatPassed = 0xAA;

// USB HID usage -> PS/2 scan code set 2. Bit 8 marks an E0-prefixed key.
constexpr uint16_t kE0 = 0x100;
constexpr std::array<uint16_t, 0xE8> makeSet2Table() {
    std::array<uint16_t, 0xE8> t{};
    // Letters a..z (HID 0x04..0x1D).
    constexpr uint8_t letters[26] = {0x1C, 0x32, 0x21, 0x23, 0x24, 0x2B, 0x34, 0x33, 0x43, 0x3B, 0x42, 0x4B, 0x3A,
                                     0x31, 0x44, 0x4D, 0x15, 0x2D, 0x1B, 0x2C, 0x3C, 0x2A, 0x1D, 0x22, 0x35, 0x1A};
    for (size_t i = 0; i < 26; ++i) t[0x04 + i] = letters[i];
    // Digits 1..9, 0 (HID 0x1E..0x27).
    constexpr uint8_t digits[10] = {0x16, 0x1E, 0x26, 0x25, 0x2E, 0x36, 0x3D, 0x3E, 0x46, 0x45};
    for (size_t i = 0; i < 10; ++i) t[0x1E + i] = digits[i];
    t[0x28] = 0x5A; t[0x29] = 0x76; t[0x2A] = 0x66; t[0x2B] = 0x0D; t[0x2C] = 0x29; // Enter Esc BS Tab Space
    t[0x2D] = 0x4E; t[0x2E] = 0x55; t[0x2F] = 0x54; t[0x30] = 0x5B; t[0x31] = 0x5D; // - = [ ] backslash
    t[0x33] = 0x4C; t[0x34] = 0x52; t[0x35] = 0x0E; t[0x36] = 0x41; t[0x37] = 0x49; // ; ' ` , .
    t[0x38] = 0x4A; t[0x39] = 0x58;                                                 // / CapsLock
    t[0x32] = 0x5D; t[0x64] = 0x61;                       // ISO keys: # (next to Enter), \ (next to left Shift)
    constexpr uint8_t fkeys[12] = {0x05, 0x06, 0x04, 0x0C, 0x03, 0x0B, 0x83, 0x0A, 0x01, 0x09, 0x78, 0x07};
    for (size_t i = 0; i < 12; ++i) t[0x3A + i] = fkeys[i];                            // F1..F12
    t[0x47] = 0x7E;                                                                 // ScrollLock
    t[0x49] = kE0 | 0x70; t[0x4A] = kE0 | 0x6C; t[0x4B] = kE0 | 0x7D;               // Insert Home PgUp
    t[0x4C] = kE0 | 0x71; t[0x4D] = kE0 | 0x69; t[0x4E] = kE0 | 0x7A;               // Delete End PgDn
    t[0x4F] = kE0 | 0x74; t[0x50] = kE0 | 0x6B; t[0x51] = kE0 | 0x72; t[0x52] = kE0 | 0x75; // arrows R L D U
    t[0x53] = 0x77; t[0x54] = kE0 | 0x4A; t[0x55] = 0x7C; t[0x56] = 0x7B; t[0x57] = 0x79; // NumLock KP/ * - +
    t[0x58] = kE0 | 0x5A;                                                           // KP Enter
    constexpr uint8_t keypad[10] = {0x69, 0x72, 0x7A, 0x6B, 0x73, 0x74, 0x6C, 0x75, 0x7D, 0x70}; // KP1..9, KP0
    for (size_t i = 0; i < 10; ++i) t[0x59 + i] = keypad[i];
    t[0x63] = 0x71;                                                                 // KP .
    t[0xE0] = 0x14; t[0xE1] = 0x12; t[0xE2] = 0x11; t[0xE3] = kE0 | 0x1F;           // LCtrl LShift LAlt LGUI
    t[0xE4] = kE0 | 0x14; t[0xE5] = 0x59; t[0xE6] = kE0 | 0x11; t[0xE7] = kE0 | 0x27; // RCtrl RShift RAlt RGUI
    return t;
}
constexpr auto kSet2 = makeSet2Table();

// USB HID usage -> PS/2 scan code set 3 (IBM PS/2 reference). Set 3 has no E0
// prefixes; most alphanumeric codes equal set 2, the differences are filled in.
constexpr std::array<uint8_t, 0xE8> makeSet3Table() {
    std::array<uint8_t, 0xE8> t{};
    for (size_t i = 0; i < t.size(); ++i) {
        if (kSet2[i] != 0 && (kSet2[i] & kE0) == 0) t[i] = static_cast<uint8_t>(kSet2[i]);
    }
    t[0x29] = 0x08; t[0x31] = 0x5C; t[0x39] = 0x14;                                 // Esc \ CapsLock
    t[0x32] = 0x53; t[0x64] = 0x13;                                                 // ISO # and \ keys
    constexpr uint8_t fkeys[12] = {0x07, 0x0F, 0x17, 0x1F, 0x27, 0x2F, 0x37, 0x3F, 0x47, 0x4F, 0x56, 0x5E};
    for (size_t i = 0; i < 12; ++i) t[0x3A + i] = fkeys[i];                         // F1..F12
    t[0x46] = 0x57; t[0x47] = 0x5F; t[0x48] = 0x62;                                 // PrtSc ScrLk Pause
    t[0x49] = 0x67; t[0x4A] = 0x6E; t[0x4B] = 0x6F;                                 // Insert Home PgUp
    t[0x4C] = 0x64; t[0x4D] = 0x65; t[0x4E] = 0x6D;                                 // Delete End PgDn
    t[0x4F] = 0x6A; t[0x50] = 0x61; t[0x51] = 0x60; t[0x52] = 0x63;                 // arrows R L D U
    t[0x53] = 0x76; t[0x54] = 0x77; t[0x55] = 0x7E; t[0x56] = 0x84; t[0x57] = 0x7C; // NumLock KP/ * - +
    t[0x58] = 0x79;                                                                 // KP Enter
    t[0xE0] = 0x11; t[0xE1] = 0x12; t[0xE2] = 0x19; t[0xE3] = 0x8B;                 // LCtrl LShift LAlt LGUI
    t[0xE4] = 0x58; t[0xE5] = 0x59; t[0xE6] = 0x39; t[0xE7] = 0x8C;                 // RCtrl RShift RAlt RGUI
    return t;
}
constexpr auto kSet3 = makeSet3Table();

} // namespace

void Kbc8042::updateIrq() {
    bool level = false;
    if (!output_.empty()) {
        const bool mouse = output_.front().source == Source::Mouse;
        level = (commandByte_ & (mouse ? 0x02 : 0x01)) != 0;
    }
    if (level != irqLevel_) {
        irqLevel_ = level;
        if (irq_) irq_(level);
    }
}

void Kbc8042::reset() {
    output_.clear();
    commandByte_    = 0;
    outputPort_     = 0;
    system_         = false;
    lastWasCommand_ = false;
    pendingCommand_ = 0;
    ram_.fill(0);
    kbdEnabled_     = true;
    kbdScanSet_     = 2;
    kbdPendingArg_  = 0;
    kbdKeyList_     = false;
    kbdLastSent_    = kBatPassed;
    mouseEnabled_   = false;
    mousePendingArg_ = 0;
    mouseRate_      = 100;
    mouseResolution_ = 2;
    mouseRemote_    = false;
    mouseScale21_   = false;
    mouseButtons_   = 0;
    updateIrq();
}

// ---------------------------------------------------------------------------
// CPU side
// ---------------------------------------------------------------------------

uint8_t Kbc8042::readStatus() const {
    uint8_t status = kStatusINH;
    if (!output_.empty()) {
        status |= kStatusOBF;
        if (output_.front().source == Source::Mouse) {
            status |= kStatusAUXB;
        }
    }
    if (system_) status |= kStatusSYS;
    if (lastWasCommand_) status |= kStatusA2;
    return status;
}

uint8_t Kbc8042::readData() {
    if (output_.empty()) {
        return kbdLastSent_; // the latch keeps the last byte
    }
    const uint8_t value = output_.front().value;
    if (trace_) log::tagged("KBC", "CPU read data {:#04x} ({})", value,
                            output_.front().source == Source::Mouse ? "mouse" : "kbd");
    output_.pop_front();
    updateIrq();
    return value;
}

void Kbc8042::writeCommand(uint8_t value) {
    if (trace_) log::tagged("KBC", "CPU command {:#04x}", value);
    lastWasCommand_ = true;
    pendingCommand_ = 0;

    if (value >= 0x20 && value <= 0x3F) { // read controller RAM (0x20 = command byte)
        queue(value == 0x20 ? commandByte_ : ram_[value & 0x1F], Source::Keyboard);
        return;
    }
    if ((value >= 0x60 && value <= 0x7F) || value == 0xD1 || value == 0xD2 || value == 0xD3 || value == 0xD4) {
        pendingCommand_ = value; // data byte follows on the data port
        return;
    }
    switch (value) {
    case 0xA7: commandByte_ |= kCmdByteAuxDisabled; return;             // disable mouse port
    case 0xA8: commandByte_ &= static_cast<uint8_t>(~kCmdByteAuxDisabled); return; // enable mouse port
    case 0xA9: queue(0x00, Source::Keyboard); return;                   // test mouse port: OK
    case 0xAA:                                                          // controller self-test
        system_ = true;
        queue(0x55, Source::Keyboard);
        return;
    case 0xAB: queue(0x00, Source::Keyboard); return;                   // test keyboard port: OK
    case 0xAD: commandByte_ |= kCmdByteKbdDisabled; return;             // disable keyboard port
    case 0xAE: commandByte_ &= static_cast<uint8_t>(~kCmdByteKbdDisabled); return; // enable keyboard port
    case 0xC0: queue(0x00, Source::Keyboard); return;                   // read input port
    case 0xD0: queue(outputPort_, Source::Keyboard); return;            // read output port
    case 0xE0: queue(0x00, Source::Keyboard); return;                   // read test inputs
    default:
        if (value >= 0xF0) {
            if ((value & 0x01) == 0 && warnings_.first(0x100u | value)) {
                log::warn("8042: output pulse {:#04x} (system reset line) ignored", value);
            }
            return;
        }
        if (warnings_.first(value)) {
            log::warn("8042: unknown controller command {:#04x} ignored", value);
        }
        return;
    }
}

void Kbc8042::writeData(uint8_t value) {
    if (trace_) log::tagged("KBC", "CPU data {:#04x} (after command {:#04x})", value, pendingCommand_);
    lastWasCommand_ = false;
    const uint8_t pending = std::exchange(pendingCommand_, 0);

    if (pending >= 0x60 && pending <= 0x7F) { // write controller RAM (0x60 = command byte)
        if (pending == 0x60) {
            commandByte_ = value;
            system_ = (value & kCmdByteSys) != 0;
            updateIrq();
        } else {
            ram_[pending & 0x1F] = value;
        }
        return;
    }
    switch (pending) {
    case 0xD1: outputPort_ = value; return;
    case 0xD2: queue(value, Source::Keyboard); return; // echo as if from the keyboard
    case 0xD3: queue(value, Source::Mouse); return;    // echo as if from the mouse
    case 0xD4: mouseCommand(value); return;            // next byte goes to the mouse
    default:   keyboardCommand(value); return;         // plain data goes to the keyboard
    }
}

// ---------------------------------------------------------------------------
// PS/2 keyboard
// ---------------------------------------------------------------------------

void Kbc8042::keyboardCommand(uint8_t value) {
    auto send = [&](uint8_t b) {
        queue(b, Source::Keyboard);
        kbdLastSent_ = b;
    };

    if (kbdPendingArg_ != 0 && value < 0xED) { // argument for the previous command
        const uint8_t cmd = std::exchange(kbdPendingArg_, 0);
        send(kAck);
        if (cmd == 0xF0) {
            if (value == 0) {
                send(kbdScanSet_); // query current set
            } else {
                kbdScanSet_ = value & 0x03;
                if (kbdScanSet_ == 1 && warnings_.first(0x201u)) {
                    log::warn("8042: guest selected scan code set 1; host keys are sent as set 2");
                }
            }
        }
        return; // ED (LEDs) and F3 (typematic) arguments are just acknowledged
    }
    if (kbdKeyList_ && value < 0xED) { // set-3 key attribute lists
        send(kAck);
        return;
    }
    kbdPendingArg_ = 0;
    kbdKeyList_ = false;

    switch (value) {
    case 0xFF: // reset: ACK, then basic assurance test passed
        kbdEnabled_ = true;
        kbdScanSet_ = 2;
        send(kAck);
        send(kBatPassed);
        return;
    case 0xFE: queue(kbdLastSent_, Source::Keyboard); return; // resend
    case 0xF6: kbdEnabled_ = true; send(kAck); return;        // set defaults
    case 0xF5: kbdEnabled_ = false; send(kAck); return;       // disable scanning
    case 0xF4: kbdEnabled_ = true; send(kAck); return;        // enable scanning
    case 0xF3: case 0xF0: case 0xED:                          // commands with one argument
        kbdPendingArg_ = value;
        send(kAck);
        return;
    case 0xF2: send(kAck); send(0xAB); send(0x83); return;    // identify: MF2 keyboard
    case 0xEE: send(0xEE); return;                            // echo
    case 0xF7: case 0xF8: case 0xF9: case 0xFA: send(kAck); return; // set-3 all-key attributes
    case 0xFB: case 0xFC: case 0xFD:                          // set-3 per-key attributes
        kbdKeyList_ = true;
        send(kAck);
        return;
    default:
        send(kResend);
        return;
    }
}

void Kbc8042::hostKey(uint16_t hidUsage, bool pressed) {
    if (trace_) log::tagged("KBC", "host key {:#04x} {} (set {}, enabled {}, cmd byte {:#04x})", hidUsage,
                            pressed ? "down" : "up", kbdScanSet_, kbdEnabled_, commandByte_);
    if (hidUsage >= kSet2.size() || kSet2[hidUsage] == 0) {
        return;
    }
    if (!kbdEnabled_ || (commandByte_ & kCmdByteKbdDisabled) != 0) {
        return;
    }
    if (kbdScanSet_ == 3) {
        if (const uint8_t code = kSet3[hidUsage]; code != 0) {
            if (!pressed) queue(0xF0, Source::Keyboard); // break = F0 + make, no prefixes
            queue(code, Source::Keyboard);
        }
        return;
    }
    const uint16_t code = kSet2[hidUsage];
    if ((code & kE0) != 0) queue(0xE0, Source::Keyboard);
    if (!pressed) queue(0xF0, Source::Keyboard);
    queue(static_cast<uint8_t>(code), Source::Keyboard);
}

// ---------------------------------------------------------------------------
// PS/2 mouse
// ---------------------------------------------------------------------------

void Kbc8042::mouseCommand(uint8_t value) {
    auto send = [&](uint8_t b) { queue(b, Source::Mouse); };

    if (mousePendingArg_ != 0) {
        const uint8_t cmd = std::exchange(mousePendingArg_, 0);
        if (cmd == 0xF3) mouseRate_ = value;
        if (cmd == 0xE8) mouseResolution_ = value & 0x03;
        send(kAck);
        return;
    }
    switch (value) {
    case 0xFF: // reset: ACK, BAT passed, device ID 0; defaults, stream mode, disabled
    case 0xF6: // set defaults
        mouseEnabled_ = false;
        mouseRate_ = 100;
        mouseResolution_ = 2;
        mouseRemote_ = false;
        mouseScale21_ = false;
        send(kAck);
        if (value == 0xFF) { send(kBatPassed); send(0x00); }
        return;
    case 0xF5: mouseEnabled_ = false; send(kAck); return;
    case 0xF4: mouseEnabled_ = true; send(kAck); return;
    case 0xF3: case 0xE8: mousePendingArg_ = value; send(kAck); return;
    case 0xF2: send(kAck); send(0x00); return;                       // device ID: standard mouse
    case 0xE9: {                                                      // status request
        // [6] remote, [5] enabled, [4] 2:1 scaling, [2] left, [1] middle, [0] right.
        const uint8_t buttons = static_cast<uint8_t>(((mouseButtons_ & kMouseLeft) != 0 ? 4 : 0) |
                                                     ((mouseButtons_ & kMouseMiddle) != 0 ? 2 : 0) |
                                                     ((mouseButtons_ & kMouseRight) != 0 ? 1 : 0));
        send(kAck);
        send(static_cast<uint8_t>((mouseRemote_ ? 0x40 : 0) | (mouseEnabled_ ? 0x20 : 0) |
                                  (mouseScale21_ ? 0x10 : 0) | buttons));
        send(mouseResolution_);
        send(mouseRate_);
        return;
    }
    case 0xEB:                                                        // read data: one packet
        send(kAck);
        send(static_cast<uint8_t>(0x08 | (mouseButtons_ & 7))); send(0x00); send(0x00);
        return;
    case 0xF0: mouseRemote_ = true; send(kAck); return;
    case 0xEA: mouseRemote_ = false; send(kAck); return;
    case 0xE6: mouseScale21_ = false; send(kAck); return;
    case 0xE7: mouseScale21_ = true; send(kAck); return;
    case 0xEC: case 0xEE:
        send(kAck);
        return;
    default:
        send(kResend);
        return;
    }
}

void Kbc8042::hostMouse(int dx, int dy, uint8_t buttons) {
    mouseButtons_ = buttons & 7;
    if (!mouseEnabled_ || mouseRemote_ || (commandByte_ & kCmdByteAuxDisabled) != 0) {
        return;
    }
    // Each packet carries 9-bit two's complement deltas; split large moves.
    do {
        const int px = std::clamp(dx, -256, 255);
        const int py = std::clamp(dy, -256, 255);
        dx -= px;
        dy -= py;
        queue(static_cast<uint8_t>(0x08 | mouseButtons_ | (px < 0 ? 0x10 : 0) | (py < 0 ? 0x20 : 0)), Source::Mouse);
        queue(static_cast<uint8_t>(px), Source::Mouse);
        queue(static_cast<uint8_t>(py), Source::Mouse);
    } while (dx != 0 || dy != 0);
}

} // namespace indy
