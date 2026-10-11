#pragma once

#include "indy/core/Commands.hpp"

#include <string_view>
#include <vector>

namespace indy {

// Turns text into the key presses a person would type on a US-layout
// keyboard: press/release pairs as USB HID usages (what cmd::KeyEvent and the
// PS/2 keyboard expect), wrapped in Left Shift where the character needs it.
//
//   * letters, digits, space and the US punctuation set
//   * "\n", "\r" and "\r\n" each become one Enter; "\t" becomes Tab
//   * anything else (non-ASCII, other control characters) is skipped and counted
struct TypedText {
    std::vector<cmd::KeyEvent> events;
    size_t skipped = 0; // characters that have no key on a US keyboard
};

[[nodiscard]] TypedText keyEventsForText(std::string_view text);

} // namespace indy
