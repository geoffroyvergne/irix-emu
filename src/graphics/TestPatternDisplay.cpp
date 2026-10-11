#include "indy/graphics/TestPatternDisplay.hpp"

#include <algorithm>
#include <array>

namespace indy {

void TestPatternDisplay::onVBlank(Cycles /*now*/) {
    static constexpr std::array<uint32_t, 8> kBars = {
        0xC0C0C0, 0xC0C000, 0x00C0C0, 0x00C000, 0xC000C0, 0xC00000, 0x0000C0, 0x101010,
    };

    const uint32_t width  = output_.width();
    const uint32_t height = output_.height();
    std::span<uint32_t> pixels = output_.backBuffer();

    // Build one row of bars, then copy it down: memcpy-speed fill.
    const auto row0 = pixels.first(width);
    for (uint32_t x = 0; x < width; ++x) {
        row0[x] = kBars[(x * kBars.size()) / width];
    }
    for (uint32_t y = 1; y < height; ++y) {
        std::ranges::copy(row0, pixels.begin() + static_cast<std::ptrdiff_t>(y * width));
    }

    // A white bar sweeping down the screen shows the emulated clock is ticking.
    constexpr uint32_t kSweepHeight = 8;
    const auto sweepY = static_cast<uint32_t>((frame_ * 4) % (height - kSweepHeight));
    std::fill_n(pixels.begin() + static_cast<std::ptrdiff_t>(sweepY * width), kSweepHeight * width, 0xFFFFFFu);

    ++frame_;
    output_.publish();
}

} // namespace indy
