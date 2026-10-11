#pragma once

#include "indy/core/Types.hpp"

#include <array>
#include <optional>

namespace indy {

// R4600 joint TLB: 48 fully-associative entries, each mapping an even/odd pair
// of virtual pages to two physical pages. Register layouts follow the MIPS
// R4000-family (64-bit) definitions:
//
//   EntryHi : R[63:62] | VPN2[39:13] | ASID[7:0]
//   EntryLo : PFN[29:6] | C[5:3] | D[2] | V[1] | G[0]
//   PageMask: MASK[24:13]   (0 = 4 KiB pages ... 0xFFF = 16 MiB pages)
class Tlb {
public:
    static constexpr unsigned kEntries = 48;

    // Writable bits of the COP0 registers that feed the TLB.
    static constexpr uint64_t kEntryHiMask  = 0xC000'00FF'FFFF'E0FF;
    static constexpr uint64_t kEntryLoMask  = 0x3FFF'FFFF;
    static constexpr uint64_t kPageMaskMask = 0x01FF'E000;

    struct Entry {
        uint64_t pageMask = 0;
        uint64_t entryHi  = 0; // VPN2 bits under pageMask are cleared
        uint64_t entryLo0 = 0;
        uint64_t entryLo1 = 0;
        bool     global   = false;
    };

    enum class Outcome {
        Hit,
        Miss,     // no matching entry      -> TLB refill exception
        Invalid,  // matching entry, V = 0  -> TLB invalid exception
        Modified, // store to page with D=0 -> TLB modified exception
    };

    struct Result {
        Outcome  outcome;
        PhysAddr phys = 0;
    };

    void reset() { entries_.fill(Entry{}); }

    // TLBWI/TLBWR: G is the AND of both EntryLo G bits, as on hardware.
    void write(unsigned index, uint64_t pageMask, uint64_t entryHi, uint64_t entryLo0, uint64_t entryLo1);
    [[nodiscard]] const Entry& entry(unsigned index) const { return entries_[index % kEntries]; }

    // TLBP: index of the entry matching EntryHi (VPN2 + ASID), if any.
    [[nodiscard]] std::optional<unsigned> probe(uint64_t entryHi) const;

    // Translate a mapped virtual address for the current ASID.
    [[nodiscard]] Result translate(VirtAddr vaddr, uint8_t asid, bool isWrite) const;

private:
    // Region + VPN2 bits compared on lookup.
    static constexpr uint64_t kVpn2RegionMask = 0xC000'00FF'FFFF'E000;

    [[nodiscard]] std::optional<unsigned> find(VirtAddr vaddr, uint8_t asid) const;

    std::array<Entry, kEntries> entries_{};
};

} // namespace indy
