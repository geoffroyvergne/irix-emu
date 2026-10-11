#include "indy/hpc/Pit8254.hpp"

#include "indy/core/Log.hpp"

#include <numeric>

namespace indy {

void Pit8254::connect(Scheduler& scheduler, Clock clock, uint64_t cpuClockHz, OutputEdge edge) {
    scheduler_ = &scheduler;
    clock_ = std::move(clock);
    const uint64_t g = std::gcd(kInputHz, cpuClockHz);
    ticksNum_ = kInputHz / g;
    ticksDen_ = cpuClockHz / g;
    edge_ = std::move(edge);
    reset();
}

void Pit8254::reset() {
    for (Counter& c : counters_) {
        if (scheduler_ != nullptr && c.event != 0) {
            scheduler_->cancel(c.event);
        }
        c = Counter{};
    }
    anchor_ = now();
}

// ---------------------------------------------------------------------------
// Timeline
// ---------------------------------------------------------------------------

uint64_t Pit8254::ticks2(Cycles at) const {
    return at <= anchor_ ? 0 : (at - anchor_) * ticksNum_ / ticksDen_;
}

uint64_t Pit8254::edges2(Cycles at) const {
    // Counter 2 emits a rising edge each time it passes its terminal count:
    // first after `value` ticks, then every `reload` ticks.
    const Counter& c2 = counters_[2];
    if (!c2.armed || !periodic(c2.mode)) {
        return 0;
    }
    const uint64_t t = ticks2(at);
    return t < c2.value ? 0 : 1 + (t - c2.value) / c2.reload;
}

Cycles Pit8254::cycleOfEdge2(uint64_t edge) const {
    const Counter& c2 = counters_[2];
    const uint64_t tick = c2.value + (edge - 1) * c2.reload;
    return anchor_ + (tick * ticksDen_ + ticksNum_ - 1) / ticksNum_; // round up
}

uint64_t Pit8254::elapsed(unsigned i, Cycles at) const {
    if (!counters_[i].armed) {
        return 0;
    }
    return i == 2 ? ticks2(at) : edges2(at);
}

uint32_t Pit8254::valueAt(unsigned i, Cycles at) const {
    const Counter& c = counters_[i];
    if (!c.armed || (!periodic(c.mode) && !oneShot(c.mode))) {
        return c.value; // stopped: no count loaded, or waiting for a gate trigger
    }
    const uint64_t e = elapsed(i, at);
    if (periodic(c.mode)) {
        // N, N-1, ..., 1, then reload to N.
        return static_cast<uint32_t>((c.value - 1 + c.reload - e % c.reload) % c.reload + 1);
    }
    return static_cast<uint32_t>((c.value - e) & 0xFFFF); // one-shot keeps counting, wrapping
}

bool Pit8254::outputAt(unsigned i, Cycles at) const {
    const Counter& c = counters_[i];
    if (!c.armed) {
        return c.mode != 0; // mode 0 drives OUT low after the control word
    }
    if (oneShot(c.mode)) {
        return c.fired || elapsed(i, at) >= c.value;
    }
    const uint32_t v = valueAt(i, at);
    return c.mode == 2 ? v != 1 : v > c.reload / 2;
}

void Pit8254::rebase(Cycles at) {
    // Re-anchor every counter at `at` with its current count, so a change to
    // one counter (notably counter 2, which clocks 0 and 1) starts cleanly.
    std::array<uint32_t, 3> values{};
    std::array<bool, 3> reached{};
    for (unsigned i = 0; i < 3; ++i) {
        values[i]  = valueAt(i, at);
        reached[i] = oneShot(counters_[i].mode) && counters_[i].armed && elapsed(i, at) >= counters_[i].value;
    }
    for (unsigned i = 0; i < 3; ++i) {
        counters_[i].value = values[i] == 0 && !oneShot(counters_[i].mode) ? counters_[i].reload : values[i];
        counters_[i].fired = counters_[i].fired || reached[i];
    }
    anchor_ = at;
}

void Pit8254::reschedule(unsigned i, Cycles at) {
    Counter& c = counters_[i];
    if (scheduler_ != nullptr && c.event != 0) {
        scheduler_->cancel(c.event);
        c.event = 0;
    }
    const Counter& c2 = counters_[2];
    if (scheduler_ == nullptr || i == 2 || !c.armed || !c2.armed || !periodic(c2.mode)) {
        return;
    }
    // Next terminal count, in counter-i clocks (= counter-2 edges) since the anchor.
    const uint64_t e = elapsed(i, at);
    uint64_t terminal;
    if (periodic(c.mode)) {
        terminal = e < c.value ? c.value : c.value + ((e - c.value) / c.reload + 1) * c.reload;
    } else if (oneShot(c.mode) && !c.fired && e < c.value) {
        terminal = c.value;
    } else {
        return;
    }
    c.event = scheduler_->scheduleAt(cycleOfEdge2(terminal), [this, i](Cycles when) {
        Counter& counter = counters_[i];
        counter.event = 0;
        if (oneShot(counter.mode)) {
            counter.fired = true;
        }
        if (edge_) {
            edge_(i);
        }
        reschedule(i, when);
    });
}

void Pit8254::rescheduleAll(Cycles at) {
    reschedule(0, at);
    reschedule(1, at);
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------

void Pit8254::loadCount(unsigned i, uint32_t count) {
    const Cycles at = now();
    rebase(at);
    Counter& c = counters_[i];
    c.reload = count == 0 ? 0x10000 : count;
    c.value  = c.reload;
    c.armed  = true;
    c.fired  = false;
    rescheduleAll(at);
}

void Pit8254::controlWord(uint8_t value) {
    const unsigned select = value >> 6;
    const unsigned access = (value >> 4) & 3;
    const Cycles at = now();

    if (select == 3) { // read-back: bit 5 = !COUNT, bit 4 = !STATUS, bits 3:1 select counters
        for (unsigned i = 0; i < 3; ++i) {
            if ((value & (2u << i)) == 0) continue;
            Counter& c = counters_[i];
            if ((value & 0x20) == 0 && !c.latch) {
                c.latch = static_cast<uint16_t>(valueAt(i, at));
                c.latchMsbNext = false;
            }
            if ((value & 0x10) == 0 && !c.status) {
                c.status = static_cast<uint8_t>((outputAt(i, at) ? 0x80 : 0) | (c.armed ? 0 : 0x40) | c.control);
            }
        }
        return;
    }
    Counter& c = counters_[select];
    if (access == 0) { // counter latch command
        if (!c.latch) {
            c.latch = static_cast<uint16_t>(valueAt(select, at));
            c.latchMsbNext = false;
        }
        return;
    }
    if ((value & 1) != 0 && warnings_.first(select)) {
        log::warn("8254: BCD counting requested on counter {}; counting in binary", select);
    }
    rebase(at);
    uint8_t mode = (value >> 1) & 7;
    if (mode > 5) mode -= 4; // 6 and 7 alias 2 and 3
    c.control = value & 0x3F;
    c.mode = mode;
    c.access = static_cast<uint8_t>(access);
    c.armed = false;          // counting stops until a count is written
    c.fired = false;
    c.writeMsbNext = false;
    c.readMsbNext = false;
    c.latch.reset();
    c.status.reset();
    rescheduleAll(at);
}

void Pit8254::write(unsigned port, uint8_t value) {
    if (port == 3) {
        controlWord(value);
        return;
    }
    Counter& c = counters_[port];
    switch (c.access) {
    case 1: loadCount(port, value); return;
    case 2: loadCount(port, uint32_t{value} << 8); return;
    default:
        if (!c.writeMsbNext) {
            c.writeLsb = value;
            c.writeMsbNext = true;
        } else {
            c.writeMsbNext = false;
            loadCount(port, uint32_t{value} << 8 | c.writeLsb);
        }
        return;
    }
}

uint8_t Pit8254::read(unsigned port) {
    if (port == 3) {
        return 0; // the control word is write-only
    }
    Counter& c = counters_[port];
    if (c.status) {
        const uint8_t s = *c.status;
        c.status.reset();
        return s;
    }
    const bool latched = c.latch.has_value();
    const auto v = latched ? *c.latch : static_cast<uint16_t>(valueAt(port, now()));
    bool& msbNext = latched ? c.latchMsbNext : c.readMsbNext;

    uint8_t byte;
    bool done = true;
    switch (c.access) {
    case 1: byte = static_cast<uint8_t>(v); break;
    case 2: byte = static_cast<uint8_t>(v >> 8); break;
    default:
        byte = msbNext ? static_cast<uint8_t>(v >> 8) : static_cast<uint8_t>(v);
        done = msbNext;
        msbNext = !msbNext;
        break;
    }
    if (latched && done) {
        c.latch.reset(); // a latch holds until it has been read out
    }
    return byte;
}

uint16_t Pit8254::peek(unsigned counter) const {
    return static_cast<uint16_t>(valueAt(counter, now()));
}

} // namespace indy
