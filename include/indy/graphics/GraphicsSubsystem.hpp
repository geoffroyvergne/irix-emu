#pragma once

#include "indy/core/FrameBuffer.hpp"
#include "indy/core/MmioDevice.hpp"

namespace indy {

// Abstract graphics board. The guest talks to it through MMIO (it is an
// MmioDevice); the host only ever sees finished frames in a FrameBuffer.
// This boundary is what lets us later replace the LLE Newport/REX3 model with
// an HLE implementation that translates the pipeline to OpenGL/Vulkan.
class GraphicsSubsystem : public MmioDevice {
public:
    explicit GraphicsSubsystem(FrameBuffer& output) : output_(output) {}

    // Called by System at each emulated vertical blank: compose the visible
    // image into output_.backBuffer() and publish() it.
    virtual void onVBlank(Cycles now) = 0;

protected:
    FrameBuffer& output_;
};

} // namespace indy
