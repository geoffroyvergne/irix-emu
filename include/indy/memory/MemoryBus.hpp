#pragma once

#include "indy/core/MmioDevice.hpp"
#include "indy/core/Types.hpp"
#include "indy/memory/PromImage.hpp"
#include "indy/memory/Ram.hpp"

#include <bit>
#include <cassert>
#include <concepts>
#include <cstring>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace indy {

struct PromInfo {
    uint64_t size;            // bytes loaded
    PromByteOrder sourceOrder; // layout of the file before normalisation
    bool orderDetected;       // false: no layout matched, loaded verbatim
};

template <typename T>
concept BusWord = std::same_as<T, uint8_t> || std::same_as<T, uint16_t> ||
                  std::same_as<T, uint32_t> || std::same_as<T, uint64_t>;

// The Indy's physical memory system: owns main RAM and the boot PROM, and
// decodes every physical access to host memory or an MmioDevice.
//
// Decoding: the low 4 GiB is split into 64 KiB pages; each page points either
// straight at host memory (RAM/PROM: one branch + memcpy) or at a device.
// Lookup is O(1) with no searching on the hot path.
//
// Byte order: guest memory is stored big-endian, exactly as the Indy sees it
// (so a PROM dump is copied in verbatim). The accessors convert to/from host
// order, which costs a single bswap instruction on little-endian hosts and
// nothing on big-endian ones.
//
// Alignment: callers (the CPU) must raise address errors for misaligned
// accesses before reaching the bus, so an access never straddles a page.
class MemoryBus {
public:
    static constexpr unsigned kPageShift    = 16;
    static constexpr uint64_t kPageSize     = uint64_t{1} << kPageShift;
    static constexpr uint64_t kPageMask     = kPageSize - 1;
    static constexpr uint64_t kAddressSpace = uint64_t{1} << 32;
    static constexpr size_t   kPageCount    = kAddressSpace >> kPageShift;

    MemoryBus();
    MemoryBus(const MemoryBus&) = delete;
    MemoryBus& operator=(const MemoryBus&) = delete;

    // ---- Memory ownership ------------------------------------------------
    // Allocate zeroed main RAM storage without making it visible. Size must be
    // a non-zero multiple of the page size and at most physmap::kRamMaxSize.
    // Once only. On the Indy the MC then decides where RAM appears.
    void allocateRam(uint64_t sizeBytes);

    // Convenience for headless tools/tests: allocate and map flat at `base`.
    void installRam(PhysAddr base, uint64_t sizeBytes);

    // A view of RAM storage in the physical address space. The window repeats
    // the storage range [offset, offset+period) every `period` bytes, which is
    // how a SIMM bank smaller than its configured window appears (mirrored).
    struct RamWindow {
        PhysAddr base;
        uint64_t size;
        uint64_t offset;       // into RAM storage
        uint64_t period;       // power of two, >= page size
        bool floating = false; // decoded but unpopulated: reads 0, writes vanish, no bus error
    };

    // Replace every RAM mapping with `windows` (later windows win on overlap).
    // Pages owned by the PROM or a device are never overridden.
    void setRamWindows(std::span<const RamWindow> windows);

    // Load a raw PROM dump and map it read-only at physmap::kPromBase,
    // replacing any previous image. Byte-swapped dumps are detected from the
    // reset-vector instruction and normalised to big-endian.
    // Errors: file missing/unreadable, empty, larger than physmap::kPromMaxSize,
    // or not a whole number of 32-bit words.
    [[nodiscard]] std::expected<PromInfo, std::string> loadProm(const std::filesystem::path& path);

    [[nodiscard]] std::span<uint8_t> ram() noexcept { return ram_ ? ram_->bytes() : std::span<uint8_t>{}; }
    [[nodiscard]] std::span<const uint8_t> prom() const noexcept { return {prom_.data(), promSize_}; }

    // ---- Device mapping ----------------------------------------------------
    // Accesses in [base, base+size) are forwarded with offset = addr - base.
    // A page can host only one device; sub-page decoding is the device's job.
    void mapDevice(PhysAddr base, uint64_t size, MmioDevice& device);

    // ---- Accessors ---------------------------------------------------------
    [[nodiscard]] std::optional<uint32_t> read32(PhysAddr addr) {
        uint32_t value;
        return read(addr, value) ? std::optional<uint32_t>{value} : std::nullopt;
    }
    // Returns false on bus error (unmapped address or write to PROM).
    [[nodiscard]] bool write32(PhysAddr addr, uint32_t value) { return write(addr, value); }

    // Width-generic forms used by the CPU for LB/LH/LW/LD and stores.
    // Return false on bus error.
    template <BusWord T>
    [[nodiscard]] bool read(PhysAddr addr, T& out) {
        assert(addr % sizeof(T) == 0 && "CPU must raise address error for misaligned access");
        if (addr >= kAddressSpace) [[unlikely]] {
            return false;
        }
        const Page& page = pages_[addr >> kPageShift];
        if (page.host != nullptr) [[likely]] {
            T raw;
            std::memcpy(&raw, page.host + (addr & kPageMask), sizeof(T));
            out = fromGuest(raw);
            return true;
        }
        if (page.floating) {
            out = 0;
            return true;
        }
        if (page.device != nullptr && addr >= page.deviceBase && addr < page.deviceEnd) {
            uint64_t value = 0;
            if (!page.device->read(static_cast<uint32_t>(addr - page.deviceBase), sizeOf<T>(), value)) {
                return false;
            }
            out = static_cast<T>(value);
            return true;
        }
        return false;
    }

    template <BusWord T>
    [[nodiscard]] bool write(PhysAddr addr, T value) {
        assert(addr % sizeof(T) == 0 && "CPU must raise address error for misaligned access");
        if (addr >= kAddressSpace) [[unlikely]] {
            return false;
        }
        const Page& page = pages_[addr >> kPageShift];
        if (page.host != nullptr) [[likely]] {
            if (!page.writable) {
                return false;
            }
            const T raw = toGuest(value);
            std::memcpy(page.host + (addr & kPageMask), &raw, sizeof(T));
            return true;
        }
        if (page.floating) {
            return true;
        }
        if (page.device != nullptr && addr >= page.deviceBase && addr < page.deviceEnd) {
            return page.device->write(static_cast<uint32_t>(addr - page.deviceBase), sizeOf<T>(), value);
        }
        return false;
    }

private:
    struct Page {
        uint8_t*    host       = nullptr; // start of this page in host memory
        MmioDevice* device     = nullptr;
        PhysAddr    deviceBase = 0;
        PhysAddr    deviceEnd  = 0;       // exclusive
        bool        writable   = false;
        bool        ram        = false;   // page belongs to a RAM window (populated or floating)
        bool        floating   = false;   // decoded RAM address with no SIMM behind it
    };

    template <BusWord T>
    static constexpr AccessSize sizeOf() noexcept {
        return static_cast<AccessSize>(sizeof(T));
    }

    // The Indy is big-endian; byte-swap only on little-endian hosts.
    template <BusWord T>
    static constexpr T fromGuest(T value) noexcept {
        if constexpr (std::endian::native == std::endian::little) {
            return std::byteswap(value);
        } else {
            return value;
        }
    }
    template <BusWord T>
    static constexpr T toGuest(T value) noexcept { return fromGuest(value); }

    void mapMemory(PhysAddr base, std::span<uint8_t> backing, bool writable);
    void unmap(PhysAddr base, uint64_t size);
    void requireFree(size_t firstPage, size_t lastPage) const;

    std::vector<Page> pages_;
    std::optional<Ram> ram_;
    std::vector<uint8_t> prom_; // page-padded with 0xFF (erased EPROM)
    uint64_t promSize_ = 0;     // bytes actually loaded
};

} // namespace indy
