#pragma once

#include "indy/core/DisplayOverlay.hpp"

#include <cstdint>
#include <functional>
#include <memory>

namespace indy {

class R5000;

// Host side of the GL pass-through: the guest library (glpass/guest) sends
// OpenGL command batches through the GLP_SERVICE_GL hypercall; they are
// replayed on the host GPU, each guest window rendering into an offscreen
// framebuffer, and every buffer swap publishes the image as a display overlay.
// Everything runs on the emulation thread.
class GlBridge {
public:
    // Called with an overlay to show for guest window `drawable`, or nullptr
    // when it should be removed.
    using OverlaySink = std::function<void(uint32_t drawable, const DisplayOverlay* overlay)>;

    virtual ~GlBridge() = default;

    // The platform implementation (macOS: CGL); one that reports itself
    // unavailable elsewhere.
    [[nodiscard]] static std::unique_ptr<GlBridge> create(OverlaySink sink);

    [[nodiscard]] virtual bool available() const noexcept = 0;
    // Handles one GLP_SERVICE_GL hypercall (a1 = operation).
    virtual void hypercall(R5000& cpu) = 0;
};

} // namespace indy
