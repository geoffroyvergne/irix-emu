#pragma once

#include "indy/scsi/ScsiTarget.hpp"

#include <array>
#include <cstdint>
#include <memory>

namespace indy {

// SCSI host adapter base: owns the targets on its (narrow, 8-ID) bus.
// The WD33C93 register interface and HPC3 DMA hook-up will derive from this.
class ScsiController {
public:
    static constexpr uint8_t kMaxTargets = 8;

    ScsiController() = default;
    virtual ~ScsiController() = default;
    ScsiController(const ScsiController&) = delete;
    ScsiController& operator=(const ScsiController&) = delete;

    virtual void reset();

    // Takes ownership. Throws if the ID is out of range or already occupied.
    void attach(uint8_t id, std::unique_ptr<ScsiTarget> target);

    [[nodiscard]] ScsiTarget* target(uint8_t id) const noexcept {
        return id < kMaxTargets ? targets_[id].get() : nullptr;
    }

    template <typename T>
    [[nodiscard]] T* targetAs(uint8_t id) const noexcept {
        return dynamic_cast<T*>(target(id));
    }

private:
    std::array<std::unique_ptr<ScsiTarget>, kMaxTargets> targets_;
};

} // namespace indy
