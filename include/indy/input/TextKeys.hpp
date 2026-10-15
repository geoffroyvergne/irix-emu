#pragma once

#include "indy/core/Commands.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace indy {

// The keyboard layout the guest (IRIX) interprets key positions with. The
// emulated keyboard sends physical positions, so typing text needs the
// guest's layout to pick the keys.
enum class KeyLayout : uint8_t {
    Us, // US QWERTY
    Fr, // French AZERTY (PC 105-key, AltGr = right Alt)
};

// "us" or "fr" (case-insensitive); nullopt for anything else.
[[nodiscard]] std::optional<KeyLayout> keyLayoutFromName(std::string_view name);

// Turns text into the key presses a person would type on a keyboard with the
// given layout: press/release pairs as USB HID usages (what cmd::KeyEvent and
// the PS/2 keyboard expect), wrapped in Left Shift and/or AltGr where the
// character needs them.
//
//   * the layout's characters: ASCII letters, digits and punctuation, and on
//     French AZERTY its accented letters (é è à ç ù) and symbols (° £ § µ ²)
//     too; text is UTF-8
//   * "\n", "\r" and "\r\n" each become one Enter; "\t" becomes Tab
//   * anything else (other non-ASCII characters, control characters) is
//     skipped and counted
struct TypedText {
    std::vector<cmd::KeyEvent> events;
    size_t skipped = 0; // characters with no key on the layout
};

[[nodiscard]] TypedText keyEventsForText(std::string_view text, KeyLayout layout = KeyLayout::Us);

} // namespace indy
