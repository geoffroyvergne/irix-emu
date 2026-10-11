# Project Profile: SGI Indy Workstation Emulator (Guinness Architecture)

## 🎯 Project Goals & Vision
- **Objective:** Build a performant, modular, clean-room SGI Indy emulator in modern C++ to boot IRIX 6.5.22 and eventually run 3D applications (Maya, Softimage).
- **Target OS:** IRIX 6.5.22 (requires robust SCSI, DMA, and MIPS III/IV stability).
- **Graphics Strategy:** Hybrid approach. Start with a 2D Newport (XL) framebuffer mapped via SDL, but architecture must isolate the graphics subsystem to allow High-Level Emulation (HLE) or translation of SGI graphics pipelines to host OpenGL/Vulkan later.
- **UI & UX:** Native desktop experience using SDL for windowing, keyboard/mouse inputs, and hot-swapping virtual ISOs via Drag & Drop (`SDL_DROPFILE`).

## 🛠️ Tech Stack & Constraints
- **Language:** Modern C++ (C++20 / C++23 standards). Strict object-oriented design, smart pointers, explicit memory ownership, and low-latency structures.
- **Build System:** CMake (minimum 3.20+). Clean source/include separation.
- **Dependencies:** SDL3 (preferred for modern drop/display APIs) or SDL2.
- **Performance Constraint:** The emulation loop must be optimized for multi-threading where applicable (e.g., separating CPU/Bus orchestration from the SDL rendering thread to prevent UI blocking).

## 🧩 Architectural Blueprint
- `System`: Central orchestrator managing the global clock, initialization, and component lifecycle.
- `CPU (MIPS R4600)`: 64-bit MIPS III architecture interpreter. Must implement accurate TLB, COP0 (System Control Coprocessor) for exception/interrupt handling, and COP1 (FPU).
- `MemoryBus`: Central physical memory mapping. Bridges CPU requests to RAM, PROM, and Memory-Mapped I/O (MMIO).
- `HPC3 (GIO-to-SCSI/Ethernet)`: The SGI peripheral controller handling DMA, SCSI (WD33C93), and audio/serial chips.
- `GraphicsSubsystem`: Abstract interface handling the Newport (XL) REX3 chip family. Framebuffer rendering outputs directly to an uncompressed pixel vector for SDL streaming.

## 🤖 Claude Instructions & Coding Guidelines
1. **Never Hallucinate Registers:** If a specific SGI hardware register address (e.g., HPC3, INT2, or Newport REX3 registers) is required but unknown, explicitly ask the user for documentation or reference to NetBSD/Linux/MAME open-source source trees.
2. **Modular Code Generation:** Provide code in distinct, compilation-ready blocks. Avoid writing placeholder functions like `// TODO: implement later` inside critical paths unless requested.
3. **Safety First:** Use explicit types (`uint32_t`, `uint64_t`) for all hardware registers and memory addresses to avoid cross-platform alignment or sign-extension bugs.
4. **Context Preservation:** At the end of every response, summarize the current hardware state we successfully emulated (e.g., "CPU: basic interpreter done, Memory: RAM/PROM mapped, Graphics: Init").
