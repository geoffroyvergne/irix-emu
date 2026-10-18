// CPU interpreter benchmark: guest MIPS code running from RAM (KSEG0, as the
// IRIX kernel does), with no devices and no real-time pacing.
//   indy_bench [seconds per workload, default 3]
// Prints millions of guest instructions per second for each workload.
#include "indy/cpu/R5000.hpp"
#include "indy/memory/MemoryBus.hpp"
#include "indy/memory/PhysicalMap.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace indy;

namespace {

constexpr uint32_t kCodePhys = physmap::kRamBase + 0x2000;  // the workload, run at KSEG0 0x8800'2000
constexpr uint32_t kDataPhys = physmap::kRamBase + 0x10000; // its data

// A boot PROM that jumps to the workload in RAM; its exception vector
// (BEV) returns past the faulting instruction (SYSCALL in the user workload).
std::filesystem::path writeProm() {
    const auto path = std::filesystem::temp_directory_path() / "indy_bench_prom.bin";
    const uint32_t kseg0 = 0x8000'0000u | kCodePhys;
    std::vector<uint32_t> words(0x400 / 4, 0);
    words[0] = 0x3C19'0000u | (kseg0 >> 16);    // lui   $25, hi(code)
    words[1] = 0x3739'0000u | (kseg0 & 0xFFFF); // ori   $25, $25, lo(code)
    words[2] = 0x0320'0008u;                    // jr    $25
    words[0x380 / 4 + 0] = 0x401A'7000;         // mfc0  $26, EPC
    words[0x380 / 4 + 1] = 0x275A'0004;         // addiu $26, $26, 4
    words[0x380 / 4 + 2] = 0x409A'7000;         // mtc0  $26, EPC
    words[0x380 / 4 + 3] = 0x4200'0018;         // eret
    std::ofstream out(path, std::ios::binary);
    for (uint32_t w : words) {
        const char be[4] = {static_cast<char>(w >> 24), static_cast<char>(w >> 16), static_cast<char>(w >> 8),
                            static_cast<char>(w)};
        out.write(be, 4);
    }
    return path;
}

struct Workload {
    const char* name;
    std::vector<uint32_t> code; // at kCodePhys; $16 holds the data address on entry
    std::vector<uint32_t> user = {}; // user-mode code, mapped at 0x0040'0000 (its data at 0x0040'1000)
};

constexpr uint32_t kUserPhys = physmap::kRamBase + 0x20000; // even page: user code; odd page: user data

// $16 = data (KSEG0), set by a prologue added below.
const Workload kWorkloads[] = {
    {"integer loop (ALU, loads/stores, branch with delay slot)",
     {
         0x2411'0000, // addiu $17, $0, 0          counter
         0x2412'1234, // addiu $18, $0, 0x1234     accumulator
         // loop:
         0x8E08'0000, // lw    $8, 0($16)
         0x0112'4021, // addu  $8, $8, $18
         0xAE08'0004, // sw    $8, 4($16)
         0x0008'48C0, // sll   $9, $8, 3
         0x0249'9026, // xor   $18, $18, $9
         0x324A'00FF, // andi  $10, $18, 0xff
         0x0151'582A, // slt   $11, $10, $17
         0x3C0C'0001, // lui   $12, 1
         0x018B'6025, // or    $12, $12, $11
         0x0192'6823, // subu  $13, $12, $18
         0x8E0E'0004, // lw    $14, 4($16)
         0x01CD'7021, // addu  $14, $14, $13
         0xAE0E'0008, // sw    $14, 8($16)
         0x1000'FFF2, // b     loop (-14)
         0x2631'0001, // addiu $17, $17, 1        (delay slot)
     }},
    {"call-heavy loop (jal/jr, sltiu, bne, lbu/sb)",
     {
         0x2411'0000, // addiu $17, $0, 0
         // loop:
         0x0C00'0000, // jal   func (patched below)
         0x0000'0000, // nop
         0x2631'0001, // addiu $17, $17, 1
         0x2E28'1000, // sltiu $8, $17, 0x1000
         0x1500'FFFB, // bne   $8, $0, loop (-5)
         0x0000'0000, // nop
         0x1000'FFF9, // b     loop (-7): start again
         0x0000'0000, // nop
         // func:
         0x9208'0010, // lbu   $8, 16($16)
         0x2508'0001, // addiu $8, $8, 1
         0xA208'0010, // sb    $8, 16($16)
         0x03E0'0008, // jr    $31
         0x0000'0000, // nop
     }},
    {"user mode, mapped memory, a syscall every 64 iterations",
     {
         // One TLB entry: 0x0040'0000 (even page) and 0x0040'1000 (odd page), ASID 5.
         0x3C08'0040, // lui   $8, 0x0040
         0x3508'0005, // ori   $8, $8, 5
         0x4088'5000, // mtc0  $8, EntryHi
         0x3C09'0000u | (((kUserPhys >> 12) << 6 | 0x1E) >> 16),           // lui $9, hi(EntryLo0)
         0x3529'0000u | (((kUserPhys >> 12) << 6 | 0x1E) & 0xFFFF),        // ori $9, $9, lo(EntryLo0): C=3 D V
         0x4089'1000, // mtc0  $9, EntryLo0
         0x3C09'0000u | ((((kUserPhys >> 12) + 1) << 6 | 0x1E) >> 16),     // lui $9, hi(EntryLo1)
         0x3529'0000u | ((((kUserPhys >> 12) + 1) << 6 | 0x1E) & 0xFFFF),  // ori $9, $9, lo(EntryLo1)
         0x4089'1800, // mtc0  $9, EntryLo1
         0x4080'2800, // mtc0  $0, PageMask
         0x4080'0000, // mtc0  $0, Index
         0x4200'0002, // tlbwi
         0x3C08'0040, // lui   $8, 0x0040
         0x4088'7000, // mtc0  $8, EPC        (user code)
         0x3C08'0040, // lui   $8, 0x0040     Status: BEV, user mode, EXL
         0x3508'0012, // ori   $8, $8, 0x12
         0x4088'6000, // mtc0  $8, Status
         0x4200'0018, // eret
     },
     {
         0x3C10'0040, // lui   $16, 0x0040
         0x3610'1000, // ori   $16, $16, 0x1000
         // loop:
         0x8E08'0000, // lw    $8, 0($16)
         0x2508'0001, // addiu $8, $8, 1
         0xAE08'0000, // sw    $8, 0($16)
         0x8E09'0008, // lw    $9, 8($16)
         0x0128'4821, // addu  $9, $9, $8
         0xAE09'0010, // sw    $9, 16($16)
         0x310A'003F, // andi  $10, $8, 63
         0x1540'0002, // bne   $10, $0, skip
         0x0000'0000, // nop
         0x0000'000C, // syscall
         // skip:
         0x1000'FFF5, // b     loop (-11)
         0x0000'0000, // nop
     }},
};

double measure(const Workload& w, double seconds) {
    const auto prom = writeProm();
    MemoryBus bus;
    bus.installRam(physmap::kRamBase, 16u << 20);
    if (!bus.loadProm(prom)) {
        std::fprintf(stderr, "indy_bench: PROM load failed\n");
        std::exit(1);
    }
    // Prologue: $16 = data address (KSEG0).
    std::vector<uint32_t> code = {0x3C10'0000u | ((0x8000'0000u | kDataPhys) >> 16),
                                  0x3610'0000u | (kDataPhys & 0xFFFF)};
    code.insert(code.end(), w.code.begin(), w.code.end());
    for (uint32_t& insn : code) { // patch "jal func" to the word after the last "nop" of the loop
        if (insn == 0x0C00'0000) {
            const uint32_t func = (0x8000'0000u | kCodePhys) + 4 * static_cast<uint32_t>(code.size() - 5);
            insn = 0x0C00'0000u | ((func >> 2) & 0x03FF'FFFF);
        }
    }
    for (size_t i = 0; i < code.size(); ++i) {
        (void)bus.write32(kCodePhys + 4 * static_cast<uint32_t>(i), code[i]);
    }
    for (size_t i = 0; i < w.user.size(); ++i) {
        (void)bus.write32(kUserPhys + 4 * static_cast<uint32_t>(i), w.user[i]);
    }
    R5000 cpu(bus, 0, CpuModel::R5000);
    cpu.reset();
    (void)cpu.run(100'000); // warm up
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const uint64_t before = cpu.instructionsExecuted();
    double elapsed = 0;
    while (elapsed < seconds) {
        (void)cpu.run(1'000'000);
        elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    }
    if (cpu.halted()) {
        std::fprintf(stderr, "indy_bench: the CPU halted (PC %#llx)\n", static_cast<unsigned long long>(cpu.pc()));
        std::exit(1);
    }
    if (std::getenv("INDY_BENCH_CHECK") != nullptr) { // sanity: where the workload runs, what it counted
        uint32_t counter = 0;
        (void)bus.read(kUserPhys + 0x1000, counter);
        std::fprintf(stderr, "  pc %#llx, status %#llx, user counter %u\n", static_cast<unsigned long long>(cpu.pc()),
                     static_cast<unsigned long long>(cpu.cop0(12)), counter);
    }
    std::filesystem::remove(prom);
    return static_cast<double>(cpu.instructionsExecuted() - before) / elapsed / 1e6;
}

} // namespace

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 3.0;
    for (const Workload& w : kWorkloads) {
        std::printf("%-60s %8.1f MIPS\n", w.name, measure(w, seconds));
    }
    return 0;
}
