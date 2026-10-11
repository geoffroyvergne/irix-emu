#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

namespace indy {

// Lock-free triple buffer handing finished frames from the emulation thread
// (single producer) to the UI thread (single consumer). Neither side ever
// blocks: the producer always has a back buffer to draw into, and the consumer
// always has a complete frame to show. Pixels are host-endian 0x00RRGGBB
// (matches SDL_PIXELFORMAT_XRGB8888).
class FrameBuffer {
public:
    FrameBuffer(uint32_t width, uint32_t height)
        : width_(width), height_(height) {
        for (auto& buffer : buffers_) {
            buffer.assign(static_cast<size_t>(width) * height, 0u);
        }
    }

    FrameBuffer(const FrameBuffer&) = delete;
    FrameBuffer& operator=(const FrameBuffer&) = delete;

    [[nodiscard]] uint32_t width() const noexcept { return width_; }
    [[nodiscard]] uint32_t height() const noexcept { return height_; }
    [[nodiscard]] uint32_t pitchBytes() const noexcept { return width_ * sizeof(uint32_t); }

    // ---- Producer (emulation thread) -------------------------------------
    [[nodiscard]] std::span<uint32_t> backBuffer() noexcept { return buffers_[back_]; }

    // Publish the back buffer as the newest frame and take the old middle one.
    void publish() noexcept {
        const uint8_t previous = middle_.exchange(back_ | kFreshBit, std::memory_order_acq_rel);
        back_ = previous & kIndexMask;
    }

    // ---- Consumer (UI thread) --------------------------------------------
    // Returns true if a new frame was swapped into the front buffer.
    [[nodiscard]] bool acquire() noexcept {
        if ((middle_.load(std::memory_order_relaxed) & kFreshBit) == 0) {
            return false;
        }
        const uint8_t previous = middle_.exchange(front_, std::memory_order_acq_rel);
        front_ = previous & kIndexMask;
        return true;
    }

    [[nodiscard]] std::span<const uint32_t> frontBuffer() const noexcept { return buffers_[front_]; }

private:
    static constexpr uint8_t kFreshBit  = 0x80;
    static constexpr uint8_t kIndexMask = 0x03;

    uint32_t width_;
    uint32_t height_;
    std::array<std::vector<uint32_t>, 3> buffers_;

    uint8_t back_  = 0; // owned by producer
    uint8_t front_ = 1; // owned by consumer
    alignas(64) std::atomic<uint8_t> middle_{2}; // shared; own cache line
};

} // namespace indy
