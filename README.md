# irix-emu: SGI Indy emulator

A clean-room emulator of the **Silicon Graphics Indy** (IP24 board, "Guinness" architecture), written in modern C++23 with SDL3.

**Current status:** the real Indy boot PROM (5.3 Rev B10) runs every power-on diagnostic and passes. It shows its graphical boot screen and the *"Welcome to Indy"* System Maintenance Menu on the emulated Newport XL graphics board, and you can drive it with the emulated PS/2 keyboard and mouse. The PROM sees the virtual SCSI CD-ROM (`hinv`) and reads SGI volume headers from disc images. Booting IRIX itself is the next milestone.

---

## Table of contents

1. [Purpose and goals](#purpose-and-goals)
2. [Development strategy](#development-strategy)
3. [Architecture](#architecture)
4. [Emulated hardware](#emulated-hardware)
5. [Building](#building)
6. [Running](#running)
7. [Controls](#controls)
8. [Debugging and diagnostics](#debugging-and-diagnostics)
9. [Testing](#testing)
10. [Known limitations and roadmap](#known-limitations-and-roadmap)
11. [Legal](#legal)

---

## Purpose and goals

- **Boot IRIX 6.5.22**, either from a pre-installed disk image or by installing from the original CDs.
- **Run 3D applications** such as Maya and Softimage. Later this needs a graphics layer that either emulates the SGI pipeline closely (LLE) or translates it to host OpenGL/Vulkan (HLE).
- **Feel like a native desktop app:** an SDL window, keyboard and mouse input, and CD swapping by **dragging an ISO onto the window**.
- **Stay maintainable:** a modular, object-oriented C++ design with explicit ownership. The emulation core is independent of the UI and can be tested on its own.

## Development strategy

**Clean room, from documented facts.** Hardware behaviour comes from public references:

- **Linux** headers and drivers (`arch/mips/include/asm/sgi/*.h`, `arch/mips/sgi-ip22`, `include/video/newport.h`)
- **NetBSD** (`sys/arch/sgimips`)
- **MAME**, read for facts only and not copied as code (`src/mame/sgi`, `src/devices/bus/gio64`)
- Public chip datasheets (MIPS R4000/R4600, WD33C93, Z85C30, Intel 8042/8254, Dallas DS1386)

**No guessed registers.** Every SGI register address and bit used here is checked against at least one reference, and usually two. Where the references disagree or say nothing, the emulator follows what the **real PROM's own code** does, found by disassembling the PROM and tracing its accesses. Each such case is documented in the source. Examples:

- the DCB byte-lane rules;
- the SGI 1940 year base for the RTC;
- the MC decoding its whole memory segments;
- the HPC3 `ENDIAN` bit.

**The real PROM is the integration test.** Each phase was considered done only once the PROM got further than before:

1. serial output
2. diagnostics passing
3. the System Maintenance Menu
4. the Command Monitor
5. the graphical boot screen
6. mouse control

**Unit tests for every device.** Each device has tests built from documented behaviour and from sequences the PROM actually performs. They run in normal and sanitizer (ASan/UBSan) builds.

**Fault policy:**

| Situation | Behaviour |
|---|---|
| Architectural fault (address error, TLB miss, overflow, …) | A real MIPS exception, delivered through COP0 |
| Read from an address nothing decodes | Data Bus Error exception; the MC latches the address |
| Posted write to an address nothing decodes | Write is dropped; the MC latches it and raises IP6 |
| Unimplemented instruction | Warned about once per PC, then skipped |
| Unemulated device register | Warned about once, reads return 0 |
| Unrecoverable state (exception handler can't be fetched) | Halt, with a dump of the last 32 instructions |

## Architecture

### Threads

```
┌──────────────────────── main thread ─────────────────────────┐
│ SdlFrontend: window, events, texture upload, presentation    │
│ HostTerminal: stdin ──► serial console (raw terminal mode)    │
└──────────┬──────────────────────────────────▲────────────────┘
           │ CommandQueue (mutex, rare)        │ FrameBuffer (lock-free
           │ keys, mouse, serial, ISO swap     │ triple buffer)
┌──────────▼──────────────────────────────────┴────────────────┐
│ EmulationThread: owns System, runs it in 1 ms slices paced    │
│ to real time (or unthrottled)                                 │
└───────────────────────────────────────────────────────────────┘
```

Neither side ever blocks the other. The UI never touches `System` directly: it posts commands, and it reads finished frames.

### System (the machine)

`System` owns every component and the master clock. `runFor()` lets the CPU run in chunks of at most 256 cycles, and never past the next **scheduler** event. Devices schedule events instead of being polled each cycle; examples are the 8254 terminal counts, MC DMA completion and the 60 Hz vertical blank. Counters such as RPSS, the 8254 and the RTC are worked out from the current cycle when software reads them, so reads are always exact.

```
R4600 CPU ──► MemoryBus (64 KiB page table, big-endian storage)
               ├── RAM windows (driven by the MC's MEMCFG)
               ├── PROM (read-only, byte order normalised on load)
               ├── MemoryController   0x1FA00000
               ├── HPC3               0x1FB80000
               │    ├── SCSI DMA ch0 ◄──► WD33C93 ──► CD-ROM (ID 4)
               │    ├── PBUS ch6: IOC2
               │    │    ├── Z85C30 serial (console)
               │    │    ├── 8042 + PS/2 keyboard & mouse
               │    │    ├── INT3 interrupt controller ──► CPU IP2..IP5
               │    │    └── 8254 timer
               │    └── BBRAM: DS1386 RTC + 8 KiB NVRAM
               └── Newport XL8        0x1F0F0000 (REX3, VC2, XMAP9, CMAP)
```

### Physical memory map

| Range | Device |
|---|---|
| `0x00000000–0x0007FFFF` | Low alias of the first 512 KiB of RAM (KSEG0 exception vectors) |
| `0x08000000–0x17FFFFFF` | Memory segment 0 (SIMM banks, set up through the MC) |
| `0x1F0F0000–0x1F0F1FFF` | Newport REX3 registers (GIO graphics slot) |
| `0x1FA00000–0x1FA1FFFF` | MC: memory controller, RPSS counter, virtual DMA, semaphores |
| `0x1FB80000–0x1FBFFFFF` | HPC3, including IOC2 at `0x1FBD9800` and the RTC/NVRAM at `0x1FBE0000` |
| `0x1FC00000–0x1FCFFFFF` | Boot PROM (reset vector `0xBFC00000`) |
| `0x20000000–0x27FFFFFF` | Memory segment 1 |

Any other address gives a bus error, including empty GIO expansion slots. The PROM's slot probes rely on that.

### Source layout

```
CMakeLists.txt, cmake/        build configuration (warnings, LTO, sanitizers)
include/indy/                 public headers of the emulation core
  core/      System, Scheduler, EmulationThread, FrameBuffer, Commands, Log, MmioDevice
  cpu/       Cpu interface, R4600 (interpreter + COP0/COP1), Tlb, Instruction decode
  memory/    MemoryBus, MemoryController (MC), PhysicalMap, PROM image handling
  hpc/       Hpc3, Ioc2, Int3, Pit8254, Ds1386
  scsi/      ScsiController, Wd33c93, ScsiTarget, CdRomDrive, ScsiDmaPort
  serial/    Z8530
  input/     Kbc8042 (8042 + PS/2 keyboard/mouse)
  graphics/  GraphicsSubsystem, Newport, TestPatternDisplay
src/                          implementations (same layout)
src/frontend/                 SDL3 frontend: main.cpp, SdlFrontend, HostTerminal
tests/CoreTests.cpp           unit tests for the core (no SDL)
```

The core library `indy_core` has **no SDL dependency**. Only `src/frontend` uses SDL.

## Emulated hardware

| Component | Status |
|---|---|
| **R4600 CPU** | MIPS III integer interpreter with exact branch delay slots. COP0: 48-entry TLB, exceptions, interrupts IP2–IP7, Count/Compare timer. COP1: register file, data moves, loads/stores and branches (**no FPU arithmetic yet**). |
| **Memory controller (MC)** | SIMM bank decoding from MEMCFG, with mirroring and the low alias. RPSS counter. Virtual DMA engine (fill, copy, stride, zoom, DMA TLB) with completion interrupt. Bus-error latching and IP6. Semaphores. |
| **HPC3** | SCSI DMA descriptor chains with interrupts, PBUS routing, RTC/NVRAM window, register files. |
| **IOC2 / INT3** | Interrupt aggregation into CPU IP2–IP5, including the mappable sources. Board ID `0x26` (Indy). Front panel. |
| **8254 timer** | Counter 2 at 1 MHz drives counters 0/1. Latch and read-back commands. Interrupts. |
| **Z85C30 serial** | Register pointer protocol, transmit to stdout, receive from the terminal, Rx/Tx interrupts. |
| **8042 + PS/2** | Full controller command set. Keyboard in scan-code set 2 and set 3 (the Indy uses set 3). Mouse with 3-byte streaming packets. |
| **DS1386 RTC/NVRAM** | Clock from host UTC that advances with emulated time, SGI 1940 year base, 8 KiB NVRAM saved to a file. |
| **WD33C93 SCSI** | Select-and-Transfer, plus Select followed by Transfer Info (phase by phase), in polled and DMA modes. |
| **CD-ROM (ID 4)** | Presents as a Toshiba XM-3401TA with 512/2048-byte blocks. INQUIRY, READ(6/10), READ CAPACITY, MODE SENSE/SELECT, READ TOC, sense data. Disc swaps raise UNIT ATTENTION. |
| **Newport XL8** | REX3 drawing engine: spans, blocks, patterns, logic ops, clipping, screen-to-screen copy, host transfers, lines. DCB devices: VC2 (including the hardware cursor), XMAP9, CMAP. 1280×1024 output. |
| **Not emulated yet** | Ethernet (SEEQ 80C03), audio (HAL2), parallel port, disk targets, FPU arithmetic, caches. |

## Building

### Requirements

- A C++23 compiler: Clang 17+ / Apple Clang 15+, GCC 13+, or a recent MSVC
- CMake 3.20 or newer
- **SDL3**: install it from your package manager (for example `brew install sdl3`), or let CMake download and build it (`INDY_FETCH_SDL=ON`, the default)

### Build commands

```sh
# Optimised build with debug info (the default build type)
cmake -B build
cmake --build build -j

# Debug build with AddressSanitizer + UndefinedBehaviorSanitizer, warnings as errors
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug -DINDY_SANITIZE=ON -DINDY_WARNINGS_AS_ERRORS=ON
cmake --build build-debug -j
```

Binaries are written to `<build>/bin/`: `indy` (the emulator) and `indy_tests`.

### CMake options

| Option | Default | Meaning |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `RelWithDebInfo` | `Debug`, `Release`, `RelWithDebInfo`, `MinSizeRel` |
| `INDY_ENABLE_LTO` | `ON` | Link-time optimisation for optimised builds |
| `INDY_NATIVE_ARCH` | `OFF` | Tune for the build machine (`-march=native` / `/arch:AVX2`) |
| `INDY_SANITIZE` | `OFF` | ASan + UBSan in Debug builds |
| `INDY_WARNINGS_AS_ERRORS` | `OFF` | Treat warnings as errors |
| `INDY_FETCH_SDL` | `ON` | Download SDL3 if no system copy is found |
| `INDY_BUILD_TESTS` | `ON` | Build `indy_tests` and register it with CTest |

The code builds cleanly with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`.

## Running

You need an **Indy boot PROM image**, dumped from your own machine (see [Legal](#legal)):

```sh
./build/bin/indy /path/to/indy_prom.bin
```

- The PROM can also be passed with `--prom <file>` or the `INDY_PROM` environment variable, which is handy for IDE launch configurations. The emulator refuses to start without one.
- Byte-swapped dumps are **detected and fixed automatically**: plain big-endian, 16-bit-swapped (common with EPROM readers) and 32-bit little-endian. The log says which layout was found.

### Command-line options

| Option | Meaning |
|---|---|
| `<prom.bin>` / `--prom <file>` | Boot PROM image (required) |
| `--cdrom <file.iso>` | Disc image to insert at start-up (size must be a multiple of 2048 bytes) |
| `--nvram <file>` | NVRAM backing file (default `indy_nvram.bin` in the current directory) |
| `--ram <MiB>` | Installed memory, split into SIMM banks: 16, 32, 64 (default), 96, 128, 192 or 256 |
| `--no-gfx` | Remove the Newport board. The PROM then uses the serial console in the terminal. |
| `--trace <N>` | Log the first N executed instructions (PC and opcode) |
| `--screenshot <file.ppm>` | Save the last displayed frame when the emulator exits |
| `--type <sec>:<text>` | Type text on the PS/2 keyboard after `sec` seconds (`\r` = Enter) |
| `--mouse <sec>:<dx>,<dy>[,<btn>]` | Send a mouse event after `sec` seconds. Repeatable. Uses PS/2 directions (+dy = up); buttons L=1, R=2, M=4. |
| `--unthrottled` | Run as fast as possible instead of at real Indy speed |
| `--no-vsync` | Pace frames with a timer instead of vsync |
| `-h`, `--help` | Show usage |

### Environment variables

| Variable | Meaning |
|---|---|
| `INDY_PROM` | PROM image path, used when none is given on the command line |
| `INDY_TRACE_GFX=<n>` | Log the first `n` Newport register accesses (graphics bring-up) |

### Output streams

- **stdout:** the guest's **serial console** only.
- **stderr:** emulator logs (`[INFO ]`, `[WARN ]`, `[CPU WARNING]`, `[MC DMA]`, …).

So `indy prom.bin 2>/dev/null` shows just what the Indy prints to its serial port.

### NVRAM

The DS1386's 8 KiB of battery-backed RAM is saved to the NVRAM file on exit and reloaded at start-up. The first boot with a blank file shows `NVRAM checksum is incorrect: reinitializing`. That's normal: the PROM writes its own environment (`console`, `TimeZone`, boot paths, …), and later boots keep it. Use the Command Monitor's `printenv` and `setenv` to inspect or change it. Delete the file to return to factory defaults.

### Examples

```sh
# Normal graphical boot with an install CD inserted
./build/bin/indy indy_prom.bin --cdrom IRIX-6.5.22-install.iso

# Headless: serial console only, interact from this terminal
./build/bin/indy indy_prom.bin --no-gfx

# Scripted GUI check: press Enter after the boot dialog, nudge the mouse,
# keep the final frame
./build/bin/indy indy_prom.bin --type '28:\r' --mouse '34:100,0' --screenshot shot.ppm
```

## Controls

| Input | Effect |
|---|---|
| Typing in the **terminal** | Goes to the Indy's serial console (raw mode, no local echo) |
| Typing in the **window** | Goes to the emulated PS/2 keyboard (all keys, including F-keys) |
| **Click** in the window | Captures the mouse. The host cursor hides and the SGI cursor follows. |
| **Ctrl+Alt+G** | Releases the captured mouse (switching away from the window also releases it) |
| **Alt+Enter** | Toggle fullscreen |
| **Drag & drop** a disc image | Swaps the CD in the virtual drive; the guest sees a media change |
| **Ctrl-C** in the terminal / closing the window | Clean shutdown (saves NVRAM, restores the terminal) |

The title bar shows emulation speed and whether the mouse is captured.

## Debugging and diagnostics

- **Instruction trace:** `--trace N` prints `[CPU TRACE] PC: 0xbfc003c0 -> Opcode: 0x3c023041` for the first N instructions.
- **Crash history:** if the CPU halts, the last 32 executed instructions are printed as `[CPU HISTORY]`.
- **Exception log:** the first occurrences of each exception type and PC are logged as `[CPU EXCEPTION] …`.
- **Bus errors / unemulated hardware:** each address or device warns once (`Bus error: …`, `HPC3: … not emulated yet`).
- **Final state:** on shutdown, `Emulation stopped at cycle N, CPU PC …`.
- **Graphics bring-up:** `INDY_TRACE_GFX=<n>` traces Newport register and DCB traffic.
- **Screenshots:** `--screenshot` writes a binary PPM (any image tool can convert it).

## Testing

```sh
ctest --test-dir build --output-on-failure   # or: ./build/bin/indy_tests
```

`indy_tests` is a dependency-free test runner with tests for every component:
- the CPU core (delay slots, exceptions, TLB, FPU moves, interrupts);
- the memory bus and MC (bank mapping, RPSS, DMA, bus errors);
- the SCSI stack (CD-ROM commands, HPC3 DMA chains, WD33C93 select-and-transfer and phase-by-phase sessions);
- serial, the 8042 keyboard/mouse, INT3, the 8254 and the RTC/NVRAM;
- the Newport (register read-back, DCB devices, rasterizer, hardware cursor).

Many tests replay the exact register sequences the real PROM performs. Run them in the sanitizer build too (see [Building](#building)).

## Known limitations and roadmap

**Next steps**

1. Boot an IRIX 6.5 install CD: *Install System Software* → `sash` → the installer. Expect gaps to show up in SCSI commands and the phase-by-phase SCSI path.
2. A SCSI disk target backed by an image file, for an installed system.
3. FPU arithmetic (`.S/.D/.W/.L`), which IRIX needs.
4. Ethernet (SEEQ 80C03 through HPC3) and audio (HAL2).
5. Performance: block caching or a JIT for the interpreter, and a micro-TLB.
6. Graphics: per-line display IDs, RGB pixel modes, the XL24 board, overlay/popup planes, then an HLE path to OpenGL/Vulkan for 3D.

**Current simplifications**

- CPU timing is one instruction per cycle; caches are not modelled.
- SCSI targets complete commands instantly and never disconnect or reselect.
- Some behaviour follows the PROM where the references are silent. These spots are marked `ASSUMPTION` or explained in comments; for example, untranslated MC DMA addresses are treated as physical.

## Legal

This is an independent, clean-room project and is not affiliated with Silicon Graphics or its successors. **No SGI firmware or software is included.** You must supply a PROM image and IRIX media that you are legally entitled to use, for example dumped from hardware you own. "SGI", "Indy" and "IRIX" are trademarks of their respective owners.
