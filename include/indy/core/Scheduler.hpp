#pragma once

#include "indy/core/Types.hpp"

#include <functional>
#include <limits>
#include <queue>
#include <unordered_map>
#include <vector>

namespace indy {

// Cycle-based event scheduler. Devices never poll: they schedule a callback at
// an absolute cycle timestamp, and System::runFor() only lets the CPU run up to
// the next pending event. This keeps the hot loop free of per-device ticks.
class Scheduler {
public:
    using EventId  = uint64_t;
    // Invoked with the timestamp the event was scheduled for (not the current
    // cycle), so periodic events can reschedule relative to it without drift.
    using Callback = std::function<void(Cycles when)>;

    static constexpr Cycles kNever = std::numeric_limits<Cycles>::max();

    EventId scheduleAt(Cycles when, Callback callback);
    void cancel(EventId id);
    void clear();

    // Earliest pending timestamp, or kNever. May report a cancelled event,
    // which only costs one early return from the CPU run loop.
    [[nodiscard]] Cycles nextEventTime() const noexcept {
        return queue_.empty() ? kNever : queue_.top().when;
    }

    // Fire every event with when <= now, in timestamp order.
    void runDue(Cycles now);

private:
    struct Entry {
        Cycles  when;
        EventId id; // monotonically increasing: breaks ties in FIFO order
        [[nodiscard]] bool operator>(const Entry& other) const noexcept {
            return when != other.when ? when > other.when : id > other.id;
        }
    };

    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> queue_;
    std::unordered_map<EventId, Callback> callbacks_;
    EventId nextId_ = 1;
};

} // namespace indy
