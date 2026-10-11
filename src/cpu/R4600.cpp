// R4600: reset, tick loop, MMU, exceptions, COP0 and diagnostics.
// Instruction semantics live in R4600Execute.cpp.

#include "indy/cpu/R4600.hpp"

#include "indy/core/Log.hpp"
#include "indy/cpu/AddressSpace.hpp"

#include "indy/memory/PhysicalMap.hpp"

#include <algorithm>
#include <cstdlib>
#include <cassert>
#include <format>

namespace indy {

namespace {

// ---- Status ----------------------------------------------------------------
constexpr uint64_t kStatusIE  = uint64_t{1} << 0;  // interrupt enable
constexpr uint64_t kStatusEXL = uint64_t{1} << 1;  // exception level
constexpr uint64_t kStatusERL = uint64_t{1} << 2;  // error level
constexpr unsigned kStatusKsuShift = 3;            // 0 kernel, 1 supervisor, 2 user
constexpr uint64_t kStatusUX  = uint64_t{1} << 5;  // 64-bit user addressing
constexpr uint64_t kStatusSX  = uint64_t{1} << 6;  // 64-bit supervisor addressing
constexpr uint64_t kStatusKX  = uint64_t{1} << 7;  // 64-bit kernel addressing
constexpr uint64_t kStatusBEV = uint64_t{1} << 22; // bootstrap exception vectors
constexpr unsigned kStatusCuShift = 28;            // CU0..CU3

// ---- Cause -------------------------------------------------------------------
// IP0..IP7 at bits 8..15 (IP2..IP7 = Int[5:0], IP7 doubles as the timer).
constexpr uint64_t kInterruptMask     = 0xFF00;    // IP bits in Cause, IM bits in Status
constexpr unsigned kCauseHwIpShift    = 10;
constexpr uint64_t kCauseTimerIp      = uint64_t{1} << 15;
constexpr uint64_t kCauseWritableMask = uint64_t{3} << 8; // IP0/IP1 (software interrupts)
constexpr uint64_t kCauseExcCodeMask  = uint64_t{0x1F} << 2;
constexpr uint64_t kCauseCeMask       = uint64_t{3} << 28;
constexpr uint64_t kCauseBD           = uint64_t{1} << 31;

// ---- Exception vectors -------------------------------------------------------
constexpr VirtAddr kVectorBaseNormal = 0xFFFF'FFFF'8000'0000; // BEV = 0 (KSEG0)
constexpr VirtAddr kVectorBaseBoot   = 0xFFFF'FFFF'BFC0'0200; // BEV = 1 (PROM)
constexpr uint64_t kVectorTlbRefill  = 0x000;
constexpr uint64_t kVectorGeneral    = 0x180;

// ---- Identification / configuration -------------------------------------------
// PRId: implementation 0x20 = R4600, revision 2.0.
constexpr uint64_t kR4600PRId = 0x2020;
// Config (R4000-family layout), fixed by hardware except K0.
constexpr uint64_t kConfigBE = uint64_t{1} << 15;  // big-endian
constexpr uint64_t kConfigSC = uint64_t{1} << 17;  // 1 = no secondary cache (R4600PC)
constexpr uint64_t kConfigIC = uint64_t{2} << 9;   // I-cache 2^(12+2) = 16 KiB
constexpr uint64_t kConfigDC = uint64_t{2} << 6;   // D-cache 16 KiB
constexpr uint64_t kConfigIB = uint64_t{1} << 5;   // I-cache line 32 bytes
constexpr uint64_t kConfigDB = uint64_t{1} << 4;   // D-cache line 32 bytes
constexpr uint64_t kConfigReset = kConfigBE | kConfigSC | kConfigIC | kConfigDC | kConfigIB | kConfigDB;
constexpr uint64_t kConfigWritableMask = 0x7;      // K0: KSEG0 cache coherency

// ---- TLB-related register fields ----------------------------------------------
constexpr uint64_t kIndexMask       = 0x3F;
constexpr uint64_t kIndexProbeFail  = uint64_t{1} << 31;
constexpr uint64_t kContextBadVpn2  = 0x7F'FFFF;        // bits 22:0 (BadVPN2 at 22:4)
constexpr uint64_t kXContextLow     = 0x1'FFFF'FFFF;    // bits 32:0 (R + BadVPN2)
constexpr uint64_t kEntryHiVpn2     = 0xC000'00FF'FFFF'E000;
constexpr uint64_t kEntryHiAsid     = 0xFF;

constexpr std::string_view excName(R4600::ExcCode code) {
    using E = R4600::ExcCode;
    switch (code) {
    case E::Int:  return "Interrupt";
    case E::Mod:  return "TLB modified";
    case E::TLBL: return "TLB miss/invalid (load/fetch)";
    case E::TLBS: return "TLB miss/invalid (store)";
    case E::AdEL: return "Address error (load/fetch)";
    case E::AdES: return "Address error (store)";
    case E::IBE:  return "Instruction bus error";
    case E::DBE:  return "Data bus error";
    case E::Sys:  return "Syscall";
    case E::Bp:   return "Breakpoint";
    case E::RI:   return "Reserved instruction";
    case E::CpU:  return "Coprocessor unusable";
    case E::Ov:   return "Integer overflow";
    case E::Tr:   return "Trap";
    case E::FPE:  return "Floating point";
    }
    return "Unknown";
}

} // namespace

// ---------------------------------------------------------------------------
// Reset & run loop
// ---------------------------------------------------------------------------

void R4600::reset() {
    gpr_.fill(0);
    hi_ = 0;
    lo_ = 0;

    // TLB contents are undefined after reset; the PROM initialises them.
    tlb_.reset();
    fpr_.fill(0);
    fcr31_ = 0;
    cop0_.fill(0);
    cop0_[kStatus] = kStatusBEV | kStatusERL;
    cop0_[kRandom] = Tlb::kEntries - 1;
    cop0_[kPRId]   = kR4600PRId;
    cop0_[kConfig] = kConfigReset;
    countPhase_ = false;

    // PC holds the *virtual* reset vector 0xBFC00000 (sign-extended). KSEG1
    // maps it to physical 0x1FC00000 where the PROM lives.
    pc_            = mips::kResetVector;
    nextPc_        = pc_ + 4;
    currentPc_     = pc_;
    branchPending_ = false;
    inDelaySlot_   = false;
    llBit_         = false;

    halted_         = false;
    executed_       = 0;
    history_.fill({});
    historyNext_    = 0;
    traceRemaining_ = traceLimit_;
    unimplementedWarnings_.clear();
    unmappedWarnings_.clear();
    exceptionLog_.clear();
    traceNull_ = std::getenv("INDY_TRACE_NULL") != nullptr;
    traceExceptions_ = std::getenv("INDY_TRACE_EXCEPTIONS") != nullptr;
    tracePc_.reset();
    if (const char* env = std::getenv("INDY_TRACE_PC"); env != nullptr && *env != '\0') {
        tracePc_ = static_cast<VirtAddr>(static_cast<int64_t>(static_cast<int32_t>(std::strtoul(env, nullptr, 16))));
    }
}

void R4600::logTracePc() {
    // Arguments a0..a3, each shown as a string when it points into KSEG0/KSEG1
    // (read straight from physical memory, so no TLB state is disturbed).
    auto text = [this](uint64_t addr) -> std::string {
        const auto a = static_cast<uint32_t>(addr);
        if ((a & 0xC000'0000u) != 0x8000'0000u) return {};
        std::string out;
        for (uint32_t i = 0; i < 64; ++i) {
            const PhysAddr phys = (a + i) & 0x1FFF'FFFFu;
            const auto w = bus_.read32(phys & ~PhysAddr{3});
            if (!w) break;
            const auto c = static_cast<char>(*w >> (8 * (3 - (phys & 3))));
            if (c == '\0') break;
            out += (c >= 0x20 && c < 0x7F) ? c : '.';
        }
        return out;
    };
    log::tagged("CPU TRACE PC", "{} ra={:#010x} a0={:#010x} \"{}\" a1={:#010x} \"{}\" a2={:#010x} a3={:#010x}",
                mips::formatAddress(currentPc_), static_cast<uint32_t>(gpr_[31]),
                static_cast<uint32_t>(gpr_[4]), text(gpr_[4]), static_cast<uint32_t>(gpr_[5]), text(gpr_[5]),
                static_cast<uint32_t>(gpr_[6]), static_cast<uint32_t>(gpr_[7]));
}

Cycles R4600::run(Cycles budget) {
    // Placeholder timing: one instruction per cycle. Time keeps passing while
    // halted so the scheduler and devices still run.
    for (runElapsed_ = 0; runElapsed_ < budget && !halted_; ++runElapsed_) {
        tick();
    }
    runElapsed_ = 0;
    return budget;
}

bool R4600::tick() {
    if (halted_) {
        return false;
    }

    currentPc_     = pc_;
    inDelaySlot_   = branchPending_;
    branchPending_ = false;

    // Interrupts are taken between instructions.
    if (interruptPending()) [[unlikely]] {
        raiseException(ExcCode::Int);
        advanceTimers();
        return true;
    }

    // ---- Fetch -------------------------------------------------------------
    const VirtAddr pc = pc_;
    if ((pc & 3) != 0) [[unlikely]] {
        raiseAddressError(pc, Access::Fetch);
        advanceTimers();
        return true;
    }
    const auto phys = translate(pc, Access::Fetch);
    if (!phys) [[unlikely]] {
        advanceTimers();
        return !halted_;
    }
    const auto word = bus_.read32(*phys);
    if (!word) [[unlikely]] {
        if ((cop0_[kStatus] & kStatusEXL) != 0) {
            // The exception handler itself cannot be fetched: nothing can recover.
            const bool noProm = bus_.prom().empty() && *phys >= physmap::kPromBase &&
                                *phys < physmap::kPromBase + physmap::kPromMaxSize;
            halt(std::format("instruction bus error at PC {} (physical {:#010x}) inside an exception handler{}",
                             mips::formatAddress(pc), *phys,
                             noProm ? " - no PROM image is loaded, so nothing is mapped at 0x1FC00000" : ""));
            return false;
        }
        log::tagged("CPU WARNING", "Instruction fetch from unmapped physical {:#010x} (PC {})",
                    *phys, mips::formatAddress(pc));
        raiseException(ExcCode::IBE);
        advanceTimers();
        return true;
    }

    if (traceRemaining_ != 0) [[unlikely]] {
        --traceRemaining_;
        log::tagged("CPU TRACE", "PC: {} -> Opcode: {:#010x}", mips::formatAddress(pc), *word);
    }
    if (tracePc_ && pc == *tracePc_) [[unlikely]] {
        logTracePc();
    }
    history_[historyNext_] = {pc, *word};
    historyNext_ = (historyNext_ + 1) % kHistorySize;

    // ---- Advance, then execute ---------------------------------------------
    // Advancing first means pc_ already points at the delay slot when a branch
    // executes, so branches only rewrite nextPc_; exceptions overwrite both.
    pc_      = nextPc_;
    nextPc_ += 4;

    execute(mips::Instruction{*word});
    ++executed_;
    advanceTimers();

    if (halted_) [[unlikely]] {
        pc_     = currentPc_; // leave the PC on the culprit for inspection
        nextPc_ = pc_ + 4;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// MMU: 32-bit compatibility address space
//
//   0x00000000-0x7FFFFFFF  kuseg   mapped (unmapped & uncached while ERL=1)
//   0x80000000-0x9FFFFFFF  kseg0   unmapped, cached     kernel only
//   0xA0000000-0xBFFFFFFF  kseg1   unmapped, uncached   kernel only
//   0xC0000000-0xDFFFFFFF  ksseg   mapped               kernel/supervisor
//   0xE0000000-0xFFFFFFFF  kseg3   mapped               kernel only
// (all sign-extended to 64 bits)
// ---------------------------------------------------------------------------

bool R4600::kernelMode() const noexcept {
    const uint64_t status = cop0_[kStatus];
    return (status & (kStatusEXL | kStatusERL)) != 0 || ((status >> kStatusKsuShift) & 3) == 0;
}

std::optional<PhysAddr> R4600::translate(VirtAddr vaddr, Access access) {
    // Fast path: KSEG0/KSEG1, where the PROM and most of the kernel run.
    if (vaddr >= mips::kKseg0Base && vaddr < mips::kKseg2Base) [[likely]] {
        if (kernelMode()) [[likely]] {
            return PhysAddr{vaddr & mips::kUnmappedSegmentMask};
        }
        raiseAddressError(vaddr, access);
        return std::nullopt;
    }

    const uint64_t status = cop0_[kStatus];
    if (!mips::isCompat32(vaddr)) {
        if ((status & (kStatusKX | kStatusSX | kStatusUX)) != 0) {
            halt(std::format("64-bit address {} with KX/SX/UX set: 64-bit segments not implemented",
                             mips::formatAddress(vaddr)));
        } else {
            raiseAddressError(vaddr, access); // outside the 32-bit space
        }
        return std::nullopt;
    }

    const auto addr32 = static_cast<uint32_t>(vaddr);
    const bool kernel = kernelMode();
    const bool supervisor = !kernel && ((status >> kStatusKsuShift) & 3) == 1;

    if (addr32 < 0x8000'0000) {
        if ((status & kStatusERL) != 0) {
            return PhysAddr{addr32}; // kuseg is an unmapped window while ERL is set
        }
    } else if (addr32 < 0xE000'0000) { // ksseg (KSEG0/1 were handled above)
        if (!kernel && !supervisor) {
            raiseAddressError(vaddr, access);
            return std::nullopt;
        }
    } else if (!kernel) { // kseg3
        raiseAddressError(vaddr, access);
        return std::nullopt;
    }

    const auto asid = static_cast<uint8_t>(cop0_[kEntryHi] & kEntryHiAsid);
    const Tlb::Result result = tlb_.translate(vaddr, asid, access == Access::Store);
    if (result.outcome != Tlb::Outcome::Hit) {
        raiseTlbException(vaddr, access, result.outcome);
        return std::nullopt;
    }
    return result.phys;
}

// ---------------------------------------------------------------------------
// Exceptions
// ---------------------------------------------------------------------------

void R4600::raiseException(ExcCode code, unsigned coprocessor, bool tlbRefill) {
    uint64_t& status = cop0_[kStatus];
    uint64_t& cause  = cop0_[kCause];

    // EPC/BD are only recorded for a first-level exception; with EXL already
    // set (nested), the original restart point is preserved and the refill
    // vector is not used.
    uint64_t vectorOffset = kVectorGeneral;
    if ((status & kStatusEXL) == 0) {
        cop0_[kEPC] = inDelaySlot_ ? currentPc_ - 4 : currentPc_;
        cause = inDelaySlot_ ? (cause | kCauseBD) : (cause & ~kCauseBD);
        if (tlbRefill) {
            vectorOffset = kVectorTlbRefill;
        }
        status |= kStatusEXL;
    }
    cause = (cause & ~(kCauseExcCodeMask | kCauseCeMask)) |
            (uint64_t{static_cast<uint8_t>(code)} << 2) | (uint64_t{coprocessor & 3} << 28);

    const VirtAddr base = (status & kStatusBEV) != 0 ? kVectorBaseBoot : kVectorBaseNormal;
    pc_            = base + vectorOffset;
    nextPc_        = pc_ + 4;
    branchPending_ = false;

    // Debug aid: the first NULL-pointer dereference shows how we got there.
    const bool addressFault = code == ExcCode::TLBL || code == ExcCode::TLBS || code == ExcCode::AdEL ||
                              code == ExcCode::AdES;
    if (traceNull_ && addressFault && cop0_[kBadVAddr] < 0x1000) {
        traceNull_ = false;
        log::tagged("CPU NULL", "access to NULL page {} at PC {} after {} instructions",
                    mips::formatAddress(cop0_[kBadVAddr]), mips::formatAddress(currentPc_), executed_);
        logRegisters();
        logHistory();
    }

    // Log the first occurrences of each (exception, PC) pair. Exceptions an OS
    // takes all the time (TLB refills, copy-on-write, syscalls, breakpoints,
    // lazy FPU switching) only with INDY_TRACE_EXCEPTIONS; a TLB fault on the
    // NULL page is always shown, as it is almost always a bug.
    const bool routine = code == ExcCode::Int || code == ExcCode::Sys || code == ExcCode::Bp ||
                         code == ExcCode::CpU || code == ExcCode::Mod ||
                         ((code == ExcCode::TLBL || code == ExcCode::TLBS) && cop0_[kBadVAddr] >= 0x1000);
    if ((traceExceptions_ ? code != ExcCode::Int : !routine) &&
        exceptionLog_.first(uint64_t{static_cast<uint8_t>(code)} << 56 ^ currentPc_)) {
        log::tagged("CPU EXCEPTION", "{} at PC {} (BadVAddr {}) -> vector {}",
                    excName(code), mips::formatAddress(currentPc_),
                    mips::formatAddress(cop0_[kBadVAddr]), mips::formatAddress(pc_));
    }
}

void R4600::raiseAddressError(VirtAddr vaddr, Access access) {
    cop0_[kBadVAddr] = vaddr;
    raiseException(access == Access::Store ? ExcCode::AdES : ExcCode::AdEL);
}

void R4600::raiseTlbException(VirtAddr vaddr, Access access, Tlb::Outcome outcome) {
    // Pre-load the registers a refill handler needs to locate and write the PTE.
    cop0_[kBadVAddr] = vaddr;
    cop0_[kContext]  = (cop0_[kContext] & ~kContextBadVpn2) | (((vaddr >> 13) & 0x7'FFFF) << 4);
    cop0_[kXContext] = (cop0_[kXContext] & ~kXContextLow) | (((vaddr >> 62) & 3) << 31) |
                       (((vaddr >> 13) & 0x7FF'FFFF) << 4);
    cop0_[kEntryHi]  = (vaddr & kEntryHiVpn2) | (cop0_[kEntryHi] & kEntryHiAsid);

    ExcCode code = access == Access::Store ? ExcCode::TLBS : ExcCode::TLBL;
    if (outcome == Tlb::Outcome::Modified) {
        code = ExcCode::Mod;
    }
    raiseException(code, 0, /*tlbRefill=*/outcome == Tlb::Outcome::Miss);
}

bool R4600::interruptPending() const noexcept {
    // Taken when IE=1, EXL=0, ERL=0 and some Cause.IP bit is enabled in Status.IM.
    const uint64_t status = cop0_[kStatus];
    return (status & (kStatusIE | kStatusEXL | kStatusERL)) == kStatusIE &&
           (status & cop0_[kCause] & kInterruptMask) != 0;
}

bool R4600::coprocessorUsable(unsigned cop) const noexcept {
    if (cop == 0 && kernelMode()) {
        return true; // COP0 is always usable in kernel mode
    }
    return (cop0_[kStatus] & (uint64_t{1} << (kStatusCuShift + cop))) != 0;
}

// ---------------------------------------------------------------------------
// COP0 registers
// ---------------------------------------------------------------------------

void R4600::advanceTimers() noexcept {
    // Random cycles from 47 down to Wired on every instruction.
    uint64_t& random = cop0_[kRandom];
    random = random <= cop0_[kWired] ? Tlb::kEntries - 1 : random - 1;

    // Count runs at half the pipeline clock; matching Compare raises IP7.
    countPhase_ = !countPhase_;
    if (countPhase_) {
        return;
    }
    cop0_[kCount] = (cop0_[kCount] + 1) & 0xFFFF'FFFF;
    if (cop0_[kCount] == cop0_[kCompare]) {
        cop0_[kCause] |= kCauseTimerIp;
    }
}

void R4600::writeCop0(unsigned reg, uint64_t value) {
    uint64_t& r = cop0_[reg];
    switch (reg) {
    case kRandom:
    case kPRId:
    case kBadVAddr:
        return; // read-only
    case kIndex:    r = value & kIndexMask; return;
    case kEntryLo0:
    case kEntryLo1: r = value & Tlb::kEntryLoMask; return;
    case kContext:  r = (value & ~kContextBadVpn2) | (r & kContextBadVpn2); return; // PTEBase only
    case kXContext: r = (value & ~kXContextLow) | (r & kXContextLow); return;
    case kPageMask: r = value & Tlb::kPageMaskMask; return;
    case kWired:
        r = value & kIndexMask;
        cop0_[kRandom] = Tlb::kEntries - 1; // writing Wired resets Random
        return;
    case kEntryHi:  r = value & Tlb::kEntryHiMask; return;
    case kCount:    r = value & 0xFFFF'FFFF; return;
    case kCompare:
        r = value & 0xFFFF'FFFF;
        cop0_[kCause] &= ~kCauseTimerIp; // writing Compare acknowledges the timer
        return;
    case kStatus:   r = value & 0xFFFF'FFFF; return;
    case kCause:    r = (r & ~kCauseWritableMask) | (value & kCauseWritableMask); return;
    case kConfig:   r = (r & ~kConfigWritableMask) | (value & kConfigWritableMask); return;
    default:        r = value; return;
    }
}

void R4600::tlbRead() {
    const Tlb::Entry& e = tlb_.entry(static_cast<unsigned>(cop0_[kIndex] & kIndexMask));
    cop0_[kPageMask] = e.pageMask;
    cop0_[kEntryHi]  = e.entryHi;
    cop0_[kEntryLo0] = e.entryLo0;
    cop0_[kEntryLo1] = e.entryLo1;
}

void R4600::tlbWrite(unsigned index) {
    tlb_.write(index, cop0_[kPageMask], cop0_[kEntryHi], cop0_[kEntryLo0], cop0_[kEntryLo1]);
}

void R4600::tlbProbe() {
    const auto index = tlb_.probe(cop0_[kEntryHi]);
    cop0_[kIndex] = index ? uint64_t{*index} : kIndexProbeFail;
}

void R4600::eret() {
    // Returns to ErrorEPC if ERL is set, else EPC. ERET has no delay slot and
    // clears the LL reservation.
    if ((cop0_[kStatus] & kStatusERL) != 0) {
        pc_ = cop0_[kErrorEPC];
        cop0_[kStatus] &= ~kStatusERL;
    } else {
        pc_ = cop0_[kEPC];
        cop0_[kStatus] &= ~kStatusEXL;
    }
    nextPc_        = pc_ + 4;
    branchPending_ = false;
    llBit_         = false;
}

void R4600::setInterruptLine(unsigned line, bool asserted) {
    assert(line < kHardwareInterruptLines);
    const uint64_t bit = uint64_t{1} << (kCauseHwIpShift + line);
    if (asserted) {
        cop0_[kCause] |= bit;
    } else {
        cop0_[kCause] &= ~bit;
    }
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

void R4600::unimplemented(mips::Instruction insn) {
    // pc_ has already advanced past this instruction, so execution simply
    // continues with the next one.
    if (unimplementedWarnings_.first(currentPc_)) {
        log::tagged("CPU WARNING", "Unimplemented instruction {:#010x} at PC {}",
                    insn.raw, mips::formatAddress(currentPc_));
        if (unimplementedWarnings_.exhausted()) {
            log::tagged("CPU WARNING", "Too many unimplemented-instruction sites; further warnings suppressed");
        }
    }
}

void R4600::unmappedAccess(PhysAddr addr, unsigned bytes, bool isWrite) {
    if (unmappedWarnings_.first(addr)) {
        log::tagged("CPU WARNING", "Bus error: {}-byte {} at undecoded physical {:#010x} (PC {})",
                    bytes, isWrite ? "write" : "read", addr, mips::formatAddress(currentPc_));
        if (unmappedWarnings_.exhausted()) {
            log::tagged("CPU WARNING", "Too many unmapped addresses; further warnings suppressed");
        }
    }
}

void R4600::halt(std::string_view reason) {
    halted_ = true;
    log::error("CPU halted after {} instructions: {}", executed_, reason);
    logHistory();
}

void R4600::logRegisters() const {
    static constexpr const char* kNames[32] = {
        "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
        "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra"};
    for (unsigned i = 0; i < 32; i += 4) {
        log::tagged("CPU REGS", "{:>4}={:<12} {:>4}={:<12} {:>4}={:<12} {:>4}={}",
                    kNames[i], mips::formatAddress(gpr_[i]), kNames[i + 1], mips::formatAddress(gpr_[i + 1]),
                    kNames[i + 2], mips::formatAddress(gpr_[i + 2]), kNames[i + 3], mips::formatAddress(gpr_[i + 3]));
    }
}

std::vector<R4600::HistoryEntry> R4600::history() const {
    // The entry for an instruction is recorded at fetch, before executed_ is
    // bumped, so a faulting instruction is included.
    const size_t valid = static_cast<size_t>(std::min<uint64_t>(executed_ + 1, kHistorySize));
    std::vector<HistoryEntry> out;
    out.reserve(valid);
    for (size_t i = 0; i < valid; ++i) {
        const HistoryEntry& e = history_[(historyNext_ + kHistorySize - valid + i) % kHistorySize];
        if (e.pc != 0 || e.insn != 0) {
            out.push_back(e);
        }
    }
    return out;
}

void R4600::logHistory() const {
    const auto entries = history();
    if (entries.empty()) {
        log::tagged("CPU HISTORY", "(no instructions executed since reset)");
        return;
    }
    log::tagged("CPU HISTORY", "last {} instructions, oldest first:", entries.size());
    for (const HistoryEntry& e : entries) {
        log::tagged("CPU HISTORY", "  {}: {:#010x}", mips::formatAddress(e.pc), e.insn);
    }
}

} // namespace indy
