#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_set>

namespace indy {

// De-duplicates diagnostics on cold paths: answers "should I log this?" true
// only the first time a key (PC, address...) is seen, and stops entirely after
// `maxKeys` distinct keys so a runaway guest cannot flood the console.
class WarnOnce {
public:
    explicit WarnOnce(size_t maxKeys = 256) : maxKeys_(maxKeys) {}

    [[nodiscard]] bool first(uint64_t key) {
        if (seen_.size() >= maxKeys_) {
            return false;
        }
        return seen_.insert(key).second;
    }

    // True once the limit is reached: callers print a one-off "suppressed" note.
    [[nodiscard]] bool exhausted() const noexcept { return seen_.size() >= maxKeys_; }

    void clear() { seen_.clear(); }

private:
    size_t maxKeys_;
    std::unordered_set<uint64_t> seen_;
};

} // namespace indy
