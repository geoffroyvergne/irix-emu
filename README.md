# irix-emu: SGI Indy emulator

A clean-room emulator of the **Silicon Graphics Indy** (IP24 board, "Guinness" architecture), written in modern C++23 with SDL3.

**Current status:** an R5000 Indy (150 MHz, up to 256 MB, Newport XL8) runs **IRIX 6.5** to the full 4Dwm desktop: the xdm/clogin graphical login, Toolchest menus, window moves, shells, and applications such as Maya 6.5, which starts as far as its licence check. IRIX installs from the original CDs onto a raw disk image, and MAME's uncompressed CHD disks boot as they are. The real Indy boot PROM (5.3 Rev B10) passes its power-on diagnostics. Speed is about that of a real Indy (with the PGO build).

---

## Table of contents

1. [Purpose and goals](#purpose-and-goals)
2. [Development strategy](#development-strategy)
3. [Architecture](#architecture)
4. [Emulated hardware](#emulated-hardware)
5. [Building](#building)
6. [Running](#running)
7. [Hard disk images](#hard-disk-images)
8. [Controls](#controls)
9. [Debugging and diagnostics](#debugging-and-diagnostics)
10. [Testing](#testing)
11. [Known limitations and roadmap](#known-limitations-and-roadmap)
12. [Legal](#legal)

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
- Public chip datasheets (MIPS R4000/R4600/R5000, WD33C93, Z85C30, Intel 8042/8254, Dallas DS1386)

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
R5000 CPU ──► MemoryBus (64 KiB page table, big-endian storage)
               ├── RAM windows (driven by the MC's MEMCFG)
               ├── PROM (read-only, byte order normalised on load)
               ├── MemoryController   0x1FA00000
               ├── HPC3               0x1FB80000
               │    ├── SCSI DMA ch0 ◄──► WD33C93 ──► Disk (ID 1), CD-ROM (ID 4)
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
  cpu/       Cpu interface, R5000 (interpreter + COP0/COP1, optional R4600 mode), Fpu, Tlb, Instruction decode
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
| **R5000 CPU** | An R5000PC at 150 MHz (32 KB primary caches, no secondary cache), as in the later Indys: MIPS IV (conditional moves, PREF, indexed FP loads/stores, multiply-add, RECIP/RSQRT, eight FP condition codes) on top of a MIPS III integer interpreter with exact branch delay slots. COP0: 48-entry TLB, exceptions, interrupts IP2–IP7, Count/Compare timer. COP1: register file, moves, loads/stores, branches, and IEEE arithmetic, conversions and compares (`.S/.D/.W/.L`) under the guest rounding mode, with FCR31 cause/flag/enable bits and FPE traps. |
| **Memory controller (MC)** | SIMM bank decoding from MEMCFG, with mirroring and the low alias. RPSS counter. Virtual DMA engine (fill, copy, stride, zoom, DMA TLB) with completion interrupt. Bus-error latching and IP6. Semaphores. |
| **HPC3** | SCSI DMA descriptor chains with interrupts, PBUS routing, RTC/NVRAM window, register files. |
| **IOC2 / INT3** | Interrupt aggregation into CPU IP2–IP5, including the mappable sources. Board ID `0x26` (Indy). Front panel. |
| **8254 timer** | Counter 2 at 1 MHz drives counters 0/1. Latch and read-back commands. Interrupts. |
| **Z85C30 serial** | Register pointer protocol, transmit to stdout, receive from the terminal, Rx/Tx interrupts. |
| **8042 + PS/2** | Full controller command set. Keyboard in scan-code set 2 and set 3 (the Indy uses set 3). Mouse with 3-byte streaming packets. |
| **DS1386 RTC/NVRAM** | Clock from host UTC that advances with emulated time, SGI 1940 year base, 8 KiB NVRAM saved to a file. |
| **WD33C93 SCSI** | Select-and-Transfer, plus Select followed by Transfer Info (phase by phase), in polled and DMA modes. |
| **Hard disk (ID 1)** | Raw image file, 512-byte blocks, read/write in place. INQUIRY (Seagate ST32550N), READ/WRITE(6/10), READ CAPACITY, MODE SENSE/SELECT(6/10) with geometry pages, FORMAT UNIT, VERIFY, SYNCHRONIZE CACHE, READ DEFECT DATA, power-on UNIT ATTENTION. `fx` labels and partitions it. |
| **CD-ROM (ID 4)** | Presents as a Toshiba XM-3401TA with 512/2048-byte blocks. INQUIRY, READ(6/10), READ CAPACITY, MODE SENSE/SELECT, READ TOC, sense data. Disc swaps raise UNIT ATTENTION. |
| **Newport XL8** | REX3 drawing engine: spans, blocks, patterns, logic ops, clipping, screen-to-screen copy, host transfers, lines. DCB devices: VC2 (including the hardware cursor), XMAP9, CMAP. Scan-out follows the VC2 display-ID tables, so each window gets its own XMAP9 mode (colour map, CI/RGB). RGB mode drawing packs 24-bit colours to 8-bit `BBGGGRRR`, and RGB display modes send each channel through its gamma ramp. The popup planes (menus, 4Dwm's move outline) are drawn on top through the popup colour map. Runs IRIX's X server (`Xsgi`) and the xdm login. 1280×1024 output. |
| **Not emulated yet** | Ethernet (SEEQ 80C03), audio (HAL2), parallel port, CPU caches (IDE's `icache`/`dcache` tests report a CPU-module failure; harmless). |

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
| `INDY_PGO_GENERATE` | `OFF` | Instrumented build that records a profile for profile-guided optimisation |
| `INDY_PGO_PROFILE` | *(empty)* | Build optimised with this merged `.profdata` profile |

### Fastest build: profile-guided optimisation

The emulator spends nearly all its time in the CPU interpreter, which benefits from profile-guided optimisation (PGO): the compiler lays out the hot paths from a recording of real IRIX code. One command trains on your own PROM and disk image (the disk is not modified; training runs on a copy) and builds `build-pgo/bin/indy`:

```sh
tools/build-pgo.sh indy_prom.bin irix.img        # optionally: training seconds (default 120)
./build-pgo/bin/indy indy_prom.bin --disk irix.img --ram 256
```

It takes about two minutes and makes the interpreter about 13% faster (133% of a real Indy unthrottled, against 118%). Re-run it after updating the sources. A stale profile still works but helps less.

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
| `--disk <file>` | SCSI hard disk image, read and written in place: a raw image or an uncompressed MAME CHD (detected from the contents). Repeatable: the first disk is SCSI ID 1 (`dks0d1`, the system disk), then IDs 2, 3, 5, 6, 7 (the CD-ROM's ID 4 is skipped). |
| `--disk-size <MiB>` | With `--disk`: create a blank (sparse) image of this size for any disk file that doesn't exist |
| `--nvram <file>` | NVRAM backing file (default `indy_nvram.bin` in the current directory) |
| `--mac <xx:xx:xx:xx:xx:xx>` | Ethernet address stored in the NVRAM (PROM variable `eaddr`). Default: keep the NVRAM's own, or `08:00:69:0e:1d:01` if it has none. |
| `--cpu r5000\|r4600` | Processor: `r5000` (the default: an R5000PC Indy, MIPS IV, 32 KB caches) or `r4600` (the earlier MIPS III Indy, 16 KB caches, 133 MHz). Software built for MIPS IV, such as parts of Maya 6.5, needs the R5000. On the R4600 it fails with `Program not supported by architecture`. |
| `--mhz <MHz>` | CPU clock (default 150 for the R5000, 133 for the R4600; real Indys used 100–180). The PROM measures it at power-on and IRIX follows. A higher clock needs more host speed. |
| `--ram <MiB>` | Installed memory, split into SIMM banks: 16, 32, 64 (default), 96, 128, 192 or 256. Use **256** for the IRIX 6.5 desktop: with 64 MB, only about 13 MB is free at the login screen, before a session starts. |
| `--no-gfx` | Remove the Newport board. The PROM then uses the serial console in the terminal. |
| `--trace <N>` | Log the first N executed instructions (PC and opcode) |
| `--screenshot <file.ppm>` | Save the last displayed frame when the emulator exits |
| `--screenshot-at <sec>:<file.ppm>` | Save the displayed frame `sec` seconds after start. Repeatable. |
| `--type <sec>:<text>` | Type text on the PS/2 keyboard after `sec` seconds (US layout, capitals and symbols included; `\r` = Enter). Repeatable. |
| `--mouse <sec>:<dx>,<dy>[,<btn>]` | Send a mouse event after `sec` seconds. Repeatable. Uses PS/2 directions (+dy = up); buttons L=1, R=2, M=4. |
| `--unthrottled` | Run as fast as possible instead of at real Indy speed |
| `--no-vsync` | Pace frames with a timer instead of vsync |
| `-h`, `--help` | Show usage |

### Environment variables

| Variable | Meaning |
|---|---|
| `INDY_PROM` | PROM image path, used when none is given on the command line |
| `INDY_TRACE_GFX=<n>` | Log the first `n` Newport register accesses (graphics bring-up) |
| `INDY_LOG_SPEED=1` | Print the speed meter (as in the window title) to the log once a second, with the time since start |
| `INDY_TRACE_KBC=1` | Log every byte between the CPU and the 8042 keyboard/mouse controller, and each host key |
| `INDY_TRACE_EXCEPTIONS=1` | Also log routine CPU exceptions (TLB refills, syscalls, …), which IRIX takes constantly. Without it, only unusual ones are shown (bus/address errors, reserved instructions, faults on the NULL page). |
| `INDY_TRACE_NULL=1` | On the first access to the NULL page, dump the CPU registers and the last 64 instructions |
| `INDY_TRACE_SCSI=1` | Log every SCSI command: CDB, transfer count, bytes moved, status, pauses/resumes |
| `INDY_TRACE_PC=<hex>` | Log a0–a3 (and the strings they point to) each time the CPU executes that address |

### Output streams

- **stdout:** the guest's **serial console** only.
- **stderr:** emulator logs (`[INFO ]`, `[WARN ]`, `[CPU WARNING]`, `[MC DMA]`, …).

So `indy prom.bin 2>/dev/null` shows just what the Indy prints to its serial port.

### NVRAM

The DS1386's 8 KiB of battery-backed RAM is saved to the NVRAM file on exit and reloaded at start-up. The first boot with a blank file shows `NVRAM checksum is incorrect: reinitializing`. That's normal: the PROM writes its own environment (`console`, `TimeZone`, boot paths, …), and later boots keep it. Use the Command Monitor's `printenv` and `setenv` to inspect or change it. Delete the file to return to factory defaults.

On a real Indy the NVRAM also holds the factory **Ethernet address**, which the PROM shows as the read-only `eaddr` variable. The IRIX kernel reads it at boot and crashes if it is missing (a TLB miss on address 0 right after the installer copies the miniroot). The emulator stores an address when the NVRAM has none, and updates the NVRAM checksum so the PROM keeps its settings. Give each emulated machine on the same network its own address with `--mac`.

### Examples

```sh
# Normal graphical boot with an install CD inserted
./build/bin/indy indy_prom.bin --cdrom IRIX-6.5.22-install.iso

# First time: create a 4 GiB system disk next to the install CD
# (then label it with fx: see "Hard disk images")
./build/bin/indy indy_prom.bin --cdrom IRIX-6.5.22-install.iso --disk irix.img --disk-size 4096

# Headless: serial console only, interact from this terminal
./build/bin/indy indy_prom.bin --no-gfx

# Scripted GUI check: press Enter after the boot dialog, nudge the mouse,
# keep the final frame
./build/bin/indy indy_prom.bin --type '28:\r' --mouse '34:100,0' --screenshot shot.ppm
```

## Hard disk images

The Indy's system disk is attached at SCSI ID 1 (`dksc(0,1,…)`), the conventional system disk position. Two image formats work, and the emulator recognises them from the file's contents:

- **Raw image** (the default for new disks). Block *n* of the disk is bytes `n*512 … n*512+511` of the file. There's no container format, so images are interchangeable with `dd` dumps of real SGI disks.
- **MAME CHD, uncompressed** (`.chd`, v5). Ready-made IRIX disks for MAME's Indy driver work as they are: `--disk irix65.chd`. The file stays sparse. Blocks that were never written take no space, and new data is appended to the file as IRIX writes, as MAME does. The CHD header's checksums aren't updated, so `chdman verify` reports a mismatch after the emulator has written to it, but `chdman extracthd` and `chdman info` still read it. Keep a backup (`cp -c`) of a CHD you care about.

  A MAME setup may come with **several disks**. Pass them in SCSI ID order, `--disk irix65.chd --disk disk2.chd`. If the system's `/etc/fstab` mounts a disk you don't have (for example `/dev/dsk/dks0d2s0 /disk2`), IRIX boots normally but reports `mount: … giving up on: /disk2`. To stop that, remove the line as root:

  ```sh
  cp /etc/fstab /etc/fstab.orig
  grep -v ' /disk2 ' /etc/fstab.orig > /etc/fstab
  ```

  **Compressed CHDs** (MAME's default) and CHDs that depend on a parent image are refused, with a message saying how to convert them:

  ```sh
  chdman copy -i disk.chd -o disk-rw.chd -c none   # uncompressed CHD, stays compact
  chdman extracthd -i disk.chd -o disk.img         # or a raw image (as large as the disk)
  ```

### 1. Create the image

The recommended way is to let the emulator create it. `--disk-size` only takes effect when the file doesn't exist yet:

```sh
./build/bin/indy indy_prom.bin --disk irix.img --disk-size 4096 --cdrom IRIX-install.iso
```

The file is created **sparse**: it reports 4 GiB but uses almost no space until IRIX writes to it. Alternatively, create it with host tools. Any size that is a multiple of 512 bytes works:

| Host | Command |
|---|---|
| macOS | `mkfile -n 4g irix.img` |
| Linux / macOS | `truncate -s 4G irix.img` |
| Windows | `fsutil file createnew irix.img 4294967296` |

**Choosing a size.** IRIX 6.5 itself fits in a few gigabytes. 4 GiB (`--disk-size 4096`) is a comfortable default for the base system plus the overlays and Applications CDs. Choose 8–9 GiB or more for large packages such as Maya. The size can't be changed after IRIX is installed, so err on the generous side: sparse files cost nothing until used.

### 2. Label and partition it with `fx`

A blank image has no SGI disk label. The PROM reports `dks0d1s10: volume header not valid`, and the installer stops with `swap partition (0) is not a valid swap area`. Label it with **`fx`**, SGI's disk utility, booted from the install CD exactly as on a real Indy. This works on the graphical console (type in the window) and on the serial console (`--no-gfx`, type in the terminal).

1. From the System Maintenance Menu choose **Enter Command Monitor** (option 5), then boot `fx` through `sash`:
   ```
   >> boot -f dksc(0,4,8)sashARCS dksc(0,4,7)stand/fx.ARCS --x
   ```
2. Answer the device prompts with **Enter** to accept each default:
   ```
   fx: "device-name" = (dksc)
   fx: ctlr# = (0)
   fx: drive# = (1)
   ...opening dksc(0,1,0)
   ...drive selftest...OK
   Scsi drive type == SEAGATE ST32550N        0022
   ...creating default bootinfo
   ...created default partitions, use /repartition menu to change
   ```
3. Make it a root drive: `r` (repartition) → `ro` (rootdrive) → **Enter** to accept `xfs` → `yes` to confirm. For a 4 GiB image:
   ```
   part  type        blocks            Megabytes   (base+size)
     0: xfs      266240 + 8122368      130 + 3966      root filesystem
     1: raw        4096 + 262144         2 + 128       swap (the installer puts the miniroot here)
     8: volhdr        0 + 4096           0 + 2         volume header
    10: volume        0 + 8388608        0 + 4096      whole disk
   ```
4. Leave with `..` then `exi`. The label is written to block 0 of the image, and you're back at the menu.

**Check it:** in the Command Monitor, `hinv` lists `SCSI Disk: scsi(0)disk(1)`. You can also inspect the label from the host: the first four bytes of the image are the SGI volume-header magic `0B E5 A9 41`:

```sh
xxd -l 4 irix.img      # 00000000: 0be5 a941
```

### 3. Install IRIX

With the disk labelled and the install CD inserted, choose **Install System Software** (option 2), accept **Local CD-ROM** (`scsi(0)cdrom(4)`) and press **Enter** at `Insert the installation CD-ROM`. The installer copies the miniroot (~20 MB) onto the swap partition and boots it. On a new disk the miniroot then offers to make the root filesystem: answer `yes`, `y`, then `4096` for the block size. It mounts it on `/root` and opens the `Inst>` menu. Swap CDs when the installer asks, by **dragging the next ISO onto the window**.

> **"It appears that a miniroot install failed":** the NVRAM remembers an unfinished install. Answer **`r`** to copy the miniroot again. Answer `c` only if the miniroot copy on this disk image's swap partition finished. With a new or re-created disk image, `c` boots a kernel whose root is empty, and it panics with `VFS_mountroot: no root found`.

> **Headless installs:** if you run with `--no-gfx`, first set the PROM to the serial console (`setenv console d` in the Command Monitor). Otherwise `sash` tries to draw graphical dialogs with no graphics board.

### 4. First boot

After the install, reboot (or start the emulator again with only `--disk`). The PROM boots `/unix` from the disk. About a minute later, IRIX offers two consoles at the same time:

- **The window:** the X server starts and shows the graphical **"Welcome to IRIS"** xdm login. Click in the window to capture the mouse, then type there.
- **The terminal:** `IRIS console login:` on the serial port. Type there (log in as `root`, no password on a fresh system). This works even with `--no-gfx`.

Nothing is hung when the terminal stops at `IRIS console login:`. The system is waiting for a login on the serial console, while the graphical login appears in the window.

### 5. Keep backups

An installed disk is just a file, so snapshot it before risky changes. Use a sparse-aware copy, or the copy expands to its full size:

| Host | Command |
|---|---|
| macOS (APFS) | `cp -c irix.img irix-backup.img` (instant clone) |
| Linux | `cp --sparse=always irix.img irix-backup.img` |
| any | `tar -cSf irix.tar irix.img` (`-S` keeps holes) |

Don't copy or edit the image while the emulator is running: writes go straight to the file, and it's flushed when the emulator exits.

### Limits

- One hard disk, at SCSI ID 1, chosen at start-up. Disks can't be hot-swapped; CDs can.
- The emulated drive reports a fixed geometry (16 heads × 63 sectors per track, cylinders derived from the size). IRIX addresses disks by block number, so the geometry only affects what `fx` displays.
- `FORMAT UNIT` is accepted but doesn't rewrite the image, since there's no physical medium to format. Use `fx`'s label/partition menus as above; there's no need to "format" the disk.

## Controls

| Input | Effect |
|---|---|
| Typing in the **terminal** | Goes to the Indy's serial console (raw mode, no local echo) |
| Typing in the **window** | Goes to the emulated PS/2 keyboard (all keys, including F-keys) |
| **Click** in the window | Captures the mouse. The host cursor hides and the SGI cursor follows. |
| **Ctrl+Alt+G** | Releases the captured mouse (switching away from the window also releases it) |
| **Ctrl+Alt+V** (or **Cmd+V** on macOS) | Paste the host clipboard: it is typed into the PS/2 keyboard (see below). Press again to cancel a long paste. |
| **Alt+Enter** | Toggle fullscreen |
| **Drag & drop** a disc image | Swaps the CD in the virtual drive; the guest sees a media change |
| **Ctrl-C** in the terminal / closing the window | Clean shutdown (saves NVRAM, restores the terminal) |

The title bar shows emulation speed and whether the mouse is captured.

### Host-GPU OpenGL (glpass)

IRIX's own OpenGL renders in software on the emulated CPU, too slow for 3D applications. [glpass/](glpass/) is a separate component in this repository: a replacement `libGL.so` for IRIX that sends a program's OpenGL calls to the emulator, which draws them with the host GPU (macOS) and shows them in the program's window. Quake III and Maya 6.5 run with it. See [glpass/README.md](glpass/README.md) for its design, building, installing in IRIX and use.

### Copy and paste

- **Into the graphics window** (PROM console, IRIX): copy text on the host, click the emulator window and press **Ctrl+Alt+V** (or **Cmd+V** on macOS). The text is *typed* through the emulated PS/2 keyboard, the only way it can reach the Indy, using the layout given with `--keymap` (US by default; see the note below). Capitals and symbols use Shift; line breaks (`\n`, `\r` or `\r\n`) become Enter; tabs become Tab.
  - Typing starts once you release the hotkey's modifiers, so the Indy doesn't see Ctrl/Alt/Cmd held. It runs at about 1,000 characters per second.
  - Characters with no key on a US keyboard (accented letters, emoji, …) are skipped, and the log says how many.
  - Pressing the hotkey again during a paste cancels it.
- **Into the serial console** (`--no-gfx`): paste into the terminal as usual. Bytes go straight to the Z85C30 serial port, unchanged.

The same text-to-keys conversion is used by `--type`, so scripted typing also handles capitals and symbols.

> **Keyboard layouts:** the emulator sends *physical* key positions, as a real PS/2 keyboard does, and IRIX applies its own keymap. Typing on your keyboard therefore works when IRIX's layout matches your keyboard (e.g. a French AZERTY Mac and an IRIX set up for French). Paste and `--type` must pick the keys for IRIX's layout: tell the emulator which one with `--keymap us|fr` (or `INDY_KEYMAP=fr`; default `us`). With `--keymap fr`, paste types AZERTY keys, AltGr characters (`@ # { } [ ] | \ ~ ^ ``) and the French accented letters (`é è à ç ù`) included. With the wrong layout, `a`/`q`, digits and most symbols come out wrong (for example `/` becomes `!`).

## Debugging and diagnostics

- **Instruction trace:** `--trace N` prints `[CPU TRACE] PC: 0xbfc003c0 -> Opcode: 0x3c023041` for the first N instructions.
- **Crash history:** if the CPU halts, the last 32 executed instructions are printed as `[CPU HISTORY]`.
- **Quiet by default:** what IRIX does routinely is not logged: hardware probes that take bus errors (empty GIO slots), unemulated registers it tolerates, exceptions the kernel handles. `INDY_VERBOSE=1` shows them: the first occurrence of each exception type and PC (`[CPU EXCEPTION] …`), and each undecoded address or device once (`Bus error: …`, `HPC3: … not emulated yet`).
- **Exception log:** `INDY_TRACE_EXCEPTIONS=1` logs the first occurrence of every exception, routine ones included.
- **Final state:** on shutdown, `Emulation stopped at cycle N, CPU PC …`.
- **DMA:** `INDY_TRACE_DMA=1` logs the first memory controller DMA transfers (silent otherwise; aborts are always reported).
- **Graphics bring-up:** `INDY_TRACE_GFX=<n>` traces Newport register and DCB traffic.
- **Screenshots:** `--screenshot` writes a binary PPM (any image tool can convert it).

## Testing

```sh
ctest --test-dir build --output-on-failure   # or: ./build/bin/indy_tests
```

`indy_tests` is a dependency-free test runner with tests for every component:
- the CPU core (delay slots, exceptions, TLB, FPU moves and IEEE arithmetic, interrupts);
- the memory bus and MC (bank mapping, RPSS, DMA, bus errors);
- the SCSI stack (CD-ROM and disk commands, HPC3 DMA chains, WD33C93 select-and-transfer and phase-by-phase sessions);
- serial, the 8042 keyboard/mouse, INT3, the 8254 and the RTC/NVRAM;
- the Newport (register read-back, DCB devices, rasterizer, hardware cursor).

Many tests replay the exact register sequences the real PROM performs. Run them in the sanitizer build too (see [Building](#building)).

## Known limitations and roadmap

**Next steps**

1. Use the installed system: the 4Dwm desktop after login, applications, and the remaining overlay and application CDs. IRIX 6.5 now installs, boots from disk and reaches the X login.
2. Ethernet (SEEQ 80C03 through HPC3) and audio (HAL2).
3. Performance: a decoded-block cache or JIT for the interpreter. With the PGO build, the heaviest IRIX phases (kernel start-up, boot scripts, starting a desktop session) run at 75–95% of a real Indy; everything else keeps up.
4. Graphics: dithering and shaded (Gouraud) spans, the XL24 board, then an HLE path to OpenGL/Vulkan for 3D.

**Current simplifications**

- CPU timing is one instruction per cycle; caches are not modelled.
- Count and Random are computed from an instruction counter when read, and the Count = Compare interrupt is a precomputed deadline.
- The interpreter keeps three translation caches, all invisible to the guest: a 256-entry micro-TLB in front of the 48-entry TLB, the host page of the current code page for instruction fetch, and a host-page cache for loads and stores to RAM. Context and mode switches (ASID, KSU/EXL/ERL) simply miss. TLB writes drop the affected pages, and memory remaps drop everything.
- SCSI targets have a single logical unit (LUN 0) and complete commands instantly, without disconnecting or reselecting. A select-and-transfer whose data phase outlasts the transfer count pauses ("unexpected phase") and resumes when the driver re-issues it, as on the real chip.
- Some behaviour follows the PROM where the references are silent. These spots are marked `ASSUMPTION` or explained in comments; for example, untranslated MC DMA addresses are treated as physical.

## Legal

This is an independent, clean-room project and is not affiliated with Silicon Graphics or its successors. **No SGI firmware or software is included.** You must supply a PROM image and IRIX media that you are legally entitled to use, for example dumped from hardware you own. "SGI", "Indy" and "IRIX" are trademarks of their respective owners.
