#pragma once

#include "indy/hpc/Ds1386.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace indy {

// The IP22 PROM's layout of the DS1386 SRAM: a 256-byte variable area at chip
// address 0x40, guarded by a checksum. Taken from the PROM itself (5.3 Rev B10
// IP24): its variable table (name, default, offset, length) puts `eaddr` at
// offset 0xFA (6 bytes) and `netaddr` at 0xB5 (4 bytes), which matches IRIX
// IP22nvram.h (NVOFF_ENET 250); the validator at 0xBFC116E8 checks the byte at
// offset 0 against the sum below and wants revision 8 in bits 5:0 of offset 1.
//
// The Ethernet address is factory data. The PROM never writes it (`eaddr` is
// "informative only"), so an NVRAM without one leaves the variable unset and
// the IRIX kernel dereferences the NULL it gets back from the PROM.
namespace sgi_nvram {

using MacAddress = std::array<uint8_t, 6>;

inline constexpr uint16_t kBase          = 0x40; // chip address of variable offset 0
inline constexpr uint16_t kSize          = 256;
inline constexpr uint16_t kChecksum      = 0x00;
inline constexpr uint16_t kRevision      = 0x01;
inline constexpr uint8_t  kRevisionValue = 8;
inline constexpr uint16_t kEthernet      = 0xFA;

// SGI's OUI (08:00:69) with a fixed serial. Used when the NVRAM has none.
inline constexpr MacAddress kDefaultMac = {0x08, 0x00, 0x69, 0x0E, 0x1D, 0x01};

// The PROM's checksum of the variable area (PROM routine at 0xBFC11050).
[[nodiscard]] uint8_t checksum(Ds1386& nvram);
[[nodiscard]] bool valid(Ds1386& nvram);

[[nodiscard]] MacAddress ethernetAddress(Ds1386& nvram);

// Stores `mac` if given, else the default when the NVRAM holds no address
// (all 0x00 or all 0xFF). The checksum is kept valid when it was valid
// before; a blank NVRAM is left for the PROM to initialise, which keeps the
// address. Returns the address the NVRAM now holds.
MacAddress provideEthernetAddress(Ds1386& nvram, const std::optional<MacAddress>& mac);

// "08:00:69:0e:1d:01" (also accepts '-' separators).
[[nodiscard]] std::optional<MacAddress> parseMac(std::string_view text);

} // namespace sgi_nvram
} // namespace indy
