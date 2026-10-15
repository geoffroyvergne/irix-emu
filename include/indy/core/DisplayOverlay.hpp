#pragma once

#include <cstdint>
#include <vector>

namespace indy {

// An image composited over the emulated display, e.g. a guest OpenGL window
// rendered by the host GPU. It covers the rectangle (x, y, width, height) in
// screen pixels, but only where the framebuffer holds the 8-bit `key` value,
// so windows stacked above it (and menus) stay visible, and an unmapped
// window simply stops showing it.
//
// Layer 1 holds overlay-plane images (a program's GL overlay window). Their
// windows live in the popup planes, so `key` is a popup-plane value there:
// the image shows where the popup planes hold it, its opaque pixels
// replacing the popup colour and its transparent ones letting the main
// planes through.
struct DisplayOverlay {
    static constexpr uint32_t kOpaque = 0xFF00'0000u; // pixel flag; without it a layer-1 pixel is transparent

    int32_t x = 0, y = 0;
    uint32_t width = 0, height = 0;
    uint8_t key = 0;
    uint8_t layer = 0;
    std::vector<uint32_t> pixels; // 0xFFRRGGBB (0x00...... transparent), row-major, top row first
};

} // namespace indy
