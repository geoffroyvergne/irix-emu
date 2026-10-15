# Prompt 0 Context initialization

Context: I am developing a custom SGI Indy emulator using C++ and SDL, running IRIX 5.6.22 / 6.5. We are improving an OpenGL pass-through system called "glpass". 
The IRIX guest (MIPS R5000, Big-Endian, n32 ABI, compiled with freeware gcc) encodes OpenGL/GLX calls and triggers a custom MIPS instruction hypercall (0x00000668). The Host (macOS/Modern PC, Little-Endian) captures this instruction via the emulator core and replays the commands using a host-side OpenGL context.

I have attached the architecture documentation. We are currently experiencing visual glitches, frame drops, and application freezes when running complex 3D software like Maya, Quake 3, or SGI Demos. 

Please acknowledge that you understand:
1. The guest-to-host hypercall protocol and batching system.
2. The Endianness gap: The guest is Big-Endian (MIPS) and the host is Little-Endian (x86_64/ARM). Any multi-byte data (ints, floats, doubles, pixel streams) sent over the hypercall or read from guest memory MUST be byte-swapped on the host side.
3. The core limitations identified in the document (thread safety, X11 lag, and color index/overlay issues).

Do not write code yet. Just confirm your architectural understanding and tell me which files you need to see first (e.g., guest/glpass.c, host/GlBridgeMac.cpp, guest/glpass_protocol.h) to begin fixing the thread-safety issue.


# Prompt 1 Thread-Safe Contexts

Context: In our glpass implementation, the current GLX context is stored globally per process inside IRIX. Because professional 3D applications like Maya are multi-threaded, this global context causes severe race conditions and freezes. 

Task: Rewrite the context tracking mechanism in 'guest/glpass.c' and 'host/GlBridgeMac.cpp' to support true thread-safe execution.

Technical Constraints & Crucial Guardrails:
1. IRIX Guest Environment: We are using an older IRIX n32 userland with freeware gcc. Do NOT use modern C11 '_Thread_local' or compiler attributes that might not be supported. Instead, strictly implement POSIX threads storage using `pthread_key_t`, `pthread_key_create`, `pthread_getspecific`, and `pthread_setspecific` to track the active `GLXContext` per-thread.
2. Host Handling: Ensure the Host `GlBridge` maps incoming command streams (`SUBMIT`, `MAKE_CURRENT`) to the correct host-side OpenGL context based on a unique thread or context ID sent via the hypercall arguments (`a0`-`a3` registers).
3. Endianness Check: When passing context handles or IDs between guest and host, ensure they are properly byte-swapped if they are read directly from guest memory pointers.

Please provide the exact modified code sections for 'guest/glpass.c' and 'guest/glpass_internal.h', as well as the host-side structural changes in 'host/GlBridgeMac.cpp'.
