#pragma once

#include "indy/core/DisplayOverlay.hpp"

#include "indy/core/WarnOnce.hpp"
#include "indy/graphics/GraphicsSubsystem.hpp"

#include <array>
#include <map>
#include <functional>
#include <vector>

namespace indy {

// SGI Newport XL8 graphics board (REX3 raster engine + VC2 + 2x XMAP9 +
// 2x CMAP + BT445), REX3 registers at physical 0x1F0F0000 (GIO graphics slot).
//
// Register layout: Linux include/video/newport.h. Behaviour cross-checked
// with MAME src/devices/bus/gio64/newport.cpp (clean-room: facts only).
//
//  * Every REX3 register has a "GO" alias at +0x800: writing (or reading) it
//    performs the register access and then runs the command in DRAWMODE0.
//  * Coordinates are 21.11 fixed point (integer forms xstarti/xystarti...);
//    XYWIN adds a window offset where 0x1000 means zero.
//  * VRAM holds 1344 x 1024 pixel words; 1280 x 1024 is visible.
//  * The VC2, XMAP9s, CMAPs and RAMDAC sit on the Display Control Bus, reached
//    through DCBMODE (device + register select + width) and DCBDATA0. Lanes
//    by width: 1 byte = bits 7:0, 2 = bits 15:0, 3 = bits 31:8, 4 = all.
//    Accesses to devices that are not fitted time out with STATUS.BBUSY set
//    for about 1 ms.
//  * Scan-out: VRAM byte -> XMAP9 mode entry -> CMAP palette -> RGB.
//
// Implemented: DRAW/S2S/RD with span and block addressing (patterns, opaque
// background, logic ops, write mask, screen mask 0 + window clipping, 32-pixel
// limit, host data port), integer lines, DCB devices, vertical retrace status.
// The VC2 hardware cursor (32x32 two-plane / 64x64 glyph) is drawn on top.
// Not yet: shading/blending/alpha, fractional & antialiased lines (drawn as
// integer lines), overlay/popup/CID plane display, crosshair cursor, per-line
// DIDs (mode entry 0 is used for the whole screen), RGB pixel modes.
class Newport final : public GraphicsSubsystem {
public:
    static constexpr uint32_t kVramWidth  = 1344;
    static constexpr uint32_t kVramHeight = 1024;
    static constexpr uint32_t kVisibleWidth  = 1280;
    static constexpr uint32_t kVisibleHeight = 1024;
    static constexpr uint64_t kRegisterWindow = 0x2000;

    // REX3 register offsets (byte offsets in the REX3 window; +0x800 = GO).
    enum Reg : uint32_t {
        kDrawMode1 = 0x000, kDrawMode0 = 0x004, kLsMode = 0x008, kLsPattern = 0x00C,
        kLsPatSave = 0x010, kZPattern = 0x014, kColorBack = 0x018, kColorVram = 0x01C,
        kAlphaRef = 0x020, kSmask0X = 0x028, kSmask0Y = 0x02C, kSetup = 0x030,
        kStepZ = 0x034, kLsRestore = 0x038, kLsSave = 0x03C,
        kXStart = 0x100, kYStart = 0x104, kXEnd = 0x108, kYEnd = 0x10C,
        kXSave = 0x110, kXYMove = 0x114,
        kBresD = 0x118, kBresS1 = 0x11C, kBresOctInc1 = 0x120, kBresRndInc2 = 0x124,
        kBresE1 = 0x128, kBresS2 = 0x12C, kAWeight0 = 0x130, kAWeight1 = 0x134,
        kXStartF = 0x138, kYStartF = 0x13C, kXEndF = 0x140, kYEndF = 0x144,
        kXStartI = 0x148, kXEndF1 = 0x14C, kXYStartI = 0x150, kXYEndI = 0x154, kXStartEndI = 0x158,
        kColorRed = 0x200, kColorAlpha = 0x204, kColorGreen = 0x208, kColorBlue = 0x20C,
        kSlopeRed = 0x210, kSlopeAlpha = 0x214, kSlopeGreen = 0x218, kSlopeBlue = 0x21C,
        kWrMask = 0x220, kColorI = 0x224, kColorX = 0x228, kSlopeRed1 = 0x22C,
        kHostRw0 = 0x230, kHostRw1 = 0x234, kDcbMode = 0x238, kDcbData0 = 0x240, kDcbData1 = 0x244,
        kGo = 0x800,
        // Configuration registers.
        kSmask1X = 0x1300, kTopScan = 0x1320, kXYWin = 0x1324, kClipMode = 0x1328,
        kConfig = 0x1330, kStatus = 0x1338, kUStatus = 0x133C, kDcbReset = 0x1340,
    };

    // Board identification (XL8).
    static constexpr uint32_t kRex3Version  = 3;    // STATUS[2:0]
    static constexpr uint8_t kXmap9Revision = 1;
    static constexpr uint8_t kCmapRevision  = 0xA1;

    explicit Newport(FrameBuffer& output);

    [[nodiscard]] std::string_view name() const noexcept override { return "Newport"; }
    void reset() override;
    [[nodiscard]] bool read(uint32_t offset, AccessSize size, uint64_t& value) override;
    [[nodiscard]] bool write(uint32_t offset, AccessSize size, uint64_t value) override;
    void onVBlank(Cycles now) override;

    // Time base for DCB timeouts.
    void setClock(std::function<Cycles()> clock, uint64_t cpuClockHz) {
        clock_ = std::move(clock);
        cpuClockHz_ = cpuClockHz;
    }

    // Vertical retrace interrupt (INT3 mappable bit 0), when the VC2 enables it.
    void setRetraceIrqCallback(std::function<void(bool)> irq) { retraceIrq_ = std::move(irq); }

    // Inspection/tests.
    [[nodiscard]] uint32_t vramAt(uint32_t x, uint32_t y) const { return vram_[y * kVramWidth + x]; }

    // Host-rendered images shown over the display (GL pass-through), keyed by
    // guest window; nullptr removes one. See DisplayOverlay.
    void setOverlay(uint32_t id, const DisplayOverlay* overlay);
    [[nodiscard]] uint32_t paletteAt(uint32_t index) const { return cmap_[index & 0x1FFF]; }
    [[nodiscard]] uint16_t vc2Register(unsigned index) const { return vc2Regs_[index & 0x1F]; }

private:
    // ---- REX3 ----------------------------------------------------------------
    [[nodiscard]] uint32_t readRegister(uint32_t reg);
    void writeRegister(uint32_t reg, uint32_t value);
    void runCommand();
    void doSetup();
    void drawBlock(bool screenToScreen);
    void drawLine();
    void readPixels();
    void writePixel(int32_t x, int32_t y, uint32_t source);
    [[nodiscard]] bool clipPass(int32_t x, int32_t y) const;
    [[nodiscard]] uint32_t* pixelAddress(int32_t x, int32_t y);
    [[nodiscard]] uint32_t sourceColor(bool patternBit, bool& draw);
    [[nodiscard]] uint32_t applyLogicOp(uint32_t src, uint32_t dst) const;

    static int32_t fixedToInt(uint32_t value) { return static_cast<int16_t>(value >> 11); }
    // Coordinates keep their 21.11 register image (for exact read-back) and
    // the integer position the rasterizer uses.
    void setXStartFixed(uint32_t v);
    void setXStart(int32_t x) { setXStartFixed(static_cast<uint32_t>(x) << 11); }
    void setOctant(uint32_t octant);

    // ---- Display Control Bus ---------------------------------------------------
    void dcbWrite(uint32_t data);
    [[nodiscard]] uint32_t dcbRead();
    void vc2Write(unsigned crs, uint32_t raw, unsigned width);
    [[nodiscard]] uint32_t vc2Read(unsigned crs);
    void xmapWrite(unsigned which, unsigned crs, uint32_t raw);
    [[nodiscard]] uint32_t xmapRead(unsigned which, unsigned crs) const;
    void cmapWrite(unsigned which, unsigned crs, uint32_t raw);
    [[nodiscard]] uint32_t cmapRead(unsigned crs) const;

    void compose();
    [[nodiscard]] uint8_t cursorPixel(int32_t x, int32_t y, bool large) const;
    // Colour of framebuffer value `v` under XMAP9 mode-table entry `mode`.
    [[nodiscard]] uint32_t displayColor(uint32_t mode, uint32_t v) const;
    // Pixel value as the selected planes (DRAWMODE1) hold it: the main 8 bits,
    // or the 2-bit popup/CID value.
    [[nodiscard]] uint32_t planeValue(int32_t x, int32_t y);

    std::vector<uint32_t> vram_;    // RGB/CI planes, one word per pixel
    // Auxiliary planes per pixel: CID (window ID) in bits 0-1 and 4-5, popup
    // in bits 2-3 and 6-7 (write masks 0x33 / 0xCC as IRIX's X server uses
    // them; values are replicated into both pairs). The XL8 has no overlay
    // planes. Popup pixels show on top of the main planes.
    std::vector<uint8_t> auxPlanes_;
    // Most recently presented last: where windows' images overlap (a program
    // switching between two windows at the same place), the newest shows.
    std::vector<std::pair<uint32_t, DisplayOverlay>> overlays_;
    // The layer-1 (overlay planes) image pixel shown at (x, y) where the
    // popup planes hold `popup`, or nullptr.
    [[nodiscard]] const uint32_t* overlayPixel(int32_t x, int32_t y, uint32_t popup) const;
    std::map<uint32_t, std::pair<int32_t, int32_t>> overlayShift_; // per overlay: alignment with its key pixels
    bool dirty_ = true;

    // REX3 state.
    uint32_t drawMode0_ = 0, drawMode1_ = 0, lsMode_ = 0, lsPattern_ = 0, lsPatSave_ = 0, zPattern_ = 0;
    uint32_t colorBack_ = 0, colorVram_ = 0, colorI_ = 0, colorRed_ = 0, colorGreen_ = 0, colorBlue_ = 0;
    uint32_t colorAlpha_ = 0, alphaRef_ = 0, wrMask_ = 0xFFFFFF;
    uint32_t smask0X_ = 0, smask0Y_ = 0, clipMode_ = 0, config_ = 0, topScan_ = 0;
    std::array<uint32_t, 8> smaskN_{}; // masks 1-4 (x, y pairs)
    int32_t xStart_ = 0, yStart_ = 0, xEnd_ = 0, yEnd_ = 0, xSave_ = 0;
    uint32_t xStartFix_ = 0, yStartFix_ = 0, xEndFix_ = 0, yEndFix_ = 0;
    uint32_t bresD_ = 0, bresS1_ = 0, bresOctInc1_ = 0, bresRndInc2_ = 0, bresE1_ = 0, bresS2_ = 0;
    uint32_t aWeight0_ = 0, aWeight1_ = 0, colorX_ = 0;
    uint32_t slopeRed_ = 0, slopeAlpha_ = 0, slopeGreen_ = 0, slopeBlue_ = 0;
    int32_t xMove_ = 0, yMove_ = 0, xWindow_ = 0x1000, yWindow_ = 0x1000;
    int32_t dx_ = 1, dy_ = 1;
    uint64_t hostData_ = 0;
    uint32_t status_ = 0;

    // DCB state.
    uint32_t dcbMode_ = 0x780;
    uint32_t dcbData0_ = 0, dcbData1_ = 0;

    // DCB devices.
    std::array<uint16_t, 32> vc2Regs_{};
    std::vector<uint16_t> vc2Ram_;
    uint8_t vc2Index_ = 0;
    struct Xmap9 {
        uint8_t config = 0, cursorMsb = 0, popupMsb = 0, modeIndex = 0;
        std::array<uint32_t, 32> modes{};
    };
    std::array<Xmap9, 2> xmap_{};
    std::vector<uint32_t> cmap_;    // 8192 entries, 0x00RRGGBB
    uint16_t cmapIndex_ = 0;
    uint8_t ramdacIndex_ = 0;

    void dcbTimeout();                // access to an absent DCB device
    [[nodiscard]] uint32_t statusBits();

    std::function<void(bool)> retraceIrq_;
    std::function<Cycles()> clock_;
    uint64_t cpuClockHz_ = 133'000'000;
    Cycles backBusyUntil_ = 0;
    WarnOnce warnings_{64};
};

} // namespace indy
