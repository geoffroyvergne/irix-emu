// Core unit tests. Dependency-free on purpose: run with `ctest --test-dir build`.

#include "indy/cpu/AddressSpace.hpp"
#include "indy/cpu/Fpu.hpp"
#include "indy/cpu/R5000.hpp"
#include "indy/cpu/Tlb.hpp"
#include "indy/graphics/Newport.hpp"
#include "indy/hpc/Ds1386.hpp"
#include "indy/hpc/Hpc3.hpp"
#include "indy/hpc/Int3.hpp"
#include "indy/hpc/Ioc2.hpp"
#include "indy/hpc/Pit8254.hpp"
#include "indy/hpc/SgiNvram.hpp"
#include "indy/input/Kbc8042.hpp"
#include "indy/input/TextKeys.hpp"
#include "indy/scsi/CdRomDrive.hpp"
#include "indy/scsi/DiskImage.hpp"
#include "indy/scsi/ScsiDisk.hpp"
#include "indy/scsi/Wd33c93.hpp"
#include "indy/serial/Z8530.hpp"
#include "indy/memory/MemoryBus.hpp"
#include "indy/memory/MemoryController.hpp"
#include "indy/memory/PhysicalMap.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <stdexcept>
#include <vector>

using namespace indy;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;

// Variadic so expressions containing brace-init commas (Banks{a, b}) work.
#define CHECK(...)                                                                 \
    do {                                                                           \
        if (!(__VA_ARGS__)) {                                                      \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__); \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)

// Temporary file removed when the guard goes out of scope.
struct TempFile {
    fs::path path;
    explicit TempFile(const std::vector<uint8_t>& bytes) {
        static int counter = 0;
        path = fs::temp_directory_path() / ("indy_test_" + std::to_string(counter++) + ".bin");
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    ~TempFile() { std::error_code ec; fs::remove(path, ec); }
};

// Big-endian words as they would appear in a raw PROM dump.
std::vector<uint8_t> beWords(std::initializer_list<uint32_t> words) {
    std::vector<uint8_t> out;
    for (uint32_t w : words) {
        out.push_back(static_cast<uint8_t>(w >> 24));
        out.push_back(static_cast<uint8_t>(w >> 16));
        out.push_back(static_cast<uint8_t>(w >> 8));
        out.push_back(static_cast<uint8_t>(w));
    }
    return out;
}
std::vector<uint8_t> beWords(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> out;
    for (uint32_t w : words) {
        for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(w >> shift));
    }
    return out;
}

void ramIsBigEndian() {
    MemoryBus bus;
    bus.installRam(physmap::kRamBase, 1 * MiB);

    CHECK(bus.write32(physmap::kRamBase, 0x1122'3344));
    CHECK(bus.read32(physmap::kRamBase) == 0x1122'3344u);

    // Most significant byte at the lowest address, as on the Indy.
    uint8_t b0 = 0, b3 = 0;
    CHECK(bus.read(physmap::kRamBase + 0, b0) && b0 == 0x11);
    CHECK(bus.read(physmap::kRamBase + 3, b3) && b3 == 0x44);
    CHECK(bus.ram()[0] == 0x11 && bus.ram()[3] == 0x44);

    uint64_t d = 0;
    CHECK(bus.write<uint64_t>(physmap::kRamBase + 8, 0x0102'0304'0506'0708));
    CHECK(bus.read(physmap::kRamBase + 8, d) && d == 0x0102'0304'0506'0708u);
    CHECK(bus.read32(physmap::kRamBase + 12) == 0x0506'0708u);
}

void unmappedAccessFails() {
    MemoryBus bus;
    bus.installRam(physmap::kRamBase, 1 * MiB);
    CHECK(!bus.read32(0x0000'0000));
    CHECK(!bus.read32(physmap::kRamBase + 1 * MiB)); // one past the end
    CHECK(!bus.write32(physmap::kPromBase, 0));      // no PROM loaded
}

void invalidRamSizeThrows() {
    auto throws = [](uint64_t size) {
        try {
            MemoryBus bus;
            bus.installRam(physmap::kRamBase, size);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    CHECK(throws(0));
    CHECK(throws(1 * MiB + 4));                  // not page aligned
    CHECK(throws(physmap::kRamMaxSize + 1 * MiB));
    CHECK(!throws(128 * MiB));
}

void promLoadsVerbatimAndIsReadOnly() {
    TempFile prom(beWords({0x3C08'BFC0, 0x0000'0000, 0xDEAD'BEEF}));
    MemoryBus bus;
    const auto loaded = bus.loadProm(prom.path);
    CHECK(loaded && loaded->size == 12);
    CHECK(bus.read32(physmap::kPromBase + 0) == 0x3C08'BFC0u);
    CHECK(bus.read32(physmap::kPromBase + 8) == 0xDEAD'BEEFu);
    CHECK(bus.read32(physmap::kPromBase + 12) == 0xFFFF'FFFFu); // padding = erased EPROM
    CHECK(!bus.write32(physmap::kPromBase, 0));
    CHECK(bus.read32(physmap::kPromBase) == 0x3C08'BFC0u);

    // Reloading replaces the image in place.
    TempFile prom2(beWords({0x1234'5678}));
    CHECK(bus.loadProm(prom2.path).has_value());
    CHECK(bus.read32(physmap::kPromBase) == 0x1234'5678u);
}

void promErrors() {
    MemoryBus bus;
    CHECK(!bus.loadProm("/nonexistent/indy_prom.bin"));

    TempFile empty(std::vector<uint8_t>{});
    CHECK(!bus.loadProm(empty.path));

    TempFile tooLarge(std::vector<uint8_t>(physmap::kPromMaxSize + 4, 0));
    CHECK(!bus.loadProm(tooLarge.path));

    TempFile ragged(std::vector<uint8_t>{1, 2, 3});
    CHECK(!bus.loadProm(ragged.path));

    TempFile exactMax(std::vector<uint8_t>(physmap::kPromMaxSize, 0));
    CHECK(bus.loadProm(exactMax.path).has_value());

    // A failed load must not disturb the PROM already in place.
    CHECK(!bus.loadProm(tooLarge.path));
    CHECK(bus.read32(physmap::kPromBase) == 0u);
}

void segmentTranslation() {
    CHECK(mips::translateUnmapped(mips::kResetVector) == PhysAddr{0x1FC0'0000});
    CHECK(mips::translateUnmapped(0xFFFF'FFFF'8800'0000) == PhysAddr{0x0800'0000}); // KSEG0
    CHECK(mips::translateUnmapped(0xFFFF'FFFF'A800'0000) == PhysAddr{0x0800'0000}); // KSEG1
    CHECK(!mips::translateUnmapped(0x0000'0000'0040'0000));                         // kuseg
    CHECK(!mips::translateUnmapped(0xFFFF'FFFF'C000'0000));                         // kseg2
    CHECK(!mips::translateUnmapped(0x0000'0000'BFC0'0000));                         // not sign-extended
}

void cpuFetchesFromResetVector() {
    TempFile prom(beWords({0x3C08'BFC0, 0x2508'0010}));
    MemoryBus bus;
    CHECK(bus.loadProm(prom.path).has_value());

    R5000 cpu(bus);
    cpu.reset();
    CHECK(cpu.pc() == mips::kResetVector);

    CHECK(cpu.tick()); // lui $8, 0xbfc0
    CHECK(cpu.gpr(8) == 0xFFFF'FFFF'BFC0'0000u);
    CHECK(cpu.pc() == mips::kResetVector + 4);

    CHECK(cpu.tick()); // addiu $8, $8, 0x10
    CHECK(cpu.gpr(8) == 0xFFFF'FFFF'BFC0'0010u);
    CHECK(!cpu.halted());
}

void cpuHaltsWithoutProm() {
    MemoryBus bus;
    R5000 cpu(bus);
    cpu.reset();
    CHECK(cpu.tick());   // fetch fails -> instruction bus error exception
    CHECK(cpu.cop0(R5000::kEPC) == mips::kResetVector);
    CHECK(!cpu.tick());  // the handler cannot be fetched either -> halt
    CHECK(cpu.halted());
    cpu.reset();
    CHECK(!cpu.halted());
}

void promByteOrderDetection() {
    // Reset vector "j 0xbfc003c0; nop", as in the real Indy PROM.
    const std::vector<uint8_t> be = beWords({0x0BF0'00F0, 0x0000'0000, 0x1122'3344, 0x5566'7788});
    std::vector<uint8_t> swapped16 = be;
    for (size_t i = 0; i < swapped16.size(); i += 2) std::swap(swapped16[i], swapped16[i + 1]);
    std::vector<uint8_t> le32 = be;
    for (size_t i = 0; i < le32.size(); i += 4) {
        std::swap(le32[i], le32[i + 3]);
        std::swap(le32[i + 1], le32[i + 2]);
    }
    // Pad so the jump target (offset 0x3c0) lies inside the image.
    for (auto* image : {&swapped16, &le32}) image->resize(0x400, 0);
    std::vector<uint8_t> bePadded = be;
    bePadded.resize(0x400, 0);

    CHECK(detectPromByteOrder(bePadded) == PromByteOrder::BigEndian);
    CHECK(detectPromByteOrder(swapped16) == PromByteOrder::Swapped16);
    CHECK(detectPromByteOrder(le32) == PromByteOrder::LittleEndian32);
    CHECK(!detectPromByteOrder(beWords({0x3C08'BFC0})));

    TempFile file(swapped16);
    MemoryBus bus;
    const auto info = bus.loadProm(file.path);
    CHECK(info && info->orderDetected && info->sourceOrder == PromByteOrder::Swapped16);
    CHECK(bus.read32(physmap::kPromBase + 0) == 0x0BF0'00F0u);
    CHECK(bus.read32(physmap::kPromBase + 8) == 0x1122'3344u);
}

// A PROM image holding `program`, plus RAM, plus a reset CPU.
struct Harness {
    TempFile prom;
    MemoryBus bus;
    R5000 cpu;

    explicit Harness(std::initializer_list<uint32_t> program, CpuModel model = CpuModel::R5000)
        : Harness(std::vector<uint32_t>(program), model) {}
    explicit Harness(const std::vector<uint32_t>& program, CpuModel model = CpuModel::R5000)
        : prom(beWords(program)), cpu(bus, 0, model) {
        bus.installRam(physmap::kRamBase, 1 * MiB);
        if (!bus.loadProm(prom.path)) throw std::runtime_error("harness: PROM load failed");
        cpu.reset();
    }
    void step(int n) { for (int i = 0; i < n; ++i) cpu.tick(); }
};

void executesRegisterSetup() {
    Harness h({
        0x3C08'1234, // lui   $8, 0x1234
        0x3508'5678, // ori   $8, $8, 0x5678
        0x2409'FFFF, // addiu $9, $zero, -1
        0x2400'0005, // addiu $zero, $zero, 5   (must be discarded)
        0x3C0A'8000, // lui   $10, 0x8000       (sign-extends)
    });
    h.step(5);
    CHECK(h.cpu.gpr(8) == 0x1234'5678u);
    CHECK(h.cpu.gpr(9) == 0xFFFF'FFFF'FFFF'FFFFu);
    CHECK(h.cpu.gpr(0) == 0u);
    CHECK(h.cpu.gpr(10) == 0xFFFF'FFFF'8000'0000u);
    CHECK(h.cpu.instructionsExecuted() == 5);
}

void executesJumpsWithDelaySlots() {
    Harness h({
        0x0BF0'0004, // 00: j     0xbfc00010
        0x2408'0001, // 04: addiu $8, $zero, 1   (delay slot: executes)
        0x2409'0002, // 08: addiu $9, $zero, 2   (skipped)
        0x0000'0000, // 0c: nop
        0x0411'0002, // 10: bal   0xbfc0001c
        0x240A'0003, // 14: addiu $10, $zero, 3  (delay slot: executes)
        0x240B'0004, // 18: addiu $11, $zero, 4  (skipped)
        0x5008'0005, // 1c: beql  $zero, $8, ... (not taken: slot nullified)
        0x240C'0005, // 20: addiu $12, $zero, 5  (nullified)
        0x240D'0006, // 24: addiu $13, $zero, 6
    });
    h.step(6);
    CHECK(h.cpu.gpr(8) == 1u);
    CHECK(h.cpu.gpr(9) == 0u);
    CHECK(h.cpu.gpr(10) == 3u);
    CHECK(h.cpu.gpr(11) == 0u);
    CHECK(h.cpu.gpr(31) == 0xFFFF'FFFF'BFC0'0018u); // BAL links to branch + 8
    CHECK(h.cpu.gpr(12) == 0u);
    CHECK(h.cpu.gpr(13) == 6u);
    CHECK(h.cpu.pc() == 0xFFFF'FFFF'BFC0'0028u);
}

void executesLoadsAndStores() {
    Harness h({
        0x3C08'A800, // lui $8, 0xa800        (KSEG1 alias of RAM at 0x08000000)
        0x3C09'8765, // lui $9, 0x8765
        0x3529'4321, // ori $9, $9, 0x4321
        0xAD09'0000, // sw  $9, 0($8)
        0x8D0A'0000, // lw  $10, 0($8)
        0x9D0B'0000, // lwu $11, 0($8)
        0x910C'0000, // lbu $12, 0($8)
        0x810D'0000, // lb  $13, 0($8)
    });
    h.step(8);
    CHECK(h.bus.ram()[0] == 0x87 && h.bus.ram()[3] == 0x21); // big-endian in RAM
    CHECK(h.cpu.gpr(10) == 0xFFFF'FFFF'8765'4321u);
    CHECK(h.cpu.gpr(11) == 0x8765'4321u);
    CHECK(h.cpu.gpr(12) == 0x87u);
    CHECK(h.cpu.gpr(13) == 0xFFFF'FFFF'FFFF'FF87u);
    CHECK(!h.cpu.halted());
}

void unimplementedInstructionIsSkipped() {
    Harness h({
        0x7000'0000, // primary opcode 0x1C: reserved in MIPS III
        0x2408'0007, // addiu $8, $zero, 7
    });
    h.step(2);
    CHECK(!h.cpu.halted());
    CHECK(h.cpu.gpr(8) == 7u);
}

void busErrorOnUndecodedAccess() {
    Harness h({
        0x3C01'BFA0, // lui   $1, 0xbfa0   (nothing decodes 0x1FA00000 in this harness)
        0xAC20'00EC, // sw    $zero, 0xec($1)  -> posted write: execution continues
        0x2403'0001, // addiu $3, $zero, 1
        0x8C22'0004, // lw    $2, 4($1)        -> Data Bus Error exception
        0x2404'0001, // addiu $4, $zero, 1     (not reached)
    });
    std::vector<std::pair<PhysAddr, bool>> reported;
    h.cpu.setBusErrorHook([&](PhysAddr a, bool w) { reported.emplace_back(a, w); });
    h.step(4); // stop at the vector (the test PROM has no handler there)
    CHECK(h.cpu.gpr(3) == 1u);
    CHECK(((h.cpu.cop0(R5000::kCause) >> 2) & 0x1F) == 7);       // DBE
    CHECK(h.cpu.cop0(R5000::kEPC) == 0xFFFF'FFFF'BFC0'000Cu);
    CHECK(h.cpu.gpr(4) == 0u);
    CHECK(reported.size() == 2 && reported[0] == std::pair<PhysAddr, bool>{0x1FA0'00EC, true} &&
          reported[1] == std::pair<PhysAddr, bool>{0x1FA0'0004, false});
}

void cop0ConfigMatchesR5000() {
    Harness h({
        0x4002'8000, // mfc0 $2, $16 (Config)
        0x4080'8000, // mtc0 $zero, $16
        0x4003'8000, // mfc0 $3, $16
        0x4004'7800, // mfc0 $4, $15 (PRId)
    });
    h.step(4);
    CHECK((h.cpu.gpr(2) & 0x8000) != 0);   // BE: the PROM refuses to boot otherwise
    CHECK((h.cpu.gpr(2) & 0x2'0000) != 0); // SC: no secondary cache
    CHECK((h.cpu.gpr(3) & 0x7) == 0);      // K0 writable
    CHECK((h.cpu.gpr(3) & 0x8000) != 0);   // BE read-only
    CHECK(((h.cpu.gpr(4) >> 8) & 0xFF) == 0x23); // R5000 implementation number
}

// ---- Exceptions and TLB ------------------------------------------------------

constexpr uint64_t kBootGeneralVector = 0xFFFF'FFFF'BFC0'0380; // BEV=1, offset 0x180
constexpr uint64_t kBootRefillVector  = 0xFFFF'FFFF'BFC0'0200; // BEV=1, offset 0x000

uint64_t excCode(const R5000& cpu) { return (cpu.cop0(R5000::kCause) >> 2) & 0x1F; }

void misalignedLoadRaisesAddressError() {
    Harness h({
        0x3C08'A800, // lui $8, 0xa800
        0x8D09'0002, // lw  $9, 2($8)   -> AdEL
    });
    h.step(2);
    CHECK(!h.cpu.halted());
    CHECK(h.cpu.pc() == kBootGeneralVector);
    CHECK(excCode(h.cpu) == 4);
    CHECK(h.cpu.cop0(R5000::kEPC) == 0xFFFF'FFFF'BFC0'0004u);
    CHECK(h.cpu.cop0(R5000::kBadVAddr) == 0xFFFF'FFFF'A800'0002u);
    CHECK((h.cpu.cop0(R5000::kStatus) & 2) != 0); // EXL
}

void tlbTranslation() {
    Tlb tlb;
    // Global 4 KiB pair: VA 0x00400000 -> PA 0x08000000 (even), 0x08001000 (odd, read-only).
    tlb.write(0, 0, 0x0040'0000, (0x08000u << 6) | 0x7, (0x08001u << 6) | 0x3);
    auto r = tlb.translate(0x0040'0123, 5, false);
    CHECK(r.outcome == Tlb::Outcome::Hit && r.phys == 0x0800'0123);
    r = tlb.translate(0x0040'1FFC, 5, false);
    CHECK(r.outcome == Tlb::Outcome::Hit && r.phys == 0x0800'1FFC);
    CHECK(tlb.translate(0x0040'1000, 5, true).outcome == Tlb::Outcome::Modified); // D=0
    CHECK(tlb.translate(0x0040'2000, 5, false).outcome == Tlb::Outcome::Miss);

    // Non-global entry: ASID must match. Odd page invalid.
    tlb.write(1, 0, 0x1000'0000 | 0x22, (0x09000u << 6) | 0x6, 0);
    CHECK(tlb.translate(0x1000'0010, 0x22, false).outcome == Tlb::Outcome::Hit);
    CHECK(tlb.translate(0x1000'0010, 0x23, false).outcome == Tlb::Outcome::Miss);
    CHECK(tlb.translate(0x1000'1010, 0x22, false).outcome == Tlb::Outcome::Invalid);
    CHECK(tlb.probe(0x1000'0000 | 0x22) == 1u);
    CHECK(!tlb.probe(0x1000'0000 | 0x23));

    // 16 KiB pages (PageMask 0x6000): the pair spans 32 KiB, odd page starts at +16 KiB.
    tlb.write(2, 0x6000, 0x2000'0000, (0x0A000u << 6) | 0x7, (0x0B000u << 6) | 0x7);
    r = tlb.translate(0x2000'3ABC, 0, false);
    CHECK(r.outcome == Tlb::Outcome::Hit && r.phys == 0x0A00'3ABC);
    r = tlb.translate(0x2000'4004, 0, false);
    CHECK(r.outcome == Tlb::Outcome::Hit && r.phys == 0x0B00'0004);
}

void tlbRefillException() {
    Harness h({
        0x3C01'0040, // lui  $1, 0x0040
        0x4081'6000, // mtc0 $1, $12      Status = BEV (ERL cleared: kuseg now mapped)
        0x3C02'1234, // lui  $2, 0x1234
        0x8C43'5678, // lw   $3, 0x5678($2) -> VA 0x12345678: TLB miss
    });
    h.step(4);
    CHECK(h.cpu.pc() == kBootRefillVector);
    CHECK(excCode(h.cpu) == 2); // TLBL
    CHECK(h.cpu.cop0(R5000::kEPC) == 0xFFFF'FFFF'BFC0'000Cu);
    CHECK(h.cpu.cop0(R5000::kBadVAddr) == 0x1234'5678u);
    CHECK(h.cpu.cop0(R5000::kEntryHi) == 0x1234'4000u);                 // VPN2
    CHECK((h.cpu.cop0(R5000::kContext) & 0x7F'FFF0) == (0x12345678u >> 13) << 4); // BadVPN2
}

void tlbWriteThenAccess() {
    Harness h({
        0x3C01'0040, // lui  $1, 0x0040
        0x4081'6000, // mtc0 $1, $12          Status = BEV
        0x4080'2800, // mtc0 $zero, $5        PageMask = 4 KiB
        0x3C02'0040, // lui  $2, 0x0040
        0x4082'5000, // mtc0 $2, $10          EntryHi = VPN2 0x00400000, ASID 0
        0x3C03'0020, // lui  $3, 0x0020
        0x3463'0007, // ori  $3, $3, 7
        0x4083'1000, // mtc0 $3, $2           EntryLo0 = PA 0x08000000, D V G
        0x3C04'0020, // lui  $4, 0x0020
        0x3484'0047, // ori  $4, $4, 0x47
        0x4084'1800, // mtc0 $4, $3           EntryLo1 = PA 0x08001000, D V G
        0x4080'0000, // mtc0 $zero, $0        Index = 0
        0x4200'0002, // tlbwi
        0x3C05'1234, // lui  $5, 0x1234
        0xAC45'1000, // sw   $5, 0x1000($2)   VA 0x00401000 -> PA 0x08001000
        0x4200'0008, // tlbp
        0x4006'0000, // mfc0 $6, $0           Index after probe
        0x4007'0800, // mfc0 $7, $1           Random
    });
    h.step(18);
    CHECK(excCode(h.cpu) == 0 && (h.cpu.cop0(R5000::kStatus) & 2) == 0); // no exception taken
    CHECK(h.bus.ram()[0x1000] == 0x12 && h.bus.ram()[0x1001] == 0x34);
    CHECK(h.cpu.gpr(6) == 0u);
    CHECK(h.cpu.gpr(7) <= 47u);
    CHECK(h.cpu.tlb().entry(0).global);
}

// Remapping a page with TLBWI must not leave a stale micro-TLB translation.
void tlbRewriteInvalidatesMicroTlb() {
    Harness h({
        0x3C01'0040, // lui  $1, 0x0040
        0x4081'6000, // mtc0 $1, $12          Status = BEV
        0x4080'2800, // mtc0 $zero, $5        PageMask = 4 KiB
        0x3C02'0040, // lui  $2, 0x0040
        0x4082'5000, // mtc0 $2, $10          EntryHi = VPN2 0x00400000
        0x3C03'0020, // lui  $3, 0x0020
        0x3463'0007, // ori  $3, $3, 7
        0x4083'1000, // mtc0 $3, $2           EntryLo0 = PA 0x08000000, D V G
        0x4080'1800, // mtc0 $zero, $3        EntryLo1 invalid
        0x4080'0000, // mtc0 $zero, $0        Index = 0
        0x4200'0002, // tlbwi
        0x3C05'1234, // lui  $5, 0x1234
        0xAC45'0000, // sw   $5, 0($2)        -> PA 0x08000000
        0x8C46'0000, // lw   $6, 0($2)        cached translation
        0x3463'0080, // ori  $3, $3, 0x80     EntryLo0 = PA 0x08002000
        0x4083'1000, // mtc0 $3, $2
        0x4200'0002, // tlbwi                 same VPN, new frame
        0x8C47'0000, // lw   $7, 0($2)        must see the new frame
        0xAC45'0004, // sw   $5, 4($2)        -> PA 0x08002004
    });
    h.step(19);
    CHECK(excCode(h.cpu) == 0 && (h.cpu.cop0(R5000::kStatus) & 2) == 0);
    CHECK(h.cpu.gpr(6) == 0x1234'0000u);
    CHECK(h.cpu.gpr(7) == 0u);
    CHECK(h.bus.ram()[0x2004] == 0x12 && h.bus.ram()[0x0004] == 0x00);
}

// Random counts down from 47 to Wired once per instruction; Count advances
// every other instruction; Count reaching Compare raises IP7. (Computed
// lazily from an instruction counter, so pin the exact sequence.)
void cp0TimersRandomCountCompare() {
    std::vector<uint32_t> program = {
        0x3C01'0040, // 0: lui  $1, 0x0040
        0x4081'6000, // 1: mtc0 $1, $12        Status = BEV (interrupts off)
    };
    program.resize(10, 0);                       // 2..9: nop
    program.push_back(0x2402'0028);              // 10: addiu $2, $zero, 40
    program.push_back(0x4082'3000);              // 11: mtc0 $2, $6   Wired = 40
    program.resize(19, 0);                       // 12..18: nop
    program.push_back(0x2403'000D);              // 19: addiu $3, $zero, 13
    program.push_back(0x4083'5800);              // 20: mtc0 $3, $11  Compare = 13
    program.resize(40, 0);
    Harness h(program);
    CHECK(h.cpu.cop0(R5000::kRandom) == 47u && h.cpu.cop0(R5000::kCount) == 0u);
    h.step(1);
    CHECK(h.cpu.cop0(R5000::kRandom) == 46u && h.cpu.cop0(R5000::kCount) == 0u);
    h.step(9);                                   // 10 instructions
    CHECK(h.cpu.cop0(R5000::kCount) == 5u && h.cpu.cop0(R5000::kRandom) == 37u);
    h.step(2);                                   // Wired = 40: Random restarts at 47
    CHECK(h.cpu.cop0(R5000::kRandom) == 46u);
    h.step(7);                                   // 46..40, then back to 47
    CHECK(h.cpu.cop0(R5000::kRandom) == 47u);
    h.step(2);                                   // Compare = 13 written at Count 10
    CHECK((h.cpu.cop0(R5000::kCause) & 0x8000) == 0);
    h.step(4);                                   // Count 12
    CHECK(h.cpu.cop0(R5000::kCount) == 12u && (h.cpu.cop0(R5000::kCause) & 0x8000) == 0);
    h.step(1);                                   // Count 13 = Compare
    CHECK(h.cpu.cop0(R5000::kCount) == 13u && (h.cpu.cop0(R5000::kCause) & 0x8000) != 0);
}

// MIPS IV on the R5000: conditional moves, FP condition codes 1-7, COP1X
// multiply-add and indexed load/store, RECIP. The R4600 does not have them.
void r5000MipsIvInstructions() {
    const std::initializer_list<uint32_t> program = {
        0x3C01'2440, // lui    $1, 0x2440        CU1 | FR | BEV
        0x4081'6000, // mtc0   $1, $12
        0x2402'0005, // addiu  $2, $zero, 5
        0x2403'0000, // addiu  $3, $zero, 0
        0x0043'200A, // movz   $4, $2, $3        $3 == 0: $4 = 5
        0x0043'280B, // movn   $5, $2, $3        $3 == 0: no move
        0x2406'0002, // addiu  $6, $zero, 2
        0x4486'1000, // mtc1   $6, $f2
        0x4680'10A1, // cvt.d.w $f2, $f2         2.0
        0x2407'0003, // addiu  $7, $zero, 3
        0x4487'2000, // mtc1   $7, $f4
        0x4680'2121, // cvt.d.w $f4, $f4         3.0
        0x2408'0001, // addiu  $8, $zero, 1
        0x4488'3000, // mtc1   $8, $f6
        0x4680'31A1, // cvt.d.w $f6, $f6         1.0
        0x4CC4'1221, // madd.d  $f8, $f6, $f2, $f4    2*3 + 1 = 7
        0x4CC4'12B9, // nmsub.d $f10, $f6, $f2, $f4   -(2*3 - 1) = -5
        0x4624'133C, // c.lt.d  $fcc3, $f2, $f4       2 < 3
        0x004D'4801, // movt   $9, $2, $fcc3     -> 5
        0x004C'5001, // movf   $10, $2, $fcc3    -> no move
        0x4620'2315, // recip.d $f12, $f4        1/3
        0x3C0B'A800, // lui    $11, 0xa800       RAM through KSEG1
        0x4D63'4009, // sdxc1  $f8, $3($11)
        0x4D63'0381, // ldxc1  $f14, $3($11)
        0x442C'7000, // dmfc1  $12, $f14
        0x442D'5000, // dmfc1  $13, $f10
        0x442E'6000, // dmfc1  $14, $f12
    };
    Harness h(program, CpuModel::R5000);
    h.step(static_cast<int>(program.size()));
    CHECK(h.cpu.cop0(R5000::kPRId) == 0x2321u);
    CHECK(h.cpu.gpr(4) == 5u && h.cpu.gpr(5) == 0u);
    CHECK(h.cpu.gpr(9) == 5u && h.cpu.gpr(10) == 0u);
    CHECK((h.cpu.fcr31() & (1u << 27)) != 0 && (h.cpu.fcr31() & (1u << 23)) == 0); // cc3, not cc0
    CHECK(h.cpu.gpr(12) == std::bit_cast<uint64_t>(7.0));
    CHECK(h.cpu.gpr(13) == std::bit_cast<uint64_t>(-5.0));
    CHECK(h.cpu.gpr(14) == std::bit_cast<uint64_t>(1.0 / 3.0));
    CHECK(excCode(h.cpu) == 0 && (h.cpu.cop0(R5000::kStatus) & 2) == 0);

    Harness old(program, CpuModel::R4600);                       // MIPS III: MOVZ is not an instruction
    old.step(6);
    CHECK(old.cpu.cop0(R5000::kPRId) == 0x2020u && old.cpu.gpr(4) == 0u);
}

void exceptionInDelaySlotSetsBD() {
    Harness h({
        0x3C01'0040, // lui  $1, 0x0040
        0x4081'6000, // mtc0 $1, $12     Status = BEV
        0x0BF0'0040, // j    0xbfc00100
        0x8C02'4000, // lw   $2, 0x4000($zero) (delay slot: TLB miss)
    });
    h.step(4);
    CHECK(h.cpu.pc() == kBootRefillVector);
    CHECK(h.cpu.cop0(R5000::kEPC) == 0xFFFF'FFFF'BFC0'0008u); // the jump, not the slot
    CHECK((h.cpu.cop0(R5000::kCause) >> 31) == 1);           // BD
}

void eretReturnsToEpc() {
    Harness h({
        0x3C01'BFC0, // lui  $1, 0xbfc0
        0x3421'0020, // ori  $1, $1, 0x20
        0x4081'7000, // mtc0 $1, $14       EPC = 0xbfc00020
        0x3C02'0040, // lui  $2, 0x0040
        0x3442'0002, // ori  $2, $2, 2
        0x4082'6000, // mtc0 $2, $12       Status = BEV | EXL
        0x4200'0018, // eret
    });
    h.step(7);
    CHECK(h.cpu.pc() == 0xFFFF'FFFF'BFC0'0020u);
    CHECK((h.cpu.cop0(R5000::kStatus) & 2) == 0);
}

void syscallAndCoprocessorUnusable() {
    Harness sys({0x0000'000C}); // syscall
    sys.step(1);
    CHECK(sys.cpu.pc() == kBootGeneralVector);
    CHECK(excCode(sys.cpu) == 8);
    CHECK(sys.cpu.cop0(R5000::kEPC) == mips::kResetVector);

    Harness cpu({0xC400'0000}); // lwc1 $f0, 0($zero) with CU1 = 0
    cpu.step(1);
    CHECK(excCode(cpu.cpu) == 11);
    CHECK(((cpu.cpu.cop0(R5000::kCause) >> 28) & 3) == 1);
}

void timerInterrupt() {
    Harness h({
        0x3C01'0040, // lui  $1, 0x0040
        0x3421'8001, // ori  $1, $1, 0x8001   BEV | IM7 | IE
        0x4080'4800, // mtc0 $zero, $9        Count = 0
        0x2402'0003, // addiu $2, $zero, 3
        0x4082'5800, // mtc0 $2, $11          Compare = 3
        0x4081'6000, // mtc0 $1, $12          enable
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    });
    bool taken = false;
    for (int i = 0; i < 16 && !taken; ++i) {
        h.cpu.tick();
        taken = h.cpu.pc() == kBootGeneralVector;
    }
    CHECK(taken);
    CHECK(excCode(h.cpu) == 0);                         // Int
    CHECK((h.cpu.cop0(R5000::kCause) & 0x8000) != 0);   // IP7 (timer)
}

// ---- Memory controller ---------------------------------------------------------

constexpr PhysAddr kMc = physmap::kMcBase;

// Bus + MC with a hand-driven clock. A 50 MHz "CPU" makes the RPSS maths
// exact: with the reset divider (divide by 5) the counter ticks every 5 cycles.
struct McHarness {
    MemoryBus bus;
    Cycles clock = 0;
    std::unique_ptr<MemoryController> mc;

    explicit McHarness(std::array<uint64_t, MemoryController::kBanks> banks) {
        uint64_t total = 0;
        for (uint64_t b : banks) total += b;
        bus.allocateRam(total);
        mc = std::make_unique<MemoryController>(
            bus, MemoryController::Config{.bankBytes = banks, .cpuClockHz = 50'000'000},
            [this] { return clock; });
        bus.mapDevice(physmap::kMcBase, physmap::kMcSize, *mc);
    }
};

void mcSysIdAndSlotAliasing() {
    McHarness h({16 * MiB});
    CHECK(h.bus.read32(kMc + 0x1C) == MemoryController::kIndySysId); // word the PROM uses
    CHECK(h.bus.read32(kMc + 0x18) == MemoryController::kIndySysId); // same 8-byte slot
    CHECK((*h.bus.read32(kMc + 0x1C) & 0x10) == 0);                  // Indy: no EISA
    CHECK(h.bus.write32(kMc + 0x1C, 0xFF));                          // read-only
    CHECK(h.bus.read32(kMc + 0x1C) == MemoryController::kIndySysId);

    CHECK(h.bus.write32(kMc + 0x04, 0x1234'5678));                   // CPUCTRL0
    CHECK(h.bus.read32(kMc + 0x00) == 0x1234'5678u);
    CHECK(h.mc->peek(MemoryController::kCpuCtrl0) == 0x1234'5678u);
    CHECK(h.bus.read32(kMc + 0x2C) == 0x0104u);                      // RPSS divider reset value
}

void mcRpssCounter() {
    McHarness h({16 * MiB});
    CHECK(h.bus.read32(kMc + 0x1004) == 0u);
    h.clock = 50;                                    // 10 ticks of 100 ns
    CHECK(h.bus.read32(kMc + 0x1004) == 10u);
    CHECK(h.bus.read32(kMc + 0x1000) == 10u);        // slot alias
    h.clock = 52;                                    // fraction carried, not lost
    CHECK(h.bus.read32(kMc + 0x1004) == 10u);
    h.clock = 55;
    CHECK(h.bus.read32(kMc + 0x1004) == 11u);

    CHECK(h.bus.write32(kMc + 0x2C, 0x0200));        // divide by 1, increment 2
    h.clock = 65;                                    // 10 cycles -> 10 ticks x 2
    CHECK(h.bus.read32(kMc + 0x1004) == 31u);
    CHECK(h.bus.write32(kMc + 0x1004, 0));           // read-only
    CHECK(h.bus.read32(kMc + 0x1004) == 31u);
}

void mcMemoryConfigMapsAndMirrors() {
    McHarness h({16 * MiB, 16 * MiB});
    CHECK(h.bus.read32(physmap::kRamBase) == 0u);    // no bank valid: segment reads float

    // Bank A: valid, 64 MiB window (size field 15) at 0x08000000 (base 0x20).
    CHECK(h.bus.write32(kMc + 0xC4, 0x2F20'0000));
    CHECK(h.bus.write32(physmap::kRamBase, 0xCAFE'F00D));
    CHECK(h.bus.read32(physmap::kRamBase + 16 * MiB) == 0xCAFE'F00Du); // 16 MiB bank mirrors
    CHECK(h.bus.read32(physmap::kRamBase + 48 * MiB) == 0xCAFE'F00Du);
    CHECK(h.bus.read32(physmap::kLowAliasBase) == 0xCAFE'F00Du);       // low 512 KiB alias
    CHECK(!h.bus.read32(physmap::kLowAliasSize));                      // alias is only 512 KiB
    CHECK(h.bus.read32(physmap::kRamBase + 64 * MiB) == 0u);           // past the window: floats

    // Banks A and B side by side, 16 MiB each (size field 3), B at 0x09000000.
    CHECK(h.bus.write32(kMc + 0xC4, 0x2320'2324));
    CHECK(h.bus.write32(physmap::kRamBase + 16 * MiB, 0x1111'2222));
    CHECK(h.bus.read32(physmap::kRamBase) == 0xCAFE'F00Du);            // A untouched
    CHECK(h.bus.ram()[16 * MiB] == 0x11);                              // B is storage +16 MiB

    CHECK(h.bus.write32(kMc + 0xC4, 0));                               // invalidate both
    CHECK(h.bus.read32(physmap::kRamBase) == 0u);                      // RAM gone, segment floats
    CHECK(!h.bus.read32(physmap::kLowAliasBase));                      // alias gone: undecoded
    CHECK(!h.bus.read32(physmap::kMemSeg1Base + physmap::kMemSeg1Size)); // beyond segment 1
}

// SBANKS (two sub-banks): 128 MiB banks of double-sided SIMMs fill their whole
// window; a bank no larger than half the window has only the first sub-bank.
// (The PROM sizes banks this way; halving every SBANKS window made 256 MiB
// machines show 128 MiB.)
void mcSubbankWindows() {
    McHarness h({128 * MiB, 128 * MiB});
    // A: valid | SBANKS | 128 MiB (size 31) at 0x08000000; B: same at 0x10000000.
    CHECK(h.bus.write32(kMc + 0xC4, 0x7F20'7F40));
    CHECK(h.bus.write32(physmap::kRamBase + 100 * MiB, 0xAAAA'0001));     // second half of A
    CHECK(h.bus.write32(0x1000'0000 + 100 * MiB, 0xBBBB'0002));           // second half of B
    CHECK(h.bus.ram()[100 * MiB] == 0xAA);
    CHECK(h.bus.ram()[128 * MiB + 100 * MiB] == 0xBB);

    McHarness single({64 * MiB});
    CHECK(single.bus.write32(kMc + 0xC4, 0x7F20'0000));                   // SBANKS, 128 MiB window
    CHECK(single.bus.write32(physmap::kRamBase, 0x1234'5678));
    CHECK(single.bus.read32(physmap::kRamBase + 64 * MiB) == 0u);          // no second sub-bank: floats
}

void mcSemaphoresAndClearOnWrite() {
    McHarness h({16 * MiB});
    CHECK(h.bus.read32(kMc + 0x10004) == 0u);        // semaphore 0: free, now taken
    CHECK(h.bus.read32(kMc + 0x10004) == 1u);
    CHECK(h.bus.read32(kMc + 0x11004) == 0u);        // semaphore 1 independent
    CHECK(h.bus.write32(kMc + 0x10004, 0));          // release
    CHECK(h.bus.read32(kMc + 0x10004) == 0u);

    CHECK(h.bus.write32(kMc + 0xEC, 0xFFFF'FFFF));   // CPU error status: write clears
    CHECK(h.bus.read32(kMc + 0xEC) == 0u);
}

void mcBankLayout() {
    using Banks = std::array<uint64_t, MemoryController::kBanks>;
    CHECK(MemoryController::bankLayoutFor(64 * MiB) == Banks{64 * MiB, 0, 0, 0});
    CHECK(MemoryController::bankLayoutFor(96 * MiB) == Banks{64 * MiB, 32 * MiB, 0, 0});
    CHECK(MemoryController::bankLayoutFor(256 * MiB) == Banks{128 * MiB, 128 * MiB, 0, 0});
    CHECK(!MemoryController::bankLayoutFor(100 * MiB));
    CHECK(!MemoryController::bankLayoutFor(384 * MiB));
}

// ---- HPC3 / IOC2 / Z85C30 --------------------------------------------------------

void sccPointerProtocolAndTransmit() {
    std::string sent;
    Z8530 scc([&](Z8530::Channel ch, uint8_t c) { if (ch == Z8530::Channel::A) sent.push_back(static_cast<char>(c)); });

    CHECK((scc.readControl(Z8530::Channel::A) & Z8530::kRr0TxEmpty) != 0); // RR0 by default

    scc.writeControl(Z8530::Channel::A, 12);   // WR0: point at WR12
    scc.writeControl(Z8530::Channel::A, 0x0A); // WR12 = baud constant low
    scc.writeControl(Z8530::Channel::A, 12);   // point at RR12
    CHECK(scc.readControl(Z8530::Channel::A) == 0x0A);
    scc.writeControl(Z8530::Channel::A, 0x08 | 7); // "point high" + 7 -> WR15
    scc.writeControl(Z8530::Channel::A, 0x55);
    scc.writeControl(Z8530::Channel::A, 7);        // without point high: RR7
    CHECK(scc.readControl(Z8530::Channel::A) == 0);// RR7 (image of RR3): no interrupts
    scc.writeControl(Z8530::Channel::A, 0x08 | 7); // RR15 needs point high
    CHECK(scc.readControl(Z8530::Channel::A) == 0x55);

    scc.writeData(Z8530::Channel::A, 'O');
    scc.writeData(Z8530::Channel::A, 'K');
    scc.writeData(Z8530::Channel::B, '!');         // other channel, filtered above
    CHECK(sent == "OK");

    scc.receive(Z8530::Channel::B, 'x');
    CHECK((scc.readControl(Z8530::Channel::B) & Z8530::kRr0RxAvailable) != 0);
    CHECK(scc.readData(Z8530::Channel::B) == 'x');
    CHECK((scc.readControl(Z8530::Channel::B) & Z8530::kRr0RxAvailable) == 0);

    scc.writeControl(Z8530::Channel::A, 9);        // WR9 = force hardware reset
    scc.writeControl(Z8530::Channel::A, 0xC0);
    scc.writeControl(Z8530::Channel::A, 12);
    CHECK(scc.readControl(Z8530::Channel::A) == 0);// WR12 cleared by the reset
}

// IOC2 behind HPC3 PBUS channel 6, as on the board.
struct HpcHarness {
    std::string serial;
    MemoryBus bus;
    Ioc2 ioc2{[this](Z8530::Channel, uint8_t c) { serial.push_back(static_cast<char>(c)); }};
    Hpc3 hpc3{bus};

    HpcHarness() {
        hpc3.attachPbusDevice(Hpc3::kIoc2Channel, ioc2);
        bus.mapDevice(physmap::kHpc3Base, physmap::kHpc3Size, hpc3);
    }
    bool writeByte(PhysAddr a, uint8_t v) { return bus.write<uint8_t>(a, v); }
    uint8_t readByte(PhysAddr a) { uint8_t v = 0; (void)bus.read(a, v); return v; }
};

void ioc2SerialAndIdsThroughBus() {
    HpcHarness h;
    // Byte accesses at slot + 3, exactly as the PROM does them.
    CHECK((h.readByte(0x1FBD'9833) & Z8530::kRr0TxEmpty) != 0); // channel B RR0
    CHECK(h.writeByte(0x1FBD'983F, 'H'));                        // channel A data
    CHECK(h.writeByte(0x1FBD'9837, 'i'));                        // channel B data
    CHECK(h.bus.write32(0x1FBD'983C, 0x0000'0021));              // word write: low byte
    CHECK(h.serial == "Hi!");

    CHECK(h.readByte(0x1FBD'985B) == Ioc2::kGuinnessSysId);      // Indy board ID
    CHECK((h.readByte(0x1FBD'985B) & 0x01) == 0);                 // not Full House
    CHECK(h.bus.read32(0x1FBD'9858) == uint32_t{Ioc2::kGuinnessSysId});
    CHECK(h.readByte(0x1FBD'9853) == Ioc2::kPanelResetValue);    // front panel

    CHECK(h.writeByte(0x1FBD'9887, 0x5A));                        // INT3 local0 mask
    CHECK(h.readByte(0x1FBD'9887) == 0x5A);
    CHECK(h.writeByte(0x1FBD'985B, 0xFF));                        // board ID read-only
    CHECK(h.readByte(0x1FBD'985B) == Ioc2::kGuinnessSysId);
}

void hpc3RegisterFiles() {
    HpcHarness h;
    // The PROM's walking-ones register test at 0x1FB94004 (Ethernet DMA register).
    for (uint32_t bit = 0x8000'0000; bit != 0; bit >>= 1) {
        CHECK(h.bus.write32(0x1FB9'4004, bit));
        CHECK(h.bus.read32(0x1FB9'4004) == bit);
    }
    CHECK(h.bus.write32(0x1FBD'C200, 0x1234'5678));              // PBUS DMA config
    CHECK(h.bus.read32(0x1FBD'C200) == 0x1234'5678u);
    CHECK(h.readByte(0x1FBD'C203) == 0x78);                       // big-endian byte lane
    CHECK(h.bus.read32(0x1FBB'0000) == 0u);                       // INTSTAT: nothing pending
}

// ---- COP1 ---------------------------------------------------------------------------

void fpuMovesWithFr0Pairing() {
    Harness h({
        0x3C01'2040, // lui   $1, 0x2040      CU1 | BEV, FR = 0
        0x4081'6000, // mtc0  $1, $12
        0x3C02'8765, // lui   $2, 0x8765
        0x4482'0800, // mtc1  $2, $f1         FR=0: upper half of $f0
        0x2403'1234, // addiu $3, $zero, 0x1234
        0x4483'0000, // mtc1  $3, $f0         lower half of $f0
        0x4404'0800, // mfc1  $4, $f1
        0x4425'0000, // dmfc1 $5, $f0
        0x4446'0000, // cfc1  $6, $0          FCR0
        0x3C07'0100, // lui   $7, 0x0100      FS
        0x34E7'0003, // ori   $7, $7, 3       RM = 3
        0x44C7'F800, // ctc1  $7, $31
        0x4448'F800, // cfc1  $8, $31
    });
    h.step(13);
    CHECK(h.cpu.fpr(0) == 0x8765'0000'0000'1234u);
    CHECK(h.cpu.gpr(4) == 0xFFFF'FFFF'8765'0000u); // MFC1 sign-extends
    CHECK(h.cpu.gpr(5) == 0x8765'0000'0000'1234u);
    CHECK(h.cpu.gpr(6) == R5000::kFcr0R5000);
    CHECK(h.cpu.gpr(8) == 0x0100'0003u);
    CHECK(excCode(h.cpu) == 0 && (h.cpu.cop0(R5000::kStatus) & 2) == 0);
}

void fpuMovesWithFr1() {
    Harness h({
        0x3C01'2440, // lui   $1, 0x2440      CU1 | FR | BEV
        0x4081'6000, // mtc0  $1, $12
        0x2402'0007, // addiu $2, $zero, 7
        0x4482'0800, // mtc1  $2, $f1         FR=1: its own register
        0x4404'0000, // mfc1  $4, $f0
    });
    h.step(5);
    CHECK(h.cpu.fpr(1) == 7u);
    CHECK(h.cpu.fpr(0) == 0u);
    CHECK(h.cpu.gpr(4) == 0u);
}

void fpuLoadStore() {
    Harness h({
        0x3C01'2040, // lui  $1, 0x2040
        0x4081'6000, // mtc0 $1, $12
        0x3C08'A800, // lui  $8, 0xa800       RAM via KSEG1
        0x3C02'1122, // lui  $2, 0x1122
        0x3442'3344, // ori  $2, $2, 0x3344
        0x4482'1000, // mtc1 $2, $f2
        0x3C03'5566, // lui  $3, 0x5566
        0x3463'7788, // ori  $3, $3, 0x7788
        0x4483'1800, // mtc1 $3, $f3          upper half of the $f2 pair
        0xF502'0000, // sdc1 $f2, 0($8)
        0xC504'0004, // lwc1 $f4, 4($8)
        0x4409'2000, // mfc1 $9, $f4
    });
    h.step(12);
    CHECK(h.bus.ram()[0] == 0x55 && h.bus.ram()[4] == 0x11); // big-endian doubleword
    CHECK(h.cpu.gpr(9) == 0x1122'3344u);
}

void fpuBranchAndFpe() {
    Harness h({
        0x3C01'2040, // 00: lui  $1, 0x2040
        0x4081'6000, // 04: mtc0 $1, $12
        0x3C02'0080, // 08: lui  $2, 0x0080    C bit
        0x44C2'F800, // 0c: ctc1 $2, $31
        0x4501'0002, // 10: bc1t +2 -> 0x1c
        0x2403'0001, // 14: addiu $3, $zero, 1 (delay slot)
        0x2404'0002, // 18: addiu $4, $zero, 2 (skipped)
        0x2405'0003, // 1c: addiu $5, $zero, 3
    });
    h.step(7);
    CHECK(h.cpu.gpr(3) == 1u && h.cpu.gpr(4) == 0u && h.cpu.gpr(5) == 3u);

    Harness fpe({
        0x3C01'2040, // lui  $1, 0x2040
        0x4081'6000, // mtc0 $1, $12
        0x3C02'0001, // lui  $2, 0x0001       cause V
        0x3442'0800, // ori  $2, $2, 0x0800   enable V
        0x44C2'F800, // ctc1 $2, $31          -> floating-point exception
    });
    fpe.step(5);
    CHECK(excCode(fpe.cpu) == 15);
}

void fpuArithmeticAndCompare() {
    Harness h({
        0x3C01'2040, // lui    $1, 0x2040     CU1 | BEV, FR = 0
        0x4081'6000, // mtc0   $1, $12
        0x2402'FFFD, // addiu  $2, $zero, -3
        0x4482'1000, // mtc1   $2, $f2
        0x4680'1121, // cvt.d.w $f4, $f2      -3.0
        0x4624'2180, // add.d  $f6, $f4, $f4  -6.0
        0x4624'303C, // c.lt.d $f6, $f4       -6 < -3
        0x4425'3000, // dmfc1  $5, $f6
        0x2403'0400, // addiu  $3, $zero, 0x400  enable Z
        0x44C3'F800, // ctc1   $3, $31
        0x4620'3203, // div.d  $f8, $f6, $f0  -6 / 0 -> trap
    });
    h.step(8);
    CHECK(h.cpu.gpr(5) == 0xC018'0000'0000'0000u);
    CHECK((h.cpu.fcr31() & 0x0080'0000) != 0); // C set
    CHECK(excCode(h.cpu) == 0);
    h.step(3);
    CHECK(excCode(h.cpu) == 15);               // FPE
    CHECK((h.cpu.fcr31() & 0x0000'8000) != 0); // cause Z
    CHECK((h.cpu.fcr31() & 0x0000'0020) == 0); // flag Z not set when trapping
    CHECK(h.cpu.fpr(8) == 0u);                 // destination untouched
}

void fpuOperationsIeee() {
    using namespace fpu;
    auto d = [](double v) { return std::bit_cast<uint64_t>(v); };
    auto f = [](float v) { return uint64_t{std::bit_cast<uint32_t>(v)}; };
    auto asD = [](uint64_t v) { return std::bit_cast<double>(v); };

    Result r = operate(0x00, kDouble, d(1.5), d(2.25), 0);                // add.d
    CHECK(asD(r.value) == 3.75 && r.cause == 0 && r.wide);
    r = operate(0x03, kDouble, d(1.0), d(0.0), 0);                        // div.d by zero
    CHECK(std::isinf(asD(r.value)) && r.cause == kDivideByZero);
    r = operate(0x04, kDouble, d(-1.0), 0, 0);                            // sqrt(-1)
    CHECK(r.value == kDefaultNanD && r.cause == kInvalid);
    r = operate(0x02, kSingle, f(1e38f), f(10.0f), 0);                    // mul.s overflow
    CHECK(r.value == 0x7F80'0000u && r.cause == (kOverflow | kInexact) && !r.wide);
    r = operate(0x00, kSingle, 0x7FC0'0000, f(1.0f), 0);                  // MIPS sNaN operand
    CHECK(r.value == kDefaultNanS && r.cause == kInvalid);
    r = operate(0x00, kSingle, 0x7F80'0001, f(1.0f), 0);                  // MIPS qNaN propagates
    CHECK(r.value == 0x7F80'0001u && r.cause == 0);
    r = operate(0x05, kDouble, d(-2.0), 0, 0);                            // abs.d
    CHECK(asD(r.value) == 2.0);
    r = operate(0x07, kSingle, f(2.0f), 0, 0);                            // neg.s
    CHECK(r.value == f(-2.0f));

    // Rounding modes: 1/3 rounded towards zero vs upward differ by one ulp.
    const uint64_t rz = operate(0x03, kDouble, d(1.0), d(3.0), 1).value;
    const uint64_t rp = operate(0x03, kDouble, d(1.0), d(3.0), 2).value;
    CHECK(rp == rz + 1);

    // Conversions.
    r = operate(0x21, kWord, 0xFFFF'FFFB, 0, 0);                          // cvt.d.w -5
    CHECK(asD(r.value) == -5.0 && r.wide);
    r = operate(0x20, kDouble, d(0.1), 0, 0);                             // cvt.s.d inexact
    CHECK(r.value == f(0.1f) && r.cause == kInexact);
    r = operate(0x24, kDouble, d(2.5), 0, 0);                             // cvt.w.d, RN: ties to even
    CHECK(r.value == 2u && r.cause == kInexact && !r.wide);
    r = operate(0x24, kDouble, d(2.5), 0, 3);                             // cvt.w.d, RM
    CHECK(r.value == 2u);
    r = operate(0x0D, kDouble, d(-2.7), 0, 0);                            // trunc.w.d
    CHECK(r.value == 0xFFFF'FFFEu);
    r = operate(0x0E, kSingle, f(1.2f), 0, 0);                            // ceil.w.s
    CHECK(r.value == 2u);
    r = operate(0x0B, kDouble, d(-1.5), 0, 0);                            // floor.l.d
    CHECK(r.value == 0xFFFF'FFFF'FFFF'FFFEu && r.wide);
    r = operate(0x24, kDouble, d(3e9), 0, 0);                             // out of range
    CHECK(r.value == 0x7FFF'FFFFu && r.cause == kInvalid);
    r = operate(0x25, kDouble, d(-3e9), 0, 0);                            // cvt.l.d
    CHECK(r.value == static_cast<uint64_t>(int64_t{-3'000'000'000}));
    r = operate(0x20, kSingle, f(1.0f), 0, 0);                            // cvt.s.s reserved
    CHECK(r.cause == kUnimplemented);
    r = operate(0x00, kWord, 1, 2, 0);                                    // add.w reserved
    CHECK(r.cause == kUnimplemented);

    // FS: a denormal result flushes to zero.
    r = operate(0x02, kSingle, f(1e-30f), f(1e-10f), kFcr31FlushToZero);
    CHECK(r.value == 0u && (r.cause & kUnderflow) != 0);

    // Compares: lt, ueq with a NaN, signalling compare.
    CHECK(compare(0xC, kDouble, d(1.0), d(2.0)).condition);              // c.lt
    CHECK(!compare(0xC, kDouble, d(2.0), d(2.0)).condition);
    CHECK(compare(0xE, kDouble, d(2.0), d(2.0)).condition);              // c.le
    Comparison c = compare(0x3, kDouble, kDefaultNanD, d(1.0));           // c.ueq: unordered
    CHECK(c.condition && c.cause == 0);
    c = compare(0xA, kDouble, kDefaultNanD, d(1.0));                      // c.seq: signals
    CHECK(!c.condition && c.cause == kInvalid);
    CHECK(compare(0x2, kSingle, f(-0.0f), f(0.0f)).condition);            // -0 == +0
}

// The PROM's own sequence (bfc01b50 / bfc01a0c): park a value in $f2 with no
// RAM available, read it back as the TLB-init loop bound.
void fpuPromScratchRegisterPattern() {
    Harness h({
        0x3C01'2040, // lui  $1, 0x2040
        0x4081'6000, // mtc0 $1, $12
        0x3C02'0800, // lui  $2, 0x800
        0x4482'1000, // mtc1 $2, $f2
        0x4402'1000, // mfc1 $2, $f2
        0x0002'1642, // srl  $2, $2, 25
    });
    h.step(6);
    CHECK(h.cpu.gpr(2) == 4u); // a sane loop bound instead of 2^32 iterations
}

// ---- MC virtual DMA -------------------------------------------------------------------

// The PROM's "VDMA Clear" sequence (bfc01244..bfc012dc), with a pattern instead of 0.
void mcDmaFillLikePromAndStatusLatency() {
    McHarness h({16 * MiB});
    CHECK(h.bus.write32(kMc + 0xC4, 0x2320'0000));    // bank A: 16 MiB at 0x08000000
    CHECK(h.bus.write32(kMc + 0x16C, 0));             // DMA_CTRL
    CHECK(h.bus.write32(kMc + 0x2004, 0x0800'0000));  // memory address (physical, no DMA TLB)
    CHECK(h.bus.write32(kMc + 0x2034, 0x5A));         // ToHost | Fill | Incr | 0x40
    CHECK(h.bus.write32(kMc + 0x201C, 0x0001'0000));  // zoom 1, stride 0
    CHECK(h.bus.write32(kMc + 0x2014, 0x0002'0010));  // 2 lines x 16 bytes
    CHECK(h.bus.write32(kMc + 0x202C, 0xDEAD'BEEF));  // fill pattern + start

    CHECK(h.bus.read32(kMc + 0x204C) == MemoryController::kDmaRunActive); // PROM checks "started"
    h.clock += 100;
    CHECK(h.bus.read32(kMc + 0x204C) == MemoryController::kDmaRunDone);   // and then "finished"

    for (uint32_t off = 0; off < 32; off += 4) {
        CHECK(h.bus.read32(physmap::kRamBase + off) == 0xDEAD'BEEFu);
    }
    CHECK(h.bus.read32(physmap::kRamBase + 32) == 0u);                     // exactly 32 bytes
    CHECK(h.bus.read32(kMc + 0x2004) == 0x0800'0020u);                     // final address
}

void mcDmaTlbTranslationAndStride() {
    McHarness h({16 * MiB});
    CHECK(h.bus.write32(kMc + 0xC4, 0x2320'0000));
    // DMA TLB entry 0: virtual region 0x40000000, page table at physical 0x08100000.
    CHECK(h.bus.write32(kMc + 0x184, 0x4000'0000));
    CHECK(h.bus.write32(kMc + 0x18C, 0x0810'0000 >> 6));
    CHECK(h.bus.write32(0x0810'0000, 0x0820'0000 >> 6));   // PTE: page 0 -> frame 0x08200000
    CHECK(h.bus.write32(kMc + 0x2004, 0x4000'0000));
    CHECK(h.bus.write32(kMc + 0x2034, 0x1A));              // ToHost | Fill | Incr
    CHECK(h.bus.write32(kMc + 0x201C, 0x0001'000C));       // zoom 1, stride +12
    CHECK(h.bus.write32(kMc + 0x2014, 0x0002'0004));       // 2 lines x 4 bytes
    CHECK(h.bus.write32(kMc + 0x16C, 0x10));               // interrupt on completion
    CHECK(h.bus.write32(kMc + 0x2044, 1));                 // start

    CHECK(h.bus.read32(0x0820'0000) == 0u);                // GIO address (fill value) is 0...
    CHECK(h.bus.write32(kMc + 0x2024, 0x1234'5678));       // ...so refill with a pattern
    CHECK(h.bus.write32(kMc + 0x2004, 0x4000'0000));
    CHECK(h.bus.write32(kMc + 0x2014, 0x0002'0004));
    CHECK(h.bus.write32(kMc + 0x201C, 0x0001'000C));
    CHECK(h.bus.write32(kMc + 0x2044, 1));
    CHECK(h.bus.read32(0x0820'0000) == 0x1234'5678u);      // line 0
    CHECK(h.bus.read32(0x0820'0004) == 0u);                // skipped by the stride
    CHECK(h.bus.read32(0x0820'0010) == 0x1234'5678u);      // line 1 at +4 +12
    h.clock += 100;
    CHECK((*h.bus.read32(kMc + 0x164) & 0x08) != 0);       // DMA-done cause latched
}

// On a 256 MiB machine IRIX puts DMA page tables in the second bank, above
// 0x10000000 (seen: TLB lo 0x005C8042 -> table at 0x17201000); the table
// frame field must reach that far (the X server's tile uploads failed).
void mcDmaPageTableInHighMemory() {
    McHarness h({128 * MiB, 128 * MiB});
    CHECK(h.bus.write32(kMc + 0xC4, 0x7F20'7F40));                // A at 0x08000000, B at 0x10000000
    CHECK(h.bus.write32(kMc + 0x184, 0x1040'0000));               // region 0x10400000
    CHECK(h.bus.write32(kMc + 0x18C, (0x1720'1000 >> 6) | 0x2));  // table at 0x17201000, valid
    CHECK(h.bus.write32(0x1720'1000 + 0x0FD * 4, 0x1600'0000 >> 6)); // page 0x104FD000 -> frame 0x16000000
    CHECK(h.bus.write32(kMc + 0x2024, 0xCAFE'BABE));              // fill pattern (GIO address)
    CHECK(h.bus.write32(kMc + 0x2004, 0x104F'DB40));
    CHECK(h.bus.write32(kMc + 0x2034, 0x1A));                     // ToHost | Fill | Incr
    CHECK(h.bus.write32(kMc + 0x201C, 0x0001'0000));
    CHECK(h.bus.write32(kMc + 0x2014, 0x0001'0004));              // 1 line x 4 bytes
    CHECK(h.bus.write32(kMc + 0x2044, 1));
    CHECK(h.bus.read32(0x1600'0B40) == 0xCAFE'BABEu);
}

// ---- PROM persistence / vectors / history ---------------------------------------------

void promSurvivesMcRemapsAndResets() {
    TempFile prom(beWords({0x0BF0'00F0, 0x0000'0000}));
    MemoryBus bus;
    bus.allocateRam(16 * MiB);
    Cycles clock = 0;
    MemoryController mc(bus, MemoryController::Config{.bankBytes = {16 * MiB}, .cpuClockHz = 50'000'000},
                        [&] { return clock; });
    bus.mapDevice(physmap::kMcBase, physmap::kMcSize, mc);
    CHECK(bus.loadProm(prom.path).has_value());

    // Put a RAM bank window right on top of the PROM (base 0x7F * 4 MiB = 0x1FC00000).
    CHECK(bus.write32(kMc + 0xC4, 0x237F'0000));
    CHECK(bus.read32(physmap::kPromBase) == 0x0BF0'00F0u);          // PROM wins
    CHECK(!bus.write32(physmap::kPromBase, 0));                      // and stays read-only
    CHECK(bus.read32(physmap::kPromBase + MemoryBus::kPageSize).has_value()); // RAM after it

    // Reconfigure, invalidate, reset: the PROM is never touched.
    CHECK(bus.write32(kMc + 0xC4, 0x2320'0000));
    CHECK(bus.write32(kMc + 0xC4, 0));
    mc.reset();
    CHECK(bus.read32(physmap::kPromBase) == 0x0BF0'00F0u);
}

void exceptionVectorsTranslateAndFetch() {
    // Boot vector 0xBFC00380 -> physical 0x1FC00380 (inside the PROM image).
    CHECK(mips::translateUnmapped(0xFFFF'FFFF'BFC0'0380) == PhysAddr{0x1FC0'0380});
    // Normal vectors in KSEG0 -> physical 0x80/0x180, served by the MC's low alias of RAM.
    CHECK(mips::translateUnmapped(0xFFFF'FFFF'8000'0080) == PhysAddr{0x80});
    CHECK(mips::translateUnmapped(0xFFFF'FFFF'8000'0180) == PhysAddr{0x180});

    McHarness h({16 * MiB});
    CHECK(h.bus.write32(kMc + 0xC4, 0x2320'0000));                   // bank A at 0x08000000
    CHECK(h.bus.write32(physmap::kRamBase + 0x180, 0x4200'0018));    // eret, written via RAM
    CHECK(h.bus.read32(0x180) == 0x4200'0018u);                      // fetched via the vector alias
}

void haltRecordsInstructionHistory() {
    MemoryBus bus;
    R5000 cpu(bus);
    cpu.reset();
    CHECK(cpu.history().empty());

    Harness h({
        0x2408'0001, // addiu $8, $zero, 1
        0x2409'0002, // addiu $9, $zero, 2
        0x3C0A'1234, // lui   $10, 0x1234
    });
    h.step(3);
    const auto entries = h.cpu.history();
    CHECK(entries.size() == 3);
    CHECK(entries.front().pc == mips::kResetVector && entries.front().insn == 0x2408'0001u);
    CHECK(entries.back().pc == mips::kResetVector + 8 && entries.back().insn == 0x3C0A'1234u);
}

// ---- DS1386 RTC / NVRAM -----------------------------------------------------------------

using namespace std::chrono;

// 2026-10-11 08:25:33.00 UTC, a Sunday. 100 MHz "CPU": 1 ms = 100'000 cycles.
constexpr Ds1386::TimePoint kRtcEpoch =
    sys_days{year{2026} / October / 11} + hours{8} + minutes{25} + seconds{33};

struct RtcHarness {
    Cycles clock = 0;
    Ds1386 rtc;
    explicit RtcHarness(std::filesystem::path file = {})
        : rtc(Ds1386::Config{.cpuClockHz = 100'000'000, .backingFile = std::move(file), .powerOnTime = kRtcEpoch,
                             .yearBase = 2000},
              [this] { return clock; }) {}
};

void rtcEncodesBcdAndTicks() {
    RtcHarness h;
    CHECK(h.rtc.peek(Ds1386::kSeconds) == 0x33);
    CHECK(h.rtc.peek(Ds1386::kMinutes) == 0x25);
    CHECK(h.rtc.peek(Ds1386::kHours) == 0x08);
    CHECK(h.rtc.peek(Ds1386::kDays) == 1);         // Sunday
    CHECK(h.rtc.peek(Ds1386::kDate) == 0x11);
    CHECK(h.rtc.peek(Ds1386::kMonths) == 0x10);
    CHECK(h.rtc.peek(Ds1386::kYears) == 0x26);

    h.clock += 100'000'000ull * 27 + 100'000ull * 450; // +27.45 s of emulated time
    CHECK(h.rtc.peek(Ds1386::kSeconds) == 0x00);
    CHECK(h.rtc.peek(Ds1386::kMinutes) == 0x26);
    CHECK(h.rtc.peek(Ds1386::kHundredths) == 0x45);
}

void rtcTransferEnableFreezesAndSetsTime() {
    RtcHarness h;
    // Clear TE: registers freeze while the counters keep running.
    CHECK(h.rtc.write(Ds1386::kCommand * 4, AccessSize::Word, 0x04));
    h.clock += 100'000'000ull * 10;
    CHECK(h.rtc.peek(Ds1386::kSeconds) == 0x33);

    // Set 2099-12-31 23:59:58 (year base 2000) in 12-hour mode (11 PM), then re-enable TE.
    const std::pair<uint8_t, uint8_t> writes[] = {
        {Ds1386::kSeconds, 0x58}, {Ds1386::kMinutes, 0x59}, {Ds1386::kHours, 0x40 | 0x20 | 0x11},
        {Ds1386::kDate, 0x31},    {Ds1386::kMonths, 0x12},  {Ds1386::kYears, 0x99},
    };
    for (auto [reg, value] : writes) {
        CHECK(h.rtc.write(reg * 4u + 3, AccessSize::Byte, value)); // byte lane +3, as the PROM does
    }
    CHECK(h.rtc.write(Ds1386::kCommand * 4, AccessSize::Word, 0x84)); // TE | TDM
    h.clock += 100'000'000ull * 3;                                    // roll over the century
    CHECK(h.rtc.peek(Ds1386::kYears) == 0x00);
    CHECK(h.rtc.peek(Ds1386::kMonths) == 0x01);
    CHECK(h.rtc.peek(Ds1386::kDate) == 0x01);
    CHECK(h.rtc.peek(Ds1386::kHours) == (0x40 | 0x12));               // 12 AM, still 12-hour mode
    CHECK(h.rtc.peek(Ds1386::kSeconds) == 0x01);
}

void rtcNvramPersistsAcrossPowerCycles() {
    const fs::path file = fs::temp_directory_path() / "indy_test_nvram.bin";
    std::error_code ec;
    fs::remove(file, ec);
    {
        RtcHarness h(file);
        CHECK(h.rtc.peek(Ds1386::kUserRam) == 0);                   // blank chip
        CHECK(h.rtc.write(0x0E * 4, AccessSize::Word, 0xA5));
        CHECK(h.rtc.write(0x1FFF * 4 + 3, AccessSize::Byte, 0x5A)); // last byte of 8 KiB
        CHECK(h.rtc.write(0x2000 * 4, AccessSize::Word, 0xFF));     // beyond the chip: ignored
    } // saved on destruction
    {
        RtcHarness h(file);
        CHECK(h.rtc.peek(0x0E) == 0xA5);
        CHECK(h.rtc.peek(0x1FFF) == 0x5A);
        CHECK(h.rtc.peek(Ds1386::kSeconds) == 0x33);                // clock resumes at power-on time
    }
    fs::remove(file, ec);
}

void rtcThroughHpc3AtPhysicalAddress() {
    HpcHarness h;
    Cycles clock = 0;
    Ds1386 rtc(Ds1386::Config{.cpuClockHz = 100'000'000, .powerOnTime = kRtcEpoch, .yearBase = 2000},
               [&] { return clock; });
    h.hpc3.attachBbram(rtc);
    CHECK(h.readByte(0x1FBE'0007) == 0x33);                      // seconds: chip address 1
    CHECK(h.writeByte(0x1FBE'00FB, 0x77));                       // the PROM's +0x600fb: chip 0x3E
    CHECK(rtc.peek(0x3E) == 0x77);
    CHECK(h.bus.read32(0x1FBE'00F8) == 0x77u);                   // word access: low byte
    CHECK(h.readByte(0x1FBE'00F8) == 0);                         // other lanes float
}

void sgiNvramEthernetAddress() {
    namespace nv = sgi_nvram;
    Ds1386 rtc(Ds1386::Config{.cpuClockHz = 100'000'000, .powerOnTime = kRtcEpoch}, [] { return Cycles{0}; });
    // Blank chip: invalid checksum; the default address is stored and the
    // checksum left for the PROM to initialise.
    CHECK(!nv::valid(rtc));
    CHECK(nv::provideEthernetAddress(rtc, std::nullopt) == nv::kDefaultMac);
    CHECK(rtc.peek(0x13A) == 0x08 && rtc.peek(0x13B) == 0x00 && rtc.peek(0x13C) == 0x69);
    CHECK(!nv::valid(rtc));

    // A PROM-initialised area (revision 8, valid checksum) stays valid.
    rtc.poke(nv::kBase + nv::kRevision, nv::kRevisionValue);
    rtc.poke(nv::kBase + 0x74, '9');
    CHECK(nv::checksum(rtc) == 0x06); // reference: the PROM's algorithm in Python
    rtc.poke(nv::kBase + nv::kChecksum, nv::checksum(rtc));
    CHECK(nv::valid(rtc));
    const nv::MacAddress mine = {0x08, 0x00, 0x69, 0x12, 0x34, 0x56};
    CHECK(nv::provideEthernetAddress(rtc, mine) == mine);
    CHECK(nv::ethernetAddress(rtc) == mine);
    CHECK(nv::valid(rtc));
    CHECK(rtc.peek(nv::kBase + nv::kChecksum) == 0x8A);
    CHECK(nv::provideEthernetAddress(rtc, std::nullopt) == mine); // an existing address is kept

    CHECK(nv::parseMac("08:00:69:0e:1D:01") == nv::kDefaultMac);
    CHECK(nv::parseMac("08-00-69-0e-1d-01") == nv::kDefaultMac);
    CHECK(!nv::parseMac("08:00:69:0e:1d"));
    CHECK(!nv::parseMac("08:00:69:0e:1d:01:02"));
    CHECK(!nv::parseMac("0g:00:69:0e:1d:01"));
}

// ---- 8042 keyboard controller --------------------------------------------------------------

std::vector<uint8_t> drain(Kbc8042& kbc) {
    std::vector<uint8_t> out;
    while ((kbc.readStatus() & Kbc8042::kStatusOBF) != 0) out.push_back(kbc.readData());
    return out;
}

void kbcControllerAndKeyboardProtocol() {
    Kbc8042 kbc;
    CHECK((kbc.readStatus() & Kbc8042::kStatusIBF) == 0);
    kbc.writeCommand(0xAA);                                    // controller self-test
    CHECK((kbc.readStatus() & Kbc8042::kStatusSYS) != 0);
    CHECK(drain(kbc) == std::vector<uint8_t>{0x55});
    kbc.writeCommand(0xAB);                                    // keyboard interface test
    CHECK(drain(kbc) == std::vector<uint8_t>{0x00});
    kbc.writeCommand(0x60); kbc.writeData(0x47);               // write command byte
    kbc.writeCommand(0x20);
    CHECK(drain(kbc) == std::vector<uint8_t>{0x47});

    kbc.writeData(0xFF);                                       // keyboard reset
    CHECK(drain(kbc) == (std::vector<uint8_t>{0xFA, 0xAA}));
    kbc.writeData(0xF2);                                       // identify
    CHECK(drain(kbc) == (std::vector<uint8_t>{0xFA, 0xAB, 0x83}));
    kbc.writeData(0xED); kbc.writeData(0x07);                  // LEDs + argument
    CHECK(drain(kbc) == (std::vector<uint8_t>{0xFA, 0xFA}));
    kbc.writeData(0xF0); kbc.writeData(0x00);                  // query scan set
    CHECK(drain(kbc) == (std::vector<uint8_t>{0xFA, 0xFA, 0x02}));
}

void kbcHostKeysInSet2AndSet3() {
    Kbc8042 kbc;
    kbc.hostKey(0x04, true);  kbc.hostKey(0x04, false);        // 'A'
    kbc.hostKey(0x52, true);                                   // Up arrow (E0-prefixed in set 2)
    CHECK(drain(kbc) == (std::vector<uint8_t>{0x1C, 0xF0, 0x1C, 0xE0, 0x75}));

    kbc.writeData(0xF0); kbc.writeData(0x03);                  // the Indy selects set 3
    (void)drain(kbc);
    kbc.hostKey(0x04, true);  kbc.hostKey(0x04, false);        // 'A': same code as set 2
    kbc.hostKey(0x29, true);                                   // Esc: 0x08 in set 3
    kbc.hostKey(0x52, true);  kbc.hostKey(0x52, false);        // Up: 0x63, no prefix
    kbc.hostKey(0xE0, true);                                   // Left Ctrl: 0x11
    CHECK(drain(kbc) == (std::vector<uint8_t>{0x1C, 0xF0, 0x1C, 0x08, 0x63, 0xF0, 0x63, 0x11}));
}

void kbcMouseThroughD4() {
    Kbc8042 kbc;
    kbc.writeCommand(0xD4); kbc.writeData(0xFF);               // mouse reset
    CHECK((kbc.readStatus() & Kbc8042::kStatusAUXB) != 0);
    CHECK(drain(kbc) == (std::vector<uint8_t>{0xFA, 0xAA, 0x00}));
    kbc.writeCommand(0xD4); kbc.writeData(0xF2);               // device ID
    CHECK(drain(kbc) == (std::vector<uint8_t>{0xFA, 0x00}));
}

// ---- WD33C93 ---------------------------------------------------------------------------------

void wd33c93ResetStatusAndAutoIncrement() {
    Wd33c93 wd;
    CHECK((wd.readAux() & Wd33c93::kAuxINT) != 0);             // power-on reset interrupt
    wd.writeAddress(Wd33c93::kScsiStatus);
    CHECK(wd.readDataPort() == Wd33c93::kStatusReset);
    CHECK((wd.readAux() & Wd33c93::kAuxINT) == 0);             // reading status acknowledged it

    wd.writeAddress(Wd33c93::kOwnId);
    wd.writeDataPort(0x08 | 7);                                // own ID 7 + EAF
    wd.writeDataPort(0x55);                                    // auto-increments to CONTROL
    wd.writeAddress(Wd33c93::kControl);
    CHECK(wd.readDataPort() == 0x55);

    wd.writeAddress(Wd33c93::kCommand);
    wd.writeDataPort(Wd33c93::kCmdReset);
    CHECK((wd.readAux() & Wd33c93::kAuxINT) != 0);
    wd.writeAddress(Wd33c93::kScsiStatus);
    CHECK(wd.readDataPort() == Wd33c93::kStatusResetEaf);     // EAF set -> 0x01
    wd.writeAddress(Wd33c93::kControl);
    CHECK(wd.readDataPort() == 0);                             // cleared by reset
    wd.writeAddress(Wd33c93::kOwnId);
    CHECK(wd.readDataPort() == (0x08 | 7));                    // own ID kept
}

void wd33c93SelectionTimeoutAndDisconnect() {
    Wd33c93 wd;
    wd.writeAddress(Wd33c93::kScsiStatus);
    (void)wd.readDataPort();                                   // clear power-on interrupt
    wd.writeAddress(Wd33c93::kCommand);
    wd.writeDataPort(Wd33c93::kCmdSelectAtnXfer);
    wd.writeAddress(Wd33c93::kScsiStatus);
    CHECK(wd.readDataPort() == Wd33c93::kStatusSelectTimeout);
    wd.writeAddress(Wd33c93::kCommand);
    wd.writeDataPort(Wd33c93::kCmdDisconnect);                 // completes silently
    CHECK((wd.readAux() & Wd33c93::kAuxINT) == 0);
}

void keyboardAndScsiAtPhysicalAddresses() {
    HpcHarness h;
    Wd33c93 wd;
    h.hpc3.attachScsi(0, wd);
    CHECK((h.readByte(0x1FBC'0003) & Wd33c93::kAuxINT) != 0);  // WD aux status (SASR read)
    CHECK(h.writeByte(0x1FBC'0003, Wd33c93::kScsiStatus));     // select register
    CHECK(h.readByte(0x1FBC'0007) == Wd33c93::kStatusReset);   // data port

    CHECK(h.writeByte(0x1FBD'9847, 0xAA));                     // 8042 command: self-test
    CHECK((h.readByte(0x1FBD'9847) & Kbc8042::kStatusOBF) != 0);
    CHECK(h.readByte(0x1FBD'9843) == 0x55);                    // 8042 data
}

// ---- SCSI CD-ROM, HPC3 SCSI DMA, select-and-transfer ---------------------------------------

// A 2 MiB disc image whose 512-byte block n starts with the byte n & 0xFF.
struct DiscImage {
    TempFile file;
    DiscImage() : file([] {
        std::vector<uint8_t> img(2 * MiB, 0);
        for (size_t block = 0; block < img.size() / 512; ++block) img[block * 512] = static_cast<uint8_t>(block);
        return img;
    }()) {}
};

void cdromScsiCommands() {
    DiscImage disc;
    CdRomDrive cd;
    const uint8_t tur[6] = {0x00};
    CHECK(cd.execute(tur, {}).status == kScsiCheckCondition);      // no medium
    CHECK(cd.insertMedia(disc.file.path).has_value());
    CHECK(cd.execute(tur, {}).status == kScsiCheckCondition);      // unit attention: medium changed
    const uint8_t sense[6] = {0x03, 0, 0, 0, 18, 0};
    const auto s = cd.execute(sense, {});
    CHECK(s.dataIn.size() == 18 && s.dataIn[2] == 0x06 && s.dataIn[12] == 0x28);
    CHECK(cd.execute(tur, {}).status == kScsiGood);

    const uint8_t inquiry[6] = {0x12, 0, 0, 0, 36, 0};
    const auto inq = cd.execute(inquiry, {});
    CHECK(inq.dataIn.size() == 36 && inq.dataIn[0] == 0x05 && (inq.dataIn[1] & 0x80) != 0);
    CHECK(std::string(inq.dataIn.begin() + 8, inq.dataIn.begin() + 16) == "TOSHIBA ");

    const uint8_t capacity[10] = {0x25};
    auto cap = cd.execute(capacity, {}).dataIn;                    // 4096 blocks of 512
    CHECK(cap == (std::vector<uint8_t>{0x00, 0x00, 0x0F, 0xFF, 0x00, 0x00, 0x02, 0x00}));

    const uint8_t read10[10] = {0x28, 0, 0, 0, 0, 7, 0, 0, 2, 0}; // LBA 7, 2 blocks
    const auto rd = cd.execute(read10, {});
    CHECK(rd.status == kScsiGood && rd.dataIn.size() == 1024 && rd.dataIn[0] == 7 && rd.dataIn[512] == 8);
    const uint8_t pastEnd[10] = {0x28, 0, 0, 0, 0x10, 0x00, 0, 0, 1, 0};
    CHECK(cd.execute(pastEnd, {}).status == kScsiCheckCondition);

    // MODE SELECT to 2048-byte blocks.
    const uint8_t modeSelect[6] = {0x15, 0x10, 0, 0, 12, 0};
    CHECK(cd.dataOutLength(modeSelect) == 12);
    const uint8_t params[12] = {0, 0, 0, 8, 0, 0, 0, 0, 0, 0x00, 0x08, 0x00};
    CHECK(cd.execute(modeSelect, params).status == kScsiGood);
    cap = cd.execute(capacity, {}).dataIn;
    CHECK(cap == (std::vector<uint8_t>{0x00, 0x00, 0x03, 0xFF, 0x00, 0x00, 0x08, 0x00}));
}

void hpc3ScsiDmaDescriptorChain() {
    MemoryBus bus;
    bus.installRam(physmap::kRamBase, 1 * MiB);
    Hpc3 hpc3(bus);
    bus.mapDevice(physmap::kHpc3Base, physmap::kHpc3Size, hpc3);
    const PhysAddr d0 = physmap::kRamBase + 0x1000, d1 = d0 + 12;
    CHECK(bus.write32(d0 + 0, physmap::kRamBase + 0x2000));     // buffer A: 3 bytes
    CHECK(bus.write32(d0 + 4, 3));
    CHECK(bus.write32(d0 + 8, d1));
    CHECK(bus.write32(d1 + 0, physmap::kRamBase + 0x3000));     // buffer B: 4 bytes, end + irq
    CHECK(bus.write32(d1 + 4, Hpc3::kDescEox | Hpc3::kDescXie | 4));
    CHECK(bus.write32(d1 + 8, 0));
    CHECK(bus.write32(0x1FB9'0004, d0));                        // NBDP
    CHECK(bus.write32(0x1FB9'1004, Hpc3::kSctrlActive | Hpc3::kSctrlDir)); // go

    const uint8_t bytes[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    CHECK(hpc3.scsiDma(0).toMemory(bytes) == 7);                // chain holds 7 bytes
    CHECK(bus.read32(physmap::kRamBase + 0x2000) == 0x0102'0300u);
    CHECK(bus.read32(physmap::kRamBase + 0x3000) == 0x0405'0607u);
    CHECK((*bus.read32(0x1FBB'0000) & 0x100) != 0);             // HPC3 INTSTAT: SCSI0
    const uint32_t ctrl = *bus.read32(0x1FB9'1004);
    CHECK((ctrl & Hpc3::kSctrlIrq) != 0 && (ctrl & Hpc3::kSctrlActive) == 0);
    CHECK((*bus.read32(0x1FB9'1004) & Hpc3::kSctrlIrq) == 0);   // read acknowledged it
}

void wd33c93SelectAndTransferWithDma() {
    DiscImage disc;
    MemoryBus bus;
    bus.installRam(physmap::kRamBase, 1 * MiB);
    Hpc3 hpc3(bus);
    bus.mapDevice(physmap::kHpc3Base, physmap::kHpc3Size, hpc3);
    Wd33c93 wd;
    auto cd = std::make_unique<CdRomDrive>();
    CHECK(cd->insertMedia(disc.file.path).has_value());
    wd.attach(4, std::move(cd));
    wd.attachDma(hpc3.scsiDma(0));

    auto setReg = [&](uint8_t reg, uint8_t v) { wd.writeAddress(reg); wd.writeDataPort(v); };
    auto getReg = [&](uint8_t reg) { wd.writeAddress(reg); return wd.readDataPort(); };
    auto run = [&](std::initializer_list<uint8_t> cdb, uint32_t count) {
        uint8_t i = Wd33c93::kCdb1;
        for (uint8_t b : cdb) setReg(i++, b);
        setReg(Wd33c93::kCountMsb, static_cast<uint8_t>(count >> 16));
        setReg(Wd33c93::kCount, static_cast<uint8_t>(count >> 8));
        setReg(Wd33c93::kCountLsb, static_cast<uint8_t>(count));
        setReg(Wd33c93::kTargetLun, 0);                           // drivers set the LUN every time
        setReg(Wd33c93::kCommand, Wd33c93::kCmdSelectAtnXfer);
        return getReg(Wd33c93::kScsiStatus);
    };
    (void)getReg(Wd33c93::kScsiStatus);                           // clear power-on interrupt

    // Descriptor: 1024 bytes at RAM + 0x4000.
    CHECK(bus.write32(physmap::kRamBase + 0x100, physmap::kRamBase + 0x4000));
    CHECK(bus.write32(physmap::kRamBase + 0x104, Hpc3::kDescEox | 1024));
    CHECK(bus.write32(0x1FB9'0004, physmap::kRamBase + 0x100));
    CHECK(bus.write32(0x1FB9'1004, Hpc3::kSctrlActive));

    setReg(Wd33c93::kControl, 0x80);                              // DMA mode
    setReg(Wd33c93::kDestId, 4);
    // First command after inserting a disc: UNIT ATTENTION (CHECK CONDITION).
    CHECK(run({0x00, 0, 0, 0, 0, 0}, 0) == Wd33c93::kStatusSatComplete);
    CHECK(getReg(Wd33c93::kTargetLun) == kScsiCheckCondition);
    CHECK(run({0x28, 0, 0, 0, 0, 3, 0, 0, 2, 0}, 1024) == Wd33c93::kStatusSatComplete); // READ(10) LBA 3 x2
    CHECK(getReg(Wd33c93::kTargetLun) == kScsiGood);
    CHECK(getReg(Wd33c93::kCommandPhase) == Wd33c93::kPhaseComplete);
    CHECK(getReg(Wd33c93::kCountLsb) == 0);
    uint8_t first = 0, second = 0;
    CHECK(bus.read<uint8_t>(physmap::kRamBase + 0x4000, first) && first == 3);
    CHECK(bus.read<uint8_t>(physmap::kRamBase + 0x4200, second) && second == 4);

    // Another logical unit: the CD-ROM has only LUN 0.
    auto reloadDma = [&] {
        CHECK(bus.write32(0x1FB9'0004, physmap::kRamBase + 0x100));
        CHECK(bus.write32(0x1FB9'1004, Hpc3::kSctrlActive));
    };
    reloadDma();
    setReg(Wd33c93::kTargetLun, 1);
    constexpr std::array<uint8_t, 6> kInquiry = {0x12, 0, 0, 0, 36, 0};
    for (uint8_t i = 0; i < kInquiry.size(); ++i) setReg(static_cast<uint8_t>(Wd33c93::kCdb1 + i), kInquiry[i]);
    setReg(Wd33c93::kCountMsb, 0); setReg(Wd33c93::kCount, 0); setReg(Wd33c93::kCountLsb, 36);
    setReg(Wd33c93::kCommand, Wd33c93::kCmdSelectAtnXfer);
    CHECK(getReg(Wd33c93::kScsiStatus) == Wd33c93::kStatusSatComplete);
    CHECK(getReg(Wd33c93::kTargetLun) == kScsiGood);
    uint8_t qualifier = 0;
    CHECK(bus.read<uint8_t>(physmap::kRamBase + 0x4000, qualifier) && qualifier == 0x7F); // LUN not supported
    reloadDma();
    setReg(Wd33c93::kTargetLun, 1);
    CHECK(run({0x28, 0, 0, 0, 0, 3, 0, 0, 1, 0}, 512) == Wd33c93::kStatusSatComplete); // run() resets the LUN
    CHECK(getReg(Wd33c93::kTargetLun) == kScsiGood);

    // Polled mode: INQUIRY bytes come through the DATA register, completion after the last.
    setReg(Wd33c93::kTargetLun, 0);
    setReg(Wd33c93::kControl, 0x00);
    setReg(Wd33c93::kCdb1, 0x12); setReg(Wd33c93::kCdb1 + 1, 0); setReg(Wd33c93::kCdb1 + 2, 0);
    setReg(Wd33c93::kCdb1 + 3, 0); setReg(Wd33c93::kCdb1 + 4, 36); setReg(Wd33c93::kCdb1 + 5, 0);
    setReg(Wd33c93::kCountLsb, 36); setReg(Wd33c93::kCount, 0); setReg(Wd33c93::kCountMsb, 0);
    setReg(Wd33c93::kCommand, Wd33c93::kCmdSelectAtnXfer);
    std::string inquiry;
    while ((wd.readAux() & Wd33c93::kAuxDBR) != 0) {
        wd.writeAddress(Wd33c93::kData);
        inquiry.push_back(static_cast<char>(wd.readDataPort()));
    }
    CHECK(inquiry.size() == 36 && inquiry.substr(8, 7) == "TOSHIBA");
    CHECK((wd.readAux() & Wd33c93::kAuxINT) != 0);
    CHECK(getReg(Wd33c93::kScsiStatus) == Wd33c93::kStatusSatComplete);

    setReg(Wd33c93::kDestId, 2);                                  // nobody there
    CHECK(run({0x00, 0, 0, 0, 0, 0}, 0) == Wd33c93::kStatusSelectTimeout);
}

// ---- Interrupts: INT3, 8254, device lines, CPU delivery ---------------------------------------

void rtcSgiYearBase() {
    Cycles clock = 0;
    Ds1386 rtc(Ds1386::Config{.cpuClockHz = 100'000'000, .powerOnTime = kRtcEpoch}, [&] { return clock; });
    CHECK(rtc.peek(Ds1386::kYears) == 0x86);       // 2026 - 1940
}

void int3MaskingCascadeAndTimers() {
    Int3 int3;
    std::array<bool, 4> lines{};
    int3.connect([&](unsigned line, bool level) { lines[line] = level; });

    int3.setLocal0(Int3::kL0Scsi0, true);
    CHECK(int3.read(Int3::kLocal0Status) == Int3::kL0Scsi0);
    CHECK(!lines[Int3::kCpuLocal0]);                            // masked
    int3.write(Int3::kLocal0Mask, Int3::kL0Scsi0);
    CHECK(lines[Int3::kCpuLocal0]);                             // -> Cause.IP2
    int3.setLocal0(Int3::kL0Scsi0, false);
    CHECK(!lines[Int3::kCpuLocal0]);

    // Keyboard is a mappable source: through map mask 1 into local 1 bit 3.
    int3.write(Int3::kLocal1Mask, Int3::kL1Mappable);
    int3.setMappable(Int3::kMapKeyboardMouse, true);
    CHECK(!lines[Int3::kCpuLocal1]);                            // map mask 1 still 0
    int3.write(Int3::kMapMask1, Int3::kMapKeyboardMouse);
    CHECK((int3.read(Int3::kLocal1Status) & Int3::kL1Mappable) != 0);
    CHECK(lines[Int3::kCpuLocal1]);                             // -> Cause.IP3

    int3.timerFired(1);
    CHECK(lines[Int3::kCpuTimer1]);                             // -> Cause.IP5, latched
    int3.write(Int3::kTimerClear, 0x02);
    CHECK(!lines[Int3::kCpuTimer1]);
}

void pit8254CountsAndInterrupts() {
    Scheduler scheduler;
    Cycles clock = 0;
    std::vector<unsigned> edges;
    Pit8254 pit;
    pit.connect(scheduler, [&] { return clock; }, 100'000'000, [&](unsigned c) { edges.push_back(c); });

    // Counter 2: rate generator, 1000 counts at 1 MHz (Linux's calibration setup).
    pit.write(3, 0xB4);                         // CNT2 | LSB+MSB | mode 2
    pit.write(2, 1000 & 0xFF); pit.write(2, 1000 >> 8);
    clock += 100 * 250;                         // 250 us
    pit.write(3, 0x80);                         // latch counter 2
    clock += 100 * 100;                         // reading later still returns the latched value
    const uint16_t latched = static_cast<uint16_t>(pit.read(2) | pit.read(2) << 8);
    CHECK(latched == 750);
    CHECK(pit.peek(2) == 650);

    // Counter 0: rate generator /10 clocked by counter 2 -> one edge every 10 ms.
    pit.write(3, 0x34);                         // CNT0 | LSB+MSB | mode 2
    pit.write(0, 10); pit.write(0, 0);
    for (int ms = 0; ms < 35; ++ms) {
        clock += 100'000;                       // 1 ms
        scheduler.runDue(clock);
    }
    CHECK(edges.size() == 3);                   // at ~10, 20, 30 ms
    CHECK(edges.empty() || edges.front() == 0);

    // Read-back status of counter 0: OUT high, count loaded, control bits 5:0.
    pit.write(3, 0xE2);                         // read-back: status only, counter 0
    CHECK(pit.read(0) == (0x80 | 0x34));
}

void deviceInterruptReachesCpuVector() {
    Harness h({
        0x3C01'0040, // lui  $1, 0x0040
        0x3421'0401, // ori  $1, $1, 0x0401   BEV | IM2 | IE
        0x4081'6000, // mtc0 $1, $12
        0, 0, 0, 0, 0,
    });
    Int3 int3;
    int3.connect([&](unsigned line, bool level) { h.cpu.setInterruptLine(line, level); });
    int3.write(Int3::kLocal0Mask, Int3::kL0Scsi0);
    h.step(4);
    CHECK(h.cpu.pc() != kBootGeneralVector);
    int3.setLocal0(Int3::kL0Scsi0, true);               // e.g. WD33C93 command complete
    h.step(1);
    CHECK(h.cpu.pc() == kBootGeneralVector);
    CHECK(excCode(h.cpu) == 0);                          // Int
    CHECK((h.cpu.cop0(R5000::kCause) & (1u << 10)) != 0); // IP2
    CHECK(h.cpu.cop0(R5000::kEPC) == mips::kResetVector + 16);
}

void deviceIrqOutputs() {
    Wd33c93 wd;
    bool scsiIrq = false;
    wd.setIrqCallback([&](bool level) { scsiIrq = level; });
    CHECK(scsiIrq);                                      // power-on reset interrupt pending
    wd.writeAddress(Wd33c93::kScsiStatus);
    (void)wd.readDataPort();
    CHECK(!scsiIrq);                                     // acknowledged by reading status
    wd.writeAddress(Wd33c93::kCommand);
    wd.writeDataPort(Wd33c93::kCmdReset);
    CHECK(scsiIrq);

    Kbc8042 kbc;
    bool kbdIrq = false;
    kbc.setIrqCallback([&](bool level) { kbdIrq = level; });
    kbc.hostKey(0x04, true);
    CHECK(!kbdIrq);                                      // interrupts disabled in command byte
    (void)drain(kbc);
    kbc.writeCommand(0x60); kbc.writeData(0x01);         // enable keyboard interrupt
    kbc.hostKey(0x04, true);
    CHECK(kbdIrq);
    (void)drain(kbc);
    CHECK(!kbdIrq);
}

// ---- Phase 15: MC interrupts, bus errors, serial interrupts, WD33C93 phases --------------------

void mcDmaDoneAndBusErrorInterrupts() {
    McHarness h({16 * MiB});
    Scheduler scheduler;
    h.mc->setScheduler(scheduler);
    bool dmaIrq = false, busErr = false;
    h.mc->setDmaIrqCallback([&](bool l) { dmaIrq = l; });
    h.mc->setBusErrorCallback([&](bool l) { busErr = l; });

    CHECK(h.bus.write32(kMc + 0xC4, 0x2320'0000));
    CHECK(h.bus.write32(kMc + 0x16C, 0x10));                     // interrupt on completion
    CHECK(h.bus.write32(kMc + 0x2004, 0x0800'0000));
    CHECK(h.bus.write32(kMc + 0x2034, 0x1A));
    CHECK(h.bus.write32(kMc + 0x201C, 0x0001'0000));
    CHECK(h.bus.write32(kMc + 0x2014, 0x0001'0010));
    CHECK(h.bus.write32(kMc + 0x202C, 0));                       // start
    CHECK(!dmaIrq);                                              // still "running"
    h.clock += 100;
    scheduler.runDue(h.clock);                                   // completion event
    CHECK(dmaIrq);
    CHECK(h.bus.write32(kMc + 0x164, 0));                        // acknowledge
    CHECK(!dmaIrq);

    h.mc->reportCpuBusError(0x3000'0000, /*isWrite=*/true);       // posted write to nowhere
    CHECK(busErr);
    CHECK(h.bus.read32(kMc + 0xE4) == 0x3000'0000u);             // CPU error address
    CHECK((*h.bus.read32(kMc + 0xEC) & MemoryController::kCpuErrStatusAddr) != 0);
    CHECK(h.bus.write32(kMc + 0xEC, 0));                         // clear status
    CHECK(!busErr);
    h.mc->reportCpuBusError(0x1F0F'0000, /*isWrite=*/false);      // GIO probe read: no IP6
    CHECK(!busErr);
    CHECK(h.bus.read32(kMc + 0xF4) == 0x1F0F'0000u);             // GIO error address
}

void sccReceiveAndTransmitInterrupts() {
    Z8530 scc([](Z8530::Channel, uint8_t) {});
    bool irq = false;
    scc.setIrqCallback([&](bool l) { irq = l; });
    auto wr = [&](unsigned reg, uint8_t v) { scc.writeControl(Z8530::Channel::A, static_cast<uint8_t>(reg)); scc.writeControl(Z8530::Channel::A, v); };

    wr(1, 0x10);                                  // Rx interrupt on every character
    scc.receive(Z8530::Channel::A, 'x');
    CHECK(!irq);                                  // master interrupt enable still off
    wr(9, 0x08);                                  // MIE
    CHECK(irq);
    scc.writeControl(Z8530::Channel::A, 3);       // RR3 through channel A
    CHECK(scc.readControl(Z8530::Channel::A) == 0x20); // channel A Rx pending
    CHECK(scc.readData(Z8530::Channel::A) == 'x');
    CHECK(!irq);

    wr(1, 0x12);                                  // + Tx interrupt enable
    scc.writeData(Z8530::Channel::A, '!');
    CHECK(irq);                                   // transmit buffer empty
    scc.writeControl(Z8530::Channel::A, 0x28);    // WR0: reset Tx interrupt pending
    CHECK(!irq);
}

void wd33c93SelectAndTransferInfoPhases() {
    DiscImage disc;
    Wd33c93 wd;
    auto cd = std::make_unique<CdRomDrive>();
    CHECK(cd->insertMedia(disc.file.path).has_value());
    wd.attach(4, std::move(cd));
    auto setReg = [&](uint8_t reg, uint8_t v) { wd.writeAddress(reg); wd.writeDataPort(v); };
    auto status = [&] { wd.writeAddress(Wd33c93::kScsiStatus); return wd.readDataPort(); };
    auto data = [&] { wd.writeAddress(Wd33c93::kData); return wd.readDataPort(); };
    auto put = [&](uint8_t b) { wd.writeAddress(Wd33c93::kData); wd.writeDataPort(b); };
    auto transferInfo = [&](uint32_t count) {
        setReg(Wd33c93::kCountMsb, 0); setReg(Wd33c93::kCount, static_cast<uint8_t>(count >> 8));
        setReg(Wd33c93::kCountLsb, static_cast<uint8_t>(count));
        setReg(Wd33c93::kCommand, Wd33c93::kCmdTransferInfo);
    };
    (void)status();                                              // power-on interrupt

    auto command = [&](std::initializer_list<uint8_t> cdb) {
        setReg(Wd33c93::kDestId, 4);
        setReg(Wd33c93::kCommand, Wd33c93::kCmdSelectAtn);
        CHECK(status() == Wd33c93::kStatusSelectSuccess);
        CHECK(status() == 0x8E);                                 // service required: MSG OUT
        transferInfo(1);
        CHECK((wd.readAux() & Wd33c93::kAuxDBR) != 0);           // wants a byte
        put(0xC0);                                               // IDENTIFY, LUN 0
        CHECK(status() == 0x1A);                                 // done, target now in COMMAND
        transferInfo(static_cast<uint32_t>(cdb.size()));
        for (uint8_t b : cdb) put(b);
    };

    // TEST UNIT READY after the disc change: no data, CHECK CONDITION, clean disconnect.
    command({0x00, 0, 0, 0, 0, 0});
    CHECK(status() == 0x1B);                                     // -> STATUS
    transferInfo(1);
    CHECK(data() == kScsiCheckCondition);
    CHECK(status() == 0x1F);                                     // -> MSG IN
    transferInfo(1);
    CHECK(data() == 0x00);                                       // COMMAND COMPLETE
    CHECK(status() == Wd33c93::kStatusPausedMsgIn);              // ACK held
    setReg(Wd33c93::kCommand, Wd33c93::kCmdNegateAck);
    CHECK(status() == Wd33c93::kStatusDisconnect);

    // INQUIRY: data in phase, polled.
    command({0x12, 0, 0, 0, 36, 0});
    CHECK(status() == 0x19);                                     // -> DATA IN
    transferInfo(36);
    std::string inquiry;
    while ((wd.readAux() & Wd33c93::kAuxDBR) != 0) inquiry.push_back(static_cast<char>(data()));
    CHECK(inquiry.size() == 36 && inquiry.substr(8, 7) == "TOSHIBA");
    CHECK(status() == 0x1B);                                     // -> STATUS
    transferInfo(1);
    CHECK(data() == kScsiGood);
    CHECK(status() == 0x1F);
    transferInfo(1);
    CHECK(data() == 0x00);
    CHECK(status() == Wd33c93::kStatusPausedMsgIn);
    setReg(Wd33c93::kCommand, Wd33c93::kCmdNegateAck);
    CHECK(status() == Wd33c93::kStatusDisconnect);
}

// ---- Newport (REX3 / DCB / scan-out) -------------------------------------------------------

struct NewportHarness {
    FrameBuffer fb{Newport::kVisibleWidth, Newport::kVisibleHeight};
    Newport np{fb};
    void w(uint32_t reg, uint32_t v) { CHECK(np.write(reg, AccessSize::Word, v)); }
    uint32_t r(uint32_t reg) { uint64_t v = 0; CHECK(np.read(reg, AccessSize::Word, v)); return static_cast<uint32_t>(v); }
    uint8_t rb(uint32_t off) { uint64_t v = 0; CHECK(np.read(off, AccessSize::Byte, v)); return static_cast<uint8_t>(v); }
    void dcb(uint32_t mode, uint32_t data) { w(Newport::kDcbMode, mode); w(Newport::kDcbData0, data); }
};

void newportRegisterReadback() {
    NewportHarness h;
    CHECK((h.r(Newport::kStatus) & 7) == Newport::kRex3Version);
    h.w(Newport::kXStartI, 0x1234'8765);                         // the PROM's own probe
    CHECK(h.r(Newport::kXStart) == 0x043B'2800u);                 // (int16)0x8765 << 11, 21.11 field
    h.w(Newport::kXStart, 0x07FF'FF80);                           // fractional bits survive
    CHECK(h.r(Newport::kXStart) == 0x07FF'FF80u);
    h.w(Newport::kBresD, 0xFFFF'FFFF);
    CHECK(h.r(Newport::kBresD) == 0x07FF'FFFFu);
    h.w(Newport::kSlopeGreen, 0x8000'0001);                       // sign-magnitude -1
    CHECK(h.r(Newport::kSlopeGreen) == 0x000F'FFFFu);
}

void newportDcbDevices() {
    NewportHarness h;
    // XMAP9 0 revision: 1-byte read (low lane, byte +3), device 5, register 1.
    h.w(Newport::kDcbMode, (5u << 7) | (1u << 4) | 1u);
    CHECK(h.rb(Newport::kDcbData0 + 3) == Newport::kXmap9Revision);
    // CMAP revision: device 2, register 6.
    h.w(Newport::kDcbMode, (2u << 7) | (6u << 4) | 1u);
    CHECK(h.rb(Newport::kDcbData0 + 3) == Newport::kCmapRevision);
    // VC2 index + data in one 3-byte write: index 0x10 (control), value 0x0003.
    h.dcb((0u << 7) | (0u << 4) | 3u, (0x10u << 24) | (0x0003u << 8));
    CHECK(h.np.vc2Register(0x10) == 0x0003);
    // CMAP palette: address then RGB (3-byte, MSB-aligned).
    h.dcb((1u << 7) | (0u << 4) | 2u, 0x0042);                    // both CMAPs, address 0x42
    h.dcb((1u << 7) | (2u << 4) | 3u, 0x11'22'33'00);
    CHECK(h.np.paletteAt(0x42) == 0x112233u);
    // XMAP9 mode table entry 0 = CI, 8 bpp.
    h.dcb((4u << 7) | (5u << 4) | 0u, (0u << 24) | 0x000400);
    // An absent DCB device times out with BBUSY (no clock wired: stays clear).
    h.w(Newport::kDcbMode, (12u << 7) | 1u);
    (void)h.r(Newport::kDcbData0);
}

void newportBlockFillPatternAndScanout() {
    NewportHarness h;
    h.dcb((0u << 7) | 3u, (0x10u << 24) | (0x0002u << 8));        // VC2: display enable
    h.dcb((1u << 7) | 2u, 7);                                     // CMAP address 7
    h.dcb((1u << 7) | (2u << 4) | 3u, 0xFF'80'00'00);             // index 7 = orange
    h.w(Newport::kDrawMode1, 0x3000'0001);                        // CI planes, logic op "copy"
    h.w(Newport::kWrMask, 0xFF);
    h.w(Newport::kColorI, 7);

    // Solid 4x3 block with DOSETUP|STOPX|STOPY.
    h.w(Newport::kDrawMode0, 0x0326);
    h.w(Newport::kXYStartI, (10u << 16) | 20u);
    h.w(Newport::kXYEndI | Newport::kGo, (13u << 16) | 22u);
    CHECK(h.np.vramAt(10, 20) == 7u && h.np.vramAt(13, 22) == 7u);
    CHECK(h.np.vramAt(14, 20) == 0u && h.np.vramAt(10, 23) == 0u);

    // Glyph-style: Y-decrementing octant, one row per Z-pattern GO, bit 31 first.
    h.w(Newport::kDrawMode0, 0x9106);                             // block | STOPX | ZPENAB | L32
    h.w(Newport::kBresOctInc1, 0x0100'0000);                      // octant: Y decrement
    h.w(Newport::kXYStartI, (100u << 16) | 50u);
    h.w(Newport::kXYEndI, (103u << 16) | 0u);
    h.w(Newport::kZPattern | Newport::kGo, 0x9000'0000);          // row y=50: x=100, x=103
    h.w(Newport::kZPattern | Newport::kGo, 0x6000'0000);          // row y=49: x=101, x=102
    CHECK(h.np.vramAt(100, 50) == 7u && h.np.vramAt(101, 50) == 0u && h.np.vramAt(103, 50) == 7u);
    CHECK(h.np.vramAt(101, 49) == 7u && h.np.vramAt(100, 49) == 0u);

    h.np.onVBlank(0);
    (void)h.fb.acquire();
    CHECK(h.fb.frontBuffer()[20 * Newport::kVisibleWidth + 10] == 0xFF8000u); // VRAM 7 -> CMAP -> RGB
}

// X streams images and tiled backgrounds as packed host data: each GO
// (without STOPONX) draws all 8 pixels of a RWDOUBLE host word.
void newportPackedHostDataPerGo() {
    NewportHarness h;
    h.w(Newport::kDrawMode1, 0x3000'0589);                        // CI, 8-bit, RWPACKED | RWDOUBLE
    h.w(Newport::kWrMask, 0xFF);
    h.w(Newport::kDrawMode0, 0x0046);                             // draw, block, CHOST (no STOPONX)
    h.w(Newport::kXYStartI, (0u << 16) | 0u);
    h.w(Newport::kXYEndI, (15u << 16) | 1u);
    for (uint32_t go = 0; go < 3; ++go) {
        const uint32_t base = go * 8;
        h.w(Newport::kHostRw0, (base + 1) << 24 | (base + 2) << 16 | (base + 3) << 8 | (base + 4));
        h.w(Newport::kHostRw1 | Newport::kGo, (base + 5) << 24 | (base + 6) << 16 | (base + 7) << 8 | (base + 8));
    }
    CHECK(h.np.vramAt(0, 0) == 1u && h.np.vramAt(7, 0) == 8u);     // first GO: 8 pixels
    CHECK(h.np.vramAt(8, 0) == 9u && h.np.vramAt(15, 0) == 16u);   // second GO ends the row
    CHECK(h.np.vramAt(0, 1) == 17u && h.np.vramAt(7, 1) == 24u);   // third GO: next row
}

// Scan-out follows the VC2 display-ID tables: each run of pixels uses the
// XMAP9 mode entry of its DID (here, a different colour-map base).
void newportDisplayIdRuns() {
    NewportHarness h;
    auto vc2Reg = [&](uint32_t index, uint32_t value) { h.dcb((0u << 7) | 3u, (index << 24) | (value << 8)); };
    auto vc2Ram = [&](uint32_t address, std::initializer_list<uint32_t> words) {
        vc2Reg(0x07, address);                                    // RAM address
        for (uint32_t word : words) h.dcb((0u << 7) | (3u << 4) | 2u, word);
    };
    auto palette = [&](uint32_t index, uint32_t rgb) {
        h.dcb((1u << 7) | 2u, index);
        h.dcb((1u << 7) | (2u << 4) | 3u, rgb << 8);
    };
    vc2Reg(0x10, 0x0002);                                         // display enable
    vc2Reg(0x05, 0x0100);                                         // DID frame table at 0x100
    vc2Ram(0x0100, {0x0200});                                     // line 0 -> line table 0x200
    vc2Ram(0x0200, {0x0000, (4u << 5) | 1u, 0xFFFF});             // DID 0 from x=0, DID 1 from x=4
    h.dcb((4u << 7) | (5u << 4), (0u << 24) | 0x000400);          // mode 0: CI 8 bpp, map base 0
    h.dcb((4u << 7) | (5u << 4), (1u << 24) | 0x000490);          // mode 1: CI 8 bpp, map base 0x1200
    palette(0x0007, 0xFF8000);
    palette(0x1207, 0x0000FF);

    h.w(Newport::kDrawMode1, 0x3000'0001);
    h.w(Newport::kWrMask, 0xFF);
    h.w(Newport::kColorI, 7);
    h.w(Newport::kDrawMode0, 0x0326);
    h.w(Newport::kXYStartI, (0u << 16) | 0u);
    h.w(Newport::kXYEndI | Newport::kGo, (7u << 16) | 0u);
    h.np.onVBlank(0);
    (void)h.fb.acquire();
    CHECK(h.fb.frontBuffer()[3] == 0xFF8000u);                    // DID 0
    CHECK(h.fb.frontBuffer()[4] == 0x0000FFu);                    // DID 1 from x = 4
}

// Menus and 4Dwm's move outline live in the popup planes: 2-bit values
// (write mask 0xCC) shown on top through the XMAP9 popup colour map; 0 is
// transparent. CID (window ID) writes are never visible.
void newportPopupPlanes() {
    NewportHarness h;
    h.dcb((0u << 7) | 3u, (0x10u << 24) | (0x0002u << 8));       // VC2: display enable
    h.dcb((4u << 7) | (4u << 4) | 1u, 0xE6);                      // XMAP9 popup MSBs: map 0x1CC0
    h.dcb((1u << 7) | 2u, 0x1CC2);                                // popup colour 2 = red
    h.dcb((1u << 7) | (2u << 4) | 3u, 0xFF'00'00'00);
    h.dcb((1u << 7) | 2u, 0x0007);                                // main pixel 7 = grey
    h.dcb((1u << 7) | (2u << 4) | 3u, 0x80'80'80'00);
    auto fill = [&](uint32_t dm1, uint32_t mask, uint32_t color, uint32_t x0, uint32_t x1) {
        h.w(Newport::kDrawMode1, dm1);
        h.w(Newport::kWrMask, mask);
        h.w(Newport::kColorI, color);
        h.w(Newport::kDrawMode0, 0x0326);
        h.w(Newport::kXYStartI, (x0 << 16) | 0u);
        h.w(Newport::kXYEndI | Newport::kGo, (x1 << 16) | 0u);
    };
    fill(0x3000'0001, 0xFF, 7, 0, 9);                             // main planes: grey row
    fill(0x3000'0005, 0xCC, 2, 2, 3);                             // popup value 2 at x 2..3
    fill(0x3000'0006, 0x33, 3, 5, 6);                             // CID at x 5..6: invisible
    CHECK(h.np.vramAt(2, 0) == 7u);                               // main planes untouched
    h.np.onVBlank(0);
    (void)h.fb.acquire();
    CHECK(h.fb.frontBuffer()[2] == 0xFF0000u && h.fb.frontBuffer()[3] == 0xFF0000u);
    CHECK(h.fb.frontBuffer()[1] == 0x808080u && h.fb.frontBuffer()[5] == 0x808080u);

    fill(0x3000'0005, 0xCC, 0, 2, 2);                             // popup 0: transparent again
    h.np.onVBlank(0);
    (void)h.fb.acquire();
    CHECK(h.fb.frontBuffer()[2] == 0x808080u && h.fb.frontBuffer()[3] == 0xFF0000u);
}

// X plots points as I_LINE without STOPONX/STOPONY: one pixel per GO, even
// with a stale end point left by an earlier span (it once drew a long streak
// from every point of an icon outline). With the stop bits the line runs to
// its end.
void newportLineSingleStepAndFullLine() {
    NewportHarness h;
    h.w(Newport::kDrawMode1, 0x3000'0001);
    h.w(Newport::kWrMask, 0xFF);
    h.w(Newport::kColorI, 5);
    h.w(Newport::kXYEndI, (122u << 16) | 0u);                    // stale span end
    h.w(Newport::kDrawMode0, 0x000A);                             // I_LINE, no setup, no stop bits
    h.w(Newport::kXYStartI | Newport::kGo, (60u << 16) | 55u);
    h.w(Newport::kXYStartI | Newport::kGo, (57u << 16) | 55u);
    CHECK(h.np.vramAt(60, 55) == 5u && h.np.vramAt(57, 55) == 5u);
    CHECK(h.np.vramAt(61, 54) == 0u && h.np.vramAt(58, 54) == 0u);   // no streak towards (122,0)

    h.w(Newport::kDrawMode0, 0x0B2A);                             // I_LINE, setup, STOPX|STOPY, skip last
    h.w(Newport::kXYStartI, (10u << 16) | 10u);
    h.w(Newport::kXYEndI | Newport::kGo, (14u << 16) | 10u);
    CHECK(h.np.vramAt(10, 10) == 5u && h.np.vramAt(13, 10) == 5u && h.np.vramAt(14, 10) == 0u);
}

// ---- Phase 17: PS/2 mouse streaming, VC2 hardware cursor -------------------------------------

void kbcMouseStreamPackets() {
    Kbc8042 kbc;
    kbc.hostMouse(5, -3, Kbc8042::kMouseLeft);
    CHECK(drain(kbc).empty());                                  // data reporting not enabled yet
    kbc.writeCommand(0xD4); kbc.writeData(0xF4);                // enable data reporting
    (void)drain(kbc);
    kbc.hostMouse(5, -3, Kbc8042::kMouseLeft);                  // right 5, down 3, left button
    CHECK((kbc.readStatus() & Kbc8042::kStatusAUXB) != 0);      // mouse data in the buffer
    CHECK(drain(kbc) == (std::vector<uint8_t>{0x29, 0x05, 0xFD})); // 0x08|L|Ysign, dx, dy
    kbc.hostMouse(300, 0, 0);                                   // beyond 9 bits: split
    CHECK(drain(kbc) == (std::vector<uint8_t>{0x08, 0xFF, 0x00, 0x08, 0x2D, 0x00}));
    kbc.writeCommand(0xD4); kbc.writeData(0xE9);                // status: enabled, no buttons
    CHECK(drain(kbc) == (std::vector<uint8_t>{0xFA, 0x20, 0x02, 100}));
    kbc.writeCommand(0xA7);                                     // mouse port disabled
    kbc.hostMouse(1, 1, 0);
    CHECK(drain(kbc).empty());
}

void newportHardwareCursor() {
    NewportHarness h;
    // Display + cursor function + cursor display (ECURS | ECDISP), 32x32 glyph.
    h.dcb((0u << 7) | 3u, (0x10u << 24) | (0x0092u << 8));
    h.dcb((0u << 7) | 3u, (0x01u << 24) | (0x0100u << 8));      // cursor glyph at VC2 RAM 0x100
    // Glyph: row 0 plane 0 = leftmost pixel only (value 1); plane 1 empty.
    h.dcb((0u << 7) | 3u, (0x07u << 24) | (0x0100u << 8));      // RAM address 0x100
    h.dcb((0u << 7) | (3u << 4) | 2u, 0x8000);                  // RAM data, auto-increments
    // Cursor colour: XMAP cursor MSB 2 -> CMAP entry (2 << 5) | 1 = 0x41 is green.
    h.dcb((4u << 7) | (3u << 4) | 1u, 2);
    h.dcb((1u << 7) | 2u, 0x41);
    h.dcb((1u << 7) | (2u << 4) | 3u, 0x00'FF'00'00);
    // Position: glyph drawn from (x - 31, y - 31).
    h.dcb((0u << 7) | 3u, (0x02u << 24) | (131u << 8));
    h.dcb((0u << 7) | 3u, (0x03u << 24) | (81u << 8));
    h.np.onVBlank(0);
    (void)h.fb.acquire();
    const auto frame = h.fb.frontBuffer();
    CHECK(frame[50 * Newport::kVisibleWidth + 100] == 0x00FF00u); // the cursor's pixel
    CHECK(frame[50 * Newport::kVisibleWidth + 101] == h.np.paletteAt(0)); // transparent next to it
}

// ---- Unaligned loads/stores (big-endian) --------------------------------------------------

void unalignedLoadStorePairs() {
    Harness h({
        0x3C08'A800, // lui   $8, 0xa800         RAM via KSEG1
        0x3C09'1122, // lui   $9, 0x1122
        0x3529'3344, // ori   $9, $9, 0x3344
        0xAD09'0000, // sw    $9, 0($8)          bytes 11 22 33 44
        0x3C09'5566, // lui   $9, 0x5566
        0x3529'7788, // ori   $9, $9, 0x7788
        0xAD09'0004, // sw    $9, 4($8)          bytes 55 66 77 88
        0x890A'0001, // lwl   $10, 1($8)         unaligned word at +1 ...
        0x990A'0004, // lwr   $10, 4($8)         ... = 0x22334455
        0x3C0B'AABB, // lui   $11, 0xaabb
        0x356B'CCDD, // ori   $11, $11, 0xccdd
        0xA90B'0009, // swl   $11, 9($8)         store unaligned word at +9 ...
        0xB90B'000C, // swr   $11, 12($8)        ... bytes 9..12 = AA BB CC DD
        0x690C'0001, // ldl   $12, 1($8)         unaligned doubleword at +1 ...
        0x6D0C'0008, // ldr   $12, 8($8)         ... = 22 33 44 55 66 77 88 00
        0x990D'0002, // lwr   $13, 2($8)         partial LWR: low 3 bytes = 11 22 33
    });
    h.step(16);
    CHECK(h.cpu.gpr(10) == 0x2233'4455u);
    const auto ram = h.bus.ram();
    CHECK(ram[8] == 0x00 && ram[9] == 0xAA && ram[10] == 0xBB && ram[11] == 0xCC && ram[12] == 0xDD && ram[13] == 0x00);
    CHECK(h.cpu.gpr(12) == 0x2233'4455'6677'8800u);
    CHECK((h.cpu.gpr(13) & 0x00FF'FFFF) == 0x0011'2233u);
}

// ---- SCSI hard disk -------------------------------------------------------------------------------

// IRIX splits a transfer bigger than one DMA map: the transfer count covers
// the first part, the chip pauses with "unexpected phase", and the driver
// reloads DMA and re-issues SELECT-AND-TRANSFER to continue (mkfs's 256 KiB
// log writes were cut short without this).
void wd33c93SatPausesAndResumesDataPhase() {
    const fs::path image = fs::temp_directory_path() / "indy_test_sat_disk.img";
    std::error_code ec;
    fs::remove(image, ec);
    auto opened = ScsiDisk::open(image, 1 * MiB);
    CHECK(opened.has_value());
    if (!opened) return;

    MemoryBus bus;
    bus.installRam(physmap::kRamBase, 1 * MiB);
    Hpc3 hpc3(bus);
    bus.mapDevice(physmap::kHpc3Base, physmap::kHpc3Size, hpc3);
    Wd33c93 wd;
    wd.attach(1, std::move(*opened));
    wd.attachDma(hpc3.scsiDma(0));

    auto setReg = [&](uint8_t reg, uint8_t v) { wd.writeAddress(reg); wd.writeDataPort(v); };
    auto getReg = [&](uint8_t reg) { wd.writeAddress(reg); return wd.readDataPort(); };
    auto setCount = [&](uint32_t count) {
        setReg(Wd33c93::kCountMsb, static_cast<uint8_t>(count >> 16));
        setReg(Wd33c93::kCount, static_cast<uint8_t>(count >> 8));
        setReg(Wd33c93::kCountLsb, static_cast<uint8_t>(count));
    };
    auto dmaFrom = [&](uint32_t buffer, uint32_t bytes) { // one-descriptor chain
        CHECK(bus.write32(physmap::kRamBase + 0x100, physmap::kRamBase + buffer));
        CHECK(bus.write32(physmap::kRamBase + 0x104, Hpc3::kDescEox | bytes));
        CHECK(bus.write32(0x1FB9'0004, physmap::kRamBase + 0x100));
        CHECK(bus.write32(0x1FB9'1004, Hpc3::kSctrlActive));
    };
    auto sat = [&] {
        setReg(Wd33c93::kCommand, Wd33c93::kCmdSelectAtnXfer);
        return getReg(Wd33c93::kScsiStatus);
    };
    auto command = [&](std::initializer_list<uint8_t> cdb) {
        uint8_t i = Wd33c93::kCdb1;
        for (uint8_t b : cdb) setReg(i++, b);
        setReg(Wd33c93::kCommandPhase, 0);
        setReg(Wd33c93::kTargetLun, 0);
    };
    (void)getReg(Wd33c93::kScsiStatus);
    setReg(Wd33c93::kControl, 0x80);
    setReg(Wd33c93::kDestId, 1);
    command({0x00, 0, 0, 0, 0, 0});                                // clear UNIT ATTENTION
    setCount(0);
    CHECK(sat() == Wd33c93::kStatusSatComplete);

    for (uint32_t i = 0; i < 1024; ++i) CHECK(bus.write<uint8_t>(physmap::kRamBase + 0x1000 + i, uint8_t(i / 4)));
    // WRITE(10) LBA 8, 2 blocks: the first DMA load covers one block.
    command({0x2A, 0, 0, 0, 0, 8, 0, 0, 2, 0});
    dmaFrom(0x1000, 512);
    setCount(512);
    CHECK(sat() == (Wd33c93::kStatusUnexpectedPhase | 0x00));     // paused in DATA OUT
    CHECK(getReg(Wd33c93::kCommandPhase) == Wd33c93::kPhaseUnexpected);
    dmaFrom(0x1200, 512);
    setCount(512);
    CHECK(sat() == Wd33c93::kStatusSatComplete);                  // resumed, written
    CHECK(getReg(Wd33c93::kTargetLun) == kScsiGood);

    // READ(10) the same 2 blocks back, again in two DMA loads.
    command({0x28, 0, 0, 0, 0, 8, 0, 0, 2, 0});
    dmaFrom(0x8000, 512);
    setCount(512);
    CHECK(sat() == (Wd33c93::kStatusUnexpectedPhase | 0x01));     // paused in DATA IN
    dmaFrom(0x8200, 512);
    setCount(512);
    CHECK(sat() == Wd33c93::kStatusSatComplete);
    CHECK(getReg(Wd33c93::kTargetLun) == kScsiGood);
    for (uint32_t i = 0; i < 1024; i += 97) {
        uint8_t b = 0;
        CHECK(bus.read<uint8_t>(physmap::kRamBase + 0x8000 + i, b) && b == uint8_t(i / 4));
    }
    fs::remove(image, ec);
}

// MAME CHD v5, uncompressed: 4 hunks of 4 KiB; hunk 0 stored at file slot 1,
// the others missing (they read as zeros).
void chdDiskImageReadWrite() {
    const fs::path path = fs::temp_directory_path() / "indy_test.chd";
    auto put32 = [](std::vector<uint8_t>& v, size_t at, uint64_t x) {
        for (int i = 0; i < 4; ++i) v[at + static_cast<size_t>(i)] = static_cast<uint8_t>(x >> (24 - 8 * i));
    };
    auto makeChd = [&](uint32_t version, uint32_t compressor) {
        std::vector<uint8_t> file(2 * 4096, 0);
        const std::string tag = "MComprHD";
        std::copy(tag.begin(), tag.end(), file.begin());
        put32(file, 8, 124);                  // header length
        put32(file, 12, version);
        put32(file, 16, compressor);
        put32(file, 36, 4 * 4096);            // logical bytes (low word of the 64-bit field)
        put32(file, 44, 124);                 // map offset
        put32(file, 56, 4096);                // hunk bytes
        put32(file, 60, 512);                 // unit bytes
        put32(file, 124, 1);                  // map: hunk 0 -> slot 1, others missing
        for (size_t i = 0; i < 4096; ++i) file[4096 + i] = static_cast<uint8_t>(i * 7);
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(file.data()),
                                                    static_cast<std::streamsize>(file.size()));
    };

    makeChd(5, 0);
    {
        auto image = openDiskImage(path);
        CHECK(image.has_value());
        if (!image) return;
        DiskImage& d = **image;
        CHECK(d.size() == 4 * 4096u && d.format() == "CHD (uncompressed)");
        std::vector<uint8_t> buf(1024);
        CHECK(d.read(4096 - 512, buf));                              // spans stored hunk 0 and missing hunk 1
        CHECK(buf[0] == static_cast<uint8_t>((4096 - 512) * 7) && buf[511] == static_cast<uint8_t>(4095 * 7));
        CHECK(buf[512] == 0 && buf[1023] == 0);
        const std::vector<uint8_t> data(512, 0xAB);
        CHECK(d.write(2 * 4096 + 512, data));                        // missing hunk 2: appended at slot 2
        CHECK(d.write(100, std::vector<uint8_t>(4, 0x11)));          // stored hunk 0: in place
        CHECK(d.write(3 * 4096, std::vector<uint8_t>(512, 0)));      // zeros into a missing hunk: no growth
        CHECK(!d.read(4 * 4096 - 4, buf));                           // past the end
    }
    CHECK(fs::file_size(path) == 3 * 4096u);
    {
        auto image = openDiskImage(path);                            // persisted, including the map
        CHECK(image.has_value());
        if (!image) return;
        std::vector<uint8_t> buf(4096);
        CHECK((*image)->read(2 * 4096, buf));
        CHECK(buf[0] == 0 && buf[512] == 0xAB && buf[1023] == 0xAB && buf[1024] == 0);
        std::vector<uint8_t> head(4);
        CHECK((*image)->read(100, head) && head[0] == 0x11 && head[3] == 0x11);
    }
    makeChd(5, 0x7A6C6962);                                          // 'zlib': compressed
    auto compressed = openDiskImage(path);
    CHECK(!compressed.has_value() && compressed.error().find("chdman") != std::string::npos);
    makeChd(4, 0);
    CHECK(!openDiskImage(path).has_value());
    std::error_code ec;
    fs::remove(path, ec);
}

void scsiDiskCommandsAndPersistence() {
    const fs::path image = fs::temp_directory_path() / "indy_test_disk.img";
    std::error_code ec;
    fs::remove(image, ec);
    CHECK(!ScsiDisk::open(image).has_value());                   // missing, no size: refused
    {
        auto opened = ScsiDisk::open(image, 8 * MiB);            // created sparse
        CHECK(opened.has_value());
        if (!opened) return;
        ScsiDisk& disk = **opened;
        CHECK(disk.blockCount() == 8 * MiB / 512);

        const uint8_t tur[6] = {0x00};
        CHECK(disk.execute(tur, {}).status == kScsiCheckCondition); // power-on UNIT ATTENTION
        const uint8_t sense[6] = {0x03, 0, 0, 0, 18, 0};
        CHECK(disk.execute(sense, {}).dataIn[12] == 0x29);
        CHECK(disk.execute(tur, {}).status == kScsiGood);

        const uint8_t inquiry[6] = {0x12, 0, 0, 0, 36, 0};
        const auto inq = disk.execute(inquiry, {}).dataIn;
        CHECK(inq[0] == 0x00 && (inq[1] & 0x80) == 0);           // direct access, fixed
        CHECK(std::string(inq.begin() + 8, inq.begin() + 16) == "SEAGATE ");

        const uint8_t capacity[10] = {0x25};
        CHECK(disk.execute(capacity, {}).dataIn ==
              (std::vector<uint8_t>{0x00, 0x00, 0x3F, 0xFF, 0x00, 0x00, 0x02, 0x00}));

        // WRITE(10) two blocks at LBA 5, read them back with READ(6).
        const uint8_t write10[10] = {0x2A, 0, 0, 0, 0, 5, 0, 0, 2, 0};
        CHECK(disk.dataOutLength(write10) == 1024);
        std::vector<uint8_t> payload(1024);
        for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i * 7);
        CHECK(disk.execute(write10, payload).status == kScsiGood);
        const uint8_t read6[6] = {0x08, 0, 0, 5, 2, 0};
        CHECK(disk.execute(read6, {}).dataIn == payload);
        const uint8_t pastEnd[10] = {0x28, 0, 0, 0, 0x40, 0x00, 0, 0, 1, 0};
        CHECK(disk.execute(pastEnd, {}).status == kScsiCheckCondition);

        // MODE SENSE(6), all pages: header + block descriptor + pages 1,2,3,4,8.
        const uint8_t modeSense[6] = {0x1A, 0, 0x3F, 0, 255, 0};
        const auto ms = disk.execute(modeSense, {}).dataIn;
        CHECK(ms.size() == static_cast<size_t>(ms[0]) + 1 && ms[3] == 8);
        CHECK(ms[4 + 8] == 0x01);                                // first page: error recovery
        const uint8_t page4[6] = {0x1A, 0, 0x04, 0, 255, 0};
        const auto geometry = disk.execute(page4, {}).dataIn;
        CHECK(geometry[12] == 0x04 && geometry[12 + 5] == ScsiDisk::kHeads);
        const uint8_t badPage[6] = {0x1A, 0, 0x2A, 0, 255, 0};
        CHECK(disk.execute(badPage, {}).status == kScsiCheckCondition);
    }
    {
        auto reopened = ScsiDisk::open(image);                    // data survived
        CHECK(reopened.has_value());
        if (reopened) {
            const uint8_t tur[6] = {0x00};
            (void)(*reopened)->execute(tur, {});                  // consume UNIT ATTENTION
            const uint8_t read10[10] = {0x28, 0, 0, 0, 0, 5, 0, 0, 1, 0};
            const auto block = (*reopened)->execute(read10, {}).dataIn;
            CHECK(block.size() == 512 && block[1] == 7 && block[2] == 14);
        }
    }
    fs::remove(image, ec);
}

// ---- Text -> key presses (clipboard paste / --type) ----------------------------------------

void textToKeyPresses() {
    using KE = cmd::KeyEvent;
    auto same = [](const std::vector<KE>& a, const std::vector<KE>& b) {
        return a.size() == b.size() &&
               std::equal(a.begin(), a.end(), b.begin(), [](const KE& x, const KE& y) {
                   return x.hidUsage == y.hidUsage && x.pressed == y.pressed;
               });
    };
    CHECK(same(keyEventsForText("a").events, {{0x04, true}, {0x04, false}}));
    CHECK(same(keyEventsForText("A").events, {{0xE1, true}, {0x04, true}, {0x04, false}, {0xE1, false}}));
    CHECK(same(keyEventsForText("!").events, {{0xE1, true}, {0x1E, true}, {0x1E, false}, {0xE1, false}}));
    CHECK(same(keyEventsForText("_").events, {{0xE1, true}, {0x2D, true}, {0x2D, false}, {0xE1, false}}));
    CHECK(same(keyEventsForText("\r\n\t").events, {{0x28, true}, {0x28, false}, {0x2B, true}, {0x2B, false}}));

    const TypedText mixed = keyEventsForText("x\xC3\xA9y"); // "x é y": é has no US key
    CHECK(mixed.skipped == 1 && mixed.events.size() == 4);

    // Every printable ASCII character is typeable.
    std::string printable;
    for (char c = 0x20; c < 0x7F; ++c) printable.push_back(c);
    CHECK(keyEventsForText(printable).skipped == 0);
}

} // namespace

int main() {
    const std::pair<const char*, std::function<void()>> tests[] = {
        {"ramIsBigEndian", ramIsBigEndian},
        {"unmappedAccessFails", unmappedAccessFails},
        {"invalidRamSizeThrows", invalidRamSizeThrows},
        {"promLoadsVerbatimAndIsReadOnly", promLoadsVerbatimAndIsReadOnly},
        {"promErrors", promErrors},
        {"segmentTranslation", segmentTranslation},
        {"cpuFetchesFromResetVector", cpuFetchesFromResetVector},
        {"cpuHaltsWithoutProm", cpuHaltsWithoutProm},
        {"promByteOrderDetection", promByteOrderDetection},
        {"executesRegisterSetup", executesRegisterSetup},
        {"executesJumpsWithDelaySlots", executesJumpsWithDelaySlots},
        {"executesLoadsAndStores", executesLoadsAndStores},
        {"unimplementedInstructionIsSkipped", unimplementedInstructionIsSkipped},
        {"busErrorOnUndecodedAccess", busErrorOnUndecodedAccess},
        {"cop0ConfigMatchesR5000", cop0ConfigMatchesR5000},
        {"misalignedLoadRaisesAddressError", misalignedLoadRaisesAddressError},
        {"tlbTranslation", tlbTranslation},
        {"tlbRefillException", tlbRefillException},
        {"tlbWriteThenAccess", tlbWriteThenAccess},
        {"tlbRewriteInvalidatesMicroTlb", tlbRewriteInvalidatesMicroTlb},
        {"cp0TimersRandomCountCompare", cp0TimersRandomCountCompare},
        {"r5000MipsIvInstructions", r5000MipsIvInstructions},
        {"exceptionInDelaySlotSetsBD", exceptionInDelaySlotSetsBD},
        {"eretReturnsToEpc", eretReturnsToEpc},
        {"syscallAndCoprocessorUnusable", syscallAndCoprocessorUnusable},
        {"timerInterrupt", timerInterrupt},
        {"mcSysIdAndSlotAliasing", mcSysIdAndSlotAliasing},
        {"mcRpssCounter", mcRpssCounter},
        {"mcMemoryConfigMapsAndMirrors", mcMemoryConfigMapsAndMirrors},
        {"mcSemaphoresAndClearOnWrite", mcSemaphoresAndClearOnWrite},
        {"mcBankLayout", mcBankLayout},
        {"mcSubbankWindows", mcSubbankWindows},
        {"sccPointerProtocolAndTransmit", sccPointerProtocolAndTransmit},
        {"ioc2SerialAndIdsThroughBus", ioc2SerialAndIdsThroughBus},
        {"hpc3RegisterFiles", hpc3RegisterFiles},
        {"fpuMovesWithFr0Pairing", fpuMovesWithFr0Pairing},
        {"fpuMovesWithFr1", fpuMovesWithFr1},
        {"fpuLoadStore", fpuLoadStore},
        {"fpuBranchAndFpe", fpuBranchAndFpe},
        {"fpuArithmeticAndCompare", fpuArithmeticAndCompare},
        {"fpuOperationsIeee", fpuOperationsIeee},
        {"fpuPromScratchRegisterPattern", fpuPromScratchRegisterPattern},
        {"mcDmaFillLikePromAndStatusLatency", mcDmaFillLikePromAndStatusLatency},
        {"mcDmaTlbTranslationAndStride", mcDmaTlbTranslationAndStride},
        {"mcDmaPageTableInHighMemory", mcDmaPageTableInHighMemory},
        {"promSurvivesMcRemapsAndResets", promSurvivesMcRemapsAndResets},
        {"exceptionVectorsTranslateAndFetch", exceptionVectorsTranslateAndFetch},
        {"haltRecordsInstructionHistory", haltRecordsInstructionHistory},
        {"rtcEncodesBcdAndTicks", rtcEncodesBcdAndTicks},
        {"rtcTransferEnableFreezesAndSetsTime", rtcTransferEnableFreezesAndSetsTime},
        {"rtcNvramPersistsAcrossPowerCycles", rtcNvramPersistsAcrossPowerCycles},
        {"sgiNvramEthernetAddress", sgiNvramEthernetAddress},
        {"rtcThroughHpc3AtPhysicalAddress", rtcThroughHpc3AtPhysicalAddress},
        {"kbcControllerAndKeyboardProtocol", kbcControllerAndKeyboardProtocol},
        {"kbcHostKeysInSet2AndSet3", kbcHostKeysInSet2AndSet3},
        {"kbcMouseThroughD4", kbcMouseThroughD4},
        {"wd33c93ResetStatusAndAutoIncrement", wd33c93ResetStatusAndAutoIncrement},
        {"wd33c93SelectionTimeoutAndDisconnect", wd33c93SelectionTimeoutAndDisconnect},
        {"keyboardAndScsiAtPhysicalAddresses", keyboardAndScsiAtPhysicalAddresses},
        {"cdromScsiCommands", cdromScsiCommands},
        {"hpc3ScsiDmaDescriptorChain", hpc3ScsiDmaDescriptorChain},
        {"wd33c93SelectAndTransferWithDma", wd33c93SelectAndTransferWithDma},
        {"rtcSgiYearBase", rtcSgiYearBase},
        {"int3MaskingCascadeAndTimers", int3MaskingCascadeAndTimers},
        {"pit8254CountsAndInterrupts", pit8254CountsAndInterrupts},
        {"deviceInterruptReachesCpuVector", deviceInterruptReachesCpuVector},
        {"deviceIrqOutputs", deviceIrqOutputs},
        {"mcDmaDoneAndBusErrorInterrupts", mcDmaDoneAndBusErrorInterrupts},
        {"sccReceiveAndTransmitInterrupts", sccReceiveAndTransmitInterrupts},
        {"wd33c93SelectAndTransferInfoPhases", wd33c93SelectAndTransferInfoPhases},
        {"newportRegisterReadback", newportRegisterReadback},
        {"newportDcbDevices", newportDcbDevices},
        {"newportBlockFillPatternAndScanout", newportBlockFillPatternAndScanout},
        {"kbcMouseStreamPackets", kbcMouseStreamPackets},
        {"newportHardwareCursor", newportHardwareCursor},
        {"newportPackedHostDataPerGo", newportPackedHostDataPerGo},
        {"newportDisplayIdRuns", newportDisplayIdRuns},
        {"newportPopupPlanes", newportPopupPlanes},
        {"newportLineSingleStepAndFullLine", newportLineSingleStepAndFullLine},
        {"unalignedLoadStorePairs", unalignedLoadStorePairs},
        {"scsiDiskCommandsAndPersistence", scsiDiskCommandsAndPersistence},
        {"chdDiskImageReadWrite", chdDiskImageReadWrite},
        {"wd33c93SatPausesAndResumesDataPhase", wd33c93SatPausesAndResumesDataPhase},
        {"textToKeyPresses", textToKeyPresses},
    };
    for (const auto& [name, test] : tests) {
        const int before = g_failures;
        test();
        std::fprintf(stderr, "%s %s\n", g_failures == before ? "PASS" : "FAIL", name);
    }
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
