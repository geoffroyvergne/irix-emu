#include "indy/graphics/Newport.hpp"

#include "indy/core/Log.hpp"

#include <algorithm>
#include <cstdlib>

namespace indy {

namespace {

// DRAWMODE0.
constexpr uint32_t kDm0OpMask   = 0x3;      // 0 NOP, 1 RD, 2 DRAW, 3 S2S
constexpr uint32_t kDm0AdrShift = 2;        // 0 span, 1 block, 2-6 lines
constexpr uint32_t kDm0DoSetup  = 0x20;
constexpr uint32_t kDm0CHost    = 0x40;     // colour from the host data port
constexpr uint32_t kDm0StopX    = 0x100;
constexpr uint32_t kDm0StopY    = 0x200;
constexpr uint32_t kDm0Skip1st  = 0x400;
constexpr uint32_t kDm0SkipLast = 0x800;
constexpr uint32_t kDm0ZPEnab   = 0x1000;
constexpr uint32_t kDm0LsEnab   = 0x2000;
constexpr uint32_t kDm0Limit32  = 0x8000;
constexpr uint32_t kDm0ZOpaque  = 0x10000;
constexpr uint32_t kDm0LsOpaque = 0x20000;
// DRAWMODE1.
constexpr uint32_t kDm1PlanesMask = 0x7;    // 1/2 = RGB/CI planes, 4 overlay, 5 popup, 6 CID
constexpr uint32_t kDm1RwPacked   = 0x80;
constexpr uint32_t kDm1RwDouble   = 0x400;
constexpr uint32_t kDm1FastClear  = 0x20000; // source = COLORVRAM
constexpr unsigned kDm1LogicShift = 28;
// STATUS.
constexpr uint32_t kStatusBackBusy = 0x10; // DCB transfer in progress / timing out
constexpr uint32_t kStatusVrint    = 0x20;
// VC2.
constexpr unsigned kVc2CursorEntry = 0x01; // glyph address in VC2 RAM
constexpr unsigned kVc2CursorX     = 0x02;
constexpr unsigned kVc2CursorY     = 0x03;
constexpr unsigned kVc2CurrentX    = 0x04;
constexpr uint16_t kVc2CtlCursorFunc = 0x0010; // ECURS
constexpr uint16_t kVc2CtlCursorDisp = 0x0080; // ECDISP
constexpr uint16_t kVc2CtlCrosshair  = 0x0100; // ECCURS
constexpr uint16_t kVc2CtlCursor64   = 0x0200; // ECG64: 64x64 single-plane glyph
constexpr int32_t kCursorHotspot     = 31;     // glyph drawn from (x - 31, y - 31)
constexpr unsigned kVc2RamAddress = 0x07;
constexpr unsigned kVc2Control    = 0x10;
constexpr uint16_t kVc2CtlEvirq   = 0x0001; // vertical retrace interrupt enable
constexpr uint16_t kVc2CtlEdisp   = 0x0002; // display enable
constexpr size_t kVc2RamWords     = 0x8000;
// DCB devices (DCBMODE bits 10:7).
enum : unsigned {
    kDcbVc2 = 0, kDcbCmapAll = 1, kDcbCmap0 = 2, kDcbCmap1 = 3, kDcbXmapAll = 4,
    kDcbXmap0 = 5, kDcbXmap1 = 6, kDcbRamdac = 7,
};
constexpr uint32_t kDcbAutoInc = 0x8;

constexpr uint32_t kGlobalMask = 0xFF; // XL8: 8 bitplanes
constexpr uint32_t kFixedMask  = 0x07FF'FF80; // 21.11 coordinate field

// Opt-in register trace for bring-up: INDY_TRACE_GFX=<count> logs that many accesses.
unsigned traceBudget() {
    static unsigned budget = [] {
        const char* env = std::getenv("INDY_TRACE_GFX");
        return env != nullptr ? static_cast<unsigned>(std::strtoul(env, nullptr, 0)) : 0u;
    }();
    return budget;
}
unsigned g_traced = 0;
constexpr size_t kCmapEntries  = 0x2000;

} // namespace

Newport::Newport(FrameBuffer& output)
    : GraphicsSubsystem(output),
      vram_(size_t{kVramWidth} * kVramHeight, 0),
      auxPlanes_(size_t{kVramWidth} * kVramHeight, 0),
      vc2Ram_(kVc2RamWords, 0),
      cmap_(kCmapEntries, 0) {
    reset();
}

void Newport::reset() {
    std::ranges::fill(vram_, 0u);
    std::ranges::fill(auxPlanes_, uint8_t{0});
    std::ranges::fill(vc2Ram_, uint16_t{0});
    std::ranges::fill(cmap_, 0u);
    vc2Regs_.fill(0);
    vc2Index_ = 0;
    xmap_ = {};
    cmapIndex_ = 0;
    ramdacIndex_ = 0;

    drawMode1_ = 0x3002'F001; // reset values per MAME
    drawMode0_ = 0;
    lsMode_ = lsPattern_ = lsPatSave_ = zPattern_ = 0;
    colorBack_ = colorVram_ = colorI_ = colorRed_ = colorGreen_ = colorBlue_ = colorAlpha_ = alphaRef_ = 0;
    wrMask_ = 0xFFFFFF;
    smask0X_ = smask0Y_ = clipMode_ = config_ = topScan_ = 0;
    smaskN_.fill(0);
    xStart_ = yStart_ = xEnd_ = yEnd_ = xSave_ = xMove_ = yMove_ = 0;
    xStartFix_ = yStartFix_ = xEndFix_ = yEndFix_ = 0;
    bresD_ = bresS1_ = bresOctInc1_ = bresRndInc2_ = bresE1_ = bresS2_ = 0;
    aWeight0_ = aWeight1_ = colorX_ = 0;
    slopeRed_ = slopeAlpha_ = slopeGreen_ = slopeBlue_ = 0;
    xWindow_ = yWindow_ = 0x1000;
    dx_ = dy_ = 1;
    hostData_ = 0;
    status_ = 0;
    dcbMode_ = 0x780;
    dcbData0_ = dcbData1_ = 0;
    dirty_ = true;
}

// ---------------------------------------------------------------------------
// Bus interface (big-endian 32-bit registers; 64-bit accesses hit a pair)
// ---------------------------------------------------------------------------

bool Newport::read(uint32_t offset, AccessSize size, uint64_t& value) {
    const bool go = (offset & kGo) != 0;
    const uint32_t reg = offset & ~uint32_t{kGo} & ~3u;
    if (size == AccessSize::Dword) {
        value = uint64_t{readRegister(reg & ~7u)} << 32 | readRegister((reg & ~7u) + 4);
    } else {
        const uint32_t word = readRegister(reg);
        switch (size) {
        case AccessSize::Byte: value = (word >> ((3 - (offset & 3)) * 8)) & 0xFF; break;
        case AccessSize::Half: value = (word >> ((2 - (offset & 2)) * 8)) & 0xFFFF; break;
        default:               value = word; break;
        }
    }
    if (g_traced < traceBudget()) {
        ++g_traced;
        log::tagged("GFX", "read  {:#06x}{} -> {:#x} (dcbmode {:#x})", reg, go ? " GO" : "", value, dcbMode_);
    }
    if (go) {
        runCommand();
    }
    return true;
}

bool Newport::write(uint32_t offset, AccessSize size, uint64_t value) {
    const bool go = (offset & kGo) != 0;
    if (g_traced < traceBudget()) {
        ++g_traced;
        log::tagged("GFX", "write {:#06x}{} <- {:#x} ({}B, dcbmode {:#x})", offset & ~uint32_t{kGo},
                    go ? " GO" : "", value, byteCount(size), dcbMode_);
    }
    const uint32_t reg = offset & ~uint32_t{kGo} & ~3u;
    switch (size) {
    case AccessSize::Dword:
        writeRegister(reg & ~7u, static_cast<uint32_t>(value >> 32));
        writeRegister((reg & ~7u) + 4, static_cast<uint32_t>(value));
        break;
    case AccessSize::Word:
        writeRegister(reg, static_cast<uint32_t>(value));
        break;
    default: {
        // Sub-word store: merge into the current register image.
        const uint32_t current = reg == kDcbData0 ? dcbData0_ : readRegister(reg);
        const unsigned shift = size == AccessSize::Byte ? (3 - (offset & 3)) * 8 : (2 - (offset & 2)) * 8;
        const uint32_t mask = (size == AccessSize::Byte ? 0xFFu : 0xFFFFu) << shift;
        writeRegister(reg, (current & ~mask) | ((static_cast<uint32_t>(value) << shift) & mask));
        break;
    }
    }
    if (go) {
        runCommand();
    }
    return true;
}

// ---------------------------------------------------------------------------
// REX3 registers
// ---------------------------------------------------------------------------

namespace {
// Colour slopes are written sign-magnitude and held in two's complement form
// within their field (red: 24 bits, others: 20 bits), per MAME.
uint32_t slopeValue(uint32_t v, uint32_t magnitudeMask, uint32_t signBit) {
    const uint32_t magnitude = v & magnitudeMask;
    if ((v & 0x8000'0000u) == 0) {
        return magnitude;
    }
    return ((signBit - magnitude) | signBit) & (signBit | magnitudeMask);
}
} // namespace

void Newport::setXStartFixed(uint32_t v) {
    xStartFix_ = v & kFixedMask;
    xStart_ = fixedToInt(xStartFix_);
    xSave_ = xStart_;
}

void Newport::setOctant(uint32_t octant) {
    // Octant bits: 0 = Y decrement, 1 = X decrement, 2 = X major.
    bresOctInc1_ = (bresOctInc1_ & ~0x0700'0000u) | ((octant & 7) << 24);
    dx_ = (octant & 2) != 0 ? -1 : 1;
    dy_ = (octant & 1) != 0 ? -1 : 1;
}

uint32_t Newport::readRegister(uint32_t reg) {
    if (reg >= kSmask1X && reg < kSmask1X + 0x20) {
        return smaskN_[(reg - kSmask1X) / 4];
    }
    switch (reg) {
    case kDrawMode1:  return drawMode1_;
    case kDrawMode0:  return drawMode0_;
    case kLsMode:     return lsMode_;
    case kLsPattern:  return lsPattern_;
    case kLsPatSave:  return lsPatSave_;
    case kZPattern:   return zPattern_;
    case kColorBack:  return colorBack_;
    case kColorVram:  return colorVram_;
    case kAlphaRef:   return alphaRef_;
    case kSmask0X:    return smask0X_;
    case kSmask0Y:    return smask0Y_;
    // Fixed-point coordinates read back in their 21.11 field (mask 0x07FFFF80).
    case kXStart:     return xStartFix_;
    case kYStart:     return yStartFix_;
    case kXEnd:       return xEndFix_;
    case kYEnd:       return yEndFix_;
    case kBresD:      return bresD_;
    case kBresS1:     return bresS1_;
    case kBresOctInc1: return bresOctInc1_;
    case kBresRndInc2: return bresRndInc2_;
    case kBresE1:     return bresE1_;
    case kBresS2:     return bresS2_;
    case kAWeight0:   return aWeight0_;
    case kAWeight1:   return aWeight1_;
    case kSlopeRed:
    case kSlopeRed1:  return slopeRed_;
    case kSlopeAlpha: return slopeAlpha_;
    case kSlopeGreen: return slopeGreen_;
    case kSlopeBlue:  return slopeBlue_;
    case kColorX:     return colorX_;
    case kXSave:      return static_cast<uint32_t>(xSave_) & 0xFFFF;
    case kXYMove:     return (static_cast<uint32_t>(xMove_) << 16) | (static_cast<uint32_t>(yMove_) & 0xFFFF);
    case kXYStartI:   return (static_cast<uint32_t>(xStart_) << 16) | (static_cast<uint32_t>(yStart_) & 0xFFFF);
    case kXYEndI:     return (static_cast<uint32_t>(xEnd_) << 16) | (static_cast<uint32_t>(yEnd_) & 0xFFFF);
    case kColorRed:   return colorRed_;
    case kColorAlpha: return colorAlpha_;
    case kColorGreen: return colorGreen_;
    case kColorBlue:  return colorBlue_;
    case kWrMask:     return wrMask_;
    case kColorI:     return colorI_;
    case kHostRw0:    return static_cast<uint32_t>(hostData_ >> 32);
    case kHostRw1:    return static_cast<uint32_t>(hostData_);
    case kDcbMode:    return dcbMode_;
    case kDcbData0:   return dcbRead();
    case kDcbData1:   return dcbData1_;
    case kTopScan:    return topScan_;
    case kXYWin:      return (static_cast<uint32_t>(xWindow_) << 16) | (static_cast<uint32_t>(yWindow_) & 0xFFFF);
    case kClipMode:   return clipMode_;
    case kConfig:     return config_;
    case kStatus: {
        // Version, idle, FIFOs empty. Reading STATUS acknowledges retrace.
        const uint32_t value = statusBits();
        if ((status_ & kStatusVrint) != 0) {
            status_ &= ~kStatusVrint;
            if (retraceIrq_) retraceIrq_(false);
        }
        return value;
    }
    case kUStatus:    return statusBits();
    default:          return 0;
    }
}

void Newport::writeRegister(uint32_t reg, uint32_t v) {
    if (reg >= kSmask1X && reg < kSmask1X + 0x20) {
        smaskN_[(reg - kSmask1X) / 4] = v;
        return;
    }
    switch (reg) {
    case kDrawMode1:  drawMode1_ = v; return;
    case kDrawMode0:  drawMode0_ = v; return;
    case kLsMode:     lsMode_ = v & 0x0FFF'FFFF; return;
    case kLsPattern:  lsPattern_ = v; return;
    case kLsPatSave:  lsPatSave_ = v; return;
    case kZPattern:   zPattern_ = v; return;
    case kColorBack:  colorBack_ = v; return;
    case kColorVram:  colorVram_ = v; return;
    case kAlphaRef:   alphaRef_ = v & 0xFF; return;
    case kSmask0X:    smask0X_ = v; return;
    case kSmask0Y:    smask0Y_ = v; return;
    case kSetup:      doSetup(); return;
    case kStepZ:      return;
    case kLsRestore:  lsPattern_ = lsPatSave_; return;
    case kLsSave:     lsPatSave_ = lsPattern_; return;
    // Coordinates: 21.11 fixed point, fractional (GL) and integer forms.
    case kXStart:     setXStartFixed(v); return;
    case kYStart:     yStartFix_ = v & kFixedMask; yStart_ = fixedToInt(yStartFix_); return;
    case kXEnd:       xEndFix_ = v & kFixedMask; xEnd_ = fixedToInt(xEndFix_); return;
    case kYEnd:       yEndFix_ = v & kFixedMask; yEnd_ = fixedToInt(yEndFix_); return;
    // Bresenham / antialiasing state (stored for read-back; the octant sets
    // the iteration direction).
    case kBresD:      bresD_ = v & 0x07FF'FFFF; return;
    case kBresS1:     bresS1_ = v & 0x0001'FFFF; return;
    case kBresOctInc1:
        bresOctInc1_ = v & 0x070F'FFFF;
        setOctant((v >> 24) & 7);
        return;
    case kBresRndInc2: bresRndInc2_ = v & 0xFF1F'FFFF; return;
    case kBresE1:     bresE1_ = v & 0xFFFF; return;
    case kBresS2:     bresS2_ = v & 0x03FF'FFFF; return;
    case kAWeight0:   aWeight0_ = v; return;
    case kAWeight1:   aWeight1_ = v; return;
    case kSlopeRed:
    case kSlopeRed1:  slopeRed_ = slopeValue(v, 0x007F'FFFF, 0x0080'0000); return;
    case kSlopeAlpha: slopeAlpha_ = slopeValue(v, 0x0007'FFFF, 0x0008'0000); return;
    case kSlopeGreen: slopeGreen_ = slopeValue(v, 0x0007'FFFF, 0x0008'0000); return;
    case kSlopeBlue:  slopeBlue_ = slopeValue(v, 0x0007'FFFF, 0x0008'0000); return;
    case kXSave:      xSave_ = static_cast<int16_t>(v); return;
    case kXYMove:
        xMove_ = static_cast<int16_t>(v >> 16);
        yMove_ = static_cast<int16_t>(v);
        return;
    case kXStartF:    setXStartFixed(v & 0x007F'FF80); return;
    case kYStartF:    yStartFix_ = v & 0x007F'FF80; yStart_ = fixedToInt(yStartFix_); return;
    case kXEndF:      xEndFix_ = v & 0x007F'FF80; xEnd_ = fixedToInt(xEndFix_); return;
    case kYEndF:      yEndFix_ = v & 0x007F'FF80; yEnd_ = fixedToInt(yEndFix_); return;
    case kXStartI:    setXStart(static_cast<int16_t>(v)); return;
    case kXEndF1:     xEndFix_ = v & 0x007F'FF80; xEnd_ = fixedToInt(xEndFix_); return;
    case kXYStartI:
        setXStart(static_cast<int16_t>(v >> 16));
        yStart_ = static_cast<int16_t>(v);
        yStartFix_ = (static_cast<uint32_t>(yStart_) << 11) & kFixedMask;
        return;
    case kXYEndI:
        xEnd_ = static_cast<int16_t>(v >> 16);
        yEnd_ = static_cast<int16_t>(v);
        xEndFix_ = (static_cast<uint32_t>(xEnd_) << 11) & kFixedMask;
        yEndFix_ = (static_cast<uint32_t>(yEnd_) << 11) & kFixedMask;
        return;
    case kXStartEndI:
        setXStart(static_cast<int16_t>(v >> 16));
        xEnd_ = static_cast<int16_t>(v);
        xEndFix_ = (static_cast<uint32_t>(xEnd_) << 11) & kFixedMask;
        return;
    // Colour.
    case kColorRed:
        colorRed_ = v & 0xFFFFFF;
        if ((drawMode1_ & 0x8000) == 0) {
            colorI_ = (v >> 11) & 0xFF; // CI mode: index lives in the integer part
        }
        return;
    case kColorAlpha: colorAlpha_ = v & 0xFFFFF; return;
    case kColorGreen: colorGreen_ = v & 0xFFFFF; return;
    case kColorBlue:  colorBlue_ = v & 0xFFFFF; return;
    case kWrMask:     wrMask_ = v & 0xFFFFFF; return;
    case kColorI:     colorI_ = v; return;
    case kColorX:     colorX_ = v; return;
    case kHostRw0:    hostData_ = (hostData_ & 0xFFFF'FFFF) | (uint64_t{v} << 32); return;
    case kHostRw1:    hostData_ = (hostData_ & ~uint64_t{0xFFFF'FFFF}) | v; return;
    // Display control bus.
    case kDcbMode:    dcbMode_ = v & 0x1FFF'FFFF; return;
    case kDcbData0:
        dcbData0_ = v;
        dcbWrite(v);
        return;
    case kDcbData1:   dcbData1_ = v; return;
    // Configuration.
    case kTopScan:    topScan_ = v & 0x3FF; dirty_ = true; return;
    case kXYWin:
        xWindow_ = static_cast<int16_t>(v >> 16);
        yWindow_ = static_cast<int16_t>(v);
        return;
    case kClipMode:   clipMode_ = v & 0x1FFF; return;
    case kConfig:     config_ = v & 0x1F'FFFF; return;
    case kStatus:
    case kUStatus:
    case kDcbReset:   return;
    default:
        if (warnings_.first(reg)) {
            log::debug("Newport: write {:#010x} to unhandled REX3 register {:#06x}", v, reg);
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// Rasterizer
// ---------------------------------------------------------------------------

void Newport::doSetup() {
    const uint32_t ydec = yStart_ > yEnd_ ? 1 : 0;
    const uint32_t xdec = xStart_ > xEnd_ ? 2 : 0;
    const uint32_t xmajor = std::abs(xEnd_ - xStart_) > std::abs(yEnd_ - yStart_) ? 4 : 0;
    setOctant(ydec | xdec | xmajor);
}

void Newport::runCommand() {
    const uint32_t op = drawMode0_ & kDm0OpMask;
    const uint32_t adrMode = (drawMode0_ >> kDm0AdrShift) & 7;
    if (op != 0 && (drawMode0_ & kDm0DoSetup) != 0) {
        doSetup();
    }
    switch (op) {
    case 1: // read framebuffer to the host data port
        readPixels();
        return;
    case 2: // draw
        if (adrMode <= 1) {
            drawBlock(false);
        } else {
            drawLine();
        }
        return;
    case 3: // screen to screen copy
        if (adrMode <= 1) {
            drawBlock(true);
        }
        return;
    default:
        return;
    }
}

bool Newport::clipPass(int32_t x, int32_t y) const {
    if ((clipMode_ & 1) != 0) { // screen mask 0: before the window offset
        const auto x0 = static_cast<int16_t>(smask0X_ >> 16), x1 = static_cast<int16_t>(smask0X_);
        const auto y0 = static_cast<int16_t>(smask0Y_ >> 16), y1 = static_cast<int16_t>(smask0Y_);
        if (x < x0 || x > x1 || y < y0 || y > y1) return false;
    }
    if ((clipMode_ & 0x1E) != 0) { // masks 1-4: window-relative; any enabled mask may contain it
        const int32_t wx = x + xWindow_ - 0x1000, wy = y + yWindow_ - 0x1000;
        bool inside = false;
        for (unsigned m = 0; m < 4 && !inside; ++m) {
            if ((clipMode_ & (2u << m)) == 0) continue;
            const uint32_t mx = smaskN_[m * 2], my = smaskN_[m * 2 + 1];
            inside = wx >= static_cast<int16_t>(mx >> 16) - 0x1000 && wx <= static_cast<int16_t>(mx) - 0x1000 &&
                     wy >= static_cast<int16_t>(my >> 16) - 0x1000 && wy <= static_cast<int16_t>(my) - 0x1000;
        }
        if (!inside) return false;
    }
    return true;
}

uint32_t* Newport::pixelAddress(int32_t x, int32_t y) {
    const int32_t wx = x + xWindow_ - 0x1000, wy = y + yWindow_ - 0x1000;
    if (wx < 0 || wy < 0 || wx >= static_cast<int32_t>(kVramWidth) || wy >= static_cast<int32_t>(kVramHeight)) {
        return nullptr;
    }
    return &vram_[static_cast<size_t>(wy) * kVramWidth + static_cast<size_t>(wx)];
}

uint32_t Newport::applyLogicOp(uint32_t s, uint32_t d) const {
    switch (drawMode1_ >> kDm1LogicShift) {
    case 0:  return 0;
    case 1:  return s & d;
    case 2:  return s & ~d;
    case 3:  return s;
    case 4:  return ~s & d;
    case 5:  return d;
    case 6:  return s ^ d;
    case 7:  return s | d;
    case 8:  return ~(s | d);
    case 9:  return ~(s ^ d);
    case 10: return ~d;
    case 11: return s | ~d;
    case 12: return ~s;
    case 13: return ~s | d;
    case 14: return ~(s & d);
    default: return 0xFFFFFF;
    }
}

void Newport::writePixel(int32_t x, int32_t y, uint32_t source) {
    if (!clipPass(x, y)) return;
    const uint32_t planes = drawMode1_ & kDm1PlanesMask;
    const uint32_t mask = wrMask_ & kGlobalMask;
    if (planes == 1 || planes == 2) {
        uint32_t* p = pixelAddress(x, y);
        if (p == nullptr) return;
        const uint32_t value = applyLogicOp(source, *p);
        *p = (*p & ~mask) | (value & mask);
        dirty_ = true;
    } else if (planes >= 4) {
        uint32_t* p = pixelAddress(x, y); // same addressing for the auxiliary planes
        if (p == nullptr) return;
        uint8_t& aux = auxPlanes_[static_cast<size_t>(p - vram_.data())];
        aux = static_cast<uint8_t>((aux & ~mask) | (applyLogicOp(source, aux) & mask));
    }
}

uint32_t Newport::sourceColor(bool patternBit, bool& draw) {
    if (patternBit) {
        draw = true;
        return ((drawMode1_ & kDm1FastClear) != 0 ? colorVram_ : colorI_) & kGlobalMask;
    }
    draw = (drawMode0_ & (kDm0ZOpaque | kDm0LsOpaque)) != 0; // opaque: background colour
    return colorBack_ & kGlobalMask;
}

void Newport::drawBlock(bool screenToScreen) {
    const bool block = ((drawMode0_ >> kDm0AdrShift) & 7) == 1;
    const bool stopX = (drawMode0_ & kDm0StopX) != 0;
    const bool stopY = (drawMode0_ & kDm0StopY) != 0;
    const bool limit32 = (drawMode0_ & kDm0Limit32) != 0;
    const bool hostColor = (drawMode0_ & kDm0CHost) != 0;
    const bool packed = hostColor && (drawMode1_ & kDm1RwPacked) != 0;
    const unsigned packedMax = (drawMode1_ & kDm1RwDouble) != 0 ? 8 : 4; // 8-bit host pixels
    const uint32_t pattern = (drawMode0_ & kDm0ZPEnab) != 0   ? zPattern_
                             : (drawMode0_ & kDm0LsEnab) != 0 ? lsPattern_
                                                              : 0xFFFF'FFFF;
    const int32_t endX = xEnd_ + dx_;
    const int32_t endY = yEnd_ + dy_;
    int32_t x = xStart_, y = yStart_;
    unsigned hostShift = 56;
    size_t guard = size_t{kVramWidth} * kVramHeight * 2; // never spin forever on bad coordinates

    bool more = true;
    while (more && guard != 0) {
        unsigned bit = 31, n = 0;
        bool rowDone = false;
        do {
            const bool patternBit = ((pattern >> bit) & 1) != 0;
            bit = (bit - 1) & 31;
            if (screenToScreen) {
                if (const uint32_t* src = pixelAddress(x, y)) {
                    writePixel(x + xMove_, y + yMove_, *src);
                }
            } else {
                bool draw = false;
                uint32_t color = sourceColor(patternBit, draw);
                if (hostColor && patternBit) {
                    color = static_cast<uint32_t>(hostData_ >> hostShift) & kGlobalMask;
                    if (packed) hostShift -= 8;
                }
                if (draw) writePixel(x, y, color);
            }
            x += dx_;
            ++n;
            --guard;
            rowDone = x == endX;
        } while (!rowDone && stopX && guard != 0 && !(limit32 && n >= 32) && !(packed && n >= packedMax));

        if (!rowDone) {
            break; // row continues on the next GO (no STOPONX, 32-pixel limit, packed host data)
        }
        if (!block && !screenToScreen) {
            break; // a span is a single row
        }
        x = xSave_;
        y += dy_;
        more = stopY && y != endY;
    }
    xStart_ = x; // x_save is kept, so the next row restarts there
    yStart_ = y;
    xStartFix_ = (static_cast<uint32_t>(x) << 11) & kFixedMask;
    yStartFix_ = (static_cast<uint32_t>(y) << 11) & kFixedMask;
}

void Newport::drawLine() {
    // Integer Bresenham between start and end (fractional and antialiased
    // lines are drawn the same way for now).
    if (warnings_.first(0x5000'0000u | ((drawMode0_ >> kDm0AdrShift) & 7)) &&
        ((drawMode0_ >> kDm0AdrShift) & 7) != 2) {
        log::debug("Newport: fractional/antialiased line drawn as an integer line");
    }
    const bool usePattern = (drawMode0_ & kDm0LsEnab) != 0;
    int32_t x = xStart_, y = yStart_;
    const int32_t x1 = xEnd_, y1 = yEnd_;
    const int32_t adx = std::abs(x1 - x), ady = -std::abs(y1 - y);
    const int32_t sx = x < x1 ? 1 : -1, sy = y < y1 ? 1 : -1;
    int32_t err = adx + ady;
    unsigned bit = 31;
    bool first = true;
    for (int guard = 0; guard < 8192; ++guard) {
        const bool last = x == x1 && y == y1;
        const bool skip = (first && (drawMode0_ & kDm0Skip1st) != 0) || (last && (drawMode0_ & kDm0SkipLast) != 0);
        const bool patternBit = !usePattern || ((lsPattern_ >> bit) & 1) != 0;
        bit = (bit - 1) & 31;
        if (!skip) {
            bool draw = false;
            const uint32_t color = sourceColor(patternBit, draw);
            if (draw) writePixel(x, y, color);
        }
        if (last) break;
        first = false;
        const int32_t e2 = 2 * err;
        if (e2 >= ady) { err += ady; x += sx; }
        if (e2 <= adx) { err += adx; y += sy; }
    }
    xStart_ = x1;
    yStart_ = y1;
}

void Newport::readPixels() {
    // Up to 4 (8 with RWDOUBLE) 8-bit pixels packed into the host data port,
    // first pixel in bits 63:56; then step along the row.
    const unsigned maxPixels = (drawMode1_ & kDm1RwDouble) != 0 ? 8 : 4;
    const auto width = static_cast<unsigned>(std::max(1, xEnd_ - xStart_ + 1));
    const unsigned n = std::min(maxPixels, width);
    uint64_t data = 0;
    for (unsigned i = 0; i < n; ++i) {
        const uint32_t* p = pixelAddress(xStart_ + static_cast<int32_t>(i), yStart_);
        data |= uint64_t{p != nullptr ? (*p & kGlobalMask) : 0u} << (56 - 8 * i);
    }
    hostData_ = data;
    xStart_ += static_cast<int32_t>(n);
    if (xStart_ > xEnd_) {
        xStart_ = xSave_;
        yStart_ += dy_;
    }
}

// ---------------------------------------------------------------------------
// Display Control Bus: device in DCBMODE[10:7], register in [6:4], width in
// [1:0] (0 = 4 bytes). Byte lanes depend on the width (as the PROM uses them
// and as MAME masks them): 1 byte = bits 7:0, 2 bytes = bits 15:0, 3 bytes =
// bits 31:8, 4 bytes = the whole word. `raw` is the value right-aligned.
// ---------------------------------------------------------------------------

uint32_t Newport::statusBits() {
    uint32_t value = status_ | kRex3Version;
    if (clock_ && clock_() < backBusyUntil_) {
        value |= kStatusBackBusy;
    }
    return value;
}

void Newport::dcbTimeout() {
    // Nobody acknowledges: the transfer times out after about a millisecond.
    if (clock_) {
        backBusyUntil_ = clock_() + cpuClockHz_ / 1000;
    }
}

void Newport::dcbWrite(uint32_t data) {
    const unsigned width = (dcbMode_ & 3) == 0 ? 4 : (dcbMode_ & 3);
    const unsigned crs = (dcbMode_ >> 4) & 7;
    const unsigned device = (dcbMode_ >> 7) & 0xF;
    const uint32_t raw = width == 1 ? (data & 0xFF) : width == 2 ? (data & 0xFFFF) : width == 3 ? (data >> 8) : data;

    switch (device) {
    case kDcbVc2:     vc2Write(crs, raw, width); break;
    case kDcbCmapAll: cmapWrite(0, crs, raw); cmapWrite(1, crs, raw); break;
    case kDcbCmap0:   cmapWrite(0, crs, raw); break;
    case kDcbCmap1:   cmapWrite(1, crs, raw); break;
    case kDcbXmapAll: xmapWrite(0, crs, raw); xmapWrite(1, crs, raw); break;
    case kDcbXmap0:   xmapWrite(0, crs, raw); break;
    case kDcbXmap1:   xmapWrite(1, crs, raw); break;
    case kDcbRamdac:
        if (crs == 0) ramdacIndex_ = static_cast<uint8_t>(raw); // gamma LUTs are left linear
        break;
    default:
        dcbTimeout();
        if (warnings_.first(0x6000'0000u | device)) {
            log::debug("Newport: DCB write to absent device {}", device);
        }
        break;
    }
    if ((dcbMode_ & kDcbAutoInc) != 0) {
        dcbMode_ = (dcbMode_ & ~0x70u) | (((crs + 1) & 7) << 4);
    }
}

uint32_t Newport::dcbRead() {
    const unsigned width = (dcbMode_ & 3) == 0 ? 4 : (dcbMode_ & 3);
    const unsigned crs = (dcbMode_ >> 4) & 7;
    const unsigned device = (dcbMode_ >> 7) & 0xF;
    uint32_t raw = 0;
    switch (device) {
    case kDcbVc2:     raw = vc2Read(crs); break;
    case kDcbCmapAll:
    case kDcbCmap0:
    case kDcbCmap1:   raw = cmapRead(crs); break;
    case kDcbXmapAll:
    case kDcbXmap0:   raw = xmapRead(0, crs); break;
    case kDcbXmap1:   raw = xmapRead(1, crs); break;
    case kDcbRamdac:
        break; // BT445 reads are not modelled (MAME returns nothing either)
    default:
        dcbTimeout();
        if (warnings_.first(0x7000'0000u | device)) {
            log::debug("Newport: DCB read from absent device {}", device);
        }
        break;
    }
    if ((dcbMode_ & kDcbAutoInc) != 0) {
        dcbMode_ = (dcbMode_ & ~0x70u) | (((crs + 1) & 7) << 4);
    }
    return width == 1 ? (raw & 0xFF) : width == 2 ? (raw & 0xFFFF) : width == 3 ? (raw << 8) : raw;
}

void Newport::vc2Write(unsigned crs, uint32_t raw, unsigned width) {
    auto setRegister = [&](unsigned index, uint16_t value) {
        index &= 0x1F;
        vc2Regs_[index] = value;
        if (index == kVc2CursorY) {
            vc2Regs_[kVc2CurrentX] = vc2Regs_[kVc2CursorX]; // latches the current cursor X
        }
        if (index == kVc2Control || index == kVc2CursorEntry || index == kVc2CursorX || index == kVc2CursorY) {
            dirty_ = true; // display enable / cursor moved
        }
    };
    switch (crs) {
    case 0: // index, optionally followed by 16 bits of data
        if (width == 1) {
            vc2Index_ = static_cast<uint8_t>(raw & 0x1F);
        } else if (width == 3) {
            vc2Index_ = static_cast<uint8_t>((raw >> 16) & 0x1F);
            setRegister(vc2Index_, static_cast<uint16_t>(raw));
        } else if (width == 4) {
            vc2Index_ = static_cast<uint8_t>((raw >> 24) & 0x1F);
            setRegister(vc2Index_, static_cast<uint16_t>(raw >> 8));
        }
        return;
    case 1: // data for the selected register
        setRegister(vc2Index_, static_cast<uint16_t>(raw));
        return;
    case 3: { // RAM data at the RAM address register, auto-incrementing
        uint16_t& address = vc2Regs_[kVc2RamAddress];
        vc2Ram_[address & (kVc2RamWords - 1)] = static_cast<uint16_t>(raw);
        address = static_cast<uint16_t>((address + 1) & (kVc2RamWords - 1));
        dirty_ = true; // may be the cursor glyph
        return;
    }
    default:
        return;
    }
}

uint32_t Newport::vc2Read(unsigned crs) {
    switch (crs) {
    case 1: return vc2Regs_[vc2Index_];
    case 3: {
        uint16_t& address = vc2Regs_[kVc2RamAddress];
        const uint16_t value = vc2Ram_[address & (kVc2RamWords - 1)];
        address = static_cast<uint16_t>((address + 1) & (kVc2RamWords - 1));
        return value;
    }
    default: return 0;
    }
}

void Newport::xmapWrite(unsigned which, unsigned crs, uint32_t raw) {
    Xmap9& x = xmap_[which];
    switch (crs) {
    case 0: x.config = static_cast<uint8_t>(raw); break;
    case 3: x.cursorMsb = static_cast<uint8_t>(raw); break;
    case 4: x.popupMsb = static_cast<uint8_t>(raw); break;
    case 5: x.modes[(raw >> 24) & 0x1F] = raw & 0xFF'FFFF; break; // entry index in the top byte
    case 7: x.modeIndex = static_cast<uint8_t>(raw); break;
    default: return; // revision / FIFO status are read-only
    }
    dirty_ = true;
}

uint32_t Newport::xmapRead(unsigned which, unsigned crs) const {
    const Xmap9& x = xmap_[which];
    switch (crs) {
    case 0: return x.config;
    case 1: return kXmap9Revision;
    case 2: return 0x2; // FIFO empty
    case 3: return x.cursorMsb;
    case 4: return x.popupMsb;
    case 5: {
        const uint32_t entry = x.modes[(x.modeIndex & 0x7C) >> 2];
        switch (x.modeIndex & 3) {
        case 0:  return (entry >> 16) & 0xFF;
        case 1:  return (entry >> 8) & 0xFF;
        case 2:  return entry & 0xFF;
        default: return 0;
        }
    }
    case 7: return x.modeIndex;
    default: return 0;
    }
}

void Newport::cmapWrite(unsigned which, unsigned crs, uint32_t raw) {
    if (which != 0) return; // both CMAPs hold the same palette in this model
    switch (crs) {
    case 0: cmapIndex_ = static_cast<uint16_t>(raw); return;                              // address
    case 1: cmapIndex_ = static_cast<uint16_t>((cmapIndex_ & 0x00FF) | ((raw & 0xFF) << 8)); return; // address high
    case 2: // palette entry R, G, B; the address advances to the next entry
        cmap_[cmapIndex_ & (kCmapEntries - 1)] = raw & 0xFF'FFFF;
        cmapIndex_ = static_cast<uint16_t>(cmapIndex_ + 1);
        dirty_ = true;
        return;
    default:
        return;
    }
}

uint32_t Newport::cmapRead(unsigned crs) const {
    switch (crs) {
    case 4: return 0x08;           // status
    case 6: return kCmapRevision;  // revision
    default: return 0;
    }
}

// ---------------------------------------------------------------------------
// Scan-out
// ---------------------------------------------------------------------------

void Newport::onVBlank(Cycles /*now*/) {
    if ((vc2Regs_[kVc2Control] & kVc2CtlEvirq) != 0 && (status_ & kStatusVrint) == 0) {
        status_ |= kStatusVrint;
        if (retraceIrq_) retraceIrq_(true);
    }
    compose();
}

void Newport::compose() {
    if (!dirty_) {
        return; // nothing changed: the UI keeps showing the last frame
    }
    dirty_ = false;
    std::span<uint32_t> out = output_.backBuffer();
    const uint32_t width = std::min(output_.width(), kVisibleWidth);
    const uint32_t height = std::min(output_.height(), kVisibleHeight);

    if ((vc2Regs_[kVc2Control] & kVc2CtlEdisp) == 0) {
        std::ranges::fill(out, 0u); // display disabled
    } else {
        // Per-line display IDs are not modelled: mode entry 0 drives the screen.
        const uint32_t mode = xmap_[0].modes[0];
        const bool colorIndex = ((mode >> 8) & 3) == 0;
        const uint32_t ciBase = (mode & 0xF8) << 5;
        for (uint32_t y = 0; y < height; ++y) {
            const uint32_t* src = &vram_[size_t{y} * kVramWidth];
            uint32_t* dst = &out[size_t{y} * output_.width()];
            for (uint32_t x = 0; x < width; ++x) {
                const uint32_t v = src[x] & kGlobalMask;
                dst[x] = colorIndex ? cmap_[(ciBase | v) & (kCmapEntries - 1)] : (v << 16 | v << 8 | v);
            }
        }

        // VC2 hardware cursor, on top of everything (glyph mode only).
        const uint16_t ctl = vc2Regs_[kVc2Control];
        if ((ctl & kVc2CtlCursorFunc) != 0 && (ctl & kVc2CtlCursorDisp) != 0 && (ctl & kVc2CtlCrosshair) == 0) {
            const bool large = (ctl & kVc2CtlCursor64) != 0;
            const int32_t size = large ? 64 : 32;
            const int32_t left = vc2Regs_[kVc2CursorX] - kCursorHotspot;
            const int32_t top = vc2Regs_[kVc2CursorY] - kCursorHotspot;
            const uint32_t cursorBase = static_cast<uint32_t>(xmap_[0].cursorMsb) << 5;
            for (int32_t cy = 0; cy < size; ++cy) {
                const int32_t y = top + cy;
                if (y < 0 || y >= static_cast<int32_t>(height)) continue;
                for (int32_t cx = 0; cx < size; ++cx) {
                    const int32_t x = left + cx;
                    if (x < 0 || x >= static_cast<int32_t>(width)) continue;
                    if (const uint8_t p = cursorPixel(cx, cy, large); p != 0) { // 0 = transparent
                        out[static_cast<size_t>(y) * output_.width() + static_cast<size_t>(x)] =
                            cmap_[(cursorBase | p) & (kCmapEntries - 1)];
                    }
                }
            }
        }
    }
    output_.publish();
}

uint8_t Newport::cursorPixel(int32_t x, int32_t y, bool large) const {
    // Glyph rows are 16-bit words, bit 15 = leftmost pixel. 32x32: two planes
    // of 2 words per row, plane 1 64 words after plane 0. 64x64: one plane of
    // 4 words per row.
    const uint32_t entry = vc2Regs_[kVc2CursorEntry];
    const auto bit = [&](uint32_t address) {
        return (vc2Ram_[address & (kVc2RamWords - 1)] >> (15 - (x % 16))) & 1u;
    };
    if (large) {
        return static_cast<uint8_t>(bit(entry + static_cast<uint32_t>(y * 4 + x / 16)));
    }
    const uint32_t word = entry + static_cast<uint32_t>(y * 2 + x / 16);
    return static_cast<uint8_t>(bit(word) | (bit(word + 64) << 1));
}

} // namespace indy
