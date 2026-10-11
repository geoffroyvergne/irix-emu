#pragma once

#include "indy/core/Types.hpp"

// Indy (IP22) physical address map.
//
// Every entry must come from the MIPS architecture or a verified reference
// (MAME src/mame/sgi/ip22*.cpp, NetBSD sys/arch/sgimips, Linux arch/mips/sgi-ip22).
// GIO slots: graphics at 0x1F000000 (Newport REX3 at +0xF0000), expansion
// slots at 0x1F400000 / 0x1F600000; undecoded slot space bus-errors.
namespace indy::physmap {

// Main memory segment 0. The MC's memory-configuration registers decide which
// SIMM banks appear where; the PROM programs them and probes for RAM.
inline constexpr PhysAddr kRamBase    = 0x0800'0000;
inline constexpr uint64_t kRamMaxSize = 256 * MiB; // largest Indy configuration

// The MC decodes both memory segments as memory whether or not a bank covers
// an address (unpopulated parts read floating data rather than bus-erroring).
// Linux asm/sgi/mc.h: SGIMC_SEG0_BADDR/SIZE_ALL, SGIMC_SEG1_BADDR/SIZE_IP20_IP22.
inline constexpr PhysAddr kMemSeg0Base = 0x0800'0000;
inline constexpr uint64_t kMemSeg0Size = 256 * MiB;
inline constexpr PhysAddr kMemSeg1Base = 0x2000'0000;
inline constexpr uint64_t kMemSeg1Size = 128 * MiB;

// Memory controller (MC). Registers are 32-bit values in 8-byte slots; the
// window runs through the 16 semaphores at +0x10000..+0x1F000.
// Refs: Linux arch/mips/include/asm/sgi/mc.h (SGIMC_BASE), MAME src/mame/sgi/mc.cpp.
inline constexpr PhysAddr kMcBase = 0x1FA0'0000;
inline constexpr uint64_t kMcSize = 0x2'0000;

// The MC aliases the first 512 KiB of the bank at kRamBase to physical 0, where
// KSEG0 exception vectors (0x80000000) land once BEV is cleared.
inline constexpr PhysAddr kLowAliasBase = 0x0000'0000;
inline constexpr uint64_t kLowAliasSize = 512 * KiB;

// HPC3 peripheral controller (Linux asm/sgi/hpc3.h HPC3_CHIP0_BASE). Its PBUS
// PIO channel 6 is the IOC2 (serial, keyboard, INT3) at 0x1FBD9800.
inline constexpr PhysAddr kHpc3Base = 0x1FB8'0000;
inline constexpr uint64_t kHpc3Size = 0x8'0000;

// Newport REX3 register window (MAME gio64 newport map; confirmed by the
// PROM's probe of 0x1F0F0000).
inline constexpr PhysAddr kNewportBase = 0x1F0F'0000;

// Boot PROM. MIPS reset vector: virtual 0xBFC00000 (KSEG1) -> physical 0x1FC00000.
inline constexpr PhysAddr kPromBase    = 0x1FC0'0000;
inline constexpr uint64_t kPromMaxSize = 1 * MiB;

static_assert(kRamBase + kRamMaxSize <= kPromBase, "RAM window must not overlap the PROM");

} // namespace indy::physmap
