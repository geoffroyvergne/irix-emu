#include "indy/core/Scheduler.hpp"

namespace indy {

Scheduler::EventId Scheduler::scheduleAt(Cycles when, Callback callback) {
    const EventId id = nextId_++;
    callbacks_.emplace(id, std::move(callback));
    queue_.push({when, id});
    return id;
}

void Scheduler::cancel(EventId id) {
    // Lazy deletion: the heap entry stays and is skipped when popped.
    callbacks_.erase(id);
}

void Scheduler::clear() {
    queue_ = {};
    callbacks_.clear();
}

void Scheduler::runDue(Cycles now) {
    while (!queue_.empty() && queue_.top().when <= now) {
        const Entry entry = queue_.top();
        queue_.pop();

        auto it = callbacks_.find(entry.id);
        if (it == callbacks_.end()) {
            continue; // cancelled
        }
        // Move out before invoking: the callback may schedule new events.
        Callback callback = std::move(it->second);
        callbacks_.erase(it);
        callback(entry.when);
    }
}

} // namespace indy
