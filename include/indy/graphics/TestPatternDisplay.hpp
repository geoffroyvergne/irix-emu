#pragma once

#include "indy/graphics/GraphicsSubsystem.hpp"

namespace indy {

// Stand-in graphics board that renders an animated colour-bar pattern. Proves
// the emulated-vblank -> triple buffer -> SDL texture path before Newport exists.
// It is never mapped on the bus, so its MMIO handlers simply bus-error.
class TestPatternDisplay final : public GraphicsSubsystem {
public:
    using GraphicsSubsystem::GraphicsSubsystem;

    [[nodiscard]] std::string_view name() const noexcept override { return "TestPattern"; }
    void reset() override { frame_ = 0; }
    [[nodiscard]] bool read(uint32_t, AccessSize, uint64_t&) override { return false; }
    [[nodiscard]] bool write(uint32_t, AccessSize, uint64_t) override { return false; }

    void onVBlank(Cycles now) override;

private:
    uint64_t frame_ = 0;
};

} // namespace indy
