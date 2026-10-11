#pragma once

#include "indy/core/Types.hpp"

#include <cstdint>
#include <format>
#include <optional>
#include <string>

// MIPS III virtual address segments (32-bit compatibility space, which is what
// the PROM and IRIX kernel run in). Virtual addresses are 64-bit and
// sign-extended from bit 31.
namespace indy::mips {

inline constexpr VirtAddr kKseg0Base = 0xFFFF'FFFF'8000'0000; // unmapped, cached
inline constexpr VirtAddr kKseg1Base = 0xFFFF'FFFF'A000'0000; // unmapped, uncached
inline constexpr VirtAddr kKseg2Base = 0xFFFF'FFFF'C000'0000; // TLB-mapped
inline constexpr uint64_t kUnmappedSegmentMask = 0x1FFF'FFFF;

// Cold reset / NMI vector: 0xBFC00000 sign-extended.
inline constexpr VirtAddr kResetVector = 0xFFFF'FFFF'BFC0'0000;

// KSEG0 and KSEG1 are hard-wired by the architecture to physical [0, 512 MiB).
// Returns nullopt for TLB-mapped segments (kuseg, kseg2/3, 64-bit xk* spaces).
[[nodiscard]] constexpr std::optional<PhysAddr> translateUnmapped(VirtAddr vaddr) noexcept {
    if (vaddr >= kKseg0Base && vaddr < kKseg2Base) {
        return PhysAddr{vaddr & kUnmappedSegmentMask};
    }
    return std::nullopt;
}

static_assert(translateUnmapped(kResetVector) == PhysAddr{0x1FC0'0000});

// True if `vaddr` is a sign-extended 32-bit address (the compatibility space).
[[nodiscard]] constexpr bool isCompat32(VirtAddr vaddr) noexcept {
    return static_cast<VirtAddr>(static_cast<int64_t>(static_cast<int32_t>(vaddr))) == vaddr;
}

// Log-friendly form: compatibility addresses print as 8 hex digits
// (0xbfc00000), anything else as the full 16.
[[nodiscard]] inline std::string formatAddress(VirtAddr vaddr) {
    return isCompat32(vaddr) ? std::format("{:#010x}", static_cast<uint32_t>(vaddr))
                             : std::format("{:#018x}", vaddr);
}

} // namespace indy::mips
