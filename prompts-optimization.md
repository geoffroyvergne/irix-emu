
# Phase 1 : Resolve Houdini freezing and window crashes (Maximum Priority)


# Prompt 1 :Eliminating X11 Deadlocks in libglpass_x.so (Houdini/Maya-specific)

Context: In our SGI Indy emulator, Houdini's License Administrator (hkey) freezes immediately upon launch, and Maya's dropdown/marking menus trigger temporary input locks or black screens. hkey renders its interface via GL but uses heavily customized X11 hooks. Our libglpass_x.so library intercepts XStoreColors and overrides XSetErrorHandler to filter errors. If libglpass_x makes synchronous X11 calls (like XSync or round-trip queries) inside intercepted X functions while the application holds a pointer grab or server lock, a deadlock occurs.

Task: Please audit guest/xstore.c. Rewrite the interceptors to be completely asynchronous. Use a thread-safe, lock-free approach or localized atomic flags to record colormap cells and map pixels without ever blocking the main X11 event loop or triggering synchronous round-trips. Ensure that if Houdini or Maya destroys/unmaps a window or context unexpectedly, libglpass_x drops the reference silently without causing an unhandled X11 error or blocking.

# Prompt 2 : Improving the reliability of tracking for destroyed windows (Anti-Freeze)

Context: The documentation states that Houdini's License Administrator leaves an OpenGL context current on its destroyed splash window and keeps flushing. This creates a storm of X11 errors or a permanent freeze if the guest attempts to present (SWAP) a dead window. 

Task: Let's reinforce the window tracking mechanism in guest/glpass.c. When a window is destroyed or unmapped (detected via DestroyNotify, UnmapNotify, or a BadDrawable error caught by our custom handler), we must immediately mark that drawable as DEAD in our shared table. If the guest later tries to call glFlush(), glFinish(), or glXMakeCurrent() on a DEAD drawable, glpass must bypass the hypercall entirely, return False or GL_FALSE instantly to the guest application, and simulate a successful dummy operation to prevent the application from hanging or flooding the emulator thread.


# Phase 2 : Stabilizing Threads and Display in Maya 6.5

# Prompt 3 : Strict PID management for sproc threads (Maya-specific)

Context: Maya uses IRIX sproc threads (processes sharing an address space) instead of standard POSIX threads. glpass identifies each thread's state using its PID read directly from the PRDA (PRDA->t_sys.t_pid). 

Task: Please review the thread context resolution logic in guest/glpass.c. Ensure that our shared library lock perfectly isolates context switches between distinct sproc PIDs. If Process A changes a GLX context and Process B immediately executes a SUBMIT or SWAP call on the same address space, the host bridge must guarantee that commands are strictly routed to the correct host context. Optimize the lookup so that reading the PRDA PID is completely atomic and does not suffer from race conditions when Maya spawns multiple UI worker threads.

# Prompt 4 : Fix for the "Colormap Switch" (black screen in Maya menus)

Context: The documentation notes that Maya's marking menu briefly shows the rest of Maya's interface black with red outlines while it is up, due to a colormap switch on the X11 side which is restored on release. 

Task: Look at the palette and colormap tracking inside guest/glpass.c and how the host updates it in host/GlBridgeMac.cpp. When Maya forces a temporary colormap switch for its UI overlays, glpass must cache the main window's palette. Instead of triggering a destructive redraw or clearing the host FBO to black, the host should dual-composite the cached main image and the new overlay menu image. Provide a fix that maintains visual continuity of the underlying 3D viewport when marking menus are active.


# Phase 3 : Secure Memory and Fix Visual Bugs


# Prompt 5 : Robustness of paged memory copying (Hypercall & Bulk Transfers)

Context: In our glpass architecture, guest buffers larger than the R5000 TLB maps (around 384 KB) are copied page by page. If a page fault occurs, the copy is interrupted and must resume exactly where it stopped when IRIX re-executes the hypercall. 

Task: Review the SUBMIT and QUERY handlers in guest/glpass.c and host/GlBridgeMac.cpp. Let's audit how the continuation state (offset, remaining length, and per-context structures) is preserved. Ensure that when a partial copy is interrupted, no internal command buffer indices are corrupted, and that interleaved calls from another thread/process (sproc or pthread) cannot overwrite or mix with this pending state. Provide a robust, clean implementation of this page-fault resume mechanism.

# Prompt 6 : Robustness of write masks (Multi-pass Index Write Masks)

Context: glpass handles 12-bit color-index visuals (for Maya) by translating indices into colors via the window's palette. Complex index masks (like 0x83e or 0xfc0 in Maya) require up to two host passes (GL_AND, GL_OR, GL_XOR). 

Task: Audit the implementation of the per-bit rule for index masks inside guest/glpass.c and its replay in host/GlBridgeMac.cpp. Ensure that vertex arrays (glDrawArrays, glDrawElements) and geometry drawn inside a glBegin/glEnd block are properly cached and re-emitted for each required pass without memory leaks or state corruption. Provide the missing logic to handle edge cases where the index changes mid-block or during a display list execution.

# Prompt 7 : Pixel format alignment and Quake III regression

Context: Quake III Arena requests 4/4/4-bit or 5/6/5-bit color formats and renders at that depth, which currently causes the image to appear darker or visually glitched following changes made for Maya's visual compliance. 

Task: Let's analyze how the Host GPU (Legacy OpenGL 2.1 via CGL) initializes the Offscreen Framebuffer Objects (FBO) and handles color-packing/gamma ramps. Review the texture generation and framebuffer blending configurations in host/GlBridgeMac.cpp. Ensure that when a guest application requests lower bit-depth visuals, the host accurately maps the color channels without crushing dark values, maintaining strict isolation between standard RGB/RGBA visual logic and color-index visual logic.

# Phase 4 : Improvement of Tools and Automation

# Prompt 8 : Improvement of the Code Generator (glpass-gen.py)

Context: We have a generator script tools/glpass-gen.py that parses guest/gl_api.txt and outputs guest/glpass_gen.c, guest/glpass_fn.h, and host/GlDispatchGen.inc. 

Task: Update tools/glpass-gen.py to systematically validate argument sizes for array types. If a generated function receives a pointer argument (e.g., glVertex3fv), the generator must automatically generate code that determines the proper number of bytes to copy based on the command context, preventing out-of-bounds reads on the guest memory bus. Any unsupported extension or function must be cleanly stubbed to log exactly once using GLPASS_TRACE without halting the emulation pipeline.
