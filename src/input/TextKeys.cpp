#include "indy/input/TextKeys.hpp"

#include <array>

namespace indy {

namespace {

constexpr uint16_t kLeftShift = 0xE1;
constexpr uint16_t kEnter = 0x28;
constexpr uint16_t kTab = 0x2B;

struct Key {
    uint16_t usage = 0; // 0 = not typeable
    bool shift = false;
};

// US layout for printable ASCII (0x20..0x7E).
constexpr std::array<Key, 128> makeLayout() {
    std::array<Key, 128> k{};
    for (char c = 'a'; c <= 'z'; ++c) {
        const auto usage = static_cast<uint16_t>(0x04 + (c - 'a'));
        k[static_cast<size_t>(c)] = {usage, false};
        k[static_cast<size_t>(c - 'a' + 'A')] = {usage, true};
    }
    constexpr char digits[] = "1234567890";
    constexpr char shifted[] = "!@#$%^&*()";
    for (size_t i = 0; i < 10; ++i) {
        const auto usage = static_cast<uint16_t>(0x1E + i);
        k[static_cast<size_t>(digits[i])] = {usage, false};
        k[static_cast<size_t>(shifted[i])] = {usage, true};
    }
    // Punctuation keys: unshifted / shifted character per HID usage.
    struct Pair { uint16_t usage; char plain; char shift; };
    constexpr Pair punct[] = {
        {0x2C, ' ', 0},   {0x2D, '-', '_'}, {0x2E, '=', '+'}, {0x2F, '[', '{'}, {0x30, ']', '}'},
        {0x31, '\\', '|'}, {0x33, ';', ':'}, {0x34, '\'', '"'}, {0x35, '`', '~'}, {0x36, ',', '<'},
        {0x37, '.', '>'}, {0x38, '/', '?'},
    };
    for (const Pair& p : punct) {
        k[static_cast<size_t>(p.plain)] = {p.usage, false};
        if (p.shift != 0) k[static_cast<size_t>(p.shift)] = {p.usage, true};
    }
    return k;
}
constexpr auto kLayout = makeLayout();

void press(std::vector<cmd::KeyEvent>& out, uint16_t usage, bool shift) {
    if (shift) out.push_back({kLeftShift, true});
    out.push_back({usage, true});
    out.push_back({usage, false});
    if (shift) out.push_back({kLeftShift, false});
}

} // namespace

TypedText keyEventsForText(std::string_view text) {
    TypedText result;
    result.events.reserve(text.size() * 2);
    for (size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == '\r' || c == '\n') {
            press(result.events, kEnter, false);
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i; // CRLF = one Enter
            continue;
        }
        if (c == '\t') {
            press(result.events, kTab, false);
            continue;
        }
        if (c < 0x80 && kLayout[c].usage != 0) {
            press(result.events, kLayout[c].usage, kLayout[c].shift);
            continue;
        }
        // Not typeable. Count a UTF-8 sequence as one character.
        if (c < 0x80 || (c & 0xC0) == 0xC0) {
            ++result.skipped;
        }
    }
    return result;
}

} // namespace indy
