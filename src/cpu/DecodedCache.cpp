#include "indy/cpu/DecodedCache.hpp"

#include <bit>
#include <cstring>

namespace indy {

DecodedCache::Page& DecodedCache::fill(size_t id, const uint8_t* host, DecodedHandler undecoded, bool& full) {
    full = false;
    std::unique_ptr<Page>& slot = pages_[id];
    if (!slot) {
        if (live_ >= kMaxPages) { // a bounded working set: start again
            clear();
            full = true;
        }
        slot = std::make_unique<Page>();
        ++live_;
    }
    Page& page = *slot;
    for (size_t i = 0; i < kOpsPerPage; ++i) {
        uint32_t raw;
        std::memcpy(&raw, host + 4 * i, sizeof raw);
        DecodedOp& op = page.ops[i];
        op.handler = undecoded;
        op.raw = std::endian::native == std::endian::little ? std::byteswap(raw) : raw;
    }
    page.stale = false;
    return page;
}

} // namespace indy
