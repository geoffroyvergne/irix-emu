// GL pass-through on platforms without a host GL backend yet: the guest
// library sees HELLO fail and leaves IRIX's own OpenGL in charge.
#include "GlBridge.hpp"

#include "indy/cpu/R5000.hpp"

namespace indy {

namespace {

class UnavailableGlBridge final : public GlBridge {
public:
    [[nodiscard]] bool available() const noexcept override { return false; }
    void hypercall(R5000& cpu) override { cpu.setHypercallResult(static_cast<uint64_t>(-1)); }
};

} // namespace

std::unique_ptr<GlBridge> GlBridge::create(OverlaySink) {
    return std::make_unique<UnavailableGlBridge>();
}

} // namespace indy
