#include "indy/memory/MemoryBus.hpp"

#include "indy/memory/PhysicalMap.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace indy {

namespace {
constexpr uint64_t roundUpToPage(uint64_t bytes) noexcept {
    return (bytes + MemoryBus::kPageMask) & ~MemoryBus::kPageMask;
}
} // namespace

MemoryBus::MemoryBus() : pages_(kPageCount) {}

// ---------------------------------------------------------------------------
// Memory ownership
// ---------------------------------------------------------------------------

void MemoryBus::allocateRam(uint64_t sizeBytes) {
    if (ram_) {
        throw std::logic_error("MemoryBus: RAM is already installed");
    }
    if (sizeBytes == 0 || (sizeBytes & kPageMask) != 0 || sizeBytes > physmap::kRamMaxSize) {
        throw std::invalid_argument(std::format(
            "MemoryBus: invalid RAM size {} bytes (must be a multiple of {} KiB, at most {} MiB)",
            sizeBytes, kPageSize / KiB, physmap::kRamMaxSize / MiB));
    }
    ram_.emplace(sizeBytes);
}

void MemoryBus::installRam(PhysAddr base, uint64_t sizeBytes) {
    allocateRam(sizeBytes);
    const RamWindow flat{base, sizeBytes, 0, sizeBytes};
    if ((sizeBytes & (sizeBytes - 1)) == 0) {
        setRamWindows({&flat, 1});
    } else {
        // Non power-of-two sizes cannot be expressed as a mirrored window.
        mapMemory(base, ram_->bytes(), /*writable=*/true);
        const size_t first = static_cast<size_t>(base >> kPageShift);
        for (size_t i = 0; i < (sizeBytes >> kPageShift); ++i) {
            pages_[first + i].ram = true;
        }
    }
}

void MemoryBus::setRamWindows(std::span<const RamWindow> windows) {
    if (!ram_) {
        throw std::logic_error("MemoryBus::setRamWindows: no RAM allocated");
    }
    for (Page& page : pages_) {
        if (page.ram) {
            page = Page{};
        }
    }

    const std::span<uint8_t> storage = ram_->bytes();
    for (const RamWindow& w : windows) {
        const bool periodOk = w.floating || (w.period >= kPageSize && (w.period & (w.period - 1)) == 0);
        if ((w.base & kPageMask) != 0 || (w.size & kPageMask) != 0 || !periodOk ||
            (!w.floating && w.offset + w.period > storage.size()) || w.base + w.size > kAddressSpace) {
            throw std::invalid_argument(std::format(
                "MemoryBus::setRamWindows: bad window base={:#x} size={:#x} offset={:#x} period={:#x}",
                w.base, w.size, w.offset, w.period));
        }
        const size_t first = static_cast<size_t>(w.base >> kPageShift);
        for (uint64_t off = 0; off < w.size; off += kPageSize) {
            Page& page = pages_[first + static_cast<size_t>(off >> kPageShift)];
            if ((page.host != nullptr && !page.ram) || page.device != nullptr) {
                continue; // PROM or device: never shadowed by RAM
            }
            page.ram = true;
            if (w.floating) {
                page.host     = nullptr;
                page.writable = false;
                page.floating = true;
                continue;
            }
            page.host     = storage.data() + w.offset + (off & (w.period - 1));
            page.writable = true;
            page.floating = false;
        }
    }
}

std::expected<PromInfo, std::string> MemoryBus::loadProm(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return std::unexpected(std::format("PROM image '{}' not found", path.string()));
    }
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::unexpected(std::format("cannot stat PROM image '{}': {}", path.string(), ec.message()));
    }
    if (size == 0) {
        return std::unexpected(std::format("PROM image '{}' is empty", path.string()));
    }
    if (size > physmap::kPromMaxSize) {
        return std::unexpected(std::format("PROM image '{}' is too large ({} bytes, maximum {})",
                                           path.string(), size, physmap::kPromMaxSize));
    }
    if (size % sizeof(uint32_t) != 0) {
        return std::unexpected(std::format(
            "PROM image '{}' is {} bytes, not a whole number of 32-bit words (truncated dump?)",
            path.string(), size));
    }

    // Read into a fresh buffer first so a failed load leaves the old PROM intact.
    std::vector<uint8_t> image(static_cast<size_t>(roundUpToPage(size)), 0xFF);
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(image.data()), static_cast<std::streamsize>(size))) {
        return std::unexpected(std::format("failed to read PROM image '{}'", path.string()));
    }

    // Store in the Indy's big-endian layout, fixing up byte-swapped dumps.
    const auto order = detectPromByteOrder(std::span<const uint8_t>(image.data(), static_cast<size_t>(size)));
    if (order) {
        normalizePromByteOrder(std::span<uint8_t>(image.data(), static_cast<size_t>(size)), *order);
    }

    if (!prom_.empty()) {
        unmap(physmap::kPromBase, prom_.size());
    }
    prom_     = std::move(image);
    promSize_ = static_cast<uint64_t>(size);
    mapMemory(physmap::kPromBase, prom_, /*writable=*/false);
    return PromInfo{promSize_, order.value_or(PromByteOrder::BigEndian), order.has_value()};
}

// ---------------------------------------------------------------------------
// Page table management
// ---------------------------------------------------------------------------

void MemoryBus::requireFree(size_t firstPage, size_t lastPage) const {
    if (lastPage >= kPageCount) {
        throw std::out_of_range("MemoryBus: mapping exceeds decoded address space");
    }
    for (size_t i = firstPage; i <= lastPage; ++i) {
        if (pages_[i].host != nullptr || pages_[i].device != nullptr) {
            throw std::logic_error(std::format(
                "MemoryBus: page at {:#010x} is already mapped", uint64_t{i} << kPageShift));
        }
    }
}

void MemoryBus::mapMemory(PhysAddr base, std::span<uint8_t> backing, bool writable) {
    if ((base & kPageMask) != 0 || (backing.size() & kPageMask) != 0 || backing.empty()) {
        throw std::invalid_argument("MemoryBus::mapMemory: base and size must be non-zero multiples of the page size");
    }
    const size_t first = static_cast<size_t>(base >> kPageShift);
    const size_t count = backing.size() >> kPageShift;
    requireFree(first, first + count - 1);

    for (size_t i = 0; i < count; ++i) {
        Page& page    = pages_[first + i];
        page.host     = backing.data() + (i << kPageShift);
        page.writable = writable;
    }
}

void MemoryBus::mapDevice(PhysAddr base, uint64_t size, MmioDevice& device) {
    if (size == 0) {
        throw std::invalid_argument("MemoryBus::mapDevice: size must be non-zero");
    }
    const PhysAddr end   = base + size;
    const size_t   first = static_cast<size_t>(base >> kPageShift);
    const size_t   last  = static_cast<size_t>((end - 1) >> kPageShift);
    requireFree(first, last);

    for (size_t i = first; i <= last; ++i) {
        Page& page      = pages_[i];
        page.device     = &device;
        page.deviceBase = base;
        page.deviceEnd  = end;
    }
}

void MemoryBus::unmap(PhysAddr base, uint64_t size) {
    const size_t first = static_cast<size_t>(base >> kPageShift);
    const size_t count = static_cast<size_t>(roundUpToPage(size) >> kPageShift);
    std::fill_n(pages_.begin() + static_cast<std::ptrdiff_t>(first), count, Page{});
}

} // namespace indy
