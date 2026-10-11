#pragma once

#include "indy/core/Types.hpp"

#include <string_view>

namespace indy {

// Interface for every chip that appears in the physical address space
// (MC, HPC3, INT2, Newport REX3, ...). The MemoryBus routes accesses here.
class MmioDevice {
public:
    MmioDevice() = default;
    virtual ~MmioDevice() = default;
    MmioDevice(const MmioDevice&) = delete;
    MmioDevice& operator=(const MmioDevice&) = delete;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    virtual void reset() = 0;

    // `offset` is relative to the base address the device was mapped at.
    // Values are right-justified and in host byte order: the bus has already
    // dealt with guest (big-endian) layout. Return false to raise a bus error.
    [[nodiscard]] virtual bool read(uint32_t offset, AccessSize size, uint64_t& value) = 0;
    [[nodiscard]] virtual bool write(uint32_t offset, AccessSize size, uint64_t value) = 0;
};

} // namespace indy
