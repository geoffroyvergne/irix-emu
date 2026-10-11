#pragma once

#include "indy/scsi/ScsiTarget.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

// Small helpers shared by the SCSI targets: big-endian fields, standard
// INQUIRY data and fixed-format sense data (SPC).
namespace indy::scsi {

// Sense keys and additional sense codes used by the targets.
inline constexpr uint8_t kSenseNotReady       = 0x02;
inline constexpr uint8_t kSenseMediumError    = 0x03;
inline constexpr uint8_t kSenseIllegalRequest = 0x05;
inline constexpr uint8_t kSenseUnitAttention  = 0x06;
inline constexpr uint8_t kAscInvalidOpcode    = 0x20;
inline constexpr uint8_t kAscLbaOutOfRange    = 0x21;
inline constexpr uint8_t kAscInvalidField     = 0x24;
inline constexpr uint8_t kAscLunNotSupported  = 0x25;
inline constexpr uint8_t kAscPowerOnReset     = 0x29;
inline constexpr uint8_t kAscMediumChanged    = 0x28;
inline constexpr uint8_t kAscNoMedium         = 0x3A;
inline constexpr uint8_t kAscUnrecoveredRead  = 0x11;

inline void putBe16(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    v[at] = static_cast<uint8_t>(x >> 8);
    v[at + 1] = static_cast<uint8_t>(x);
}
inline void putBe24(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    v[at] = static_cast<uint8_t>(x >> 16);
    v[at + 1] = static_cast<uint8_t>(x >> 8);
    v[at + 2] = static_cast<uint8_t>(x);
}
inline void putBe32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    v[at] = static_cast<uint8_t>(x >> 24);
    v[at + 1] = static_cast<uint8_t>(x >> 16);
    v[at + 2] = static_cast<uint8_t>(x >> 8);
    v[at + 3] = static_cast<uint8_t>(x);
}
[[nodiscard]] inline uint32_t be16(std::span<const uint8_t> b, size_t at) {
    return uint32_t{b[at]} << 8 | b[at + 1];
}
[[nodiscard]] inline uint32_t be32(std::span<const uint8_t> b, size_t at) {
    return uint32_t{b[at]} << 24 | uint32_t{b[at + 1]} << 16 | uint32_t{b[at + 2]} << 8 | b[at + 3];
}

[[nodiscard]] inline ScsiCommandResult good(std::vector<uint8_t> data = {}) {
    return {kScsiGood, std::move(data)};
}

// Trim a response to the initiator's allocation length.
inline std::vector<uint8_t> truncated(std::vector<uint8_t> data, size_t allocation) {
    data.resize(std::min(data.size(), allocation));
    return data;
}

// Standard 36-byte INQUIRY data (SCSI-2).
[[nodiscard]] inline std::vector<uint8_t> inquiryData(ScsiDeviceType type, bool removable, std::string_view vendor,
                                                     std::string_view product, std::string_view revision) {
    std::vector<uint8_t> d(36, 0);
    d[0] = static_cast<uint8_t>(type);
    d[1] = removable ? 0x80 : 0x00;
    d[2] = 0x02; // SCSI-2
    d[3] = 0x02; // response data format
    d[4] = 31;   // additional length
    auto field = [&](size_t at, size_t width, std::string_view text) {
        std::memset(&d[at], ' ', width);
        std::memcpy(&d[at], text.data(), std::min(width, text.size()));
    };
    field(8, 8, vendor);
    field(16, 16, product);
    field(32, 4, revision);
    return d;
}

// Fixed-format sense data for REQUEST SENSE.
struct Sense {
    uint8_t key = 0, asc = 0, ascq = 0;
};
[[nodiscard]] inline std::vector<uint8_t> senseData(const Sense& sense) {
    std::vector<uint8_t> s(18, 0);
    s[0] = 0x70; // current error, fixed format
    s[2] = sense.key;
    s[7] = 10;   // additional sense length
    s[12] = sense.asc;
    s[13] = sense.ascq;
    return s;
}

// What a single-LUN target answers when addressed at any other logical unit
// (SCSI-2): INQUIRY reports peripheral qualifier 3 / type 0x1F ("not
// supported"), REQUEST SENSE reports ILLEGAL REQUEST / LUN NOT SUPPORTED, and
// everything else ends in CHECK CONDITION.
[[nodiscard]] inline ScsiCommandResult unsupportedLun(std::span<const uint8_t> cdb) {
    const size_t allocation = cdb.size() > 4 ? cdb[4] : 0;
    switch (cdb.empty() ? 0xFF : cdb[0]) {
    case 0x12: { // INQUIRY
        std::vector<uint8_t> d(36, 0);
        d[0] = 0x7F;
        d[2] = 0x02; // SCSI-2
        d[3] = 0x02; // response format
        d[4] = 31;   // additional length
        return good(truncated(std::move(d), allocation));
    }
    case 0x03: // REQUEST SENSE
        return good(truncated(senseData({kSenseIllegalRequest, kAscLunNotSupported, 0}), allocation));
    default:
        return {kScsiCheckCondition, {}};
    }
}

} // namespace indy::scsi
