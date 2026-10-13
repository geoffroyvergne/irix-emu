#include "indy/cpu/Tlb.hpp"

namespace indy {

namespace {
constexpr uint64_t kLoGlobal = 1u << 0;
constexpr uint64_t kLoValid  = 1u << 1;
constexpr uint64_t kLoDirty  = 1u << 2;
constexpr unsigned kLoPfnShift = 6;
constexpr uint64_t kLoPfnMask  = 0x00FF'FFFF; // 24-bit PFN -> 36-bit physical
constexpr uint64_t kAsidMask   = 0xFF;
} // namespace

void Tlb::write(unsigned index, uint64_t pageMask, uint64_t entryHi, uint64_t entryLo0, uint64_t entryLo1) {
    Entry& e   = entries_[index % kEntries];
    e.pageMask = pageMask & kPageMaskMask;
    e.entryHi  = entryHi & kEntryHiMask & ~e.pageMask;
    e.global   = (entryLo0 & entryLo1 & kLoGlobal) != 0;
    // Stored G bits mirror the combined flag so TLBR reads back what hardware would.
    const uint64_t g = e.global ? kLoGlobal : 0;
    e.entryLo0 = (entryLo0 & kEntryLoMask & ~kLoGlobal) | g;
    e.entryLo1 = (entryLo1 & kEntryLoMask & ~kLoGlobal) | g;
}

std::optional<unsigned> Tlb::find(VirtAddr vaddr, uint8_t asid) const {
    // Linear search; R5000 keeps recent translations in a micro-TLB in front
    // of this, so it only runs on a miss there.
    for (unsigned i = 0; i < kEntries; ++i) {
        const Entry& e = entries_[i];
        const uint64_t compareMask = kVpn2RegionMask & ~e.pageMask;
        if (((e.entryHi ^ vaddr) & compareMask) != 0) {
            continue;
        }
        if (!e.global && (e.entryHi & kAsidMask) != asid) {
            continue;
        }
        return i;
    }
    return std::nullopt;
}

std::optional<unsigned> Tlb::probe(uint64_t entryHi) const {
    return find(entryHi, static_cast<uint8_t>(entryHi & kAsidMask));
}

Tlb::Result Tlb::translate(VirtAddr vaddr, uint8_t asid, bool isWrite) const {
    const auto index = find(vaddr, asid);
    if (!index) {
        return {Outcome::Miss};
    }
    const Entry& e = entries_[*index];

    // Offset within one page of the pair; the bit above it selects even/odd.
    const uint64_t offsetMask = (e.pageMask >> 1) | 0xFFF;
    const uint64_t lo = (vaddr & (offsetMask + 1)) != 0 ? e.entryLo1 : e.entryLo0;

    if ((lo & kLoValid) == 0) {
        return {Outcome::Invalid};
    }
    if (isWrite && (lo & kLoDirty) == 0) {
        return {Outcome::Modified};
    }
    const uint64_t pageBase = ((lo >> kLoPfnShift) & kLoPfnMask) << 12;
    return {Outcome::Hit, (pageBase & ~offsetMask) | (vaddr & offsetMask), (lo & kLoDirty) != 0};
}

} // namespace indy
