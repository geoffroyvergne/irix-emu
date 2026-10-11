# Prompt 0

Act as an expert systems engineer and emulator developer specializing in C++, CMake, and low-level hardware emulation. 

We are starting a brand new, clean-room emulator project for the Silicon Graphics (SGI) Indy workstation (Guinness architecture). The ultimate long-term goals of this project are:
1. To boot a pre-installed virtual disk image or install IRIX 6.5.22 from original ISOs.
2. To eventually support 3D applications like Maya or Softimage by implementing a high-level emulation (HLE) or low-level emulation (LLE) layer for SGI graphics pipelines (like XZ/Extreme or translation to host OpenGL).
3. To feature modern UI integrations, such as drag-and-dropping ISO files directly onto the emulator window to swap virtual SCSI CD-ROM media.

For our technical stack, we are strictly using:
- Modern C++ (C++20 or C++23)
- CMake as the build system
- SDL3 (or SDL2, please advise which is better suited for raw framebuffer rendering and drag-and-drop today) for window management, inputs, and visual output.

Right now, we only want to build Phase 1: The Modern C++ Architecture Framework. Do not write the CPU interpreter or specific chip logic yet. Instead, provide a clean, modern, object-oriented, and highly modular project layout.

Please generate:
1. A robust `CMakeLists.txt` file that properly links SDL, handles compiler optimization flags, and sets up a clean directory structure.
2. A main entry point (`main.cpp`) initializing SDL, creating a 1024x768 window (standard SGI resolution), handling the main execution loop (tick/frame timing), and gracefully shutting down.
3. A robust SDL Event Handler snippet that specifically captures `SDL_DROPFILE` events to print the dropped ISO file path to the console (as a placeholder for our SCSI CD-ROM swap logic).
4. Skeletons/Interfaces for the core architectural components in modern C++ (e.g., `System`, `CPU` (MIPS R4600), `MemoryBus`, `SCSIController`, `GraphicsSubsystem`). Show how they should interact via a central bus or system class.

Please keep the code clean, well-commented, and optimized for low-latency emulation loops. Do not implement heavy logic yet, just the clean backbone.

# Prompt 2

Excellent! Phase 1 is compiling and running perfectly with the SDL window and the drag-and-drop placeholder. Let's move to Phase 2: Memory Bus Mapping and PROM Loading.

Our goal in this phase is to establish the physical memory map of the SGI Indy and create a system to load an official SGI Indy PROM dump (usually a 512KB or 1MB binary file) into our emulator's memory space, prepping the CPU to read its first instructions.

Based on standard SGI Indy specs (Guinness architecture):
- Physical RAM usually starts at 0x08000000.
- The SGI PROM (BIOS) is mapped to physical address 0x1FC00000 (standard MIPS reset vector area).

Please implement:
1. The `MemoryBus` class:
   - It should hold a dynamic allocation or fixed array representing system RAM (let's start with a configurable size, e.g., 64MB or 128MB).
   - It should hold a vector for the `PROM` memory (up to 1MB).
   - Implement `read32(uint32_t address)` and `write32(uint32_t address, uint32_t value)` methods.
   - Crucial constraint: SGI Indy hardware is Big-Endian. Since our host PC is likely Little-Endian, implement proper byte-swapping logic inside the read/write methods where necessary, or abstract it cleanly.

2. A PROM Boot Loader utility:
   - A method inside the `System` or `MemoryBus` class that takes a file path to a local raw binary file (the PROM dump), reads it, and copies it directly into the mapped 0x1FC00000 region.
   - Include basic error checking (file not found, file too large).

3. CPU Initialization Connection:
   - Wire the `CPU` class skeleton to hold a reference or pointer to the `MemoryBus`.
   - Implement a `reset()` method in the CPU that sets the Program Counter (PC) to the MIPS physical reset vector address: 0x1FC00000.
   - Create a single step `tick()` method stub in the CPU that reads 4 bytes from the current PC via the MemoryBus, ready for decoding in the next phase.

Please provide clean, production-ready modern C++ code for these updated classes. Integrate them into our existing structure and update our `main.cpp` to accept a command-line argument for the PROM file path (e.g., `./sgi_emulator /path/to/indy_prom.bin`).

# Prompt 3

Great news! We have an authentic `indy_prom.bin` file ready to be loaded "/Users/gv/data/emulators/indy/indy_prom.bin". Now that Phase 2 is complete, the Memory Bus is mapped, and the CPU PC points to 0x1FC00000, we need to build Phase 3: The MIPS R4600 Instruction Decoder and Basic Execution Core.

The MIPS III/IV architecture uses fixed-length 32-bit instructions. We need to implement a clean opcode decoding mechanism to start parsing and executing what the SGI PROM is feeding into the CPU.

Please implement the following in the `CPU` class:

1. MIPS Instruction Decoding Structure:
   - Create a clean way to split a 32-bit instruction into MIPS fields: `opcode` (6 bits), `rs` (5 bits), `rt` (5 bits), `rd` (5 bits), `shamt` (5 bits), `funct` (6 bits), and `immediate`/`offset` (16 bits). Using a C++ struct or bitwise helper functions is preferred.

2. The Core Execution Loop (`tick()` function):
   - Update the `tick()` method to read the 32-bit instruction at the current Program Counter (PC).
   - Fetch the instruction from the MemoryBus (remembering our Big-Endian requirement from Phase 2).
   - Log the current PC and the hex value of the instruction to the console for debugging (e.g., `PC: 0x1FC00000 -> Opcode: 0x24040000`).
   - Implement a basic `switch` statement or a jump table based on the top 6 bits (`opcode`).

3. First Critical Opcodes & Fallback:
   - Implement the decoding for basic jump/branch and register setup opcodes that early PROMs use to initialize the system (such as `ADDIU`, `ORI`, `LUI`, `J`, `BAL`).
   - For now, if the CPU encounters an unimplemented instruction, do not crash the emulator. Instead, print a clear warning to the console: `[CPU WARNING] Unimplemented instruction 0xXXXXXXXX at PC 0xXXXXXXXX`, increment the PC by 4, and allow the execution loop to continue.

4. Register State Initialization:
   - Add the 32 general-purpose 64-bit registers (MIPS R4600 is 64-bit, so use `uint64_t gpr[32]`) and ensure Register 0 (`$zero`) is hardwired to always return 0, even if written to.

Please provide the updated `CPU.hpp` and `CPU.cpp` files incorporating this modern C++ design, optimized for readability and future expansion as we add more instructions.

# Prompt 4

Awesome! Phase 3 is working, and we can see the CPU decoding the first instructions from the authentic SGI Indy PROM. 

Now we need to implement Phase 4: The MIPS Coprocessor 0 (COP0) and Virtual Memory (TLB) Foundations. On the MIPS R4600 architecture, COP0 is responsible for system configuration, MMU/TLB address translation, and exception handling. The SGI PROM will fail to initialize if COP0 registers are missing.

Please implement the following:

1. COP0 Registers Array:
   - Add an array or struct to the `CPU` class representing the COP0 registers (at least 32 registers of 64 bits each).
   - Define explicit constants or an enum for the critical COP0 registers that the SGI PROM touches early on:
     * `Index` (Reg 0)
     * `Random` (Reg 1)
     * `EntryLo0` (Reg 2)
     * `EntryLo1` (Reg 3)
     * `Context` (Reg 4)
     * `PageMask` (Reg 5)
     * `Wired` (Reg 6)
     * `BadVAddr` (Reg 8)
     * `Count` (Reg 9 - incremented automatically in the main loop for timing)
     * `EntryHi` (Reg 10)
     * `Compare` (Reg 11)
     * `Status` (Reg 12 - extremely critical for kernel mode and interrupt masks)
     * `Cause` (Reg 13 - exception tracking)
     * `EPC` (Reg 14 - Exception Program Counter)
     * `PRId` (Reg 15 - Processor ID, should return the correct R4600 value, e.g., 0x00002020 or similar for SGI Indy)
     * `Config` (Reg 16 - Cache and architecture configuration)

2. COP0 Move Instructions:
   - Implement the MIPS instructions to read/write from/to COP0:
     * `MTC0 rt, rd`: Move To Coprocessor 0 (Copy GPR[rt] into COP0[rd]).
     * `MFC0 rt, rd`: Move From Coprocessor 0 (Copy COP0[rd] into GPR[rt]).

3. Basic MMU Segment Translation (The MIPS Memory Map):
   - MIPS uses virtual addressing by default. Update the `MemoryBus` or `CPU` to handle the standard MIPS 32-bit virtual memory segments before accessing physical memory:
     * **KSEG0 (0x80000000 to 0x9FFFFFFF):** Uncached, directly mapped. Convert to physical by stripping the top bit (Subtract 0x80000000). SGI PROM code runs heavily here.
     * **KSEG1 (0xA0000000 to 0xBFFFFFFF):** Cached, directly mapped. Convert to physical by stripping the top 3 bits (Subtract 0xA0000000). The SGI reset vector (0x1FC00000) is physically inside this zone when addressed virtually as 0xBFC00000.
     * **KUSEG / KSEG2 / KSEG3:** For now, if accessed, print a TLB unhandled warning.

4. Architecture Update:
   - Adjust the CPU's Program Counter (PC) to initialize at the correct virtual address `0xBFC00000` (which maps to physical `0x1FC00000` via KSEG1 rules). Update the execution loop to translate the PC before fetching instructions from the `MemoryBus`.

Please provide the updated, well-commented code files matching our modern C++ standard.

# Prompt 5

Fantastic! The compilation is successful and the architecture handles COP0 and KSEG memory segments perfectly. 

Since the CPU is now hitting a wall at the Memory Controller (MC) registers, let's build Phase 5: The SGI Indy Memory Controller (MC) Subsystem. 

On the SGI Indy (Guinness architecture), the Memory Controller chip sits on the local bus and occupies the physical memory range 0x1FA00000 to 0x1FA000FF. The SGI PROM reads and writes to this space early on to configure memory banks, GIO bus timing, and CPU subsystem parameters.

Please implement the following:

1. The `MemoryController` (MC) Class:
   - Create a clean class or component for the SGI MC.
   - It should hold a structural array or a mapped structure for its registers (each register is 32-bit, though some are 64-bit on upper paths, let's handle them as 32-bit registers for now).
   - Define explicit constants for the critical MC registers that the PROM touches:
     * `MC_CPUCTRL0` / `MC_CPUCTRL1` (0x1FA00000 / 0x1FA00004) - CPU Control registers.
     * `MC_WATCHDOG` (0x1FA00008) - Watchdog timer.
     * `MC_SYSID` (0x1FA0000C) - System ID (Crucial: The PROM checks this to see if it's running on an Indy or Indigo2. For Indy/Guinness, bit 4 is usually set to 1, or returns a specific value like 0x30. Let's make it configurable or return a standard Indy ID).
     * `MC_MEMCFG0` / `MC_MEMCFG1` (0x1FA000C0 / 0x1FA000C4) - RAM Bank Configuration registers (tells the PROM how much RAM is installed in each bank).

2. Memory Bus Integration (Memory-Mapped I/O):
   - Update the `MemoryBus::read32` and `MemoryBus::write32` methods to intercept accesses to the physical range `0x1FA00000` - `0x1FA000FF`.
   - Forward these read/write requests directly to your new `MemoryController` class.
   - Ensure proper Big-Endian handling for these register reads/writes.

3. Standard/Sanity Default Values:
   - When the Memory Controller initializes, populate its registers with safe default values that a real SGI Indy hardware board would report, ensuring the PROM doesn't freeze waiting for an invalid flag. For example, `MC_SYSID` should correctly report an Indy board revision.

Please generate the code for `MemoryController.hpp` and `MemoryController.cpp`, and show how to wire it up cleanly inside our central `System` and `MemoryBus` framework.

# Prompt 6

The Memory Controller is now integrated and compiling perfectly. The next logical dependency is the HPC3 (High-performance Peripheral Controller) chip, specifically its integrated UART serial port.

On the SGI Indy, early PROM boot diagnostic text is sent out through the first serial port (UART 0) mapped via the HPC3 subsystem before the main graphics framebuffer is even initialized. Capturing this text and printing it to our host console is the single best way to debug the boot sequence.

The HPC3 base address for the serial port (Zilog Z8530 compatible UART) is physically mapped at:
- `0x1FBD9800` to `0x1FBD9FFF` (Specifically, UART Channel B/A Control and Data registers are located in this window, with `0x1FBD9804` and `0x1FBD980C` commonly used for data/status).

Please implement Phase 6: The HPC3 UART Console Redirection:

1. Create an `HPC3` Class:
   - This component will manage the SGI Indy's main input/output peripheral bus.
   - For this sub-phase, implement a minimalist UART emulation loop inside it. 
   - Define a simple state tracker for the TX (Transmit) buffer status register so the PROM thinks the serial port is always ready to receive characters (e.g., bit 2 of the status register often needs to report "Transmit Buffer Empty" / 1).

2. Memory Bus Interception:
   - Update `MemoryBus::read32` and `MemoryBus::write32` to trap the physical range `0x1FBD0000` to `0x1FBDFFFF` (the general HPC3 area).
   - Route writes inside the UART register offsets directly to a function that captures the ASCII byte character.

3. Host Console Redirection:
   - When the PROM writes a 32-bit word (containing an ASCII byte) to the UART Data transmit register, intercept it, extract the character, and immediately print it to the host machine's standard output (`std::cout << character; std::cout.flush();`).

4. Update Architecture:
   - Wire the `HPC3` component into the `System` and `MemoryBus` just like we did with the Memory Controller.

Please provide the modern C++ code for `HPC3.hpp` and `HPC3.cpp` along with the wiring modifications. Let's get the SGI Indy PROM to print its very first text words to our terminal!

# Phase 7

Fantastic progress! The compilation is solid, and the PROM is now hitting the FPU (Floating Point Unit) register moves blocker. This means the BIOS is entering its advanced initialization phase.

On the MIPS R4600, the FPU is designated as Coprocessor 1 (COP1). It has its own set of 32 floating-point registers (FPRs) which can be accessed as either 32-bit single-precision or 64-bit double-precision depending on the Status register configuration. The SGI PROM needs to clear, initialize, or read these registers early on.

Please implement Phase 7: MIPS COP1 (FPU) Architecture and Register Moves:

1. FPU Register State:
   - Add an array or structure to the `CPU` class to represent the 32 Floating-Point Registers (FPRs). Since the R4600 supports 64-bit float operations, model them as a union or an array of `uint64_t fpr[32]` to cleanly handle both 32-bit and 64-bit views.
   - Add the FPU Control and Status Register (`FCR31`) and the FPU Implementation/Revision Register (`FCR0`). `FCR0` should return a valid R4600 FPU ID value (e.g., `0x00000500` or similar for the built-in R4600 FPU).

2. COP1 Move Instructions:
   - Implement the following MIPS instructions in the CPU decoding/execution engine:
     * `MTC1 rt, fs`: Move To Coprocessor 1 (Copy the lower 32 bits of General Purpose Register GPR[rt] into FPU Register FPR[fs]).
     * `MFC1 rt, fs`: Move From Coprocessor 1 (Copy the lower 32 bits of FPU Register FPR[fs] into General Purpose Register GPR[rt], sign-extending if in 64-bit mode).
     * `CTC1 rt, fs`: Move Control To Coprocessor 1 (Copy GPR[rt] into FPU Control Register, usually targeting FCR31).
     * `CFC1 rt, fs`: Move Control From Coprocessor 1 (Copy FPU Control Register into GPR[rt], usually reading FCR0 or FCR31).

3. Coprocessor 1 Usability Verification:
   - In the MIPS architecture, accessing COP1 instructions requires Bit 29 (`CU1` - Coprocessor 1 Usable) of the COP0 `Status` register to be set to 1. 
   - Add a quick check in your COP1 instruction execution: if `CU1` is not enabled, print a warning or prepare a stub for a "Coprocessor Unusable" exception. (Note: The SGI PROM usually enables this bit very early, so make sure our COP0 `Status` register writes support saving this bit).

Please provide the updated `CPU.hpp` and `CPU.cpp` snippets to cleanly integrate COP1 support into our modern C++ emulator core.

# Prompt 8 

Fantastic! The CPU and FPU are fully cooperative, and the PROM is now initializing the Memory Controller's (MC) DMA engine. This means the system is ready to move larger chunks of data between the ROM/RAM spaces.

On the SGI Indy, the Memory Controller (MC) manages DMA transfers using specific memory-mapped registers. Critical registers involved in DMA operations include:
- `MC_DMA_MEMAD` (Memory Address register)
- `MC_DMA_LINE_CNT` (Line Count / Transfer Size register)
- `MC_DMA_CTRL` (Control register - writing here usually triggers or configures the DMA channel, and reading it reveals the status, such as whether the DMA is "busy" or "done").

Please implement Phase 8: Synchronous Memory Controller DMA Execution:

1. Update the `MemoryController` Class:
   - Add state variables or registers representing `MC_DMA_MEMAD`, `MC_DMA_LINE_CNT`, and `MC_DMA_CTRL`.
   - Map these to their exact physical offsets within the `0x1FA00000` memory window (refer to Indy/Guinness register documentation or NetBSD hardware headers if needed).

2. Implement a High-Level Synchronous DMA Trigger:
   - When the PROM writes to the `MC_DMA_CTRL` register with the "Start/Enable" bit set, intercept this event inside `MemoryController::write32`.
   - Instead of running an asynchronous thread for now, perform a instant **Synchronous DMA Copy** to keep things stable and simple:
     * Read the source/destination addresses from the registers.
     * Copy the requested number of bytes directly within the `MemoryBus` memory arrays.
     * Print a debug log to the host console showing the transfer details, for example: `[MC DMA] Transferred X bytes from 0xXXXXXXXX to 0xXXXXXXXX`.

3. DMA Status Management:
   - Immediately after the synchronous copy completes, clear the "Busy" bit and set the "Done" or "Idle" bit in the `MC_DMA_CTRL` register state.
   - This ensures that when the PROM subsequently polls the `MC_DMA_CTRL` register to check if the transfer is finished, it will read that the operation completed successfully and will continue booting.

Please provide the updated `MemoryController` implementation and any necessary glue code in `MemoryBus` to handle this DMA execution cleanly in modern C++.

# Prompt 9

We have encountered a critical CPU Halted error during boot! The emulator crashed with an "Instruction bus error" after 433 million cycles. 

Here is the log trace:
`[CPU WARNING] Instruction fetch from unmapped physical 0x1fc00000 (PC 0xbfc00000)`
`[CPU EXCEPTION] Instruction bus error at PC 0xbfc00000 -> vector 0xbfc00380`
`[ERROR] CPU halted after 0 instructions: instruction bus error at PC 0xbfc00380`

This means that after the DMA transfer or during a configuration change in the Memory Controller, the PROM physical memory region (0x1FC00000 - 0x1FCFFFFF) became unmapped, inaccessible, or masked in our `MemoryBus`. When the CPU generated a legitimate exception and jumped to the exception vector `0xBFC00380`, it caused a fatal double-fault because that was unmapped too.

Please implement Phase 9: Persistent PROM Mapping and Bus Error Guardrails:

1. Fix Persistent PROM Range in `MemoryBus`:
   - Review `MemoryBus::read32` (and the instruction fetch path). Ensure that the physical range `0x1FC00000` to `0x1FCFFFFF` (or `0x1FC00000` up to the loaded size of our `indy_prom.bin`) is **ALWAYS hardcoded to return data from the PROM vector**, regardless of any dynamic RAM configuration or dynamic mapping registers written by the PROM. It must never report "unmapped" for reads.

2. Verify Virtual-to-Physical Masking for KSEG1 Exception Vectors:
   - Ensure that when the CPU jumps to exception handler addresses like `0xBFC00380` or `0x80000080`, your virtual-to-physical translation cleanly masks them to physical `0x1FC00380` or the respective physical RAM address, and that the instruction fetcher can read them without a bus error.

3. Debug Logger Enhancement:
   - To help us see exactly what happened just before the crash next time, implement a small **circular history buffer** (e.g., storing the last 20 executed PCs and instructions). If a fatal exception or halt occurs, print this history trace to the console so we can see what the CPU was doing right before it hit `0xBFC00000`.

Please update `MemoryBus.cpp` and `CPU.cpp` to make the PROM region permanently readable, and let's see if the SGI Indy can bypass this crash and keep execution alive!

# Prompt 10

Brilliant! The persistent PROM fix worked flawlessly. The emulator ran for 1.49 billion cycles, and we successfully saw the SGI Indy internal diagnostic text printed to our console via the UART! 

However, the boot loop halts because the PROM fails the Real-Time Clock and NVRAM integrity tests:
`RTC path test *FAILED*`
`Can't set tod clock.`
`NVRAM checksum is incorrect: reinitializing.`

Additionally, we see the PROM trying to initialize the Newport (XL) graphics card by writing to physical addresses `0x20000000` and `0x24000000` (the REX3 engine/framebuffer).

Let's build Phase 10: The Dallas DS1386 RTC/NVRAM and Graphics Framebuffer Hook:

1. Implement the `DallasDS1386` (RTC/NVRAM) Component:
   - On the SGI Indy, the RTC/NVRAM is managed via the IOC2/HPC3 interface. The registers and NVRAM SRAM (usually 8KB) are mapped to physical space starting at `0x1FBE0000` (often matching offsets the PROM is warning about, like +0x600fb, etc.).
   - Create a basic array for the 8KB NVRAM storage. 
   - Fill the NVRAM header with a **valid SGI Indy default environment layout**. Crucially, calculate a valid SGI NVRAM checksum so the PROM stops reporting `NVRAM checksum is incorrect` and freezing.
   - For the clock (Time of Day), when the PROM reads the seconds/minutes/hours registers, return valid mock numbers or map them to the host PC's real system time (`std::chrono`) so the clock successfully ticks.

2. Framebuffer Stubbing (Physical 0x20000000 and 0x24000000):
   - Map the Newport graphics memory windows in `MemoryBus`:
     * `0x2F000000` / `0x20000000` range (Newport Rex3 control registers).
     * `0x24000000` range (Newport Framebuffer pixel memory).
   - For now, implement a quiet catch-all so that writes to `0x24000000` do not flood the console with warnings, but instead write into a `std::vector<uint8_t>` representing the 1024x768 VRAM. We will use this vector soon to draw on our SDL surface!

3. Clean Compilation Integration:
   - Wire the RTC component into the system bus so the reads at `0x1FBE0000` fetch the proper NVRAM configuration blocks.

Please provide the updated modern C++ implementation. Let's make the SGI Indy pass the RTC path test!

# Prompt 11

Phenomenal! The RTC and NVRAM integration is fully successful. The SGI Indy PROM now bypasses the clock failure and displays the official "Running power-on diagnostics..." text layout! It even prompts "[Press any key to continue.]".

However, it halts on two critical diagnostic checks:
1. `SCSI controller 0 diagnostic *FAILED*`
2. `PC keyboard/mouse controller diagnostic *FAILED* -> Cannot connect to keyboard -- check the cable.`

On the SGI Indy, the keyboard/mouse controller is a standard Intel 8042 (PS/2 style) mapped through the IOC2 peripheral space. The Western Digital WD33C93 SCSI controller is also exposed via MMIO/HPC3. The PROM reads specific status registers on these chips; returning 0 makes it think the hardware is physically broken.

Let's implement Phase 11: IOC2 Clavier/Keyboard (Intel 8042) Emulation and SCSI Stubs:

1. Intel 8042 Keyboard Controller in `IOC2`:
   - Identify the IOC2 registers that handle the keyboard. Typically, the 8042 Data register and Status/Command register are exposed at physical offsets within the IOC2 space (often matching the unimplemented reads/writes like +0x43, +0x47, +0xbf, etc.).
   - Implement a basic state machine for the 8042 Status Register. When the PROM reads the Status register, bit 1 (`IBF` - Input Buffer Full) and bit 0 (`OBF` - Output Buffer Full) need to report accurate states. Crucially, return a status value (like `0x18` or `0x00` depending on the check) that convinces the PROM a keyboard is attached.
   - Wire your `SDL_EVENT` loop from Phase 1 so that when a host keyboard event (`SDL_EVENT_KEY_DOWN`) occurs, it pushes the corresponding PS/2 scancode or character code into a small queue inside this 8042 component, turning on the Output Buffer Full (`OBF`) flag.

2. Minimal WD33C93 SCSI Controller Stub:
   - Map the Western Digital SCSI registers (typically accessed near the HPC3 area, like physical `0x1fbc0003` or similar address lines shown in your logs).
   - Implement a basic read stub: when the PROM reads the SCSI ID or Status register, do not return `0`. Return a mock byte showing the controller chip status is "Idle / Ready" (often `0x00` or a specific revision ID register value).

3. Keep the Loop Alive:
   - Ensure that when the PROM stalls at `[Press any key to continue.]`, your CPU execution loop doesn't immediately shut down. It should keep polling the memory, waiting for our new keyboard component to inject a key press from the SDL loop.

Please update the `IOC2`, `HPC3`, and main event-loop integration code to make our virtual keyboard and SCSI interface alive!

# Prompt 12

AMAZING! The SGI Indy System Maintenance Menu is now fully rendering in our text console! This is a massive milestone. The output ends perfectly at:
`System Maintenance Menu`
`1) Start System ... 5) Enter Command Monitor`
`Option? `

Right now, the emulator immediately shuts down because the execution loop doesn't handle non-blocking keyboard input from the host terminal to feed back into the HPC3 UART read buffer. We need to bridge host terminal inputs into the SGI machine so we can type options (like '5' for Command Monitor).

Please implement Phase 12: Host Console Input Routing to HPC3 UART:

1. Asynchronous Terminal Input Thread:
   - In `main.cpp` or the `System` class, spin up a lightweight, detached background thread (`std::thread`) that listens for standard input from the host console (`std::cin.get()` or `std::cin`).
   - When a character is typed by the user in the host terminal, push this ASCII character into a thread-safe thread-shared queue inside the `HPC3` UART module.

2. Implement UART Read Register Status:
   - Update the `HPC3` UART registers so the PROM knows input characters are waiting. 
   - Specifically, when the PROM reads the UART Status register (Channel A Control, typically near `0x1FBD9804` or `0x1FBD980C`), set the "Receive Character Available" bit (usually bit 0 or bit 1 depending on the Z8530 register index state) to `1` IF our thread-safe input queue is not empty. If the queue is empty, return `0`.

3. Implement UART Data Read:
   - When the PROM reads the UART Data register to fetch the typed key, pop the front character from your input queue, return it as a 32-bit word (properly byte-swapped for Big-Endian if your architecture treats MMIO as 32-bit blocks), and clear the "Receive Character Available" status bit if the queue becomes empty.

4. Prevent Emulator Halt on Idle:
   - Ensure our main execution loop does not bail out or treat empty cycles as a reason to shut down. The CPU should continue executing instructions (spinning in the PROM's idle/polling loop) while waiting for the user to type a selection.

Please provide the updated modern C++ implementation to turn our static text output into a fully interactive SGI console!

# Prompt 13

The Command Monitor and interactive terminal are working flawlessly! Let's pursue the first direction proposed: Implemeting the Western Digital WD33C93 SCSI Bus Controller and linking it to our SDL Drag-and-Drop ISO framework.

Our goal is to make the SGI PROM recognize our virtual CD-ROM drive (at SCSI ID 4) so that typing `hinv` lists the drive, and selecting "2) Install System Software" allows the PROM to fetch the boot files (like `sash` or `unix`) from the ISO image.

Please implement Phase 13: WD33C93 SCSI Controller Architecture and ISO Data Streaming:

1. Expand the `WD33C93` SCSI Component:
   - Map the two primary registers of the WD33C93 chip exposed via the HPC3 bus mapping (typically an Address/Register Pointer register and a Data register).
   - Implement the internal register array of the WD33C93 (such as `SCSI_COMMAND`, `SCSI_STATUS`, `SCSI_SOURCE_ID`, `SCSI_DEST_ID`, `SCSI_OWN_ID`).

2. Implement a High-Level SCSI Command State Machine:
   - When the PROM writes a command byte to the `SCSI_COMMAND` register, intercept the core commands used during SGI boot probing:
     * `RESET` (0x00): Reset the SCSI interface and set status to idle.
     * `INQUIRY` (0x12): Return a valid SCSI inquiry block identifying SCSI ID 4 as a "TOSHIBA CD-ROM" or standard SGI compatible optical drive.
     * `READ_CAPACITY` (0x25): Return the structural size of our virtual disk based on the loaded ISO size.
     * `READ_10` (0x28): The critical data retrieval command. When the PROM requests specific blocks, fetch the bytes directly from the raw ISO file path provided by our SDL drag-and-drop mechanism.

3. Connect to the HPC3 DMA Engine:
   - SGI Indy uses the HPC3 chip to perform DMA transfers for SCSI data. When a `READ_10` command is executed, instead of single-byte transfers, coordinate with our Phase 8 DMA subsystem to copy the requested disk blocks (2048 bytes per CD-ROM sector) directly into the mapped system RAM address.
   - Once data is transferred, update the `SCSI_STATUS` register to report "Command Complete" (usually status byte `0x10` or `0x85` depending on interrupt requirements) and trigger the interrupt bit if applicable.

4. Seamless ISO Swap Integration:
   - Wire the `SDL_DROPFILE` path so that if a user drops a new `.iso` file into the window, it instantly updates the file descriptor pointer inside this SCSI component, acting as a physical media change.

Please provide the modern C++ structural code for the expanded SCSI subsystem, and show how the PROM can now successfully complete its SCSI bus probe without failing.

# Prompt 14

Excellent! The SCSI implementation is holding up perfectly under the PROM's polling mechanism. Now, let's prepare the architecture for booting IRIX by implementing Phase 14: The INT3 Interrupt Controller, Intel 8254 Timer, and CPU Interrupt Routing.

While the PROM is happy polling registers, the IRIX operating system kernel requires active hardware interrupts to drive its scheduler and handle asynchronous SCSI disk transfers. On the SGI Indy, peripheral interrupts are aggregated by the INT3 chip and routed directly to the MIPS CPU hardware interrupt lines (specifically IP2/IP3 in the COP0 Cause register).

Please implement:

1. The `INT3` Interrupt Controller Class:
   - Map the INT3 memory-mapped registers, which sit physically in the IOC2/GIO space (typically starting around `0x1FBD0000`, specifically mapping registers like `INT3_L0_STATUS`, `INT3_L0_MASK`, `INT3_L1_STATUS`, `INT3_L1_MASK`).
   - Implement `raise_interrupt(uint8_t line)` and `clear_interrupt(uint8_t line)` methods.
   - When a bit is set in a `STATUS` register AND its corresponding bit is set in the `MASK` register, trigger a hardware interrupt on the `CPU` class by setting the appropriate Interrupt Pending (IP) bit in the COP0 `Cause` register.

2. Intel 8254 Programmable Interval Timer (PIT) Simulation:
   - SGI Indy uses an Intel 8254 compatible timer connected to INT3 to generate the master system heartbeat clock (usually running at 100Hz or 1000Hz for IRIX).
   - Implement a lightweight ticker inside our central `System` loop: every X emulation cycles (matching the real hardware time elapsed), trigger the 8254 timer, which in turn calls `INT3::raise_interrupt()` for the timer line.

3. CPU Interrupt Checking Loop:
   - Update the `CPU::tick()` core method. At the start of every cycle, check if any unmasked interrupt bits are active in both the COP0 `Status` and COP0 `Cause` registers.
   - If an interrupt condition is met and interrupts are globally enabled (bit 0 `IE` in COP0 `Status` is 1), suspend normal instruction execution, save the current PC to COP0 `EPC`, update the exception vector, and jump directly to the MIPS hardware exception vector (`0x80000080` or `0xA0000080` / `0xbfc00380` depending on context).

4. Wire the WD33C93 SCSI Interrupt Line:
   - Update the SCSI controller from Phase 13 so that when a command (like `RESET` or `INQUIRY`) completes, instead of just setting a status byte, it explicitly calls `INT3::raise_interrupt()` on the SCSI line slot.

Please provide the modern C++ design for the `INT3` controller and the necessary updates to our execution loop to handle asynchronous timing and device interrupts smoothly!

# Prompt 15

Brilliant job on Phase 14! The interrupt subsystem architecture is compiling cleanly. Now let's address the remaining critical hardware routing blocks that the IRIX kernel will strictly require to boot, focusing on Phase 15: Complete Interrupt Routing and Advanced SCSI Bus Phases.

We need to interconnect our existing components so that hardware events explicitly trigger the INT3 lines we created, and the SCSI state machine can handle multi-phase OS commands.

Please implement the following updates:

1. Wire MC DMA-Done and Error Interrupts to INT3:
   - Update the `MemoryController` (MC) class: when a synchronous or asynchronous DMA transfer finishes (Phase 8), ensure it sets the DMA-Done bit in the MC's internal Interrupt Cause register.
   - Route this event directly to `INT3`: call `INT3::raise_interrupt()` for the specific local DMA interrupt line.
   - Implement the Bus-Error register handling: if the CPU attempts an invalid physical access or an unmapped write, latch the offending address in the MC error registers and trigger MIPS Hardware Interrupt Line 4 (IP6) on the CPU.

2. Implement Advanced SCSI Phases (Select & Transfer Info) in `WD33C93`:
   - Expand the `WD33C93` SCSI controller state machine to move beyond simple high-level commands and support phase-based OS driving:
     * `Select Without ATN` (0x06) / `Select With ATN` (0x07): Intercept when the guest driver selects our target CD-ROM (SCSI ID 4). Set the SCSI bus phase register to "Command Phase" and update the status register to signal target selected.
     * `Transfer Info` (0x08): This is the workhorse command IRIX uses to stream bytes. When the guest driver executes this command, check the current bus phase (Command, Status, Message In, or Data In) and feed or consume the appropriate bytes (e.g., streaming the actual ISO blocks into system memory during a Data In phase).
     * Handle basic Disconnect/Reselect status flags so the driver knows it can release the bus during heavy operations.

3. Wire Interrupt-Driven Serial (HPC3 UART):
   - Update the `HPC3` UART module: when a new character is pushed into the thread-safe console input queue from our background host terminal thread, don't just update the status register bit. Explicitly call `INT3::raise_interrupt()` on the serial/UART interrupt line to alert the CPU immediately.

Please provide the structural modern C++ updates for `MemoryController`, `WD33C93`, and `HPC3` to finalize our interrupt grid and prepare us for launching a kernel bootloader.

# Prompt 16

Amazing work! The core system, advanced SCSI phases, and interrupt routing are fully stable. Now, let's tackle the final major milestone of our initial architecture: The Newport (SGI XL) Graphics Subsystem and REX3 Engine.

During boot, the SGI PROM successfully probed the graphics chip at its exact physical register base address: `0x1F0F0000`. Our objective now is to map these REX3 engine control registers and translate writes into actual visual pixel output inside our SDL window/texture, resolving the "Cannot open video() for output" warning.

Please implement Phase 16: Newport (REX3) Graphics Core and SDL Blitting:

1. Create the `NewportGraphics` Class:
   - Map the critical REX3 control registers starting at physical address `0x1F0F0000` (and its alias ranges like `0x1F000000` / `0x1F400000` / `0x1F0F1330` seen in our early logs).
   - Define and track essential REX3 registers:
     * `REX3_DRAWMODE` (Controls drawing operations: logic ops, color depth 8-bit vs 24-bit).
     * `REX3_XSTART`, `REX3_YSTART`, `REX3_XEND`, `REX3_YEND` (Coordinate boundaries for drawing/clearing lines and rectangles).
     * `REX3_COMMAND` (Writing a specific value here triggers the actual drawing operation, such as Block Copy, Screen Clear, or Line Draw).
     * `REX3_COLORREDI`, `REX3_COLORGREEN`, `REX3_COLORBLUE` (Color source registers).

2. Implement a Basic REX3 Rasterizer Stub:
   - Inside `NewportGraphics::write32`, when the PROM writes to `REX3_COMMAND` with an operation like "Draw/Fill Rectangle", intercept it.
   - Extract the start/end coordinates and color, and perform a fast pixel loop to write these color bytes into our internal VRAM byte vector (`std::vector<uint8_t> m_vram` matching the SGI 1024x768 resolution).

3. Connect VRAM to the SDL3/SDL2 Rendering Loop:
   - Provide a safe thread-safe method (e.g., `get_framebuffer_data()`) to expose this VRAM vector to our main application loop.
   - In `main.cpp`, inside the SDL rendering tick, copy/upload this VRAM byte array directly into an `SDL_Texture` using `SDL_UpdateTexture`, and render it via `SDL_RenderPresent`. If the SGI is in 8-bit color index mode, map the pixels using a basic 256-color palette.

4. Route MMIO Accesses:
   - Ensure `MemoryBus::read32` and `MemoryBus::write32` accurately forward all traffic in the `0x1F000000` - `0x1F4FFFFF` ranges directly to this new `NewportGraphics` module.

Please provide the modern C++ design for the `NewportGraphics` component and show how to seamlessly update our SDL display thread to draw the SGI Indy's visual output in real-time.

# Prompt 17

Brilliant! The SGI Indy PROM graphical interface is now successfully rendering inside our SDL window! This is an incredible milestone for our clean-room C++ emulator.

To make the graphical console fully usable before we attempt to boot the IRIX installer, we need to implement the mouse integration. On the SGI Indy, mouse data is processed by the same Intel 8042 controller inside the IOC2 space that handles the keyboard. The mouse operates on a standard PS/2 3-byte packet stream transmitting button states and relative X/Y movement deltas.

Please implement Phase 17: SDL Mouse Input Mapping to IOC2 Intel 8042:

1. Expand the Intel 8042 Mouse State Machine in `IOC2`:
   - Add a secondary queue or mechanism inside your 8042 component to store incoming mouse packets (3 bytes per movement/click event).
   - Track if the PROM has enabled the "Mouse Auxiliary Device" via standard 8042 command bytes (like `0xA8` to enable auxiliary device, or `0xE8`/`0xF3` for resolution/sampling setups).
   - When mouse data is present in the queue, latch the Auxiliary Output Buffer Full flag (usually bit 5 `AUX_OBF` and bit 0 `OBF` in the 8042 Status Register) so the SGI software knows the data comes from the mouse and not the keyboard.

2. Map SDL Mouse Events in `main.cpp`:
   - Update your main SDL event loop to capture mouse activity within our window:
     * `SDL_EVENT_MOUSE_MOTION`: Extract relative movements `event.motion.xrel` and `event.motion.yrel`. (Note: SGI/IRIX expects standard Y-axis orientation, so you might need to invert or scale the Y delta).
     * `SDL_EVENT_MOUSE_BUTTON_DOWN` / `SDL_EVENT_MOUSE_BUTTON_UP`: Capture Left, Middle, and Right clicks to map them to the first byte of the PS/2 packet (Bit 0 = Left, Bit 1 = Right, Bit 2 = Middle).
   - Package these deltas and button states into a standard 3-byte PS/2 packet structure and push them into the 8042 mouse queue.

3. SGI Hardware Cursor Alignment:
   - Since the hardware cursor register in Newport isn't fully drawn yet, please implement a simple visual fallback: either let the host system cursor remain visible over the window, or inject a tiny crosshair blit over our SDL VRAM texture using the X/Y coordinates to easily see where the user is pointing.

Please provide the updated modern C++ implementation for the IOC2 input processing and the updated SDL main loop. Once done, we will be fully equipped to insert our virtual IRIX CD image and select "Install System Software"!
