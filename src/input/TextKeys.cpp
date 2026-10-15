#include "indy/input/TextKeys.hpp"

#include <array>
#include <cctype>
#include <string>

namespace indy {

namespace {

constexpr uint16_t kLeftShift = 0xE1;
constexpr uint16_t kRightAlt = 0xE6; // AltGr on European layouts
constexpr uint16_t kEnter = 0x28;
constexpr uint16_t kTab = 0x2B;

struct Key {
    uint16_t usage = 0; // 0 = not typeable
    bool shift = false;
    bool altGr = false;
};

// One table entry per typeable character (Unicode code point).
struct Mapping {
    char32_t ch;
    Key key;
};

// US layout for printable ASCII (0x20..0x7E).
constexpr std::array<Key, 128> makeUsLayout() {
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
constexpr auto kUsLayout = makeUsLayout();

// French AZERTY (PC 105-key), by the HID usage of the key's position on a US
// keyboard. AltGr is the right Alt key. '*' uses the keypad key, which every
// layout has (the ISO key next to Enter differs between keyboards).
constexpr Mapping kFrLayout[] = {
    // Letters that move: A<->Q, Z<->W, M on the US ';' key.
    {U'a', {0x14}}, {U'z', {0x1A}}, {U'q', {0x04}}, {U'w', {0x1D}}, {U'm', {0x33}},
    {U'A', {0x14, true}}, {U'Z', {0x1A, true}}, {U'Q', {0x04, true}}, {U'W', {0x1D, true}}, {U'M', {0x33, true}},
    // Digit row: symbols unshifted, digits shifted, AltGr extras.
    {U'&', {0x1E}}, {U'é', {0x1F}}, {U'"', {0x20}}, {U'\'', {0x21}}, {U'(', {0x22}},
    {U'-', {0x23}}, {U'è', {0x24}}, {U'_', {0x25}}, {U'ç', {0x26}}, {U'à', {0x27}},
    {U'1', {0x1E, true}}, {U'2', {0x1F, true}}, {U'3', {0x20, true}}, {U'4', {0x21, true}}, {U'5', {0x22, true}},
    {U'6', {0x23, true}}, {U'7', {0x24, true}}, {U'8', {0x25, true}}, {U'9', {0x26, true}}, {U'0', {0x27, true}},
    {U'~', {0x1F, false, true}}, {U'#', {0x20, false, true}}, {U'{', {0x21, false, true}},
    {U'[', {0x22, false, true}}, {U'|', {0x23, false, true}}, {U'`', {0x24, false, true}},
    {U'\\', {0x25, false, true}}, {U'^', {0x26, false, true}}, {U'@', {0x27, false, true}},
    {U')', {0x2D}}, {U'°', {0x2D, true}}, {U']', {0x2D, false, true}},
    {U'=', {0x2E}}, {U'+', {0x2E, true}}, {U'}', {0x2E, false, true}},
    // Right of P, L and the left Shift.
    {U'$', {0x30}}, {U'£', {0x30, true}},
    {U'ù', {0x34}}, {U'%', {0x34, true}},
    {U'µ', {0x32, true}}, {U'*', {0x55}},
    {U'²', {0x35}},
    {U'<', {0x64}}, {U'>', {0x64, true}},
    // Bottom row punctuation.
    {U',', {0x10}}, {U'?', {0x10, true}},
    {U';', {0x36}}, {U'.', {0x36, true}},
    {U':', {0x37}}, {U'/', {0x37, true}},
    {U'!', {0x38}}, {U'§', {0x38, true}},
    {U' ', {0x2C}},
};

Key frKey(char32_t c) {
    for (const Mapping& m : kFrLayout) {
        if (m.ch == c) return m.key;
    }
    // The letters that keep their US position.
    if (c >= U'a' && c <= U'z') return {static_cast<uint16_t>(0x04 + (c - U'a'))};
    if (c >= U'A' && c <= U'Z') return {static_cast<uint16_t>(0x04 + (c - U'A')), true};
    return {};
}

Key keyFor(char32_t c, KeyLayout layout) {
    if (layout == KeyLayout::Fr) return frKey(c);
    return c < 0x80 ? kUsLayout[c] : Key{};
}

void press(std::vector<cmd::KeyEvent>& out, const Key& key) {
    if (key.shift) out.push_back({kLeftShift, true});
    if (key.altGr) out.push_back({kRightAlt, true});
    out.push_back({key.usage, true});
    out.push_back({key.usage, false});
    if (key.altGr) out.push_back({kRightAlt, false});
    if (key.shift) out.push_back({kLeftShift, false});
}

// Decodes one UTF-8 sequence at text[i], advancing i past it; U+FFFD if invalid.
char32_t nextCodePoint(std::string_view text, size_t& i) {
    const auto c = static_cast<unsigned char>(text[i++]);
    if (c < 0x80) return c;
    int extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
    if (extra < 0) return 0xFFFD;
    char32_t cp = c & (0x3F >> extra);
    for (; extra > 0; --extra) {
        if (i >= text.size() || (static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) return 0xFFFD;
        cp = cp << 6 | (static_cast<unsigned char>(text[i++]) & 0x3F);
    }
    return cp;
}

} // namespace

std::optional<KeyLayout> keyLayoutFromName(std::string_view name) {
    std::string lower;
    for (const char c : name) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (lower == "us") return KeyLayout::Us;
    if (lower == "fr") return KeyLayout::Fr;
    return std::nullopt;
}

TypedText keyEventsForText(std::string_view text, KeyLayout layout) {
    TypedText result;
    result.events.reserve(text.size() * 2);
    for (size_t i = 0; i < text.size();) {
        const char32_t c = nextCodePoint(text, i);
        if (c == U'\r' || c == U'\n') {
            press(result.events, {kEnter});
            if (c == U'\r' && i < text.size() && text[i] == '\n') ++i; // CRLF = one Enter
            continue;
        }
        if (c == U'\t') {
            press(result.events, {kTab});
            continue;
        }
        if (const Key key = keyFor(c, layout); key.usage != 0) {
            press(result.events, key);
            continue;
        }
        ++result.skipped; // not typeable on this layout
    }
    return result;
}

} // namespace indy
