#pragma once

#include "indy/core/Scheduler.hpp"
#include "indy/core/Types.hpp"
#include "indy/core/WarnOnce.hpp"

#include <array>
#include <functional>
#include <optional>

namespace indy {

// Intel 8254 programmable interval timer in the Indy's IOC2 (+0xB3/B7/BB for
// counters 0-2, +0xBF control word). Counter 2 runs from a 1 MHz clock and its
// output clocks counters 0 and 1 (MAME ioc2; Linux SGINT_TIMER_CLOCK), whose
// outputs interrupt the CPU through INT3 (Cause.IP4/IP5).
//
// Counting is evaluated lazily against emulated time, so latch/read-back always
// see the exact count; only counter 0/1 terminal counts become scheduler events.
// Modes: 0/4 one-shot, 2 rate generator, 3 square wave (counted like mode 2);
// gate-triggered modes 1/5 never start (gates are not wired). Binary only.
class Pit8254 {
public:
    static constexpr uint64_t kInputHz = 1'000'000;

    using Clock = std::function<Cycles()>;
    using OutputEdge = std::function<void(unsigned counter)>; // counter 0/1 OUT rising edge

    void connect(Scheduler& scheduler, Clock clock, uint64_t cpuClockHz, OutputEdge edge);
    void reset();

    // port 0-2: counters, port 3: control word.
    [[nodiscard]] uint8_t read(unsigned port);
    void write(unsigned port, uint8_t value);

    // Current count of a counter (no latch side effects; 0 means 65536).
    [[nodiscard]] uint16_t peek(unsigned counter) const;

private:
    struct Counter {
        uint8_t control = 0;        // last control word bits 5:0
        uint8_t mode = 0;
        uint8_t access = 3;         // 1 = LSB, 2 = MSB, 3 = LSB then MSB
        uint32_t reload = 0x10000;  // N (a written 0 means 65536)
        uint32_t value = 0;         // count at the anchor
        bool armed = false;         // a count has been loaded since the control word
        bool fired = false;         // one-shot: terminal count reached
        bool writeMsbNext = false;
        uint8_t writeLsb = 0;
        bool readMsbNext = false;
        std::optional<uint16_t> latch;
        bool latchMsbNext = false;
        std::optional<uint8_t> status;
        Scheduler::EventId event = 0;
    };

    [[nodiscard]] Cycles now() const { return clock_ ? clock_() : 0; }
    [[nodiscard]] static bool periodic(uint8_t mode) { return mode == 2 || mode == 3; }
    [[nodiscard]] static bool oneShot(uint8_t mode) { return mode == 0 || mode == 4; }

    // Timeline (all relative to the shared anchor).
    [[nodiscard]] uint64_t ticks2(Cycles at) const;          // 1 MHz ticks
    [[nodiscard]] uint64_t edges2(Cycles at) const;          // counter-2 OUT rising edges
    [[nodiscard]] uint64_t elapsed(unsigned i, Cycles at) const;
    [[nodiscard]] uint32_t valueAt(unsigned i, Cycles at) const;
    [[nodiscard]] bool outputAt(unsigned i, Cycles at) const;
    [[nodiscard]] Cycles cycleOfEdge2(uint64_t edge) const;  // when counter 2 emits edge #edge

    void rebase(Cycles at);
    void reschedule(unsigned i, Cycles at);
    void rescheduleAll(Cycles at);
    void loadCount(unsigned i, uint32_t count);
    void controlWord(uint8_t value);

    Scheduler* scheduler_ = nullptr;
    Clock clock_;
    uint64_t ticksNum_ = 1, ticksDen_ = 133; // cycles -> 1 MHz ticks ratio (reduced)
    OutputEdge edge_;
    std::array<Counter, 3> counters_{};
    Cycles anchor_ = 0;
    WarnOnce warnings_{16};
};

} // namespace indy
