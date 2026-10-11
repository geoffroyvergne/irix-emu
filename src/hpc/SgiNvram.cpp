#include "indy/hpc/SgiNvram.hpp"

#include <algorithm>
#include <charconv>

namespace indy::sgi_nvram {

namespace {

uint8_t byteAt(Ds1386& nvram, uint16_t offset) { return nvram.peek(static_cast<uint16_t>(kBase + offset)); }

} // namespace

uint8_t checksum(Ds1386& nvram) {
    // Start at 0xA5, XOR in every byte but the checksum itself, and rotate
    // left by one bit after each odd offset.
    uint8_t sum = 0xA5;
    for (uint16_t i = 0; i < kSize; ++i) {
        if (i != kChecksum) {
            sum ^= byteAt(nvram, i);
        }
        if ((i & 1) != 0) {
            sum = static_cast<uint8_t>((sum << 1) | (sum >> 7));
        }
    }
    return sum;
}

bool valid(Ds1386& nvram) {
    return byteAt(nvram, kChecksum) == checksum(nvram) && (byteAt(nvram, kRevision) & 0x3F) == kRevisionValue;
}

MacAddress ethernetAddress(Ds1386& nvram) {
    MacAddress mac{};
    for (size_t i = 0; i < mac.size(); ++i) {
        mac[i] = byteAt(nvram, static_cast<uint16_t>(kEthernet + i));
    }
    return mac;
}

MacAddress provideEthernetAddress(Ds1386& nvram, const std::optional<MacAddress>& mac) {
    const MacAddress current = ethernetAddress(nvram);
    const bool blank = std::ranges::all_of(current, [](uint8_t b) { return b == 0x00; }) ||
                       std::ranges::all_of(current, [](uint8_t b) { return b == 0xFF; });
    const MacAddress wanted = mac.value_or(blank ? kDefaultMac : current);
    if (wanted == current) {
        return current;
    }
    const bool wasValid = valid(nvram);
    for (size_t i = 0; i < wanted.size(); ++i) {
        nvram.poke(static_cast<uint16_t>(kBase + kEthernet + i), wanted[i]);
    }
    if (wasValid) {
        nvram.poke(kBase + kChecksum, checksum(nvram));
    }
    return wanted;
}

std::optional<MacAddress> parseMac(std::string_view text) {
    MacAddress mac{};
    for (size_t i = 0; i < mac.size(); ++i) {
        if (i != 0) {
            if (text.empty() || (text.front() != ':' && text.front() != '-')) return std::nullopt;
            text.remove_prefix(1);
        }
        if (text.size() < 2) return std::nullopt;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + 2, mac[i], 16);
        if (ec != std::errc{} || end != text.data() + 2) return std::nullopt;
        text.remove_prefix(2);
    }
    if (!text.empty()) return std::nullopt;
    return mac;
}

} // namespace indy::sgi_nvram
