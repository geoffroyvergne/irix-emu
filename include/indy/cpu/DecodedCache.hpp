#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace indy {

class R5000;
struct DecodedOp;

// A decoded instruction's semantics: called with the CPU and the slot (which
// a lazy decode rewrites in place).
using DecodedHandler = void (*)(R5000&, DecodedOp&);

// One guest instruction, decoded once: the handler for its operation and the
// register fields it reads, so executing it needs no decode switch. 16 bytes,
// four to a host cache line; the immediate is the low half of `raw`.
struct DecodedOp {
    DecodedHandler handler = nullptr;
    uint32_t raw = 0; // the instruction word (host order): fallback semantics, history, immediates
    uint8_t rs = 0, rt = 0, rd = 0, sa = 0;
};
static_assert(sizeof(DecodedOp) == 16);

// Decoded instructions by guest page. A page holds one slot per word of a
// 4 KiB page of host memory (RAM or PROM), indexed by (PC & 0xFFF) >> 2, so
// any PC is an entry point: exception returns, branch targets and delay
// slots need no block lookup. Slots are decoded lazily: a new page holds
// every word with the `undecoded` handler, which decodes its slot on first
// execution.
//
// Pages are identified by their storage page (see R5000::storagePage), not by
// virtual or physical address: TLB changes never invalidate them, and RAM
// mirrors share one page. A write to the memory behind a page marks it stale
// (the bus reports writes to code pages); it is filled again from memory the
// next time the CPU fetches from it.
class DecodedCache {
public:
    static constexpr size_t kOpsPerPage = 1024;
    // Decoded pages kept before all are dropped (16 KiB each: 128 MiB).
    static constexpr size_t kMaxPages = 8192;

    struct Page {
        std::array<DecodedOp, kOpsPerPage> ops;
        bool stale = true;
    };

    // `pages`: how many storage pages can be decoded (RAM and PROM pages).
    void resize(size_t pages) {
        pages_.clear();
        pages_.resize(pages);
        live_ = 0;
    }
    [[nodiscard]] size_t capacity() const noexcept { return pages_.size(); }
    [[nodiscard]] size_t livePages() const noexcept { return live_; }

    // The page for storage page `id`, filled from `host` (big-endian guest
    // words) when it is new or stale. `full` is set when the cache had to be
    // emptied first (kMaxPages): every other Page pointer is then invalid.
    Page& fill(size_t id, const uint8_t* host, DecodedHandler undecoded, bool& full);

    [[nodiscard]] Page* find(size_t id) noexcept { return id < pages_.size() ? pages_[id].get() : nullptr; }

    // Storage page `id` was written: its decoded instructions are out of date.
    void invalidate(size_t id) noexcept {
        if (Page* page = find(id)) page->stale = true;
    }

    // Every page is out of date (memory remapped); none is freed.
    void invalidateAll() noexcept {
        for (auto& page : pages_) {
            if (page) page->stale = true;
        }
    }

    void clear() noexcept {
        for (auto& page : pages_) page.reset();
        live_ = 0;
    }

private:
    std::vector<std::unique_ptr<Page>> pages_;
    size_t live_ = 0;
};

} // namespace indy
