#pragma once

#include "indy/core/Types.hpp"

#include <memory>
#include <span>

namespace indy {

// Main memory backing store. Owns a single contiguous, zeroed host allocation;
// the MemoryBus maps pages of it directly for zero-overhead access.
class Ram {
public:
    explicit Ram(uint64_t sizeBytes)
        : size_(sizeBytes), data_(std::make_unique<uint8_t[]>(sizeBytes)) {}

    [[nodiscard]] uint64_t size() const noexcept { return size_; }
    [[nodiscard]] std::span<uint8_t> bytes() noexcept { return {data_.get(), size_}; }

private:
    uint64_t size_;
    std::unique_ptr<uint8_t[]> data_;
};

} // namespace indy
