#include "indy/scsi/ScsiController.hpp"

#include <format>
#include <stdexcept>

namespace indy {

void ScsiController::reset() {
    for (auto& target : targets_) {
        if (target) {
            target->reset();
        }
    }
}

void ScsiController::attach(uint8_t id, std::unique_ptr<ScsiTarget> target) {
    if (id >= kMaxTargets) {
        throw std::out_of_range(std::format("SCSI ID {} out of range", id));
    }
    if (targets_[id]) {
        throw std::logic_error(std::format("SCSI ID {} already occupied by {}", id, targets_[id]->name()));
    }
    targets_[id] = std::move(target);
}

} // namespace indy
