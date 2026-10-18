#pragma once

#include "indy/core/WarnOnce.hpp"
#include "indy/cpu/Cpu.hpp"
#include "indy/cpu/DecodedCache.hpp"
#include "indy/cpu/Instruction.hpp"
#include "indy/cpu/Tlb.hpp"
#include "indy/memory/MemoryBus.hpp"

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace indy {

// MIPS R5000 (MIPS IV, 64-bit, big-endian on the Indy), as fitted to the
// R5000PC Indy: 32 KiB primary caches, no secondary cache. It can also present
// itself as the earlier R4600 "Orion" (CpuModel::R4600: MIPS III only, 16 KiB
// caches, its own PRId/FCR0), the Indy's other processor.
//
// Implemented: MIPS III and MIPS IV integer ISA, exact branch delay slots,
// COP0 (TLB, exceptions, interrupts, Count/Compare timer), 32-bit
// compatibility address space with full segment/mode checks, COP1 (IEEE
// arithmetic; MIPS IV: COP1X, multiply-add, RECIP/RSQRT, eight condition codes).
// Not yet: caches, 64-bit address segments (KX/SX/UX).
//
// Dispatch: instructions are decoded once per page of guest code into
// DecodedOps (a handler and the register fields), kept by DecodedCache, and
// run through the handler: one indirect call per instruction, no decode
// switch. Hot instructions have handlers of their own; the others run
// execute(), the reference semantics. Writes to decoded code (stores, DMA)
// drop its decoded form (MemoryBus code pages).
//
// Fault policy:
//   * architectural faults raise real MIPS exceptions (vectored via COP0)
//   * unimplemented instruction  -> warn once per PC, skip it, keep running
//   * access to an undecoded physical address -> bus error: reads take a Data
//     Bus Error exception, posted writes complete; both are reported to the
//     board (the MC latches them and interrupts on IP6 for writes)
//   * unrecoverable states (handler itself unfetchable) -> halt
class R5000 final : public Cpu {
public:
    // `traceInstructions`: log this many executed instructions after each reset.
    // `decoded`: run through the decoded-instruction cache (false: decode
    // every instruction, the reference path, for comparisons and debugging).
    explicit R5000(MemoryBus& bus, uint64_t traceInstructions = 0, CpuModel model = CpuModel::R4600,
                   bool decoded = true);
    ~R5000() override;
    R5000(const R5000&) = delete;
    R5000& operator=(const R5000&) = delete;

    [[nodiscard]] CpuModel model() const noexcept { return model_; }
    [[nodiscard]] bool mipsIV() const noexcept { return model_ == CpuModel::R5000; }
    [[nodiscard]] uint32_t fcr0() const noexcept { return mipsIV() ? kFcr0R5000 : kFcr0R4600; }

    void reset() override;
    [[nodiscard]] Cycles run(Cycles budget) override;
    [[nodiscard]] Cycles elapsedInRun() const noexcept override { return runElapsed_; }
    void setInterruptLine(unsigned line, bool asserted) override;

    // Board hook for bus errors on data accesses (physical address, write?).
    using BusErrorHook = std::function<void(PhysAddr, bool isWrite)>;
    void setBusErrorHook(BusErrorHook hook) { busErrorHook_ = std::move(hook); }

    // ---- Hypercalls (emulator services for guest software) -------------------
    // The reserved SPECIAL word kHypercall (function 0x28, shift 0x1A) calls the
    // handler instead of being skipped. Convention: a0 = service, a1 = operation,
    // a2 = guest buffer, a3 = length; the handler sets v0/v1. Guest memory is
    // reached with guestAccessible() first (it raises the TLB/address exception
    // a load or store would, so the instruction re-executes once IRIX has mapped
    // the page) and then guestRead()/guestWrite(), which cannot fail afterwards.
    static constexpr uint32_t kHypercall = 0x0000'0668;
    using HypercallHandler = std::function<void(R5000&)>;
    void setHypercallHandler(HypercallHandler handler) { hypercall_ = std::move(handler); }
    [[nodiscard]] bool guestAccessible(VirtAddr vaddr, size_t length, bool forWrite);
    void guestRead(VirtAddr vaddr, std::span<uint8_t> out);
    void guestWrite(VirtAddr vaddr, std::span<const uint8_t> data);
    // Resumable copies for buffers larger than the TLB maps at once (48 entry
    // pairs): they copy page by page from `done` on and return false at the
    // first page not mapped, having raised its exception. The caller keeps
    // `done` and calls again when the hypercall re-executes; true once all
    // bytes are copied.
    [[nodiscard]] bool guestCopyIn(VirtAddr vaddr, std::span<uint8_t> out, size_t& done);
    [[nodiscard]] bool guestCopyOut(VirtAddr vaddr, std::span<const uint8_t> data, size_t& done);
    void setHypercallResult(uint64_t v0, uint64_t v1 = 0) noexcept {
        setGpr(2, v0);
        setGpr(3, v1);
    }

    // Address of the next instruction to execute.
    [[nodiscard]] uint64_t pc() const noexcept override { return pc_; }

    // Service a pending interrupt or fetch/decode/execute one instruction.
    // Returns false if the CPU is (or just became) halted.
    bool tick();

    // ---- State inspection (debugger, tests) ------------------------------
    [[nodiscard]] uint64_t gpr(unsigned index) const noexcept { return gpr_[index & 31]; }
    [[nodiscard]] uint64_t hi() const noexcept { return hi_; }
    [[nodiscard]] uint64_t lo() const noexcept { return lo_; }
    [[nodiscard]] uint64_t cop0(unsigned reg) const noexcept { return readCop0(reg & 31); }
    [[nodiscard]] const Tlb& tlb() const noexcept { return tlb_; }
    [[nodiscard]] uint64_t fpr(unsigned index) const noexcept { return fpr_[index & 31]; }
    [[nodiscard]] uint32_t fcr31() const noexcept { return fcr31_; }
    [[nodiscard]] bool halted() const noexcept { return halted_; }

    // Recently executed instructions, oldest first (for post-mortems).
    struct HistoryEntry {
        VirtAddr pc;
        uint32_t insn;
    };
    static constexpr size_t kHistorySize = 64;
    [[nodiscard]] std::vector<HistoryEntry> history() const;
    void logHistory() const;
    void logRegisters() const;
    [[nodiscard]] uint64_t instructionsExecuted() const noexcept { return executed_; }

    static constexpr unsigned kHardwareInterruptLines = 6;

    // FCR0 in R4600 mode: FPU implementation 0x20, revision 2.0.
    static constexpr uint32_t kFcr0R4600 = 0x2020;
    // FCR0 on the R5000: implementation 0x23 (as its PRId), revision 2.1.
    static constexpr uint32_t kFcr0R5000 = 0x2321;

    // Architectural COP0 register numbers (MIPS III / R4000 family).
    enum Cop0Reg : unsigned {
        kIndex    = 0,  // TLB index for TLBR/TLBWI; P bit set by a failed TLBP
        kRandom   = 1,  // TLB index for TLBWR; cycles Wired..47
        kEntryLo0 = 2,  // even-page PFN and flags
        kEntryLo1 = 3,  // odd-page PFN and flags
        kContext  = 4,  // PTEBase | BadVPN2: page-table pointer for refill handlers
        kPageMask = 5,  // TLB page size
        kWired    = 6,  // entries below this are never chosen by TLBWR
        kBadVAddr = 8,  // faulting virtual address
        kCount    = 9,  // free-running timer, half the pipeline clock
        kEntryHi  = 10, // VPN2 and current ASID
        kCompare  = 11, // timer interrupt (IP7) when Count == Compare
        kStatus   = 12, // CU, BEV, IM, KSU, ERL, EXL, IE
        kCause    = 13, // BD, CE, IP, ExcCode
        kEPC      = 14, // restart address after an exception
        kPRId     = 15, // processor implementation/revision
        kConfig   = 16, // cache and endianness configuration
        kLLAddr   = 17,
        kWatchLo  = 18,
        kWatchHi  = 19,
        kXContext = 20,
        kECC      = 26,
        kCacheErr = 27,
        kTagLo    = 28,
        kTagHi    = 29,
        kErrorEPC = 30, // restart address after reset/NMI/cache error (ERL)
    };

    // Cause.ExcCode values.
    enum class ExcCode : uint8_t {
        Int  = 0,  // interrupt
        Mod  = 1,  // TLB modified
        TLBL = 2,  // TLB miss/invalid on load or fetch
        TLBS = 3,  // TLB miss/invalid on store
        AdEL = 4,  // address error on load or fetch
        AdES = 5,  // address error on store
        IBE  = 6,  // bus error on fetch
        DBE  = 7,  // bus error on data access
        Sys  = 8,  // SYSCALL
        Bp   = 9,  // BREAK
        RI   = 10, // reserved instruction
        CpU  = 11, // coprocessor unusable
        Ov   = 12, // integer overflow
        Tr   = 13, // trap
        FPE  = 15, // floating point
    };

private:
    friend struct R5000Ops; // the decoded-instruction handlers (R5000Execute.cpp)

    enum class Access { Fetch, Load, Store };

    // ---- Register file -----------------------------------------------------
    // $zero is hard-wired: every write path goes through here and re-zeroes it,
    // which is cheaper than branching on the register index.
    void setGpr(unsigned index, uint64_t value) noexcept {
        gpr_[index] = value;
        gpr_[0] = 0;
    }

    // ---- Execution (R5000Execute.cpp) ---------------------------------------
    void execute(mips::Instruction insn);
    void executeSpecial(mips::Instruction insn);
    void executeRegImm(mips::Instruction insn);
    void executeCop0(mips::Instruction insn);
    void executeCop1(mips::Instruction insn);
    void executeFpuOperation(mips::Instruction insn);
    void executeCop1x(mips::Instruction insn);         // MIPS IV: indexed FP load/store, multiply-add
    // FP condition codes: cc 0 is FCR31 bit 23 (MIPS III's C), cc 1-7 are
    // bits 25-31 (MIPS IV).
    [[nodiscard]] bool fpCondition(unsigned cc) const noexcept;
    void setFpCondition(unsigned cc, bool value) noexcept;  // .S/.D/.W/.L arithmetic, conversions, compares
    bool completeFpuOperation(uint32_t cause);         // FCR31 cause/flags; false = trapped

    // ---- COP1 register file ----------------------------------------------------
    // Status.FR = 1: 32 independent 64-bit registers. FR = 0 (MIPS II
    // compatible): 32-bit accesses to an odd register reach the upper half of
    // the even register, and 64-bit values live in even/odd pairs.
    [[nodiscard]] bool fpr64Mode() const noexcept;
    [[nodiscard]] uint32_t readFpr32(unsigned reg) const noexcept;
    void writeFpr32(unsigned reg, uint32_t value) noexcept;
    [[nodiscard]] uint64_t readFpr64(unsigned reg) const noexcept;
    void writeFpr64(unsigned reg, uint64_t value) noexcept;
    void writeFcr31(uint32_t value);

    // Control flow. pc_ already points at the delay slot when a branch executes.
    void branch(bool taken, int64_t offset) noexcept;
    void branchLikely(bool taken, int64_t offset) noexcept; // nullifies slot if not taken
    void jump(VirtAddr target) noexcept {
        nextPc_ = target;
        branchPending_ = true;
    }
    void link(unsigned reg) noexcept { setGpr(reg, currentPc_ + 8); }
    void trap(bool condition);

    [[nodiscard]] VirtAddr effectiveAddress(mips::Instruction insn) const noexcept {
        return gpr_[insn.rs()] + static_cast<uint64_t>(insn.simm());
    }
    template <BusWord T, bool SignExtend> void loadOp(mips::Instruction insn);
    // Unaligned access pairs (LWL/LWR, LDL/LDR, SWL/SWR, SDL/SDR), big-endian.
    template <BusWord T> void loadUnaligned(mips::Instruction insn, bool left);
    template <BusWord T> void storeUnaligned(mips::Instruction insn, bool left);
    template <BusWord T> void storeOp(mips::Instruction insn);
    template <BusWord T> [[nodiscard]] bool load(VirtAddr vaddr, T& out);
    template <BusWord T> [[nodiscard]] bool store(VirtAddr vaddr, T value);

    // ---- MMU (R5000.cpp) -----------------------------------------------------
    // Virtual -> physical for the current mode. On failure the appropriate
    // exception has already been raised and nullopt is returned.
    [[nodiscard]] std::optional<PhysAddr> translate(VirtAddr vaddr, Access access);
    [[nodiscard]] bool kernelMode() const noexcept;

    // ---- Exceptions & COP0 (R5000.cpp) ---------------------------------------
    void raiseException(ExcCode code, unsigned coprocessor = 0, bool tlbRefill = false);
    void raiseAddressError(VirtAddr vaddr, Access access);
    void raiseTlbException(VirtAddr vaddr, Access access, Tlb::Outcome outcome);
    [[nodiscard]] bool interruptPending() const noexcept;
    [[nodiscard]] bool coprocessorUsable(unsigned cop) const noexcept;

    void writeCop0(unsigned reg, uint64_t value);
    void tlbRead();
    void tlbWrite(unsigned index);
    void tlbProbe();
    void eret();
    // Random and Count advance lazily: every instruction (or exception) is one
    // tick; their values are derived from the tick counter when read, and the
    // Count = Compare interrupt is a precomputed tick (timerDeadline_).
    void advanceTimers() noexcept {
        if (++ticks_ == timerDeadline_) [[unlikely]] {
            timerFire();
        }
    }
    void timerFire() noexcept;
    void scheduleTimer() noexcept;
    [[nodiscard]] uint64_t readCop0(unsigned reg) const noexcept;
    [[nodiscard]] uint32_t randomValue() const noexcept;
    [[nodiscard]] uint32_t countValue() const noexcept {
        return static_cast<uint32_t>(countValue_ + (ticks_ >> 1) - (countTick_ >> 1));
    }

    // ---- Diagnostics -------------------------------------------------------
    void unimplemented(mips::Instruction insn);
    void unmappedAccess(PhysAddr addr, unsigned bytes, bool isWrite);
    void halt(std::string_view reason);

    MemoryBus& bus_;
    CpuModel model_;
    Tlb tlb_;
    BusErrorHook busErrorHook_;
    HypercallHandler hypercall_;

    std::array<uint64_t, 32> gpr_{};
    uint64_t hi_ = 0;
    uint64_t lo_ = 0;
    VirtAddr pc_ = 0;           // next instruction to execute
    VirtAddr nextPc_ = 0;       // the one after it (a branch rewrites this)
    VirtAddr currentPc_ = 0;    // instruction currently executing
    bool branchPending_ = false; // the next instruction is a delay slot
    bool inDelaySlot_ = false;   // the current instruction is a delay slot
    bool llBit_ = false;         // LL/SC reservation
    std::array<uint64_t, 32> cop0_{};
    uint64_t ticks_ = 0;           // instructions (and exceptions) since reset
    uint64_t randomBase_ = 0;      // tick at which Random was last 47 (reset / Wired write)
    uint32_t countValue_ = 0;      // Count as last written...
    uint64_t countTick_ = 0;       // ...at this tick; it increments on every even tick
    uint64_t timerDeadline_ = 0;   // tick at which Count reaches Compare
    std::array<uint64_t, 32> fpr_{}; // COP1 floating-point registers
    uint32_t fcr31_ = 0;             // COP1 control/status

    bool halted_ = false;
    uint64_t executed_ = 0;
    std::array<HistoryEntry, kHistorySize> history_{};
    size_t historyNext_ = 0; // ring write position; executed_ tells how many are valid
    Cycles runElapsed_ = 0;
    uint64_t traceLimit_;
    uint64_t traceRemaining_ = 0;

    WarnOnce unimplementedWarnings_;
    WarnOnce unmappedWarnings_;
    WarnOnce exceptionLog_{64};
    // Micro-TLB: recent 4 KiB translations of mapped addresses, direct-mapped
    // by virtual page. The tag holds the page, the ASID and the privilege bits
    // of Status (KSU/EXL/ERL), so a context or mode switch simply misses; a
    // TLB write drops the slots of the pages the old entry mapped.
    struct MicroTlbEntry {
        uint64_t tag = 0;
        uint64_t generation = 0; // valid only while equal to microTlbGeneration_
        PhysAddr page = 0;
        bool dirty = false;      // stores allowed without consulting the TLB
    };
    static constexpr size_t kMicroTlbEntries = 256;
    std::array<MicroTlbEntry, kMicroTlbEntries> microTlb_{};
    uint64_t microTlbGeneration_ = 1;
    void flushMicroTlb() noexcept {
        ++microTlbGeneration_;
        dropFetchCache();
        flushFastTables();
    }
    void invalidateMicroTlb(const Tlb::Entry& replaced) noexcept;

    // Fetch cache: the decoded instructions of the 4 KiB page the PC is in,
    // so sequential instructions skip translation, the bus page lookup and
    // decoding. Dropped by anything that can change translation (COP0
    // writes, exceptions, ERET, TLB reads/writes), by physical memory remaps
    // (MemoryBus remap hook) and by writes to the page's code.
    VirtAddr fetchPage_ = 1;           // page-aligned PC the cache covers; 1 = none
    DecodedOp* fetchOps_ = nullptr;
    void dropFetchCache() noexcept { fetchPage_ = 1; }

    // ---- Decoded instructions (R5000.cpp, R5000Execute.cpp) ------------------
    bool decodedDispatch_;
    DecodedCache decoded_;
    // Storage page of host memory: RAM pages, then PROM pages; npos otherwise.
    [[nodiscard]] size_t storagePage(const uint8_t* host) const noexcept;
    // The decoded page for the 4 KiB of host memory at `host` (filled when
    // new or stale); nullptr when the memory cannot be cached.
    [[nodiscard]] DecodedOp* decodedPage(const uint8_t* host);
    void codeWritten(size_t ramPage) noexcept; // MemoryBus code write hook
    // Drop direct write pointers to host page `host` (it now holds code).
    void revokeHostWrites(const uint8_t* host) noexcept;
    static DecodedOp decode(uint32_t raw) noexcept; // R5000Execute.cpp
    static void undecodedOp(R5000& cpu, DecodedOp& op); // decodes its slot, then runs it
    bool step();                                    // one instruction or interrupt

    // ---- Fast-path data access (R5000.cpp, R5000Execute.cpp) --------------------
    // Loads and stores to RAM and PROM go straight to host memory through a
    // small direct-mapped table per CPU mode (kernel, user), indexed by
    // virtual page: one tag compare, then host = vaddr + addend. The tag is
    // the page-aligned virtual address; a load or store compares it with
    // vaddr & ~(0xFFF - (size - 1)), so a misaligned address never matches
    // and takes the slow path, which raises the address error. Read and
    // write tags are separate: a page is readable through the table once a
    // load translated it, writable once a store did (TLB D bit) and the bus
    // has RAM there; PROM, devices and decoded code are never writable here.
    //
    // The tables hold only what the current translation allows, so they
    // are flushed or updated where translation changes, never checked per
    // access: the mode picks the table (Status writes, exceptions, ERET);
    // an ASID change flushes both; a TLB write drops the pages of the entry
    // it replaces; a bus remap flushes everything (MemoryBus remap hook).
    // ERL (reset and error handling: kuseg is an unmapped window then) has a
    // table of its own; supervisor mode (unused by IRIX) one that always misses.
    // Everything runs on the emulation thread (devices and DMA included), so
    // no synchronisation is needed.
    struct FastPage {
        uint64_t readTag;  // page-aligned vaddr whose loads may use `addend`; kNoTag: none
        uint64_t writeTag; // ... stores
        uintptr_t addend;  // host address - vaddr, for that page
    };
    static constexpr uint64_t kNoTag = 1;     // page-aligned addresses never have bit 0 set
    static constexpr size_t kFastPages = 1024; // per table: 24 KiB
    enum FastTable : size_t { kFastKernel, kFastUser, kFastErl, kFastNone, kFastTables };
    std::array<std::array<FastPage, kFastPages>, kFastTables> fastTables_;
    FastPage* fastTable_ = nullptr;           // the current mode's
    void selectFastTable() noexcept;          // after a Status change
    void flushFastTables() noexcept;
    void invalidateFastPages(uint64_t firstPage, uint64_t pages) noexcept;
    template <bool Store> void fastFill(VirtAddr vaddr, PhysAddr phys) noexcept;
    template <BusWord T> [[nodiscard]] bool loadSlow(VirtAddr vaddr, T& out);
    template <BusWord T> [[nodiscard]] bool storeSlow(VirtAddr vaddr, T value);

    bool traceNull_ = false; // INDY_TRACE_NULL: dump state on the first NULL-page access
    bool traceExceptions_ = false; // INDY_TRACE_EXCEPTIONS: also log routine exceptions (TLB refills, ...)
    std::optional<VirtAddr> tracePc_; // INDY_TRACE_PC=<hex>: log arguments each time this PC executes
    void logTracePc();
};

} // namespace indy
