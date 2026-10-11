#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace indy {

// The host side of a SCSI controller's DMA request line: on the Indy, an HPC3
// SCSI DMA channel moving bytes between the chip and memory.
class ScsiDmaPort {
public:
    virtual ~ScsiDmaPort() = default;
    // Device -> memory. Returns how many bytes the channel accepted.
    virtual size_t toMemory(std::span<const uint8_t> bytes) = 0;
    // Memory -> device. Returns how many bytes the channel supplied.
    virtual size_t fromMemory(std::span<uint8_t> bytes) = 0;
};

} // namespace indy
