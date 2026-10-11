#include "indy/hpc/Int3.hpp"

namespace indy {

void Int3::reset() {
    local0_ = local1_ = map_ = 0;
    mask0_ = mask1_ = mapMask0_ = mapMask1_ = mapPolarity_ = 0;
    timer0_ = timer1_ = false;
    update();
}

void Int3::setBits(uint8_t& reg, uint8_t bits, bool asserted) {
    const uint8_t before = reg;
    reg = asserted ? static_cast<uint8_t>(reg | bits) : static_cast<uint8_t>(reg & ~bits);
    if (reg != before) {
        update();
    }
}

void Int3::timerFired(unsigned counter) {
    (counter == 0 ? timer0_ : timer1_) = true;
    update();
}

uint8_t Int3::local0Status() const {
    return static_cast<uint8_t>(local0_ | ((map_ & mapMask0_) != 0 ? kL0Mappable : 0));
}

uint8_t Int3::local1Status() const {
    return static_cast<uint8_t>(local1_ | ((map_ & mapMask1_) != 0 ? kL1Mappable : 0));
}

void Int3::update() {
    const bool levels[4] = {
        (local0Status() & mask0_) != 0,
        (local1Status() & mask1_) != 0,
        timer0_,
        timer1_,
    };
    for (unsigned line = 0; line < 4; ++line) {
        if (levels[line] != lineState_[line]) {
            lineState_[line] = levels[line];
            if (cpu_) {
                cpu_(line, levels[line]);
            }
        }
    }
}

uint8_t Int3::read(uint32_t slot) const {
    switch (slot) {
    case kLocal0Status: return local0Status();
    case kLocal0Mask:   return mask0_;
    case kLocal1Status: return local1Status();
    case kLocal1Mask:   return mask1_;
    case kMapStatus:    return map_;
    case kMapMask0:     return mapMask0_;
    case kMapMask1:     return mapMask1_;
    case kMapPolarity:  return mapPolarity_;
    default:            return 0; // timer clear (write-only), error status (no errors)
    }
}

void Int3::write(uint32_t slot, uint8_t value) {
    switch (slot) {
    case kLocal0Mask: mask0_ = value; break;
    case kLocal1Mask: mask1_ = value; break;
    case kMapMask0:   mapMask0_ = value; break;
    case kMapMask1:   mapMask1_ = value; break;
    case kMapPolarity: mapPolarity_ = value; break; // stored; polarity selection not modelled
    case kTimerClear:
        if ((value & 0x01) != 0) timer0_ = false;
        if ((value & 0x02) != 0) timer1_ = false;
        break;
    default:
        return; // status registers are read-only
    }
    update();
}

} // namespace indy
