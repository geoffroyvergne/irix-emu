#pragma once

#include <cstdint>

namespace indy {

// Physical addresses are kept 64-bit wide so any MIPS III physical address fits
// without truncation; the MemoryBus decides how many bits it actually decodes.
using PhysAddr = uint64_t;
using VirtAddr = uint64_t;

// Master clock unit: one CPU pipeline cycle. All device timing is expressed in it.
using Cycles = uint64_t;

enum class AccessSize : uint8_t {
    Byte  = 1,
    Half  = 2,
    Word  = 4,
    Dword = 8,
};

[[nodiscard]] constexpr uint32_t byteCount(AccessSize size) noexcept {
    return static_cast<uint32_t>(size);
}

inline constexpr uint64_t KiB = 1024;
inline constexpr uint64_t MiB = 1024 * KiB;

} // namespace indy
