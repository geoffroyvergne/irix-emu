#include "indy/memory/PromImage.hpp"

#include <cassert>
#include <utility>

namespace indy {

namespace {

// Reassemble the first instruction word as it would read in big-endian after
// converting from `order`.
uint32_t firstWordAs(std::span<const uint8_t> b, PromByteOrder order) {
    switch (order) {
    case PromByteOrder::BigEndian:
        return uint32_t{b[0]} << 24 | uint32_t{b[1]} << 16 | uint32_t{b[2]} << 8 | b[3];
    case PromByteOrder::Swapped16:
        return uint32_t{b[1]} << 24 | uint32_t{b[0]} << 16 | uint32_t{b[3]} << 8 | b[2];
    case PromByteOrder::LittleEndian32:
        return uint32_t{b[3]} << 24 | uint32_t{b[2]} << 16 | uint32_t{b[1]} << 8 | b[0];
    }
    return 0;
}

// Is `word`, executed at the reset vector, a jump/branch landing inside the image?
bool isPlausibleResetInstruction(uint32_t word, uint64_t imageSize) {
    constexpr uint64_t kPromVirt = 0xBFC0'0000;
    const uint32_t opcode = word >> 26;

    if (opcode == 0x02) { // J: target keeps the top 4 bits of the delay-slot address
        const uint64_t target = ((kPromVirt + 4) & 0xF000'0000) | (uint64_t{word & 0x03FF'FFFF} << 2);
        return target >= kPromVirt && target < kPromVirt + imageSize;
    }
    if ((word & 0xFFFF'0000) == 0x1000'0000) { // BEQ $0,$0 (unconditional "b")
        const int64_t offset = int64_t{static_cast<int16_t>(word)} * 4;
        const int64_t target = 4 + offset;
        return target >= 0 && static_cast<uint64_t>(target) < imageSize;
    }
    return false;
}

} // namespace

std::optional<PromByteOrder> detectPromByteOrder(std::span<const uint8_t> image) {
    if (image.size() < 4) {
        return std::nullopt;
    }
    for (PromByteOrder order : {PromByteOrder::BigEndian, PromByteOrder::Swapped16, PromByteOrder::LittleEndian32}) {
        if (isPlausibleResetInstruction(firstWordAs(image, order), image.size())) {
            return order;
        }
    }
    return std::nullopt;
}

void normalizePromByteOrder(std::span<uint8_t> image, PromByteOrder order) {
    assert(image.size() % 4 == 0);
    switch (order) {
    case PromByteOrder::BigEndian:
        return;
    case PromByteOrder::Swapped16:
        for (size_t i = 0; i + 1 < image.size(); i += 2) {
            std::swap(image[i], image[i + 1]);
        }
        return;
    case PromByteOrder::LittleEndian32:
        for (size_t i = 0; i + 3 < image.size(); i += 4) {
            std::swap(image[i], image[i + 3]);
            std::swap(image[i + 1], image[i + 2]);
        }
        return;
    }
}

} // namespace indy
