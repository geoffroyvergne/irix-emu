#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace indy {

// Byte layouts PROM dumps are found in. EPROM readers often read 16-bit-wide
// parts little-endian, producing the Swapped16 layout ("oNu aslb..." strings).
enum class PromByteOrder {
    BigEndian,      // native Indy layout, as on the board
    Swapped16,      // bytes swapped within each 16-bit halfword
    LittleEndian32, // bytes reversed within each 32-bit word
};

[[nodiscard]] constexpr std::string_view toString(PromByteOrder order) noexcept {
    switch (order) {
    case PromByteOrder::BigEndian:      return "big-endian";
    case PromByteOrder::Swapped16:      return "16-bit byte-swapped";
    case PromByteOrder::LittleEndian32: return "32-bit little-endian";
    }
    return "unknown";
}

// Identify the dump's layout from the instruction at the reset vector (offset 0),
// which on a MIPS boot PROM is a jump or unconditional branch to a target inside
// the PROM. Returns nullopt if no layout yields such an instruction.
[[nodiscard]] std::optional<PromByteOrder> detectPromByteOrder(std::span<const uint8_t> image);

// Rewrite `image` in place from `order` to big-endian. Size must be a multiple of 4.
void normalizePromByteOrder(std::span<uint8_t> image, PromByteOrder order);

} // namespace indy
