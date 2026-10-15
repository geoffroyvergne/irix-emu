# glpass: host-GPU OpenGL for IRIX programs in the Indy emulator

glpass gives IRIX programs running in the emulator hardware-accelerated
OpenGL. A replacement `libGL.so` inside IRIX sends their OpenGL calls out of
the virtual machine, the emulator draws them with the host's GPU, and the
result appears in the program's window on the emulated screen.

It lives in this repository next to the emulator, but it is a separate piece
with its own build, protocol and install steps. The emulator works without it:
IRIX then keeps its own OpenGL.

## Contents

- [Features](#features)
- [Why it exists](#why-it-exists)
- [How it works](#how-it-works)
- [Repository layout](#repository-layout)
- [Building](#building)
- [Packaging and installing in IRIX](#packaging-and-installing-in-irix)
- [Running programs](#running-programs)
- [Diagnostics](#diagnostics)
- [Status and limitations](#status-and-limitations)
- [Changing the code](#changing-the-code)

## Features

- **Host-GPU rendering** of OpenGL 1.1 programs plus common extensions,
  shown in the program's own X window. The window is composited with a key
  colour, so other windows, menus and the cursor stay on top.
- **Threads:** every GLX context is current per thread, for POSIX threads
  and sproc programs such as Maya alike. Each GL call names its context, and
  the host runs it in that context.
- **Window tracking:** the image follows the window as soon as it moves or
  is resized, and disappears when the window is unmapped or destroyed. Both
  are driven by X events, not by polling.
- **Colour index emulation** for 12-bit colour-index programs (Maya's
  views):
  - palettes come from the program's X colormaps;
  - index logic ops;
  - exact index write masks, with several passes when one isn't enough.
- **Overlay planes** (optional, `GLPASS_OVERLAY=1`): the X server's 2-bit
  overlay visual, offered only to requests it satisfies.
- **Selection and feedback:** `glRenderMode(GL_SELECT / GL_FEEDBACK)`
  results are written back into the program's buffers in IRIX byte order.
- **3D textures** (`glTexImage3D`, `GL_EXT_texture3D`), including the 3D
  pixel-store settings.
- **Large transfers:** images, readbacks and command batches bigger than the
  emulated TLB maps are copied page by page.
- **Version check:** a library built from other sources than the emulator is
  refused, with a message saying what to do.
- **X errors:** glpass's own X errors never reach the program. Windows the
  program destroys are dropped at once.
- **Test programs:** `gltri`, `glthreads`/`glthreads_pt`, `glmove`,
  `glcheck` and `glgone`.

## Why it exists

The emulator emulates the Indy's standard graphics board, Newport (XL8/XL24).
Newport draws 2D spans; it has no geometry engine and no depth buffer. On a
real Indy XL, IRIX's OpenGL renders everything in software on the CPU, which
in an emulator is far too slow for 3D applications, and some (Maya) refuse to
start without a depth-buffered visual.

The Indy board with 3D hardware, XZ, is not emulated. Its chips (HQ2, GE7
geometry engines running SGI microcode, RE3) have no public documentation,
so emulating it would mean reverse-engineering undocumented hardware, and the
result would run at mid-1990s speed.

glpass takes the other route: high-level emulation at the OpenGL API. A
program's OpenGL calls never reach emulated hardware. They are encoded inside
IRIX, handed to the emulator, and replayed on the host GPU.

## How it works

```
 IRIX (emulated R5000)                              Host (macOS)
 ─────────────────────                              ────────────
 program ──► libGL.so (glpass/guest) ── hypercall ──► GlBridge (glpass/host)
               │ encodes GL calls into a                │ replays them in a CGL
               │ command stream                         │ context, into an
               │ paints its window with a key colour    │ offscreen framebuffer
               ▼                                        ▼
            X server ──► Newport framebuffer ◄── composite: host image where
                         (emulated display)       the window shows the key
```

### The hypercall

The guest reaches the emulator with one reserved instruction word,
`0x00000668` (a SPECIAL encoding that real MIPS hardware rejects). The
emulator's R5000 hands it to the GL bridge instead of raising an exception.
Arguments travel in `a0`–`a3` (service, operation word, guest buffer, length)
and results come back in `v0`/`v1`. The operation word holds the operation in
its low 8 bits and the id of the GLX context it is for above them
(`GLP_OP_WORD`; see [Threads](#threads)). The full protocol is in
[guest/glpass_protocol.h](guest/glpass_protocol.h):

| Operation | Purpose |
|---|---|
| `HELLO` | Version check: the protocol version (`GLP_VERSION`) and the ID of the generated command table (`GLP_FN_TABLE_ID`); a library built from other sources is refused |
| `CREATE_CONTEXT`, `DESTROY_CONTEXT` | One host context per GLX context, sharing lists as GLX asks |
| `MAKE_CURRENT` | Binds a context to a drawable (window, pixmap or pbuffer) of a given size, or unbinds it (drawable 0); other contexts keep their bindings |
| `SUBMIT` | A batch of encoded GL commands |
| `QUERY` | A call that returns data (`glGet*`, `glReadPixels`, `glRenderMode`, ...); the host writes the reply into guest memory |
| `SWAP` | Shows a drawable's image (after `glXSwapBuffers`, or a flush of front-buffer drawing), shows it again at a window's new place, or takes it off the display (`GLP_SWAP_HIDE`) |
| `FINISH` | Waits for the host GPU |

The host reads and writes guest memory through the guest's own address
translation. Buffers larger than the R5000 TLB maps at once (about 384 KB)
are copied page by page; a page fault stops the copy and the hypercall
resumes where it stopped when IRIX re-executes it. Interrupted copies are kept
per context, so another thread calling in meanwhile does not restart them.

### Threads

GLX makes a context current per thread, and programs draw from several
threads at once. IRIX programs have threads of two kinds:

- **POSIX threads** share one process. Each thread's state sits under a
  pthread key (`pthread_key_create`, `pthread_getspecific`,
  `pthread_setspecific`).
- **sproc** threads (Maya) are processes sharing one address space, and
  libpthread cannot be loaded into them. Each one's state is found by its own
  pid, read from its PRDA (`PRDA->t_sys.t_pid`) without a system call.

The library does not link libpthread. It looks the key functions up at run
time and uses them when the program has them.

Each thread has its own current context, command buffer and pending
`glBegin` batch. Every GL hypercall names its context in the operation word.
The host keeps, per context, the drawable it is bound to. It makes a context's
host GL context, framebuffer and draw/read buffers current only when the
calling context differs from the one that ran last, so interleaved threads
each draw into their own context.

Tables that all threads share are guarded by one library lock, which the
thread holding it may take again. They hold drawables, window positions and
modes, palettes sent, visuals handed out, and the library's X connection.

### The command stream

Each GL call becomes a record of big-endian 32-bit words: a header with the
command number and length, then the arguments (ints as-is, floats as IEEE
bits, doubles as two words). Calls are batched and sent when a batch fills
up, or when a result, a swap or a flush needs them.

Most of the 886 entry points in IRIX's `gl.h` are generated by
[tools/glpass-gen.py](tools/glpass-gen.py) from the prototype list
[guest/gl_api.txt](guest/gl_api.txt):

| Kind | Count | How |
|---|---|---|
| Generated | 451 | Plain-value or fixed-size-array arguments. The guest encoder and the host replay are both generated. Vendor forms (`glBindTextureEXT`) replay as the core host command. |
| Hand-written (`MANUAL`) | 108 | Calls that return data, carry images (with the pixel-store state, 3D images included), use client vertex arrays, selection or feedback, evaluators, or need the colour-index emulation |
| Watched by the guest (`GUEST_MANUAL`) | 8 | Hand-written in the guest, which tracks state with them (index logic op, clears, attribute stack, display lists); replayed by generated host code |
| Stubs | 319 | Not available on the host (imaging subset, vertex programs, ...): logged on first use, otherwise ignored |

**Selection and feedback.** `glSelectBuffer` and `glFeedbackBuffer` give the
host a buffer of the same size; the guest keeps the program's pointer.
Leaving `GL_SELECT` or `GL_FEEDBACK`, `glRenderMode` is a query: the host
writes its buffer back into the program's, one big-endian word per value, so
names, depths and float coordinates arrive in IRIX byte order.

**3D textures.** `glTexImage3D` and `glTexSubImage3D`, and their `EXT` forms,
replay as the host's core 3D texture commands. The guest sizes the image with
the 3D pixel-store state (`GL_UNPACK_IMAGE_HEIGHT`, `GL_UNPACK_SKIP_IMAGES`),
and `GL_EXT_texture3D` is advertised.

`glcheck` checks all three value by value.

The GLX layer (visuals, FBConfigs, contexts, windows, pixmaps, pbuffers,
`glXUseXFont`, SGI extensions) is hand-written in
[guest/glpass.c](guest/glpass.c).

Client vertex arrays are copied out of guest memory when a draw call uses
them (`glDrawArrays`, `glDrawElements`), only the element range the draw
needs. A run of `glArrayElement` calls between `glBegin` and `glEnd` (how
Quake III draws) is collected and sent as one indexed draw at `glEnd`; if
any other command comes in between, the vertices are sent one by one as
written. Replaying them one by one on the host costs about ten times more.

### Showing the image: key-colour compositing

Every GLX drawable renders on the host into a framebuffer object with a back
buffer, a front buffer and a 24-bit depth plus 8-bit stencil buffer. All
contexts of a share group draw into the same buffers for a given window, as
GLX requires.

A window's image is shown (presented) when it is meant to be seen, as on
SGI hardware:

- at `glXSwapBuffers`, after the back buffer is copied to the front;
- at `glFlush` or `glFinish` while drawing into the front buffer;
- at `glFlush` or `glFinish` in a window that has never been swapped, taken
  for single-buffered (every visual here is double-buffered underneath);
- at `glFlush` or `glFinish` after drawing on top of the last swapped frame
  without clearing it. Maya draws its selection rectangle so: into the back
  buffer of its double-buffered view, erasing the previous rectangle, with
  only a `glFlush` after each mouse move. A frame that starts with a colour
  clear is still shown at its swap only.
- at `glXMakeCurrent` leaving such a window, as GLX flushes the context it
  leaves (Maya's menus end so, without a `glFlush`).

To present, the guest library fills the X window with a key colour (pixel
`0xC7`) and asks the emulator to show the image. The emulator reads the front
buffer back and composites it over the emulated display, but only where the
framebuffer still holds the key. So:

- windows, menus and the cursor on top stay visible;
- a closed or unmapped window stops showing: its image is taken off the
  display as soon as the library reads the window's unmap or destroy event
  (see [Window tracking](#window-tracking));
- when two GL windows overlap, the most recently presented one wins.

A small automatic offset (±4 pixels) aligns the image with the window's key
pixels, absorbing the X server's window-origin offset.

Overlay windows (only with `GLPASS_OVERLAY=1`, see
[status](#status-and-limitations)) live in the popup planes, not the main
planes. They are filled with popup value 3 instead, and their image is a
second layer: shown above the main images where the popup planes hold that
key, with colour index 0 transparent.

### Colour index emulation

The host renders only in RGBA, but IRIX programs also use colour-index
visuals. Maya, for example, draws its wireframe views in 12-bit colour index.
glpass emulates them:

- A colour-index drawable keeps the index itself in its host buffer, spread
  over the channels (R = bits 0–7, G = bits 8–9, B = bit 10, A = bit 11).
  Pixel copies and index logic operations (`GL_INDEX_LOGIC_OP`, as XOR) then
  act on indices.
- **Write masks** are exact for every mask (see below).
- The host turns indices into colours with the window's palette when it shows
  the image.

#### Index write masks

A colour mask can only keep or write whole channels. So `glIndexMask` goes
through the colour mask only when the mask is a union of the bit groups
above. Other masks are done with the host logic op, using a rule that holds
for every logic op and every mask.

**The per-bit rule.** A masked index write is, per bit,
`dst' = mask ? f(src, dst) : dst`, where `f` is the index logic op (copy when
the logic op is off). For each bit this is one of four functions of `dst`:

- 0: the bit is cleared;
- 1: the bit is set;
- `dst`: the bit is kept;
- `~dst`: the bit is inverted.

**Passes.** The bits to clear, to set and to invert never overlap, so one
host pass per kind gives the exact result in any order:

| Bits | Host pass |
|---|---|
| to clear | `GL_AND` with all the other bits |
| to set | `GL_OR` with those bits |
| to invert | `GL_XOR` with those bits |

**How many passes.**

- One pass covers a logic op such as XOR under any mask.
- One pass covers a copy of an index whose masked bits are all 0 or all 1.
- Maya's copies under `0x83e` and `0xfc0` set some bits and clear others,
  so they take two passes. A single host logic op never needs three.

**What is drawn again for each pass:** `glBegin`/`glEnd` blocks, vertex
array draws, and `glClear`. A clear becomes window-sized rectangles, because
`glClear` ignores the logic op.

**Index changes inside a block.** The block's colours are patched for each
pass, which works when the new index needs the same passes.

**Fallback: first pass only.** Draws that can't be repeated get only the
first pass, and the trace logs it once per mask:

- display lists, which hold the plain translation and run once;
- a block whose index changes need different passes;
- a block larger than the command buffer.

**Guest-side state.** The guest tracks the index state itself: mask, logic
op, current and clear index. It also tracks that state through `glPushAttrib`
and `glPopAttrib`, and display lists record their index changes for
`glCallList`. `glGet` therefore returns the program's own values, not their
host translation.

The palette is the window's X colormap. The XL8's X server has 8-bit
colormaps, so a program's stores beyond 255 fail on the server. The small
companion library `libglpass_x.so` ([guest/xstore.c](guest/xstore.c)) is
preloaded to:

- **record colours:** it notes every colour a program stores (`XStoreColors`
  and related calls) and passes the call on to libX11;
- **seed new colormaps:** a program's own PseudoColor colormap (created with
  `AllocAll`) starts with the default colormap's colours, so its Motif
  widgets inside GL windows show their intended colours; a colormap created
  without cells (`AllocNone`) for the default visual is the default colormap
  itself;
- **remap drawing pixels:** 12-bit pixel values a program hands to X (GC and
  window colours) are mapped to the nearest colour that exists.

### Visuals

Visual requests are answered as follows:

| Request | Answer |
|---|---|
| RGBA | The screen's default 8-bit PseudoColor visual |
| Colour index | The same 8-bit PseudoColor visual |
| Overlay (level 1) | None by default. With `GLPASS_OVERLAY=1`: the X server's overlay visual (2-bit on the XL8), when its depth meets `GLX_BUFFER_SIZE` (see [status](#status-and-limitations)) |

The X visual only carries the key colour, so its type doesn't limit what the
host renders. GLX reports RGBA8, double buffering, depth 24 and stencil 8.
Whether a context draws in RGBA or colour index follows the request; for
copied visual structures, it follows the window's colormap instead.

### X connections

The library talks to the X server for window positions, the key fill and
colormaps on a connection of its own, guarded by the library lock, because Xlib
connections are not thread-safe and programs such as Maya use theirs from
several threads. It avoids waiting for X replies while presenting, because a
program may hold a server grab then (Maya does during its marking menus):
window positions come from X events, not from round trips.

### X errors and destroyed windows

The process has one X error handler. When a program destroys a window it
still draws to, glpass's requests for it fail. Houdini's License
Administrator does this: it leaves a context current on its destroyed splash
window and keeps flushing. Those errors must not reach the program, because
its handler would take them as its own, and Xt's default handler exits.

**The handler.** glpass installs the process's handler. It keeps the errors
glpass caused:

- everything on glpass's own connection;
- the few requests it sends on the program's connection (the overlay key
  fill), recognised by their serial numbers.

It logs those with `GLPASS_TRACE=1` and passes every other error to the
program's handler unchanged. `libglpass_x` routes the program's
`XSetErrorHandler` calls to glpass, so a handler the program installs later
becomes the one glpass passes errors to, instead of replacing glpass's.

**Destroyed windows** are recorded from any of three sources:

- the program's `XDestroyWindow`, caught by `libglpass_x`;
- the window's `DestroyNotify`;
- a `BadWindow` or `BadDrawable` error from one of glpass's requests on it.

After that, the window is no longer shown or queried. `glXMakeCurrent` on it
returns `False`, as GLX does, and the current context stays current. A
window that is only unmapped stays a valid target: GLX allows binding to
unmapped windows.

### Window tracking

The library tracks each GL window's place on screen from X events, so it
does not have to ask the server at every present.

**First present.** The first present of a window queries the window and its
ancestors up to the root. It also selects `StructureNotify` on each of them
on the library's own connection; this changes nothing for the program.

**After that**, these events on the window or any ancestor keep its screen
position, size and visibility current:

- `ConfigureNotify` and `GravityNotify`: position and size;
- `MapNotify` and `UnmapNotify`: visibility;
- `ReparentNotify` and `DestroyNotify`: the chain of ancestors is queried
  again at the next present.

Events from before the queries are told apart by their serial number and
ignored. A window manager's synthetic `ConfigureNotify` is ignored too,
because the real event of its frame follows.

**When events are read.** The library has no thread of its own. Events are
read without waiting (`XEventsQueued`, `QueuedAfterReading`) under the
library lock, whenever any of the program's threads presents a drawable or
makes a context current. At that point:

- a shown window that is no longer viewable (it or an ancestor was unmapped
  or destroyed) has its image taken off the display (`GLP_SWAP_HIDE`);
- a shown window that moved or was resized has its image shown again at its
  new place, from the front buffer, without being redrawn. The emulator's
  ±4-pixel alignment search runs again there.

**Overlay windows** are presented while the program may hold a server grab.
They use the last known place when their chain needs a query, rather than
waiting for the grab to end.

**Command submission** (`SUBMIT`) never takes the lock or reads events.

## Repository layout

| Path | What |
|---|---|
| [guest/](guest/) | IRIX side, built inside IRIX |
| [guest/glpass.c](guest/glpass.c) | Hypercall, command buffer, hand-written GL entry points, GLX, colour-index emulation |
| [guest/glpass_gen.c](guest/glpass_gen.c), [guest/glpass_fn.h](guest/glpass_fn.h) | Generated encoders and stubs, generated command numbers (do not edit) |
| [guest/glpass_protocol.h](guest/glpass_protocol.h) | Protocol shared by guest and host (plain C) |
| [guest/glpass_internal.h](guest/glpass_internal.h) | Helpers shared by `glpass.c` and the generated code |
| [guest/xstore.c](guest/xstore.c), [guest/glpass_xstore.h](guest/glpass_xstore.h) | `libglpass_x.so`: colormap capture and pixel remapping (preloaded) |
| [guest/gl_api.txt](guest/gl_api.txt) | The 886 IRIX `gl.h` prototypes the generator reads |
| [guest/glrun](guest/glrun) | Launcher that sets the library path and the preload |
| [guest/gltri.c](guest/gltri.c) | Test program: a spinning triangle with a frame-rate counter |
| [guest/glgone.c](guest/glgone.c) | Test program: a context left current on a window the program destroys, then flushed; the program must receive no X errors |
| [guest/glcheck.c](guest/glcheck.c) | Test program: selection, feedback and 3D textures, checked value by value in the program's memory |
| [guest/glmove.c](guest/glmove.c) | Test program: moves, then unmaps, a GL window without redrawing it; its image should follow, then disappear |
| [guest/glthreads.c](guest/glthreads.c) | Test program: two threads drawing at once, each with its own context and window, checking every frame (`glthreads`: sproc, `glthreads_pt`: POSIX threads) |
| [guest/Makefile](guest/Makefile) | IRIX build (freeware gcc) |
| [host/](host/) | Emulator side, compiled into the emulator |
| [host/GlBridge.hpp](host/GlBridge.hpp) | Bridge interface used by the emulator core |
| [host/GlBridgeMac.cpp](host/GlBridgeMac.cpp) | macOS bridge: CGL legacy contexts, framebuffer objects, query replies, compositing source |
| [host/GlBridgeStub.cpp](host/GlBridgeStub.cpp) | Other hosts: a bridge that reports itself unavailable |
| [host/GlDispatchGen.inc](host/GlDispatchGen.inc) | Generated host replay (do not edit) |
| [tools/glpass-gen.py](tools/glpass-gen.py) | Code generator for both sides |
| [tools/make-guest-cd.sh](tools/make-guest-cd.sh) | Packs `guest/` into a CD image for IRIX |

Emulator code the pass-through relies on, outside this directory:

| Where | What |
|---|---|
| `src/cpu/R5000.cpp` | Hypercall instruction; guest memory access (`guestAccessible`, `guestCopyIn`, `guestCopyOut`) |
| `src/core/System.cpp` | Routes hypercall services to the bridge |
| `src/graphics/Newport.cpp` | Key-colour compositing of host images, in layers, newest first |
| `include/indy/core/DisplayOverlay.hpp` | The image handed from the bridge to the display |

## Building

### Host side (with the emulator)

Nothing extra is needed: CMake compiles `host/` into the emulator. On macOS it
links the system OpenGL framework (legacy OpenGL 2.1 profile through CGL); on
other hosts the stub bridge is used and IRIX keeps its own OpenGL.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The PGO build (`tools/build-pgo.sh`) includes it as well.

### Guest side (inside IRIX)

The IRIX libraries are compiled inside IRIX with the freeware gcc (`fw_gcc`,
`/usr/freeware/bin/gcc`), not MIPSpro `cc`, which needs a licence. They are
built for the n32 ABI, which Maya, Quake III and most IRIX 6.5 programs use.

```sh
cd /usr/local/irix-emu/glpass
make            # libGL.so, libglpass_x.so, gltri, glthreads, glthreads_pt, glmove, glcheck, glgone
```

The guest and the emulator must come from the same source tree: the command
numbers are generated. At startup the library sends the ID of its command
table (`GLP_FN_TABLE_ID`, generated) and the emulator refuses a library built
from other sources. The program then prints `glpass: this library and the
emulator come from different glpass sources` and falls back to IRIX's own
OpenGL. After changing either side, rebuild both.

## Packaging and installing in IRIX

### Packaging

On the host, pack the guest sources into a CD image:

```sh
glpass/tools/make-guest-cd.sh            # -> build/guest-cd.img
```

The image is a plain tar archive (everything under `glpass/`), padded to
2048-byte sectors, which IRIX reads from the raw CD device.

### Installing

1. Start the emulator with the CD image inserted:

   ```sh
   ./build/bin/indy <prom.bin> --disk <irix disk> --cdrom build/guest-cd.img
   ```

2. In IRIX, as root (serial console or a shell window), unpack and build:

   ```sh
   mkdir -p /usr/local/irix-emu && cd /usr/local/irix-emu
   rm -rf glpass
   dd if=/dev/rdsk/dks0d4vol bs=2048 | tar xf -
   cd glpass && make
   chmod -R a+rx /usr/local/irix-emu
   ```

   `dks0d4` is the emulator's CD-ROM (SCSI ID 4).

3. Check it as an ordinary user, in a shell on the IRIX desktop:

   ```sh
   /usr/local/irix-emu/glpass/glrun /usr/local/irix-emu/glpass/gltri 300
   ```

   It should print `gltri: irix-emu / glpass (<host GPU>)` and frame rates.
   For a fuller check:

   ```sh
   /usr/local/irix-emu/glpass/glrun /usr/local/irix-emu/glpass/glcheck
   ```

   It should end with `glcheck: PASS`.

### Updating after an emulator update

The library and the emulator must come from the same sources (see
[Building](#guest-side-inside-irix)). After pulling or rebuilding the
emulator, a stale library makes the emulator log this:

```
GL bridge: the guest glpass library (0x...) was built from other sources than this emulator (0x...)
```

Programs started with `glrun` then get no GL: Maya aborts with signal 6, and
others print `glpass: this library and the emulator come from different
glpass sources`. Rebuild the CD image (`glpass/tools/make-guest-cd.sh`) and
repeat step 2 above. The line `#define GLP_FN_TABLE_ID` in
`/usr/local/irix-emu/glpass/glpass_fn.h` should then match the emulator's ID.

The emulator keeps the CD image open and reads it on each access. While
developing, you can rebuild the image in place and repeat step 2 without
restarting the emulator.

### Uninstalling

Delete the directory: `rm -rf /usr/local/irix-emu/glpass`. Nothing outside it
is changed, and IRIX's own `/usr/lib32/libGL.so` is never touched.

### Planned: system-wide installation

Today only programs started through `glrun` use glpass. A system-wide install
is planned, so that every program uses the host GPU without a wrapper:

- prebuilt libraries on the CD (no gcc needed in IRIX);
- an install script or an IRIX `inst` package;
- builds for the o32 and 64-bit ABIs as well;
- IRIX's own libraries kept, with an automatic fallback to them when the
  emulator offers no GL bridge.

## Running programs

Start a program through the launcher:

```sh
/usr/local/irix-emu/glpass/glrun maya
/usr/local/irix-emu/glpass/glrun /usr/local/games/Quake3/quake3.irix
```

`glrun` sets two environment variables and starts the program:

| Variable | Effect |
|---|---|
| `LD_LIBRARYN32_PATH` | Puts this `libGL.so` ahead of IRIX's |
| `_RLDN32_LIST=.../libglpass_x.so:DEFAULT` | Preloads the colormap library (IRIX's equivalent of `LD_PRELOAD`) |

Programs started without `glrun` (from a desktop icon, for example) use IRIX's
own OpenGL, as before.

## Diagnostics

| Where | Variable | Effect |
|---|---|---|
| IRIX | `GLPASS_TRACE=1` | Logs, to the program's stderr: the first call of every GL entry point; GLX calls; presents and palettes; and, from `libglpass_x`, the pixel values handed to X and any non-copy X line drawing |
| IRIX | `GLPASS_TRACE=1` and the file `/tmp/glpass.dump` | While the file exists, every command sent is logged with its context and first arguments (integers and floats). Create it, do the action to study, delete it |
| IRIX | `GLPASS_OVERLAY=1` | Offers the X server's overlay visual for overlay requests that its depth satisfies (see below) |
| Emulator | `INDY_TRACE_GL=1` | Logs each composited image (size, position, key alignment, centre pixel) and the host time of each swap |
| Emulator | `INDY_GL_DUMP=<dir>` | With `INDY_TRACE_GL`, writes each window's last host image to `<dir>/gl_<window>.ppm` |
| Emulator | `kill -USR1 <pid>` | Saves a screenshot of the running emulator to `$INDY_SNAPSHOT` (default `indy-snapshot.ppm`) |
| Emulator | `INDY_INPUT_FILE=<file>` | Lines appended while it runs are input (`mouse <dx> <dy> [<buttons>]`, `type <text>`): with the screenshots, scripted tests of a running program |
| Emulator | `INDY_TRACE_PLANES=1` | Logs which Newport plane sets the X server draws into (main, overlay, popup, CID) |

An entry point that glpass does not provide logs
`glpass: <name> is not supported (ignored)` once.

## Status and limitations

Tested programs:

| Program | State |
|---|---|
| `gltri` | Works: thousands of frames per second |
| `glgone` | Pass: no X errors reach the program after it destroys a window its context is still current on (three before the fix) |
| Houdini 6 License Administrator (`hkey`) | Renders its whole interface (drawn with GL; black with IRIX's own GL) and switches tabs, with no X errors. Its File menu does not open: holding the button produces no GL or GLX call at all, so the cause is not known yet. |
| `glcheck` | Pass: selection (hits, names, depths), feedback (tokens, window coordinates, pass-through), 3D texture upload with skipped images |
| `glmove` | Pass: the image follows the moved window and disappears when it is unmapped |
| `glthreads`, `glthreads_pt` | Pass: two threads, 150 frames each, no frame drawn into the other thread's context (sproc and POSIX threads) |
| Quake III Arena (IRIX build) | Runs: menus, intro, game; host cost about 1.2 ms per frame. Image quality has dropped since the visual changes made for Maya (under investigation), and fullscreen mode shows a normal window. |
| Maya 6.5 | Usable: viewports in wireframe and smooth shade, tumbling, object creation and selection (click and selection rectangle), manipulators, dropdown and panel menus, right-click marking menus, range slider; the same with `GLPASS_OVERLAY=1`. Gaps below. |

Known limitations:

- **Quake III quality and fullscreen.** Quake III asks for 4/4/4-bit colour
  and renders at that depth; its image is darker than before the Maya
  visual changes, for a reason not yet found. Its fullscreen mode
  (`r_fullscreen 1`) reports 640×480 fullscreen but draws a decorated
  window.
- **Flushes over the last frame are shown.** A program that draws part of a
  frame on top of the previous one without clearing it, and flushes midway,
  shows that partial image. This rule exists for Maya's selection rectangle.
  Programs that clear the colour buffer at the start of each frame are
  unaffected.
- **Overlay planes** (`GLPASS_OVERLAY=1`) are off by default:
  - **The XL8 overlay.** The XL8's only overlay visual is 2-bit (its popup
    planes). Since `GLX_BUFFER_SIZE` is now honoured as a minimum, Maya's
    requests for 8- and 4-bit overlays find none, as with IRIX's GLX.
  - **Maya with overlays on** therefore uses no overlay context. It runs
    normally: menus and marking menus work.
  - **The earlier freeze.** It came from offering the 2-bit visual for those
    requests. Maya then posted its Motif menus in overlay planes it could not
    create windows in, which left an invisible menu holding the pointer grab.
- **Index write masks** are exact (see
  [Index write masks](#index-write-masks)), except in display lists and
  blocks that change to an index needing other passes. Those get the first
  pass only, logged once per mask with `GLPASS_TRACE=1`.
- **Maya's marking menu** briefly shows the rest of Maya's interface black
  with red outlines while it is up: a colormap switch on the X side,
  restored on release.
- **Host:** macOS only (CGL, legacy OpenGL 2.1). Other hosts get the stub.
- **Guest:** n32 programs only, started through `glrun`.
- **Window changes while the program is idle:** the library has no thread of
  its own, so a move or unmap shows on the display at the next present or
  `glXMakeCurrent` from any of the program's threads. Until then, the image
  shows only where its key colour still is.
- **Threads:** a context current in two threads at once is not refused as
  GLX would; both threads' commands then go to that one context.
  `glXDestroyContext` on a context current in another thread takes effect at
  once, not when that thread releases it.
- **Not available:** the imaging subset (colour tables, convolution,
  histograms), compressed textures, vertex and fragment programs, multisampling,
  accumulation buffers, stereo.

## Changing the code

- **Adding or changing a generated entry point:** edit
  [tools/glpass-gen.py](tools/glpass-gen.py) (or `gl_api.txt`), then run it on
  the host:

  ```sh
  python3 glpass/tools/glpass-gen.py
  ```

  It rewrites `guest/glpass_gen.c`, `guest/glpass_fn.h` and
  `host/GlDispatchGen.inc`, checking with the host compiler which functions
  the macOS OpenGL headers declare. Rebuild the emulator and the guest.
- **Hand-written entry points:** list the function in the generator's `MANUAL`
  set, implement it in `guest/glpass.c`, and, if it sends a command, add its
  replay case in `host/GlBridgeMac.cpp`. A function the guest only needs to
  watch (it tracks state with it, then sends the command unchanged) goes in
  `GUEST_MANUAL` instead: the host replay stays generated.
- **Protocol changes:** edit `guest/glpass_protocol.h` and bump `GLP_VERSION`.
- **Checking the guest sources on the host:** there's no MIPS cross-compiler
  in the build, but with IRIX's `GL/gl.h`, `GL/glx.h` and `GL/glxtokens.h` in
  an include directory and any X11 headers, a syntax check catches most
  mistakes. The register-variable lines of the hypercall are expected to fail
  on non-MIPS targets.

  ```sh
  clang -fsyntax-only -std=gnu89 -D_LANGUAGE_C -Wall -Wdeclaration-after-statement \
        -I<irix-gl-headers> -I/opt/homebrew/include glpass/guest/glpass.c
  ```

- **Unit tests** (`tests/CoreTests.cpp`) cover the emulator side:
  `hypercallReadsGuestMemoryOrFaults`, `hypercallCopiesResumeAcrossFaults`,
  `newportOverlayFollowsKeyPixels`, `newportOverlayNewestWins` and
  `newportOverlayLayerUsesPopupKey`. The guest library is tested in IRIX with
  `gltri`, Quake III and Maya.
