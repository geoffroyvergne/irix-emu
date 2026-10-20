/*
 * glpass: an IRIX libGL replacement that renders through the emulator's host
 * GPU (GL pass-through). OpenGL calls are batched into a command stream and
 * handed to the emulator with a hypercall; each GLX drawable is rendered by
 * the host and composited over the emulated display where the window shows
 * the key colour this library paints it with.
 *
 * Only for programs started with LD_LIBRARYN32_PATH pointing here (see the
 * README); everything else keeps IRIX's own libGL.
 *
 * This file holds the hypercall plumbing, the entry points that need guest
 * state or return data (queries, images, client vertex arrays, selection),
 * and GLX. glpass_gen.c (glpass/tools/glpass-gen.py) holds the plain commands.
 * Build: make (gcc -mabi=n32; MIPSpro cc is not needed).
 */
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef __sgi
#include <abi_mutex.h>
#include <sys/prctl.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glpass_internal.h"
#include "glpass_xstore.h"

/* ---- Hypercall -------------------------------------------------------------- */

static long glp_call(unsigned long service, unsigned long op, const void *buffer, unsigned long length, long *v1out)
{
    register unsigned long a0 __asm__("$4") = service;
    register unsigned long a1 __asm__("$5") = op;
    register const void *a2 __asm__("$6") = buffer;
    register unsigned long a3 __asm__("$7") = length;
    register long v0 __asm__("$2");
    register long v1 __asm__("$3");
    __asm__ __volatile__(".word 0x00000668"
                         : "=r"(v0), "=r"(v1)
                         : "r"(a0), "r"(a1), "r"(a2), "r"(a3)
                         : "memory");
    if (v1out) *v1out = v1;
    return v0;
}

/* A GL operation for `context` (0: none), which the host runs with that
   context's host GL context. */
static long glp_gl(unsigned int op, unsigned int context, const void *buffer, unsigned long length)
{
    return glp_call(GLP_SERVICE_GL, GLP_OP_WORD(op, context), buffer, length, 0);
}

static int glp_state = -1; /* -1 unknown, 0 unavailable, 1 available */
static int glp_why;        /* why unavailable: GLP_PROBE_* */

/* The hypercall word is a reserved instruction: on a real Indy (or any
   machine but the emulator) it raises SIGILL. The first PING runs with a
   SIGILL handler of its own, so that this only means "no emulator". */
static sigjmp_buf probe_jump;

static void probe_trap(int sig)
{
    (void)sig;
    siglongjmp(probe_jump, 1);
}

static long glp_ping(long *version)
{
    struct sigaction trap, saved;
    long result = -1;
    memset(&trap, 0, sizeof trap);
    trap.sa_handler = probe_trap;
    sigemptyset(&trap.sa_mask);
    if (sigaction(SIGILL, &trap, &saved) != 0) return -1;
    if (sigsetjmp(probe_jump, 1) == 0) result = glp_call(GLP_SERVICE_PING, 0, 0, 0, version);
    sigaction(SIGILL, &saved, 0);
    return result;
}

static int glp_available(void)
{
    if (glp_state < 0) {
        long version = 0, hello = -1;
        glp_lock(); /* the SIGILL handler is the process's */
        if (glp_state < 0) {
            if (glp_ping(&version) == (long)GLP_PING_MAGIC) {
                hello = glp_gl(GLP_OP_HELLO, 0, 0, GLP_FN_TABLE_ID);
                glp_why = hello == (long)GLP_VERSION ? GLP_PROBE_OK
                          : hello == GLP_ERR_TABLE   ? GLP_PROBE_MISMATCH
                                                     : GLP_PROBE_NO_BRIDGE;
            } else {
                glp_why = GLP_PROBE_NO_EMULATOR;
            }
            if (glp_why == GLP_PROBE_MISMATCH) {
                fprintf(stderr, "glpass: this library and the emulator come from different glpass sources: "
                                "rebuild it in IRIX (see glpass/README.md)\n");
            }
            glp_state = glp_why == GLP_PROBE_OK;
        }
        glp_unlock();
    }
    return glp_state;
}

/* For glpass-probe and the boot script: GLP_PROBE_*. */
int glpass_probe(void)
{
    (void)glp_available();
    return glp_why;
}

/* ---- Route: the host GPU, or IRIX's own OpenGL -------------------------------

   Every GL and GLX entry point checks the route first; it is decided at the
   first call. GLP_ROUTE_HOST: the emulator's bridge answers, glpass runs
   the call. GLP_ROUTE_NATIVE: the call goes on to IRIX's own libGL, the
   file /usr/lib32/libGL.so names on a system without glpass, so a disk with
   glpass installed system-wide still runs GL programs on real hardware, in
   an emulator without host GL, or with a library that no longer matches the
   emulator. GLPASS_NATIVE=1 asks for IRIX's OpenGL; GLPASS_NATIVE_LIBGL
   names its file. With neither available, the route is the host's and GL
   reports itself unavailable, as before. */
#ifndef GLP_NATIVE_LIBGL
#define GLP_NATIVE_LIBGL "/var/arch/lib32/libGL.so"
#endif

int glp_route;
static void *native_gl;

static void glx_log(const char *format, ...);

static void *native_open(void)
{
    const char *path = getenv("GLPASS_NATIVE_LIBGL");
    void *h = dlopen(path && *path ? path : GLP_NATIVE_LIBGL, RTLD_LAZY);
    if (h && dlsym(h, "glpass_probe")) h = 0; /* this library itself: it would forward to itself */
    return h;
}

static void glp_decide(void)
{
    glp_lock();
    if (!glp_route) {
        const char *force = getenv("GLPASS_NATIVE");
        const int want_native = force && *force && *force != '0';
        if (!want_native && glp_available()) {
            glp_route = GLP_ROUTE_HOST;
        } else if ((native_gl = native_open()) != 0) {
            glp_route = GLP_ROUTE_NATIVE;
        } else {
            glp_route = GLP_ROUTE_HOST;
        }
        glx_log("route: %s (probe %d)", glp_route == GLP_ROUTE_NATIVE ? "IRIX's own OpenGL" : "host GPU", glp_why);
    }
    glp_unlock();
}

int glp_forwarding(void)
{
    if (!glp_route) glp_decide();
    return glp_route == GLP_ROUTE_NATIVE;
}

void *glp_native_symbol(const char *name)
{
    return native_gl ? dlsym(native_gl, name) : 0;
}

/* ---- Diagnostics ------------------------------------------------------------ */

int glp_trace = -1;
static unsigned char traced[GLP_FN_END];
static const char *fn_names[GLP_FN_END]; /* entry points seen, for the command dump */

void glp_trace_first(unsigned int fn, const char *name)
{
    if (glp_trace < 0) {
        const char *v = getenv("GLPASS_TRACE");
        glp_trace = v && *v && *v != '0';
    }
    if (!glp_trace || fn >= GLP_FN_END) return;
    fn_names[fn] = name;
    if (traced[fn]) return;
    traced[fn] = 1;
    fprintf(stderr, "glpass: %s\n", name);
}

/* GLPASS_TRACE=1: GLX calls (repeats folded, the first thousand lines). */
static void glx_log(const char *format, ...)
{
    static int lines, repeats;
    static char last[600];
    char text[600];
    va_list ap;
    if (glp_trace < 0) glp_trace_first(GLP_FN_END, "");
    if (!glp_trace || lines >= 200000) return;
    va_start(ap, format);
    vsprintf(text, format, ap);
    va_end(ap);
    if (strcmp(text, last) == 0) {
        ++repeats;
        return;
    }
    if (repeats) fprintf(stderr, "glpass:   (repeated %d times)\n", repeats);
    repeats = 0;
    strcpy(last, text);
    ++lines;
    fprintf(stderr, "glpass: %s\n", text);
}

static void log_attributes(const char *call, const int *attributes)
{
    char text[512];
    size_t at = 0;
    const int *a;
    text[0] = 0;
    for (a = attributes; a && *a != None && at + 16 < sizeof text; ++a) {
        at += (size_t)sprintf(text + at, " %#x", (unsigned int)*a);
    }
    glx_log("%s(%s )", call, text);
}

/* A stub was called: it does nothing, and with GLPASS_TRACE its first call
   is logged. `logged` is the stub's own flag, claimed under the library
   lock, so the line appears once whatever the threads and however many
   stubs a program calls. */
void glp_unimplemented(const char *name, int *logged)
{
    int first;
    if (*logged) return;
    glp_lock();
    first = !*logged;
    *logged = 1;
    glp_unlock();
    if (glp_trace < 0) glp_trace_first(GLP_FN_END, ""); /* reads GLPASS_TRACE */
    if (first && glp_trace) fprintf(stderr, "glpass: %s is not supported (ignored)\n", name);
}

#define TRACE(name) GLP_TRACE(GLP_FN_##name, #name)

/* ---- Contexts and client state ------------------------------------------------ */

/* Client arrays. Texture coordinates have one array per texture unit
   (glClientActiveTexture picks the unit glTexCoordPointer and the client
   state calls act on): unit 0 is ARR_TEXCOORD, units 1 to 7 follow
   ARR_EDGEFLAG. The numbers are the protocol's (GLP_FN_CLIENT_DRAW). */
#define TEXTURE_UNITS 8
enum {
    ARR_VERTEX, ARR_NORMAL, ARR_COLOR, ARR_INDEX, ARR_TEXCOORD, ARR_EDGEFLAG,
    ARR_TEXCOORD1, ARR_COUNT = ARR_TEXCOORD1 + TEXTURE_UNITS - 1
};

#define GLP_TEXTURE0 0x84C0               /* GL_TEXTURE0(_ARB) */
#define GLP_CLIENT_ACTIVE_TEXTURE 0x84E1  /* GL_CLIENT_ACTIVE_TEXTURE(_ARB) */

struct glp_array {
    int enabled;
    int size;
    GLenum type;
    int stride;
    const void *pointer;
};

struct glp_pixelstore {
    int swap, lsbfirst, rowlength, skiprows, skippixels, alignment;
    int imageheight, skipimages; /* 3D images (GL 1.2, EXT_texture3D) */
};

/* GL 1.2 / EXT_texture3D pixel store and texture names, by value: the IRIX
   headers name them with an _EXT suffix. */
#define GLP_PACK_SKIP_IMAGES 0x806B
#define GLP_PACK_IMAGE_HEIGHT 0x806C
#define GLP_UNPACK_SKIP_IMAGES 0x806D
#define GLP_UNPACK_IMAGE_HEIGHT 0x806E
#define GLP_TEXTURE_3D 0x806F
#define GLP_TEXTURE_DEPTH 0x8071

struct glp_client {
    struct glp_array arrays[ARR_COUNT];
    struct glp_pixelstore pack, unpack;
    int unit; /* glClientActiveTexture, from 0 */
};

#define CLIENT_STACK 16

/* Colour index write state (see "Colour index writes"). */
struct glp_index_state {
    unsigned int mask;  /* glIndexMask */
    int logic;          /* GL_INDEX_LOGIC_OP enabled */
    GLenum op;          /* glLogicOp */
    unsigned int index; /* glIndex */
    unsigned int clear; /* glClearIndex */
    GLenum depth_func;  /* glDepthFunc: replays pass the depth test again */
};

/* What the host buffer was last set to for it. */
struct glp_index_host {
    int known;             /* 0 after a display list or attribute pop set it behind our back */
    int logic;             /* GL_COLOR_LOGIC_OP */
    GLenum op;
    unsigned int channels; /* colour mask: bit 0 R ... bit 3 A */
};

/* Index state changes a display list makes when called. */
struct glp_list_effect {
    GLuint list;
    unsigned int changed; /* INDEX_CHANGED_* */
    struct glp_index_state state;
};

/* A place in recorded commands that a pass replay rewrites or follows. */
enum {
    EV_COLOR4, /* glColor4ub payload (4 words) for index `value` */
    EV_PACKED, /* one RGBA colour of a vertex array (index array converted) */
    EV_CALL,   /* glCallList record of `len` words: list `value` */
    EV_CALLS   /* glCallLists record of `len` words */
};

struct glp_event {
    unsigned int at; /* word offset in the recording */
    unsigned int kind;
    unsigned int value;
    unsigned int len;
};

/* Commands kept as they are sent, across command buffer flushes: a draw to
   send again for further passes, or a display list being compiled. */
struct glp_recording {
    int active;
    int failed;         /* out of memory, or over its size limit: not kept */
    unsigned int start; /* first word of the thread's command buffer not yet kept */
    unsigned int limit; /* words */
    unsigned int *words;
    unsigned int nwords, capwords;
    struct glp_event *events;
    unsigned int nevents, capevents;
};

/* A display list: its index state effect, and (its shadow) the commands it
   holds, so that it can run once per pass like a draw. */
struct glp_list {
    GLuint id;
    int refs;       /* 1 while defined, plus each expansion using it */
    struct glp_list_effect effect;
    int expandable; /* see list_scan */
    unsigned int refused; /* else the command that stops it (GLP_FN_*), for the trace */
    unsigned int *words;
    unsigned int nwords;
    struct glp_event *events;
    unsigned int nevents;
    unsigned long mark; /* list_collect's walk */
    int visit;
    struct glp_list *next;
};

#define LIST_BUCKETS 256
#define LIST_MAX_WORDS (1u << 20)    /* a list's shadow: 4 MB */
#define LISTS_MAX_WORDS (4ul << 20)  /* all shadows of a share group: 16 MB */
#define GROUP_MAX_WORDS (8u << 20)   /* a recorded draw: 32 MB */
#define LIST_NESTING 64              /* GL_MAX_LIST_NESTING */

/* The display lists of contexts that share them, under the library lock. */
struct glp_lists {
    int refs;            /* contexts */
    unsigned long words; /* in all shadows */
    unsigned long mark;
    struct glp_list *bucket[LIST_BUCKETS];
};

#define INDEX_STACK 16

/* One pass of a masked index write: host logic op and the index value whose
   encoding is the colour. */
struct glp_index_pass {
    GLenum op;
    unsigned int value;
};

struct glp_index {
    struct glp_index_state state;
    struct glp_index_host host;
    struct glp_index_pass passes[3]; /* for the current index; host set for passes[0] */
    int npasses;                     /* follows from the mask and logic op alone */
    struct glp_recording group;      /* a draw being recorded for the other passes */
    struct glp_recording compile;    /* the list being compiled: its shadow */
    struct glp_lists *lists;         /* display lists (shared with share contexts) */
    struct {
        GLbitfield bits;
        struct glp_index_state state;
        struct glp_index_host host;
    } stack[INDEX_STACK];
    int depth;
    int in_begin;                  /* between glBegin and glEnd: no state changes */
    GLuint list;                   /* list being compiled; 0 none */
    GLenum list_mode;
    struct glp_list_effect compiling;
};

struct __GLXcontextRec {
    unsigned int id;
    Display *dpy;
    int screen;
    VisualID visual;
    GLXContext share;
    GLXDrawable drawable;
    unsigned int width, height;
    struct glp_client client;
    struct glp_client client_stack[CLIENT_STACK];
    GLbitfield client_masks[CLIENT_STACK];
    int client_depth;
    GLenum draw_buffer;
    GLenum read_buffer;
    GLenum render_mode;
    GLuint *select_buffer;
    GLsizei select_size;
    GLfloat *feedback_buffer;
    GLsizei feedback_size;
    int index_mode;                  /* colour index (else RGBA) */
    int pseudo;                      /* on a PseudoColor visual: the mode follows the window */
    int overlay;                     /* created for the overlay planes' visual */
    int frame_cleared;               /* a colour clear since the drawable's last swap */
    unsigned long shown_words;       /* commands sent when its image was last shown */
    struct glp_index ix;             /* colour index writes */
    struct glp_thread *volatile owner; /* the thread it is current in (GLX: one at most) */
    int destroy_pending;             /* destroyed while current in another thread: done on release */
    GLuint list_base;                /* glListBase */
};

static void context_defaults(GLXContext c)
{
    int i;
    memset(&c->client, 0, sizeof c->client);
    for (i = 0; i < ARR_COUNT; ++i) {
        c->client.arrays[i].size = 4;
        c->client.arrays[i].type = GL_FLOAT;
    }
    c->client.arrays[ARR_NORMAL].size = 3;
    c->client.arrays[ARR_INDEX].size = 1;
    c->client.arrays[ARR_EDGEFLAG].size = 1;
    c->client.arrays[ARR_EDGEFLAG].type = GL_UNSIGNED_BYTE;
    c->client.pack.alignment = 4;
    c->client.unpack.alignment = 4;
    c->render_mode = GL_RENDER;
    c->ix.state.mask = 0xFFFFFFFFu;
    c->ix.state.op = GL_COPY;
    c->ix.state.index = 1;
    c->ix.state.depth_func = GL_LESS;
    c->draw_buffer = GL_BACK; /* every visual is double-buffered */
    c->read_buffer = GL_BACK;
}

/* ---- Per-thread state ---------------------------------------------------------- */

/* GLX makes a context current per thread, so the current context and the
   command stream built for it belong to the calling thread. IRIX programs
   run threads two ways:
     * POSIX threads: one process; a pthread key holds each thread's state.
     * sproc (Maya): one process per thread, sharing the address space; the
       state is found by the thread's own pid, PRDA->t_sys.t_pid, which is
       read from memory, not with a system call.
   libpthread cannot be loaded into a sproc program, so this library does not
   link it: its key functions are looked up at run time and used when the
   program has them. */
struct glp_thread {
    pid_t pid;                         /* the process (sproc) it was made in */
    GLXContext current;
    struct __GLXcontextRec no_context; /* client state when nothing is current */
    unsigned int *cmdbuf;              /* commands not submitted yet */
    unsigned int cmdcap, cmdlen;
    unsigned long submitted;           /* words sent for the current context (trace) */
    int begin_pending;                 /* a glBegin held back while only glArrayElement follows */
    GLenum begin_mode;
    unsigned int *begin_elements;
    unsigned int begin_count, begin_capacity;
};

static pid_t thread_pid(void)
{
#ifdef __sgi
    pid_t pid = PRDA->t_sys.t_pid;
    if (pid) return pid;
#endif
    return getpid();
}

/* Written once under the threads lock and read without it, so volatile:
   `ready` is set last and other threads never see it before the rest. */
static struct {
    volatile int ready; /* 0 not looked up yet, 1 in use, -1 not available */
    volatile pthread_key_t key;
    void *(*volatile get)(pthread_key_t);
    int (*volatile set)(pthread_key_t, const void *);
} keys;

/* States found by pid: sprocs, and every thread when there are no keys. */
#define THREADS_MAX 128
static struct {
    struct glp_thread *volatile state;
    volatile pid_t pid; /* written after `state`: a match finds the state complete */
} threads[THREADS_MAX];
static int next_thread_slot;

#ifdef __sgi
static abilock_t threads_lock; /* zero: UNLOCKED */
#define THREADS_LOCK() spin_lock(&threads_lock)
#define THREADS_UNLOCK() release_lock(&threads_lock)
#else
#define THREADS_LOCK() ((void)0)
#define THREADS_UNLOCK() ((void)0)
#endif

static void thread_free(void *state)
{
    struct glp_thread *t = (struct glp_thread *)state;
    if (!t) return;
    /* An exiting thread lets go of its context (a destroy requested for it
       meanwhile is not done: the context is merely leaked). */
    if (t->current && t->current->owner == t) t->current->owner = 0;
    free(t->cmdbuf);
    free(t->begin_elements);
    free(t);
}

static void keys_lookup(void)
{
    void *program;
    int (*create)(pthread_key_t *, void (*)(void *));
    THREADS_LOCK();
    if (!keys.ready) {
        int ready = -1;
        pthread_key_t key;
        program = dlopen(0, RTLD_LAZY);
        if (program) {
            create = (int (*)(pthread_key_t *, void (*)(void *)))dlsym(program, "pthread_key_create");
            keys.get = (void *(*)(pthread_key_t))dlsym(program, "pthread_getspecific");
            keys.set = (int (*)(pthread_key_t, const void *))dlsym(program, "pthread_setspecific");
            if (create && keys.get && keys.set && create(&key, thread_free) == 0) {
                keys.key = key;
                ready = 1;
            }
        }
        keys.ready = ready; /* last */
    }
    THREADS_UNLOCK();
}

/* A new state for the calling thread: under its pthread key when `keyed`,
   else in the pid table (reusing the slot of a process that has exited). */
static struct glp_thread *thread_attach(pid_t pid, int keyed)
{
    struct glp_thread *t = (struct glp_thread *)calloc(1, sizeof *t);
    struct glp_thread *dead = 0;
    int i, slot = -1;
    if (!t) {
        fprintf(stderr, "glpass: out of memory for a thread's state\n");
        abort();
    }
    t->pid = pid;
    context_defaults(&t->no_context);
    THREADS_LOCK();
    if (keyed && keys.set(keys.key, t) == 0 && keys.get(keys.key) == t) {
        THREADS_UNLOCK();
        return t;
    }
    if (keyed) keys.ready = -1; /* functions that do not keep values (stubs): pids only */
    for (i = 0; i < THREADS_MAX && slot < 0; ++i) {
        if (!threads[i].pid || (kill(threads[i].pid, 0) < 0 && errno == ESRCH)) slot = i;
    }
    if (slot < 0) {
        slot = next_thread_slot; /* full of live threads: take the oldest slot */
        next_thread_slot = (next_thread_slot + 1) % THREADS_MAX;
        fprintf(stderr, "glpass: more than %d GL threads; thread %ld loses its state\n", THREADS_MAX,
                (long)threads[slot].pid);
    } else if (threads[slot].pid) {
        dead = threads[slot].state;
    }
    threads[slot].pid = 0;
    threads[slot].state = t;
    threads[slot].pid = pid;
    THREADS_UNLOCK();
    thread_free(dead);
    return t;
}

struct glp_thread *glp_self(void)
{
    const pid_t pid = thread_pid();
    struct glp_thread *t;
    int i;
    if (!keys.ready) keys_lookup();
    if (keys.ready > 0) {
        t = (struct glp_thread *)keys.get(keys.key);
        if (t && t->pid == pid) return t;          /* a POSIX thread, or the program itself */
        if (!t) return thread_attach(pid, 1);      /* a POSIX thread's first GL call */
        /* else a sproc, which shares its parent's key values: by pid */
    }
    for (i = 0; i < THREADS_MAX; ++i) {
        if (threads[i].pid == pid) return threads[i].state;
    }
    return thread_attach(pid, 0);
}

/* The calling thread's current context; 0 if none. */
static GLXContext current_context(void) { return glp_self()->current; }

unsigned int glp_current_id(void)
{
    GLXContext c = current_context();
    return c ? c->id : 0;
}

static GLXContext ctx(void)
{
    struct glp_thread *t = glp_self();
    return t->current ? t->current : &t->no_context;
}

/* ---- Command buffer ----------------------------------------------------------- */

/* GLPASS_TRACE=1 and the file /tmp/glpass.dump present: every command
   submitted is logged with its first payload words (as integers and
   floats), to see a program's exact GL state at a moment of interest:
   touch the file, act, remove it. */
static void dump_commands(const struct glp_thread *t)
{
    static int checks;
    static int on;
    unsigned int at = 0;
    if (glp_trace <= 0) return;
    if ((checks++ & 15) == 0) on = access("/tmp/glpass.dump", F_OK) == 0;
    if (!on) return;
    while (at < t->cmdlen) {
        const unsigned int header = t->cmdbuf[at];
        const unsigned int cmd = header >> 16;
        unsigned int n = header & 0xFFFFu, p = at + 1, k;
        char text[400];
        size_t len;
        if (n == GLP_LONG_RECORD) n = t->cmdbuf[p++];
        len = (size_t)sprintf(text, "glpass: [%u] %s", t->current ? t->current->id : 0u,
                              cmd < GLP_FN_END && fn_names[cmd] ? fn_names[cmd]
                              : cmd == GLP_FN_CLIENT_DRAW ? "(client draw)"
                              : cmd == GLP_FN_PALETTE ? "(palette)" : "?");
        if (!(cmd < GLP_FN_END && fn_names[cmd])) len += (size_t)sprintf(text + len, " %#x", cmd);
        for (k = 0; k < n && k < 6; ++k) {
            float f;
            memcpy(&f, &t->cmdbuf[p + k], 4);
            len += (size_t)sprintf(text + len, " %#x(%g)", t->cmdbuf[p + k], f);
        }
        if (n > 6) sprintf(text + len, " ... [%u words]", n);
        fprintf(stderr, "%s\n", text);
        at = p + n;
    }
}

static void recordings_keep(GLXContext c, const struct glp_thread *t);
static void recordings_restart(GLXContext c, const struct glp_thread *t);

static void thread_flush(struct glp_thread *t)
{
    dump_commands(t);
    if (t->current) recordings_keep(t->current, t); /* a draw or list being recorded keeps what goes */
    if (t->cmdlen > 0 && t->current) {
        t->submitted += t->cmdlen;
        glp_gl(GLP_OP_SUBMIT, t->current->id, t->cmdbuf, (unsigned long)t->cmdlen * 4);
    }
    t->cmdlen = 0;
    if (t->current) recordings_restart(t->current, t);
}

static void glp_flush(void) { thread_flush(glp_self()); }

static void begin_materialize(struct glp_thread *t);

unsigned int *glp_put(unsigned int cmd, unsigned int words)
{
    struct glp_thread *t = glp_self();
    unsigned int header = words < GLP_LONG_RECORD ? 1 : 2;
    unsigned int *p;
    if (t->begin_pending) begin_materialize(t); /* another command inside glBegin/glEnd */
    if (t->cmdlen + header + words > t->cmdcap) {
        thread_flush(t);
        if (header + words > t->cmdcap) {
            unsigned int cap = t->cmdcap ? t->cmdcap : 16384;
            while (cap < header + words) cap *= 2;
            free(t->cmdbuf);
            t->cmdbuf = (unsigned int *)malloc((size_t)cap * 4);
            if (!t->cmdbuf) {
                fprintf(stderr, "glpass: out of memory for a %u-word command\n", words);
                abort();
            }
            t->cmdcap = cap;
        }
    }
    p = &t->cmdbuf[t->cmdlen];
    if (header == 1) {
        p[0] = (cmd << 16) | words;
    } else {
        p[0] = (cmd << 16) | GLP_LONG_RECORD;
        p[1] = words;
    }
    t->cmdlen += header + words;
    return p + header;
}

unsigned int glp_fbits(GLfloat f)
{
    unsigned int u;
    memcpy(&u, &f, 4);
    return u;
}

void glp_dbits(unsigned int *p, GLdouble d)
{
    memcpy(p, &d, 8); /* big-endian: high word first, as the protocol wants */
}

static unsigned int words_for(unsigned int bytes) { return (bytes + 3) / 4; }

/* Synchronous call: flushes the stream, then the host answers. `reply` (may be
   0) receives `bytes` bytes written by the host; args follow the header. */
static long glp_query(unsigned int fn, void *reply, unsigned int bytes, const unsigned int *args, unsigned int nargs)
{
    unsigned int local[32];
    unsigned int *req;
    long result;
    struct glp_thread *t = glp_self();
    if (!t->current) return 0;
    req = nargs + 3 <= 32 ? local : (unsigned int *)malloc((nargs + 3) * 4);
    if (!req) return 0;
    thread_flush(t);
    if (reply && bytes) {
        /* Fault the pages in first so the host finds them mapped and writable. */
        volatile char *c = (volatile char *)reply;
        unsigned int i;
        for (i = 0; i < bytes; i += 4096) c[i] = c[i];
        c[bytes - 1] = c[bytes - 1];
    }
    req[0] = fn;
    req[1] = (unsigned int)(unsigned long)reply;
    req[2] = reply ? bytes : 0;
    if (nargs) memcpy(req + 3, args, nargs * 4);
    result = glp_gl(GLP_OP_QUERY, t->current->id, req, (nargs + 3) * 4);
    if (req != local) free(req);
    return result;
}

/* ---- Value counts --------------------------------------------------------------- */

int glp_pname_count(int group, GLenum pname)
{
    switch (group) {
    case GLP_GROUP_LIGHT:
        if (pname == GL_AMBIENT || pname == GL_DIFFUSE || pname == GL_SPECULAR || pname == GL_POSITION) return 4;
        return pname == GL_SPOT_DIRECTION ? 3 : 1;
    case GLP_GROUP_LIGHTMODEL:
        return pname == GL_LIGHT_MODEL_AMBIENT ? 4 : 1;
    case GLP_GROUP_MATERIAL:
        if (pname == GL_AMBIENT || pname == GL_DIFFUSE || pname == GL_SPECULAR || pname == GL_EMISSION ||
            pname == GL_AMBIENT_AND_DIFFUSE) return 4;
        return pname == GL_COLOR_INDEXES ? 3 : 1;
    case GLP_GROUP_FOG:
        return pname == GL_FOG_COLOR ? 4 : 1;
    case GLP_GROUP_TEXPARAM:
        return pname == GL_TEXTURE_BORDER_COLOR ? 4 : 1;
    case GLP_GROUP_TEXENV:
        return pname == GL_TEXTURE_ENV_COLOR ? 4 : 1;
    case GLP_GROUP_TEXGEN:
        return pname == GL_OBJECT_PLANE || pname == GL_EYE_PLANE ? 4 : 1;
    case GLP_GROUP_POINTPARAM:
        return pname == GL_DISTANCE_ATTENUATION_EXT ? 3 : 1;
    }
    return 1;
}

static int get_count(GLenum pname)
{
    switch (pname) {
    case GL_MODELVIEW_MATRIX: case GL_PROJECTION_MATRIX: case GL_TEXTURE_MATRIX: case GL_COLOR_MATRIX_SGI:
        return 16;
    case GL_COLOR_CLEAR_VALUE: case GL_CURRENT_COLOR: case GL_CURRENT_RASTER_COLOR:
    case GL_CURRENT_RASTER_POSITION: case GL_CURRENT_RASTER_TEXTURE_COORDS: case GL_CURRENT_TEXTURE_COORDS:
    case GL_ACCUM_CLEAR_VALUE: case GL_COLOR_WRITEMASK: case GL_FOG_COLOR: case GL_LIGHT_MODEL_AMBIENT:
    case GL_MAP2_GRID_DOMAIN: case GL_SCISSOR_BOX: case GL_VIEWPORT: case GL_BLEND_COLOR_EXT:
        return 4;
    case GL_CURRENT_NORMAL:
        return 3;
    case GL_DEPTH_RANGE: case GL_LINE_WIDTH_RANGE: case GL_POINT_SIZE_RANGE: case GL_MAP1_GRID_DOMAIN:
    case GL_MAP2_GRID_SEGMENTS: case GL_MAX_VIEWPORT_DIMS: case GL_POLYGON_MODE:
        return 2;
    default:
        return 1;
    }
}

/* ---- Pixel rectangles ------------------------------------------------------------- */

static int format_components(GLenum format)
{
    switch (format) {
    case GL_RGB: case GL_BGR: return 3;
    case GL_RGBA: case GL_BGRA: case GL_ABGR_EXT: case GL_CMYK_EXT: return 4;
    case GL_CMYKA_EXT: return 5;
    case GL_LUMINANCE_ALPHA: case GL_YCRCB_422_SGIX: return 2;
    default: return 1; /* COLOR_INDEX, STENCIL_INDEX, DEPTH_COMPONENT, RED, ..., LUMINANCE */
    }
}

/* Bytes per component, or for packed types (negative) bytes per pixel. */
static int type_size(GLenum type)
{
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: return 1;
    case GL_UNSIGNED_SHORT: case GL_SHORT: return 2;
    case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: return 4;
    case GL_DOUBLE: return 8;
    case GL_UNSIGNED_BYTE_3_3_2: case GL_UNSIGNED_BYTE_2_3_3_REV: return -1;
    case GL_UNSIGNED_SHORT_4_4_4_4_EXT: case GL_UNSIGNED_SHORT_5_5_5_1_EXT: case GL_UNSIGNED_SHORT_5_6_5:
        return -2;
    case GL_UNSIGNED_INT_8_8_8_8: case GL_UNSIGNED_INT_10_10_10_2_EXT: case GL_UNSIGNED_INT_8_8_8_8_REV:
        return -4;
    default:
        /* the other GL 1.2 packed types: 0x8364-0x8366 are shorts, 0x8368 an int */
        if (type >= 0x8364 && type <= 0x8366) return -2;
        if (type == 0x8368) return -4;
        return 1;
    }
}

/* Bytes a pixel transfer of width x height touches from its base pointer,
   following the pixel store state (OpenGL 1.1, section 3.6.4). */
static unsigned int image_bytes(const struct glp_pixelstore *ps, GLsizei width, GLsizei height, GLenum format,
                                GLenum type)
{
    unsigned int rowlength, align = (unsigned int)(ps->alignment > 0 ? ps->alignment : 1);
    unsigned int stride, group, elem;
    int ts;
    if (width <= 0 || height <= 0) return 0;
    rowlength = (unsigned int)(ps->rowlength > 0 ? ps->rowlength : width);
    if (type == GL_BITMAP) {
        stride = align * ((rowlength + 8 * align - 1) / (8 * align));
        return (unsigned int)ps->skiprows * stride + (unsigned int)(height - 1) * stride +
               ((unsigned int)ps->skippixels + (unsigned int)width + 7) / 8;
    }
    ts = type_size(type);
    if (ts < 0) {
        group = (unsigned int)-ts;
        elem = group;
    } else {
        elem = (unsigned int)ts;
        group = (unsigned int)format_components(format) * elem;
    }
    stride = rowlength * group;
    if (elem < align) stride = align * ((stride + align - 1) / align);
    return (unsigned int)ps->skiprows * stride + (unsigned int)ps->skippixels * group +
           (unsigned int)(height - 1) * stride + (unsigned int)width * group;
}

/* The same for width x height x depth: depth images, each image_height rows
   apart (the image height, or the height when 0), after skip_images. */
static unsigned int image_bytes_3d(const struct glp_pixelstore *ps, GLsizei width, GLsizei height, GLsizei depth,
                                   GLenum format, GLenum type)
{
    struct glp_pixelstore plain = *ps;
    unsigned int rows, stride;
    if (depth <= 0 || width <= 0 || height <= 0) return 0;
    rows = (unsigned int)(ps->imageheight > 0 ? ps->imageheight : height);
    plain.skiprows = 0;
    plain.skippixels = 0;
    /* One row's stride, alignment included: two rows minus one. */
    stride = image_bytes(&plain, width, 2, format, type) - image_bytes(&plain, width, 1, format, type);
    return ((unsigned int)ps->skipimages + (unsigned int)depth - 1) * rows * stride +
           image_bytes(ps, width, height, format, type);
}

/* Appends a pixel transfer: the argument words, a has-data flag, the byte
   count and the bytes the host will read from the base pointer. */
static void put_image(unsigned int fn, const unsigned int *args, unsigned int nargs, const void *pixels,
                      unsigned int bytes)
{
    unsigned int *p;
    if (!pixels) bytes = 0;
    p = glp_put(fn, nargs + 2 + words_for(bytes));
    if (nargs) memcpy(p, args, nargs * 4);
    p[nargs] = pixels != 0;
    p[nargs + 1] = bytes;
    if (bytes) {
        p[nargs + 1 + words_for(bytes)] = 0; /* padding */
        memcpy(p + nargs + 2, pixels, bytes);
    }
}

void glp_impl_glPixelStorei(GLenum pname, GLint param)
{
    struct glp_client *c = &ctx()->client;
    unsigned int *p;
    TRACE(glPixelStorei);
    switch (pname) {
    case GL_PACK_SWAP_BYTES: c->pack.swap = param != 0; break;
    case GL_PACK_LSB_FIRST: c->pack.lsbfirst = param != 0; break;
    case GL_PACK_ROW_LENGTH: c->pack.rowlength = param; break;
    case GL_PACK_SKIP_ROWS: c->pack.skiprows = param; break;
    case GL_PACK_SKIP_PIXELS: c->pack.skippixels = param; break;
    case GL_PACK_ALIGNMENT: c->pack.alignment = param; break;
    case GL_UNPACK_SWAP_BYTES: c->unpack.swap = param != 0; break;
    case GL_UNPACK_LSB_FIRST: c->unpack.lsbfirst = param != 0; break;
    case GL_UNPACK_ROW_LENGTH: c->unpack.rowlength = param; break;
    case GL_UNPACK_SKIP_ROWS: c->unpack.skiprows = param; break;
    case GL_UNPACK_SKIP_PIXELS: c->unpack.skippixels = param; break;
    case GL_UNPACK_ALIGNMENT: c->unpack.alignment = param; break;
    case GLP_PACK_SKIP_IMAGES: c->pack.skipimages = param; break;
    case GLP_PACK_IMAGE_HEIGHT: c->pack.imageheight = param; break;
    case GLP_UNPACK_SKIP_IMAGES: c->unpack.skipimages = param; break;
    case GLP_UNPACK_IMAGE_HEIGHT: c->unpack.imageheight = param; break;
    default: break;
    }
    p = glp_put(GLP_FN_glPixelStorei, 2);
    p[0] = pname;
    p[1] = (unsigned int)param;
}

void glp_impl_glPixelStoref(GLenum pname, GLfloat param) { glPixelStorei(pname, (GLint)(param + (param < 0 ? -0.5f : 0.5f))); }

void glp_impl_glTexImage1D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLint border, GLenum format,
                  GLenum type, const GLvoid *pixels)
{
    unsigned int a[7];
    TRACE(glTexImage1D);
    a[0] = target; a[1] = (unsigned int)level; a[2] = (unsigned int)internalformat; a[3] = (unsigned int)width;
    a[4] = (unsigned int)border; a[5] = format; a[6] = type;
    put_image(GLP_FN_glTexImage1D, a, 7, pixels, image_bytes(&ctx()->client.unpack, width, 1, format, type));
}

void glp_impl_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
                  GLenum format, GLenum type, const GLvoid *pixels)
{
    unsigned int a[8];
    TRACE(glTexImage2D);
    a[0] = target; a[1] = (unsigned int)level; a[2] = (unsigned int)internalformat; a[3] = (unsigned int)width;
    a[4] = (unsigned int)height; a[5] = (unsigned int)border; a[6] = format; a[7] = type;
    put_image(GLP_FN_glTexImage2D, a, 8, pixels, image_bytes(&ctx()->client.unpack, width, height, format, type));
}

/* 3D textures (GL 1.2 / EXT_texture3D): the host's core glTexImage3D. The
   signatures are the IRIX headers' (internalformat a GLenum). */
void glp_impl_glTexImage3D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth,
                  GLint border, GLenum format, GLenum type, const GLvoid *pixels)
{
    unsigned int a[9];
    TRACE(glTexImage3D);
    a[0] = target; a[1] = (unsigned int)level; a[2] = (unsigned int)internalformat; a[3] = (unsigned int)width;
    a[4] = (unsigned int)height; a[5] = (unsigned int)depth; a[6] = (unsigned int)border; a[7] = format;
    a[8] = type;
    put_image(GLP_FN_glTexImage3D, a, 9, pixels,
              image_bytes_3d(&ctx()->client.unpack, width, height, depth, format, type));
}

void glp_impl_glTexImage3DEXT(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height,
                     GLsizei depth, GLint border, GLenum format, GLenum type, const GLvoid *pixels)
{
    glTexImage3D(target, level, internalformat, width, height, depth, border, format, type, pixels);
}

void glp_impl_glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width,
                     GLsizei height, GLsizei depth, GLenum format, GLenum type, const GLvoid *pixels)
{
    unsigned int a[10];
    TRACE(glTexSubImage3D);
    a[0] = target; a[1] = (unsigned int)level; a[2] = (unsigned int)xoffset; a[3] = (unsigned int)yoffset;
    a[4] = (unsigned int)zoffset; a[5] = (unsigned int)width; a[6] = (unsigned int)height;
    a[7] = (unsigned int)depth; a[8] = format; a[9] = type;
    put_image(GLP_FN_glTexSubImage3D, a, 10, pixels,
              image_bytes_3d(&ctx()->client.unpack, width, height, depth, format, type));
}

void glp_impl_glTexSubImage3DEXT(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width,
                        GLsizei height, GLsizei depth, GLenum format, GLenum type, const GLvoid *pixels)
{
    glTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, pixels);
}

void glp_impl_glTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type,
                     const GLvoid *pixels)
{
    unsigned int a[6];
    TRACE(glTexSubImage1D);
    a[0] = target; a[1] = (unsigned int)level; a[2] = (unsigned int)xoffset; a[3] = (unsigned int)width;
    a[4] = format; a[5] = type;
    put_image(GLP_FN_glTexSubImage1D, a, 6, pixels, image_bytes(&ctx()->client.unpack, width, 1, format, type));
}

void glp_impl_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                     GLenum format, GLenum type, const GLvoid *pixels)
{
    unsigned int a[8];
    TRACE(glTexSubImage2D);
    a[0] = target; a[1] = (unsigned int)level; a[2] = (unsigned int)xoffset; a[3] = (unsigned int)yoffset;
    a[4] = (unsigned int)width; a[5] = (unsigned int)height; a[6] = format; a[7] = type;
    put_image(GLP_FN_glTexSubImage2D, a, 8, pixels, image_bytes(&ctx()->client.unpack, width, height, format, type));
}

void glp_impl_glTexSubImage1DEXT(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type,
                        const GLvoid *pixels)
{
    glTexSubImage1D(target, level, xoffset, width, format, type, pixels);
}

void glp_impl_glTexSubImage2DEXT(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                        GLenum format, GLenum type, const GLvoid *pixels)
{
    glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
}

void glp_impl_glDrawPixels(GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels)
{
    unsigned int a[4];
    TRACE(glDrawPixels);
    a[0] = (unsigned int)width; a[1] = (unsigned int)height; a[2] = format; a[3] = type;
    put_image(GLP_FN_glDrawPixels, a, 4, pixels, image_bytes(&ctx()->client.unpack, width, height, format, type));
}

void glp_impl_glBitmap(GLsizei width, GLsizei height, GLfloat xorig, GLfloat yorig, GLfloat xmove, GLfloat ymove,
              const GLubyte *bitmap)
{
    unsigned int a[6];
    TRACE(glBitmap);
    a[0] = (unsigned int)width; a[1] = (unsigned int)height; a[2] = glp_fbits(xorig); a[3] = glp_fbits(yorig);
    a[4] = glp_fbits(xmove); a[5] = glp_fbits(ymove);
    put_image(GLP_FN_glBitmap, a, 6, bitmap,
              image_bytes(&ctx()->client.unpack, width, height, GL_COLOR_INDEX, GL_BITMAP));
}

void glp_impl_glPolygonStipple(const GLubyte *mask)
{
    TRACE(glPolygonStipple);
    put_image(GLP_FN_glPolygonStipple, 0, 0, mask,
              image_bytes(&ctx()->client.unpack, 32, 32, GL_COLOR_INDEX, GL_BITMAP));
}

/* ---- Lists, textures, evaluators ------------------------------------------------- */

void glp_impl_glListBase(GLuint base)
{
    GLXContext c = ctx();
    TRACE(glListBase);
    glp_put(GLP_FN_glListBase, 1)[0] = base;
    if (!(c->ix.list && c->ix.list_mode == GL_COMPILE)) c->list_base = base; /* else compiled, not run */
}

static void index_before_lists(GLXContext c);
static void index_after_lists(GLXContext c, const GLuint *ids, GLsizei n);
static void index_update(GLXContext c);
static int list_expand(GLXContext c, const GLuint *ids, GLsizei n);
static void index_compile_calls(GLXContext c, const GLuint *ids, GLsizei n);
static int passes_begin(GLXContext c);
static void passes_end(GLXContext c);
static void passes_finish(GLXContext c, int wrapped);

/* Name `i` of a glCallLists array. */
static unsigned int list_name(GLenum type, const GLvoid *lists, GLsizei i)
{
    const unsigned char *b = (const unsigned char *)lists;
    int v;
    switch (type) {
    case GL_BYTE: v = ((const GLbyte *)lists)[i]; break;
    case GL_UNSIGNED_BYTE: v = b[i]; break;
    case GL_SHORT: v = ((const GLshort *)lists)[i]; break;
    case GL_UNSIGNED_SHORT: v = ((const GLushort *)lists)[i]; break;
    case GL_INT: case GL_UNSIGNED_INT: v = ((const GLint *)lists)[i]; break;
    case GL_FLOAT: v = (int)((const GLfloat *)lists)[i]; break;
    case GL_2_BYTES: v = b[2 * i] << 8 | b[2 * i + 1]; break;
    case GL_3_BYTES: v = b[3 * i] << 16 | b[3 * i + 1] << 8 | b[3 * i + 2]; break;
    case GL_4_BYTES: v = b[4 * i] << 24 | b[4 * i + 1] << 16 | b[4 * i + 2] << 8 | b[4 * i + 3]; break;
    default: v = 0; break;
    }
    return (unsigned int)v;
}

void glp_impl_glCallLists(GLsizei n, GLenum type, const GLvoid *lists)
{
    GLXContext c = current_context();
    const int tracked = c && c->index_mode && !c->ix.list;
    GLuint local[256], *ids = local;
    unsigned int *p;
    GLsizei i;
    TRACE(glCallLists);
    if (n <= 0 || !lists) return;
    if (tracked) {
        if (n > 256) ids = (GLuint *)malloc((size_t)n * sizeof *ids);
        if (ids) {
            for (i = 0; i < n; ++i) ids[i] = c->list_base + list_name(type, lists, i);
            if (list_expand(c, ids, n)) {
                if (ids != local) free(ids);
                return;
            }
        }
    }
    if (c && c->index_mode) index_before_lists(c);
    /* Sent as ints, so the host needs no byte order handling. */
    p = glp_put(GLP_FN_glCallLists, 1 + (unsigned int)n);
    p[0] = (unsigned int)n;
    for (i = 0; i < n; ++i) p[1 + i] = list_name(type, lists, i);
    if (c && c->index_mode && c->ix.list) {
        for (i = 0; i < n; ++i) {
            GLuint id = c->list_base + list_name(type, lists, i);
            index_compile_calls(c, &id, 1);
        }
    }
    if (tracked) {
        if (ids) {
            index_after_lists(c, ids, n);
        } else {
            for (i = 0; i < n; ++i) {
                GLuint id = c->list_base + list_name(type, lists, i);
                index_after_lists(c, &id, 1);
            }
        }
        index_update(c);
    }
    if (ids && ids != local) free(ids);
}

void glp_impl_glDeleteTextures(GLsizei n, const GLuint *textures)
{
    unsigned int *p;
    TRACE(glDeleteTextures);
    if (n <= 0 || !textures) return;
    p = glp_put(GLP_FN_glDeleteTextures, 1 + (unsigned int)n);
    p[0] = (unsigned int)n;
    memcpy(p + 1, textures, (size_t)n * 4);
}

void glp_impl_glDeleteTexturesEXT(GLsizei n, const GLuint *textures) { glDeleteTextures(n, textures); }

void glp_impl_glPrioritizeTextures(GLsizei n, const GLuint *textures, const GLclampf *priorities)
{
    unsigned int *p;
    TRACE(glPrioritizeTextures);
    if (n <= 0 || !textures || !priorities) return;
    p = glp_put(GLP_FN_glPrioritizeTextures, 1 + 2 * (unsigned int)n);
    p[0] = (unsigned int)n;
    memcpy(p + 1, textures, (size_t)n * 4);
    memcpy(p + 1 + n, priorities, (size_t)n * 4);
}

void glp_impl_glPrioritizeTexturesEXT(GLsizei n, const GLuint *textures, const GLclampf *priorities)
{
    glPrioritizeTextures(n, textures, priorities);
}

void glp_impl_glPolygonOffsetEXT(GLfloat factor, GLfloat bias)
{
    /* The EXT bias is in depth-range units; the core command counts depth
       steps (the host depth buffers have 24 bits). */
    glPolygonOffset(factor, bias * 16777215.0f);
}

static int map_components(GLenum target)
{
    switch (target) {
    case GL_MAP1_VERTEX_3: case GL_MAP2_VERTEX_3: case GL_MAP1_NORMAL: case GL_MAP2_NORMAL:
    case GL_MAP1_TEXTURE_COORD_3: case GL_MAP2_TEXTURE_COORD_3:
        return 3;
    case GL_MAP1_VERTEX_4: case GL_MAP2_VERTEX_4: case GL_MAP1_COLOR_4: case GL_MAP2_COLOR_4:
    case GL_MAP1_TEXTURE_COORD_4: case GL_MAP2_TEXTURE_COORD_4:
        return 4;
    case GL_MAP1_TEXTURE_COORD_2: case GL_MAP2_TEXTURE_COORD_2:
        return 2;
    default:
        return 1; /* INDEX, TEXTURE_COORD_1 */
    }
}

/* Record: target, u1, u2, v1, v2 (doubles), uorder, vorder, then the control
   points packed (k values each, u varying fastest; doubles for glMap*d). */
static void put_map(unsigned int fn, GLenum target, int isdouble, double u1, double u2, double v1, double v2,
                    GLint ustride, GLint uorder, GLint vstride, GLint vorder, const void *points)
{
    int k = map_components(target), i, j, c;
    unsigned int words, *p, at;
    if (uorder <= 0 || vorder <= 0 || !points) return;
    words = (unsigned int)(k * uorder * vorder) * (isdouble ? 2u : 1u);
    p = glp_put(fn, 11 + words);
    p[0] = target;
    glp_dbits(&p[1], u1);
    glp_dbits(&p[3], u2);
    glp_dbits(&p[5], v1);
    glp_dbits(&p[7], v2);
    p[9] = (unsigned int)uorder;
    p[10] = (unsigned int)vorder;
    at = 11;
    for (j = 0; j < vorder; ++j) {
        for (i = 0; i < uorder; ++i) {
            for (c = 0; c < k; ++c) {
                long idx = (long)i * ustride + (long)j * vstride + c;
                if (isdouble) {
                    glp_dbits(&p[at], ((const GLdouble *)points)[idx]);
                    at += 2;
                } else {
                    p[at++] = glp_fbits(((const GLfloat *)points)[idx]);
                }
            }
        }
    }
}

void glp_impl_glMap1f(GLenum target, GLfloat u1, GLfloat u2, GLint stride, GLint order, const GLfloat *points)
{
    TRACE(glMap1f);
    put_map(GLP_FN_glMap1f, target, 0, u1, u2, 0, 1, stride, order, 0, 1, points);
}

void glp_impl_glMap1d(GLenum target, GLdouble u1, GLdouble u2, GLint stride, GLint order, const GLdouble *points)
{
    TRACE(glMap1d);
    put_map(GLP_FN_glMap1d, target, 1, u1, u2, 0, 1, stride, order, 0, 1, points);
}

void glp_impl_glMap2f(GLenum target, GLfloat u1, GLfloat u2, GLint ustride, GLint uorder, GLfloat v1, GLfloat v2,
             GLint vstride, GLint vorder, const GLfloat *points)
{
    TRACE(glMap2f);
    put_map(GLP_FN_glMap2f, target, 0, u1, u2, v1, v2, ustride, uorder, vstride, vorder, points);
}

void glp_impl_glMap2d(GLenum target, GLdouble u1, GLdouble u2, GLint ustride, GLint uorder, GLdouble v1, GLdouble v2,
             GLint vstride, GLint vorder, const GLdouble *points)
{
    TRACE(glMap2d);
    put_map(GLP_FN_glMap2d, target, 1, u1, u2, v1, v2, ustride, uorder, vstride, vorder, points);
}

/* Record: map, size, one word per value (ushorts widened). */
static void pixel_map(unsigned int fn, GLenum map, GLint mapsize, const void *values, int shorts)
{
    unsigned int *p;
    GLint i;
    if (mapsize <= 0 || !values) return;
    p = glp_put(fn, 2 + (unsigned int)mapsize);
    p[0] = map;
    p[1] = (unsigned int)mapsize;
    for (i = 0; i < mapsize; ++i) {
        p[2 + i] = shorts ? ((const GLushort *)values)[i] : ((const unsigned int *)values)[i];
    }
}

void glp_impl_glPixelMapfv(GLenum map, GLint mapsize, const GLfloat *values) { TRACE(glPixelMapfv); pixel_map(GLP_FN_glPixelMapfv, map, mapsize, values, 0); }
void glp_impl_glPixelMapuiv(GLenum map, GLint mapsize, const GLuint *values) { TRACE(glPixelMapuiv); pixel_map(GLP_FN_glPixelMapuiv, map, mapsize, values, 0); }
void glp_impl_glPixelMapusv(GLenum map, GLint mapsize, const GLushort *values) { TRACE(glPixelMapusv); pixel_map(GLP_FN_glPixelMapusv, map, mapsize, values, 1); }

/* ---- Client vertex arrays ------------------------------------------------------------ */

/* The array of the client active texture unit's coordinates. */
static int texcoord_array(const struct glp_client *cl)
{
    return cl->unit == 0 ? ARR_TEXCOORD : ARR_TEXCOORD1 + cl->unit - 1;
}

static int array_index(GLenum array)
{
    switch (array) {
    case GL_VERTEX_ARRAY: return ARR_VERTEX;
    case GL_NORMAL_ARRAY: return ARR_NORMAL;
    case GL_COLOR_ARRAY: return ARR_COLOR;
    case GL_INDEX_ARRAY: return ARR_INDEX;
    case GL_TEXTURE_COORD_ARRAY: return texcoord_array(&ctx()->client);
    case GL_EDGE_FLAG_ARRAY: return ARR_EDGEFLAG;
    default: return -1;
    }
}

static void set_array(int which, int size, GLenum type, GLsizei stride, const void *pointer)
{
    struct glp_array *a = &ctx()->client.arrays[which];
    a->size = size;
    a->type = type;
    a->stride = stride;
    a->pointer = pointer;
}

void glp_impl_glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) { TRACE(glVertexPointer); set_array(ARR_VERTEX, size, type, stride, pointer); }
void glp_impl_glNormalPointer(GLenum type, GLsizei stride, const GLvoid *pointer) { TRACE(glNormalPointer); set_array(ARR_NORMAL, 3, type, stride, pointer); }
void glp_impl_glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) { TRACE(glColorPointer); set_array(ARR_COLOR, size, type, stride, pointer); }
void glp_impl_glIndexPointer(GLenum type, GLsizei stride, const GLvoid *pointer) { TRACE(glIndexPointer); set_array(ARR_INDEX, 1, type, stride, pointer); }
void glp_impl_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) { TRACE(glTexCoordPointer); set_array(texcoord_array(&ctx()->client), size, type, stride, pointer); }
void glp_impl_glEdgeFlagPointer(GLsizei stride, const GLboolean *pointer) { TRACE(glEdgeFlagPointer); set_array(ARR_EDGEFLAG, 1, GL_UNSIGNED_BYTE, stride, pointer); }

void glp_impl_glVertexPointerEXT(GLint size, GLenum type, GLsizei stride, GLsizei count, const GLvoid *pointer) { (void)count; glVertexPointer(size, type, stride, pointer); }
void glp_impl_glNormalPointerEXT(GLenum type, GLsizei stride, GLsizei count, const GLvoid *pointer) { (void)count; glNormalPointer(type, stride, pointer); }
void glp_impl_glColorPointerEXT(GLint size, GLenum type, GLsizei stride, GLsizei count, const GLvoid *pointer) { (void)count; glColorPointer(size, type, stride, pointer); }
void glp_impl_glIndexPointerEXT(GLenum type, GLsizei stride, GLsizei count, const GLvoid *pointer) { (void)count; glIndexPointer(type, stride, pointer); }
void glp_impl_glTexCoordPointerEXT(GLint size, GLenum type, GLsizei stride, GLsizei count, const GLvoid *pointer) { (void)count; glTexCoordPointer(size, type, stride, pointer); }
void glp_impl_glEdgeFlagPointerEXT(GLsizei stride, GLsizei count, const GLboolean *pointer) { (void)count; glEdgeFlagPointer(stride, pointer); }

/* Sent too: the host keeps it for the program's own glGet. */
static void client_active_texture(unsigned int fn, GLenum texture)
{
    const int unit = (int)texture - GLP_TEXTURE0;
    if (unit >= 0 && unit < TEXTURE_UNITS) ctx()->client.unit = unit;
    glp_put(fn, 1)[0] = texture;
}

void glp_impl_glClientActiveTexture(GLenum texture) { TRACE(glClientActiveTexture); client_active_texture(GLP_FN_glClientActiveTexture, texture); }
void glp_impl_glClientActiveTextureARB(GLenum texture) { TRACE(glClientActiveTextureARB); client_active_texture(GLP_FN_glClientActiveTextureARB, texture); }

void glp_impl_glEnableClientState(GLenum array)
{
    int i = array_index(array);
    TRACE(glEnableClientState);
    if (i >= 0) ctx()->client.arrays[i].enabled = 1;
}

void glp_impl_glDisableClientState(GLenum array)
{
    int i = array_index(array);
    TRACE(glDisableClientState);
    if (i >= 0) ctx()->client.arrays[i].enabled = 0;
}

void glp_impl_glInterleavedArrays(GLenum format, GLsizei stride, const GLvoid *pointer)
{
    /* OpenGL 1.1 table 2.5: sizes, then offsets and stride in floats. */
    static const struct {
        GLenum format;
        signed char st, sc, sv, ubcolor, pc, pn, pv, s;
    } table[] = {
        {GL_V2F, 0, 0, 2, 0, 0, -1, 0, 2},
        {GL_V3F, 0, 0, 3, 0, 0, -1, 0, 3},
        {GL_C4UB_V2F, 0, 4, 2, 1, 0, -1, 1, 3},
        {GL_C4UB_V3F, 0, 4, 3, 1, 0, -1, 1, 4},
        {GL_C3F_V3F, 0, 3, 3, 0, 0, -1, 3, 6},
        {GL_N3F_V3F, 0, 0, 3, 0, 0, 0, 3, 6},
        {GL_C4F_N3F_V3F, 0, 4, 3, 0, 0, 4, 7, 10},
        {GL_T2F_V3F, 2, 0, 3, 0, 0, -1, 2, 5},
        {GL_T4F_V4F, 4, 0, 4, 0, 0, -1, 4, 8},
        {GL_T2F_C4UB_V3F, 2, 4, 3, 1, 2, -1, 3, 6},
        {GL_T2F_C3F_V3F, 2, 3, 3, 0, 2, -1, 5, 8},
        {GL_T2F_N3F_V3F, 2, 0, 3, 0, 0, 2, 5, 8},
        {GL_T2F_C4F_N3F_V3F, 2, 4, 3, 0, 2, 6, 9, 12},
        {GL_T4F_C4F_N3F_V4F, 4, 4, 4, 0, 4, 8, 11, 15},
    };
    const char *base = (const char *)pointer;
    struct glp_array *arr = ctx()->client.arrays;
    unsigned int i;
    TRACE(glInterleavedArrays);
    for (i = 0; i < sizeof table / sizeof table[0]; ++i) {
        if (table[i].format != format) continue;
        if (stride == 0) stride = table[i].s * 4;
        arr[ARR_EDGEFLAG].enabled = 0;
        arr[ARR_INDEX].enabled = 0;
        arr[texcoord_array(&ctx()->client)].enabled = table[i].st != 0; /* the client active unit's */
        if (table[i].st) set_array(texcoord_array(&ctx()->client), table[i].st, GL_FLOAT, stride, base);
        arr[ARR_COLOR].enabled = table[i].sc != 0;
        if (table[i].sc) {
            set_array(ARR_COLOR, table[i].sc, table[i].ubcolor ? GL_UNSIGNED_BYTE : GL_FLOAT, stride,
                      base + table[i].pc * 4);
        }
        arr[ARR_NORMAL].enabled = table[i].pn >= 0;
        if (table[i].pn >= 0) set_array(ARR_NORMAL, 3, GL_FLOAT, stride, base + table[i].pn * 4);
        arr[ARR_VERTEX].enabled = 1;
        set_array(ARR_VERTEX, table[i].sv, GL_FLOAT, stride, base + table[i].pv * 4);
        return;
    }
}

static int array_elem_bytes(const struct glp_array *a)
{
    int ts = type_size(a->type);
    return a->size * (ts < 0 ? -ts : ts);
}

static unsigned int index_value(GLXContext c, unsigned int src);
static void index_event(GLXContext c, const unsigned int *p, unsigned int kind, unsigned int src);
static unsigned int packed_index(unsigned int i);

/* How array `i` is sent: 0 not, 1 as it is, 2 an index array as colours.
   In colour index mode GL draws with the index array and ignores the colour
   array; the host draws in RGBA, which ignores index arrays. */
static int array_sent(GLXContext c, int i)
{
    const struct glp_array *a = &c->client.arrays[i];
    if (!a->enabled || !a->pointer) return 0;
    if (c->index_mode && i == ARR_COLOR) return 0;
    if (c->index_mode && i == ARR_INDEX) return 2;
    return 1;
}

/* Element `n` of an index array. */
static unsigned int index_array_element(const struct glp_array *a, GLint n)
{
    const int eb = array_elem_bytes(a);
    const char *q = (const char *)a->pointer + (long)n * (a->stride ? a->stride : eb);
    long v;
    switch (a->type) {
    case GL_UNSIGNED_BYTE: v = *(const GLubyte *)q; break;
    case GL_SHORT: { GLshort x; memcpy(&x, q, sizeof x); v = x; break; }
    case GL_INT: { GLint x; memcpy(&x, q, sizeof x); v = x; break; }
    case GL_FLOAT: { GLfloat x; memcpy(&x, q, sizeof x); v = (long)x; break; }
    case GL_DOUBLE: { GLdouble x; memcpy(&x, q, sizeof x); v = (long)x; break; }
    default: v = 0; break;
    }
    return (unsigned int)v % GLPASS_PALETTE_SIZE;
}

/* Record GLP_FN_CLIENT_DRAW:
     kind (GLP_DRAW_*), mode, count, narrays,
     per array: which (0 vertex, 1 normal, 2 colour, 3 index, 4 texcoord,
       5 edge flag), size, type, bytes, then elements first..first+count-1
       packed (big-endian, as in guest memory),
     GLP_DRAW_ELEMENTS: nindices, then indices relative to `first`. */
static void client_draw_once(unsigned int kind, GLenum mode, GLint first, GLsizei count,
                             const unsigned int *indices, GLsizei nindices)
{
    GLXContext c = ctx();
    struct glp_array *arr = c->client.arrays;
    unsigned int words = 4, *p, at;
    int i, narrays = 0;
    GLsizei e;
    if (count <= 0 || (!arr[ARR_VERTEX].enabled && kind != GLP_DRAW_ELEMENT)) return;
    for (i = 0; i < ARR_COUNT; ++i) {
        const int how = array_sent(c, i);
        if (how) {
            ++narrays;
            words += 4 + (how == 2 ? (unsigned int)count
                                   : words_for((unsigned int)(array_elem_bytes(&arr[i]) * count)));
        }
    }
    if (kind == GLP_DRAW_ELEMENTS) words += 1 + (unsigned int)nindices;
    p = glp_put(GLP_FN_CLIENT_DRAW, words);
    p[0] = kind;
    p[1] = mode;
    p[2] = (unsigned int)count;
    p[3] = (unsigned int)narrays;
    at = 4;
    for (i = 0; i < ARR_COUNT; ++i) {
        const struct glp_array *a = &arr[i];
        int eb, stride;
        unsigned int bytes;
        unsigned char *dst;
        const int how = array_sent(c, i);
        if (!how) continue;
        if (how == 2) { /* RGBA bytes, one word per vertex: the colour for its index now */
            p[at] = ARR_COLOR;
            p[at + 1] = 4;
            p[at + 2] = GL_UNSIGNED_BYTE;
            p[at + 3] = (unsigned int)count * 4;
            for (e = 0; e < count; ++e) {
                const unsigned int src = index_array_element(a, first + e);
                p[at + 4 + e] = packed_index(index_value(c, src));
                index_event(c, &p[at + 4 + e], EV_PACKED, src);
            }
            at += 4 + (unsigned int)count;
            continue;
        }
        eb = array_elem_bytes(a);
        stride = a->stride ? a->stride : eb;
        bytes = (unsigned int)(eb * count);
        p[at] = (unsigned int)i;
        p[at + 1] = (unsigned int)a->size;
        p[at + 2] = a->type;
        p[at + 3] = bytes;
        p[at + 3 + words_for(bytes)] = 0; /* padding */
        dst = (unsigned char *)&p[at + 4];
        for (e = 0; e < count; ++e) {
            memcpy(dst + (long)e * eb, (const char *)a->pointer + (long)(first + e) * stride, (size_t)eb);
        }
        at += 4 + words_for(bytes);
    }
    if (kind == GLP_DRAW_ELEMENTS) {
        p[at] = (unsigned int)nindices;
        memcpy(&p[at + 1], indices, (size_t)nindices * 4);
    }
}

/* A vertex array draw, sent once per colour index pass (see index_passes). */
static void client_draw(unsigned int kind, GLenum mode, GLint first, GLsizei count, const unsigned int *indices,
                        GLsizei nindices)
{
    GLXContext c = ctx();
    const int recorded = kind != GLP_DRAW_ELEMENT && passes_begin(c);
    client_draw_once(kind, mode, first, count, indices, nindices);
    if (recorded) passes_end(c);
}

void glp_impl_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    TRACE(glDrawArrays);
    client_draw(GLP_DRAW_ARRAYS, mode, first, count, 0, 0);
}

void glp_impl_glDrawArraysEXT(GLenum mode, GLint first, GLsizei count) { glDrawArrays(mode, first, count); }

/* glBegin ... glArrayElement* ... glEnd (Quake III draws so) becomes one
   indexed draw: the glBegin is held back and the elements collected; any
   other command in between sends the glBegin and the elements as they were. */
static void begin_materialize(struct glp_thread *t)
{
    unsigned int i, count = t->begin_count;
    t->begin_pending = 0;
    glp_put(GLP_FN_glBegin, 1)[0] = t->begin_mode;
    t->begin_count = 0;
    for (i = 0; i < count; ++i) client_draw(GLP_DRAW_ELEMENT, 0, (GLint)t->begin_elements[i], 1, 0, 0);
}

void glp_impl_glBegin(GLenum mode)
{
    struct glp_thread *t = glp_self();
    GLXContext c = ctx();
    TRACE(glBegin);
    if (t->begin_pending) begin_materialize(t);
    c->ix.in_begin = 1; /* the index state's host setting cannot change until glEnd */
    passes_begin(c);    /* the block is sent again for further colour index passes */
    t->begin_mode = mode;
    t->begin_count = 0;
    t->begin_pending = 1;
}

static void end_block(void)
{
    struct glp_thread *t = glp_self();
    struct glp_array *arr = ctx()->client.arrays;
    unsigned int i, lo = 0xFFFFFFFFu, hi = 0;
    if (!t->begin_pending) {
        glp_put(GLP_FN_glEnd, 0);
        return;
    }
    if (t->begin_count == 0) { /* glBegin; glEnd: nothing drawn */
        t->begin_pending = 0;
        return;
    }
    if (!arr[ARR_VERTEX].enabled || !arr[ARR_VERTEX].pointer) {
        begin_materialize(t);
        glp_put(GLP_FN_glEnd, 0);
        return;
    }
    t->begin_pending = 0;
    for (i = 0; i < t->begin_count; ++i) {
        if (t->begin_elements[i] < lo) lo = t->begin_elements[i];
        if (t->begin_elements[i] > hi) hi = t->begin_elements[i];
    }
    for (i = 0; i < t->begin_count; ++i) t->begin_elements[i] -= lo;
    client_draw(GLP_DRAW_ELEMENTS, t->begin_mode, (GLint)lo, (GLsizei)(hi - lo + 1), t->begin_elements,
                (GLsizei)t->begin_count);
    t->begin_count = 0;
}

void glp_impl_glEnd(void)
{
    GLXContext c = ctx();
    TRACE(glEnd);
    c->ix.in_begin = 0;
    end_block();
    passes_end(c);
}

void glp_impl_glArrayElement(GLint i)
{
    struct glp_thread *t = glp_self();
    TRACE(glArrayElement);
    if (t->begin_pending && i >= 0) {
        if (t->begin_count == t->begin_capacity) {
            unsigned int capacity = t->begin_capacity ? t->begin_capacity * 2 : 1024;
            unsigned int *grown = (unsigned int *)realloc(t->begin_elements, capacity * sizeof *grown);
            if (!grown) {
                begin_materialize(t);
                client_draw(GLP_DRAW_ELEMENT, 0, i, 1, 0, 0);
                return;
            }
            t->begin_elements = grown;
            t->begin_capacity = capacity;
        }
        t->begin_elements[t->begin_count++] = (unsigned int)i;
        return;
    }
    client_draw(GLP_DRAW_ELEMENT, 0, i, 1, 0, 0);
}
void glp_impl_glArrayElementEXT(GLint i) { glArrayElement(i); }

void glp_impl_glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
    unsigned int local[1024], *idx, lo = 0xFFFFFFFFu, hi = 0;
    GLsizei i;
    TRACE(glDrawElements);
    if (count <= 0 || !indices) return;
    idx = count <= 1024 ? local : (unsigned int *)malloc((size_t)count * 4);
    if (!idx) return;
    for (i = 0; i < count; ++i) {
        unsigned int v = type == GL_UNSIGNED_BYTE ? ((const GLubyte *)indices)[i]
                         : type == GL_UNSIGNED_SHORT ? ((const GLushort *)indices)[i]
                         : ((const GLuint *)indices)[i];
        idx[i] = v;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    /* Sends elements lo..hi; the indices are rebased onto them. */
    for (i = 0; i < count; ++i) idx[i] -= lo;
    client_draw(GLP_DRAW_ELEMENTS, mode, (GLint)lo, (GLsizei)(hi - lo + 1), idx, count);
    if (idx != local) free(idx);
}

void glp_impl_glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const GLvoid *indices)
{
    (void)start; (void)end;
    glDrawElements(mode, count, type, indices);
}

void glp_impl_glDrawRangeElementsEXT(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const GLvoid *indices)
{
    glDrawRangeElements(mode, start, end, count, type, indices);
}

void glp_impl_glPushClientAttrib(GLbitfield mask)
{
    GLXContext c = ctx();
    TRACE(glPushClientAttrib);
    if (c->client_depth < CLIENT_STACK) {
        c->client_stack[c->client_depth] = c->client;
        c->client_masks[c->client_depth] = mask;
    }
    ++c->client_depth;
    glp_put(GLP_FN_glPushClientAttrib, 1)[0] = mask; /* the host keeps its pixel store state in step */
}

void glp_impl_glPopClientAttrib(void)
{
    GLXContext c = ctx();
    TRACE(glPopClientAttrib);
    if (c->client_depth <= 0) return;
    --c->client_depth;
    if (c->client_depth < CLIENT_STACK) {
        const struct glp_client *saved = &c->client_stack[c->client_depth];
        GLbitfield mask = c->client_masks[c->client_depth];
        if (mask & GL_CLIENT_PIXEL_STORE_BIT) {
            c->client.pack = saved->pack;
            c->client.unpack = saved->unpack;
        }
        if (mask & GL_CLIENT_VERTEX_ARRAY_BIT) memcpy(c->client.arrays, saved->arrays, sizeof c->client.arrays);
    }
    glp_put(GLP_FN_glPopClientAttrib, 0);
}

/* ---- Buffers, selection, feedback ------------------------------------------------------ */

static int draws_front(GLenum mode)
{
    return mode == GL_FRONT || mode == GL_FRONT_LEFT || mode == GL_FRONT_AND_BACK || mode == GL_LEFT;
}

void glp_impl_glDrawBuffer(GLenum mode)
{
    GLXContext current = current_context();
    TRACE(glDrawBuffer);
    if (current) glx_log("glDrawBuffer(%#x) context %u", mode, current->id);
    ctx()->draw_buffer = mode;
    glp_put(GLP_FN_glDrawBuffer, 1)[0] = mode;
}

void glp_impl_glReadBuffer(GLenum mode)
{
    GLXContext current = current_context();
    TRACE(glReadBuffer);
    if (current) glx_log("glReadBuffer(%#x) context %u", mode, current->id);
    ctx()->read_buffer = mode;
    glp_put(GLP_FN_glReadBuffer, 1)[0] = mode;
}

void glp_impl_glSelectBuffer(GLsizei size, GLuint *buffer)
{
    GLXContext c = ctx();
    TRACE(glSelectBuffer);
    c->select_buffer = buffer;
    c->select_size = size;
    glp_put(GLP_FN_glSelectBuffer, 1)[0] = (unsigned int)size;
}

void glp_impl_glFeedbackBuffer(GLsizei size, GLenum type, GLfloat *buffer)
{
    GLXContext c = ctx();
    unsigned int *p;
    TRACE(glFeedbackBuffer);
    c->feedback_buffer = buffer;
    c->feedback_size = size;
    p = glp_put(GLP_FN_glFeedbackBuffer, 2);
    p[0] = (unsigned int)size;
    p[1] = type;
}

GLint glp_impl_glRenderMode(GLenum mode)
{
    GLXContext c = ctx();
    void *reply = 0;
    unsigned int bytes = 0, arg = mode;
    long result;
    TRACE(glRenderMode);
    /* Leaving selection or feedback: the host copies its buffer into the program's. */
    if (c->render_mode == GL_SELECT && c->select_buffer && c->select_size > 0) {
        reply = c->select_buffer;
        bytes = (unsigned int)c->select_size * 4;
    } else if (c->render_mode == GL_FEEDBACK && c->feedback_buffer && c->feedback_size > 0) {
        reply = c->feedback_buffer;
        bytes = (unsigned int)c->feedback_size * 4;
    }
    result = glp_query(GLP_FN_glRenderMode, reply, bytes, &arg, 1);
    if (mode == GL_RENDER || mode == GL_SELECT || mode == GL_FEEDBACK) c->render_mode = mode;
    return (GLint)result;
}

/* ---- Queries ------------------------------------------------------------------------- */

GLenum glp_impl_glGetError(void)
{
    TRACE(glGetError);
    return (GLenum)glp_query(GLP_FN_glGetError, 0, 0, 0, 0);
}

/* Answers the pnames the guest keeps; returns 0 for the others. */
static int client_get(GLenum pname, double *v)
{
    GLXContext c = ctx();
    const struct glp_client *cl = &c->client;
    int i;
    switch (pname) {
    case GL_PACK_SWAP_BYTES: *v = cl->pack.swap; return 1;
    case GL_PACK_LSB_FIRST: *v = cl->pack.lsbfirst; return 1;
    case GL_PACK_ROW_LENGTH: *v = cl->pack.rowlength; return 1;
    case GL_PACK_SKIP_ROWS: *v = cl->pack.skiprows; return 1;
    case GL_PACK_SKIP_PIXELS: *v = cl->pack.skippixels; return 1;
    case GL_PACK_ALIGNMENT: *v = cl->pack.alignment; return 1;
    case GL_UNPACK_SWAP_BYTES: *v = cl->unpack.swap; return 1;
    case GL_UNPACK_LSB_FIRST: *v = cl->unpack.lsbfirst; return 1;
    case GL_UNPACK_ROW_LENGTH: *v = cl->unpack.rowlength; return 1;
    case GL_UNPACK_SKIP_ROWS: *v = cl->unpack.skiprows; return 1;
    case GL_UNPACK_SKIP_PIXELS: *v = cl->unpack.skippixels; return 1;
    case GL_UNPACK_ALIGNMENT: *v = cl->unpack.alignment; return 1;
    case GLP_PACK_SKIP_IMAGES: *v = cl->pack.skipimages; return 1;
    case GLP_PACK_IMAGE_HEIGHT: *v = cl->pack.imageheight; return 1;
    case GLP_UNPACK_SKIP_IMAGES: *v = cl->unpack.skipimages; return 1;
    case GLP_UNPACK_IMAGE_HEIGHT: *v = cl->unpack.imageheight; return 1;
    case GL_DRAW_BUFFER: *v = c->draw_buffer; return 1;
    case GL_READ_BUFFER: *v = c->read_buffer; return 1;
    case GL_RENDER_MODE: *v = c->render_mode; return 1;
    case GL_DOUBLEBUFFER: *v = 1; return 1;
    case GL_INDEX_MODE: *v = c->index_mode; return 1;
    case GL_RGBA_MODE: *v = !c->index_mode; return 1;
    case GL_STEREO: *v = 0; return 1;
    case GL_CLIENT_ATTRIB_STACK_DEPTH: *v = c->client_depth; return 1;
    /* Colour index state: the host holds its translation. */
    case GL_INDEX_WRITEMASK: if (!c->index_mode) return 0; *v = (GLint)c->ix.state.mask; return 1;
    case GL_INDEX_LOGIC_OP: if (!c->index_mode) return 0; *v = c->ix.state.logic; return 1;
    case GL_LOGIC_OP_MODE: if (!c->index_mode) return 0; *v = c->ix.state.op; return 1;
    case GL_CURRENT_INDEX: if (!c->index_mode) return 0; *v = c->ix.state.index; return 1;
    case GL_INDEX_CLEAR_VALUE: if (!c->index_mode) return 0; *v = c->ix.state.clear; return 1;
    case GL_MAX_CLIENT_ATTRIB_STACK_DEPTH: *v = CLIENT_STACK; return 1;
    case GL_SELECTION_BUFFER_SIZE: *v = c->select_size; return 1;
    case GL_FEEDBACK_BUFFER_SIZE: *v = c->feedback_size; return 1;
    case GL_VERTEX_ARRAY: case GL_NORMAL_ARRAY: case GL_COLOR_ARRAY: case GL_INDEX_ARRAY:
    case GL_TEXTURE_COORD_ARRAY: case GL_EDGE_FLAG_ARRAY:
        *v = cl->arrays[array_index(pname)].enabled; return 1;
    case GL_VERTEX_ARRAY_SIZE: *v = cl->arrays[ARR_VERTEX].size; return 1;
    case GL_COLOR_ARRAY_SIZE: *v = cl->arrays[ARR_COLOR].size; return 1;
    case GL_TEXTURE_COORD_ARRAY_SIZE: *v = cl->arrays[texcoord_array(cl)].size; return 1;
    case GLP_CLIENT_ACTIVE_TEXTURE: *v = GLP_TEXTURE0 + cl->unit; return 1;
    case GL_VERTEX_ARRAY_TYPE: i = ARR_VERTEX; break;
    case GL_NORMAL_ARRAY_TYPE: i = ARR_NORMAL; break;
    case GL_COLOR_ARRAY_TYPE: i = ARR_COLOR; break;
    case GL_INDEX_ARRAY_TYPE: i = ARR_INDEX; break;
    case GL_TEXTURE_COORD_ARRAY_TYPE: i = texcoord_array(cl); break;
    case GL_VERTEX_ARRAY_STRIDE: *v = cl->arrays[ARR_VERTEX].stride; return 1;
    case GL_NORMAL_ARRAY_STRIDE: *v = cl->arrays[ARR_NORMAL].stride; return 1;
    case GL_COLOR_ARRAY_STRIDE: *v = cl->arrays[ARR_COLOR].stride; return 1;
    case GL_INDEX_ARRAY_STRIDE: *v = cl->arrays[ARR_INDEX].stride; return 1;
    case GL_TEXTURE_COORD_ARRAY_STRIDE: *v = cl->arrays[texcoord_array(cl)].stride; return 1;
    case GL_EDGE_FLAG_ARRAY_STRIDE: *v = cl->arrays[ARR_EDGEFLAG].stride; return 1;
    default:
        return 0;
    }
    *v = cl->arrays[i].type;
    return 1;
}

/* The host answers the glGet family in big-endian words (doubles: two). */
static void get_values(unsigned int fn, GLenum pname, void *params, unsigned int elem)
{
    unsigned int arg = pname;
    if (params) glp_query(fn, params, (unsigned int)get_count(pname) * elem, &arg, 1);
}

void glp_impl_glGetBooleanv(GLenum pname, GLboolean *params)
{
    double v;
    TRACE(glGetBooleanv);
    if (client_get(pname, &v)) { params[0] = v != 0; return; }
    get_values(GLP_FN_glGetBooleanv, pname, params, 1);
}

void glp_impl_glGetIntegerv(GLenum pname, GLint *params)
{
    double v;
    TRACE(glGetIntegerv);
    if (client_get(pname, &v)) { params[0] = (GLint)v; return; }
    get_values(GLP_FN_glGetIntegerv, pname, params, 4);
}

void glp_impl_glGetFloatv(GLenum pname, GLfloat *params)
{
    double v;
    TRACE(glGetFloatv);
    if (client_get(pname, &v)) { params[0] = (GLfloat)v; return; }
    get_values(GLP_FN_glGetFloatv, pname, params, 4);
}

void glp_impl_glGetDoublev(GLenum pname, GLdouble *params)
{
    double v;
    TRACE(glGetDoublev);
    if (client_get(pname, &v)) { params[0] = v; return; }
    get_values(GLP_FN_glGetDoublev, pname, params, 8);
}

GLboolean glp_impl_glIsEnabled(GLenum cap)
{
    unsigned int arg = cap;
    int i = array_index(cap);
    TRACE(glIsEnabled);
    if (i >= 0) return (GLboolean)ctx()->client.arrays[i].enabled;
    if (cap == GL_INDEX_LOGIC_OP && ctx()->index_mode) return (GLboolean)(ctx()->ix.state.logic != 0);
    return (GLboolean)glp_query(GLP_FN_glIsEnabled, 0, 0, &arg, 1);
}

static void query2(unsigned int fn, unsigned int a, unsigned int b, void *reply, unsigned int bytes)
{
    unsigned int args[2];
    args[0] = a;
    args[1] = b;
    if (reply) glp_query(fn, reply, bytes, args, 2);
}

#define GET_PNAME(name, group, T)                                                                  \
    void glp_impl_##name(GLenum a, GLenum pname, T *params)                                        \
    {                                                                                              \
        TRACE(name);                                                                               \
        query2(GLP_FN_##name, a, pname, params, (unsigned int)(glp_pname_count(group, pname) * sizeof(T))); \
    }

GET_PNAME(glGetLightfv, GLP_GROUP_LIGHT, GLfloat)
GET_PNAME(glGetLightiv, GLP_GROUP_LIGHT, GLint)
GET_PNAME(glGetMaterialfv, GLP_GROUP_MATERIAL, GLfloat)
GET_PNAME(glGetMaterialiv, GLP_GROUP_MATERIAL, GLint)
GET_PNAME(glGetTexParameterfv, GLP_GROUP_TEXPARAM, GLfloat)
GET_PNAME(glGetTexParameteriv, GLP_GROUP_TEXPARAM, GLint)
GET_PNAME(glGetTexEnvfv, GLP_GROUP_TEXENV, GLfloat)
GET_PNAME(glGetTexEnviv, GLP_GROUP_TEXENV, GLint)
GET_PNAME(glGetTexGenfv, GLP_GROUP_TEXGEN, GLfloat)
GET_PNAME(glGetTexGeniv, GLP_GROUP_TEXGEN, GLint)
GET_PNAME(glGetTexGendv, GLP_GROUP_TEXGEN, GLdouble)

void glp_impl_glGetTexLevelParameterfv(GLenum target, GLint level, GLenum pname, GLfloat *params)
{
    unsigned int a[3];
    TRACE(glGetTexLevelParameterfv);
    a[0] = target; a[1] = (unsigned int)level; a[2] = pname;
    if (params) glp_query(GLP_FN_glGetTexLevelParameterfv, params, 4, a, 3);
}

void glp_impl_glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params)
{
    unsigned int a[3];
    TRACE(glGetTexLevelParameteriv);
    a[0] = target; a[1] = (unsigned int)level; a[2] = pname;
    if (params) glp_query(GLP_FN_glGetTexLevelParameteriv, params, 4, a, 3);
}

void glp_impl_glGetClipPlane(GLenum plane, GLdouble *equation)
{
    unsigned int arg = plane;
    TRACE(glGetClipPlane);
    if (equation) glp_query(GLP_FN_glGetClipPlane, equation, 32, &arg, 1);
}

/* Image reads: the host fills the program's buffer following the pack state. */
void glp_impl_glGetPolygonStipple(GLubyte *mask)
{
    TRACE(glGetPolygonStipple);
    if (mask) {
        glp_query(GLP_FN_glGetPolygonStipple, mask,
                  image_bytes(&ctx()->client.pack, 32, 32, GL_COLOR_INDEX, GL_BITMAP), 0, 0);
    }
}

void glp_impl_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels)
{
    unsigned int a[6], bytes = image_bytes(&ctx()->client.pack, width, height, format, type);
    TRACE(glReadPixels);
    if (!pixels || !bytes) return;
    a[0] = (unsigned int)x; a[1] = (unsigned int)y; a[2] = (unsigned int)width; a[3] = (unsigned int)height;
    a[4] = format; a[5] = type;
    glp_query(GLP_FN_glReadPixels, pixels, bytes, a, 6);
}

void glp_impl_glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid *pixels)
{
    GLint width = 0, height = 1, depth = 1;
    unsigned int a[4], bytes;
    TRACE(glGetTexImage);
    glGetTexLevelParameteriv(target, level, GL_TEXTURE_WIDTH, &width);
    if (target != GL_TEXTURE_1D) glGetTexLevelParameteriv(target, level, GL_TEXTURE_HEIGHT, &height);
    if (target == GLP_TEXTURE_3D) glGetTexLevelParameteriv(target, level, GLP_TEXTURE_DEPTH, &depth);
    bytes = target == GLP_TEXTURE_3D ? image_bytes_3d(&ctx()->client.pack, width, height, depth, format, type)
                                     : image_bytes(&ctx()->client.pack, width, height, format, type);
    if (!pixels || !bytes) return;
    a[0] = target; a[1] = (unsigned int)level; a[2] = format; a[3] = type;
    glp_query(GLP_FN_glGetTexImage, pixels, bytes, a, 4);
}

void glp_impl_glGetPointerv(GLenum pname, GLvoid **params)
{
    GLXContext c = ctx();
    int i = -1;
    TRACE(glGetPointerv);
    switch (pname) {
    case GL_VERTEX_ARRAY_POINTER: i = ARR_VERTEX; break;
    case GL_NORMAL_ARRAY_POINTER: i = ARR_NORMAL; break;
    case GL_COLOR_ARRAY_POINTER: i = ARR_COLOR; break;
    case GL_INDEX_ARRAY_POINTER: i = ARR_INDEX; break;
    case GL_TEXTURE_COORD_ARRAY_POINTER: i = texcoord_array(&c->client); break;
    case GL_EDGE_FLAG_ARRAY_POINTER: i = ARR_EDGEFLAG; break;
    case GL_SELECTION_BUFFER_POINTER: *params = c->select_buffer; return;
    case GL_FEEDBACK_BUFFER_POINTER: *params = c->feedback_buffer; return;
    default: *params = 0; return;
    }
    *params = (GLvoid *)c->client.arrays[i].pointer;
}

void glp_impl_glGetPointervEXT(GLenum pname, GLvoid **params) { glGetPointerv(pname, params); }

GLboolean glp_impl_glIsList(GLuint list)
{
    TRACE(glIsList);
    return (GLboolean)glp_query(GLP_FN_glIsList, 0, 0, &list, 1);
}

GLboolean glp_impl_glIsTexture(GLuint texture)
{
    TRACE(glIsTexture);
    return (GLboolean)glp_query(GLP_FN_glIsTexture, 0, 0, &texture, 1);
}

GLboolean glp_impl_glIsTextureEXT(GLuint texture) { return glIsTexture(texture); }

GLuint glp_impl_glGenLists(GLsizei range)
{
    unsigned int arg = (unsigned int)range;
    TRACE(glGenLists);
    return (GLuint)glp_query(GLP_FN_glGenLists, 0, 0, &arg, 1);
}

void glp_impl_glGenTextures(GLsizei n, GLuint *textures)
{
    unsigned int arg = (unsigned int)n;
    TRACE(glGenTextures);
    if (n > 0 && textures) glp_query(GLP_FN_glGenTextures, textures, (unsigned int)n * 4, &arg, 1);
}

void glp_impl_glGenTexturesEXT(GLsizei n, GLuint *textures) { glGenTextures(n, textures); }

GLboolean glp_impl_glAreTexturesResident(GLsizei n, const GLuint *textures, GLboolean *residences)
{
    TRACE(glAreTexturesResident);
    if (n <= 0 || !textures || !residences) return GL_TRUE;
    return (GLboolean)glp_query(GLP_FN_glAreTexturesResident, residences, (unsigned int)n, textures,
                                (unsigned int)n);
}

GLboolean glp_impl_glAreTexturesResidentEXT(GLsizei n, const GLuint *textures, GLboolean *residences)
{
    return glAreTexturesResident(n, textures, residences);
}

/* Extensions offered: those that only add tokens are listed when the host
   has them; the rest are provided here through core host commands. */
static const char *const host_extensions[] = {
    "GL_EXT_abgr", "GL_EXT_blend_color", "GL_EXT_blend_logic_op", "GL_EXT_blend_minmax",
    "GL_EXT_blend_subtract", "GL_EXT_packed_pixels", "GL_EXT_rescale_normal", "GL_ARB_multitexture",
    "GL_ARB_transpose_matrix", "GL_ARB_texture_env_add", "GL_ARB_texture_border_clamp", "GL_ARB_texture_cube_map",
    "GL_ARB_window_pos", "GL_EXT_bgra", "GL_EXT_separate_specular_color", "GL_EXT_texture_env_add",
    "GL_EXT_stencil_wrap", "GL_EXT_texture_filter_anisotropic", "GL_EXT_texture_lod_bias",
    "GL_EXT_secondary_color", "GL_EXT_fog_coord", "GL_ARB_texture_env_combine", "GL_EXT_texture_env_combine",
    "GL_ARB_texture_env_dot3", "GL_SGIS_generate_mipmap", 0};
static const char core_extensions[] =
    "GL_EXT_polygon_offset GL_EXT_texture GL_EXT_texture_object GL_EXT_vertex_array GL_EXT_subtexture "
    "GL_EXT_copy_texture GL_EXT_texture_edge_clamp GL_SGIS_texture_edge_clamp GL_SGIS_texture_lod "
    "GL_EXT_draw_range_elements GL_EXT_texture3D";

static int has_word(const char *list, const char *word)
{
    size_t n = strlen(word);
    const char *p = list;
    while ((p = strstr(p, word)) != 0) {
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0)) return 1;
        p += n;
    }
    return 0;
}

const GLubyte *glp_impl_glGetString(GLenum name)
{
    GLXContext current = current_context();
    static char renderer[256], host_list[8192], extensions[2048];
    TRACE(glGetString);
    switch (name) {
    case GL_VENDOR:
        return (const GLubyte *)"irix-emu";
    case GL_VERSION:
        return (const GLubyte *)"1.1 glpass";
    case GL_RENDERER:
        glp_lock();
        if (!renderer[0] && current) {
            char host[200];
            unsigned int arg = GL_RENDERER;
            memset(host, 0, sizeof host);
            glp_query(GLP_FN_glGetString, host, sizeof host - 1, &arg, 1);
            snprintf(renderer, sizeof renderer, "glpass (%s)", host);
        }
        glp_unlock();
        return (const GLubyte *)(renderer[0] ? renderer : "glpass");
    case GL_EXTENSIONS:
        glp_lock();
        if (!extensions[0] && current) {
            unsigned int arg = GL_EXTENSIONS;
            int i;
            glp_query(GLP_FN_glGetString, host_list, sizeof host_list - 1, &arg, 1);
            strcpy(extensions, core_extensions);
            for (i = 0; host_extensions[i]; ++i) {
                if (!has_word(host_list, host_extensions[i])) continue;
                if (strlen(extensions) + strlen(host_extensions[i]) + 2 >= sizeof extensions) break;
                strcat(extensions, " ");
                strcat(extensions, host_extensions[i]);
            }
        }
        glp_unlock();
        return (const GLubyte *)(extensions[0] ? extensions : core_extensions);
    default:
        return 0;
    }
}

static struct glp_drawable *find_drawable(XID id);

/* ---- The library's own X connection --------------------------------------------------- */

/* Everything this library asks the X server for itself (window positions,
   the key fill, colormaps) goes through a connection of its own: the
   program's connection may be in use by another of its threads (Maya's
   sprocs), and Xlib connections are not thread-safe. The library lock keeps
   this connection, and the tables all threads share (drawables, window
   positions and modes, palettes sent, visuals handed out), to one thread at
   a time; the thread holding it may take it again. */
#ifdef __sgi
static abilock_t glp_mutex; /* zero: UNLOCKED */
#endif
static struct glp_thread *volatile glp_owner;
static int glp_depth;

void glp_lock(void)
{
    struct glp_thread *self = glp_self();
    if (glp_owner == self) {
        ++glp_depth;
        return;
    }
#ifdef __sgi
    spin_lock(&glp_mutex);
#endif
    glp_owner = self;
    glp_depth = 1;
}

void glp_unlock(void)
{
    if (--glp_depth > 0) return;
    glp_owner = 0;
#ifdef __sgi
    release_lock(&glp_mutex);
#endif
}

/* The library's connection to the server `app` is connected to. */
static Display *own_display; /* this library's connection, once open */
static void install_error_handler(void);

static Display *xconn(Display *app)
{
    static int failed;
    if (!own_display && !failed && app) {
        install_error_handler(); /* before any request: its errors are this library's */
        own_display = XOpenDisplay(DisplayString(app));
        if (!own_display) {
            failed = 1;
            fprintf(stderr, "glpass: cannot open a second X connection; using the program's\n");
        }
    }
    return own_display ? own_display : app;
}

/* ---- Colour index mode ------------------------------------------------------------------ */

/* The host renders in RGBA. A colour-index drawable keeps the index itself
   in its host buffer, as the colour (index & 0xFF, index >> 8, 0), so pixel
   copies and logic operations act on indices as on SGI hardware; the host
   turns indices into colours with the drawable's palette when it shows the
   image (glpass_protocol.h, GLP_FN_PALETTE). RGBA contexts ignore indices,
   as GL does.

   The palette is the window's colormap. Colours a program stored beyond the
   server's 8-bit colormaps (12-bit colour index, as Maya uses) come from
   libglpass_x.so when it is preloaded (xstore.c). */
#define PALETTE_SIZE GLPASS_PALETTE_SIZE

static void *xstore_function(const char *name)
{
    static void *lib;
    static int looked;
    if (!looked) {
        looked = 1;
        lib = dlopen("libglpass_x.so", RTLD_LAZY); /* the preloaded copy */
        if (!lib) lib = dlopen(0, RTLD_LAZY);
    }
    return lib ? dlsym(lib, name) : 0;
}

static struct glpass_colormap *stored_colors(Colormap id)
{
    static struct glpass_colormap *(*find)(unsigned long);
    static int looked;
    if (!looked) {
        looked = 1;
        find = (struct glpass_colormap *(*)(unsigned long))xstore_function("glpass_find_colormap");
    }
    return find ? find(id) : 0;
}

/* Changes whenever a program stores a colour (0 without libglpass_x.so). */
static unsigned long stored_generation(void)
{
    static unsigned long (*generation)(void);
    static int looked;
    if (!looked) {
        looked = 1;
        generation = (unsigned long (*)(void))xstore_function("glpass_colormap_generation");
    }
    return generation ? generation() : 0;
}

/* The palettes last sent, per drawable: sent again only when they change. */
#define PALETTES_KNOWN 32
static struct {
    GLXDrawable drawable;
    unsigned long generation;
    unsigned long hash;
    Colormap colormap;          /* the window's, and its first entries as read */
    unsigned int base[256];
    int nbase;
} sent_palettes[PALETTES_KNOWN];
static int next_palette_slot;

/* Sends the current drawable's palette to the host if it changed. */
static void load_palette_locked(Display *dpy, GLXContext c, GLXDrawable drawable)
{
    static unsigned int palette[PALETTE_SIZE];
    XWindowAttributes wa;
    XColor colors[256];
    const struct glpass_colormap *m;
    unsigned long generation = stored_generation(), hash = 5381;
    unsigned int *p;
    int i, n, slot = -1;
    if (!c->index_mode || !dpy || !drawable || find_drawable(drawable)) return;
    dpy = xconn(dpy);
    for (i = 0; i < PALETTES_KNOWN; ++i) {
        if (sent_palettes[i].drawable == drawable) slot = i;
    }
    if (slot >= 0 && sent_palettes[slot].generation == generation && generation != 0) return; /* unchanged */
    if (slot < 0) {
        /* First time for this window: ask the server for its colormap. Later
           updates come from the noted stores alone, with no request: the
           program may hold a server grab then (see present). */
        glp_lock();
        if (!XGetWindowAttributes(dpy, drawable, &wa) || wa.colormap == None) {
            glp_unlock();
            return;
        }
        n = wa.visual && wa.visual->map_entries > 0 && wa.visual->map_entries <= 256 ? wa.visual->map_entries : 256;
        for (i = 0; i < n; ++i) colors[i].pixel = (unsigned long)i;
        XQueryColors(dpy, wa.colormap, colors, n);
        glp_unlock();
        slot = next_palette_slot;
        next_palette_slot = (next_palette_slot + 1) % PALETTES_KNOWN;
        sent_palettes[slot].drawable = drawable;
        sent_palettes[slot].hash = 0;
        sent_palettes[slot].colormap = wa.colormap;
        sent_palettes[slot].nbase = n;
        for (i = 0; i < n; ++i) {
            sent_palettes[slot].base[i] = (unsigned int)(colors[i].red >> 8) << 16 |
                                          (unsigned int)(colors[i].green >> 8) << 8 | (unsigned int)(colors[i].blue >> 8);
        }
    }
    memset(palette, 0, sizeof palette);
    memcpy(palette, sent_palettes[slot].base, (size_t)sent_palettes[slot].nbase * sizeof palette[0]);
    wa.colormap = sent_palettes[slot].colormap;
    m = stored_colors(wa.colormap);
    for (i = 0; m && i < PALETTE_SIZE; ++i) {
        if (m->set[i]) {
            palette[i] = (unsigned int)m->rgb[i][0] << 16 | (unsigned int)m->rgb[i][1] << 8 | m->rgb[i][2];
        }
    }
    for (i = 0; i < PALETTE_SIZE; ++i) hash = hash * 33 + palette[i];
    if (hash == 0) hash = 1; /* 0 marks "never sent" */
    sent_palettes[slot].generation = generation;
    if (sent_palettes[slot].hash == hash) return;
    sent_palettes[slot].hash = hash;
    p = glp_put(GLP_FN_PALETTE, 2 + PALETTE_SIZE);
    p[0] = c->overlay ? GLP_PALETTE_OVERLAY : 0;
    p[1] = PALETTE_SIZE;
    memcpy(p + 2, palette, sizeof palette);
    glx_log("palette of %#lx: colormap %#lx, %s stored colours%s", (unsigned long)drawable,
            (unsigned long)wa.colormap, m ? "with" : "no", c->overlay ? " (overlay)" : "");
    if (glp_trace && m) { /* which entries the program stored, as ranges */
        char text[512];
        size_t at = 0;
        int start = -1;
        text[0] = 0;
        for (i = 0; i <= PALETTE_SIZE && at + 24 < sizeof text; ++i) {
            int set = i < PALETTE_SIZE && m->set[i];
            if (set && start < 0) start = i;
            if (!set && start >= 0) {
                at += (size_t)sprintf(text + at, " %d-%d", start, i - 1);
                start = -1;
            }
        }
        glx_log("  stored:%s; [2176]=%06x [2240]=%06x [3264]=%06x [2054]=%06x", text, palette[2176], palette[2240],
                palette[3264], palette[2054]);
    }
}

static int window_dead(unsigned long w);

static void load_palette(Display *dpy, GLXContext c, GLXDrawable drawable)
{
    if (window_dead(drawable)) return; /* no colormap to ask a destroyed window for */
    glp_lock();
    load_palette_locked(dpy, c, drawable);
    glp_unlock();
}

/* ---- Colour index writes ------------------------------------------------------------- */

/* The host buffer stores index bits in channels (GLP_INDEX_R/G/B/A): bits 0-7
   in red, 8-9 in green, 10 in blue, 11 in alpha. A colour mask can only keep
   or write whole channels, so an index write mask is exact through it only
   when it is a union of these groups. Other masks (Maya uses 0x83e and 0xfc0)
   are made exact with the logic op instead. Per bit, the masked write
   dst' = mask ? f(src, dst) : dst, with f the index logic op (copy when off)
   and src the current index, is one of four functions of dst: 0, 1, dst or
   ~dst. The bits to clear, to set and to invert are disjoint, so one host
   pass for each kind present gives the exact result in any order:
     clear -> AND with the other bits    set -> OR with them    invert -> XOR with them
   One pass is the common case (a logic op such as XOR, or a copy of an index
   whose masked bits are all 0 or all 1); a copy of an index with masked bits
   both 0 and 1 takes two. Draws are then sent once per pass: glBegin/glEnd
   blocks, vertex array draws and clears. What cannot be repeated (display
   lists, an index changed inside glBegin/glEnd, a block too large for the
   command buffer) gets the first pass only, logged once per mask. */
static const unsigned int index_groups[4] = {0x0FFu, 0x300u, 0x400u, 0x800u};
#define INDEX_BITS (PALETTE_SIZE - 1u)

enum {
    INDEX_CHANGED_MASK = 1, INDEX_CHANGED_LOGIC = 2, INDEX_CHANGED_OP = 4,
    INDEX_CHANGED_INDEX = 8, INDEX_CHANGED_CLEAR = 16, INDEX_CHANGED_DEPTH = 32
};

/* The channels a mask touches; *exact when it covers them entirely. */
static unsigned int index_channels(unsigned int mask, int *exact)
{
    unsigned int channels = 0;
    int k;
    *exact = 1;
    mask &= INDEX_BITS;
    for (k = 0; k < 4; ++k) {
        if (mask & index_groups[k]) {
            channels |= 1u << k;
            if ((mask & index_groups[k]) != index_groups[k]) *exact = 0;
        }
    }
    return channels;
}

/* GL logic op result for one source and destination bit: the low four bits
   of the GL_CLEAR..GL_SET codes are the X11 truth table. */
static int logic_bit(GLenum op, int s, int d)
{
    return (int)(((op & 0xFu) >> ((1 - s) * 2 + (1 - d))) & 1u);
}

/* The passes writing index `src` under `st`, and the host setting for the
   first: logic op on/off and op, colour mask. Returns the number of passes.
   Which passes there are follows from the mask and the logic op alone, not
   from `src`: a pass that does nothing to `src` gets its identity value (AND
   all ones, OR and XOR zero). So every index written under one state takes
   the same passes, and a draw whose index changes midway (glIndex inside
   glBegin/glEnd, an index array, a display list) is replayed exactly. */
static int index_passes(const struct glp_index_state *st, unsigned int src, struct glp_index_host *host,
                        struct glp_index_pass pass[3])
{
    const unsigned int mask = st->mask & INDEX_BITS;
    const GLenum op = st->logic ? st->op : GL_COPY;
    unsigned int zero = 0, one = 0, invert = 0, b;
    int exact, n = 0, s, need_and = 0, need_or = 0, need_xor = 0;
    host->known = 1;
    host->channels = index_channels(mask, &exact);
    if (exact) { /* whole channels: the colour mask does it */
        host->logic = st->logic;
        host->op = st->op;
        pass[0].op = st->op;
        pass[0].value = src;
        return 1;
    }
    for (s = 0; s < 2; ++s) { /* what the op does to a destination bit, for either source bit */
        const int r0 = logic_bit(op, s, 0), r1 = logic_bit(op, s, 1);
        if (!r0 && !r1) need_and = 1;
        else if (r0 && r1) need_or = 1;
        else if (r0 && !r1) need_xor = 1;
    }
    for (b = 0; b < 12; ++b) {
        const unsigned int bit = 1u << b;
        int r0, r1;
        if (!(mask & bit)) continue;
        r0 = logic_bit(op, (src & bit) != 0, 0);
        r1 = logic_bit(op, (src & bit) != 0, 1);
        if (r0 == 0 && r1 == 0) zero |= bit;
        else if (r0 == 1 && r1 == 1) one |= bit;
        else if (r0 == 1 && r1 == 0) invert |= bit;
    }
    if (need_and) { pass[n].op = GL_AND; pass[n].value = INDEX_BITS & ~zero; ++n; }
    if (need_or) { pass[n].op = GL_OR; pass[n].value = one; ++n; }
    if (need_xor) { pass[n].op = GL_XOR; pass[n].value = invert; ++n; }
    if (n == 0) { pass[n].op = GL_NOOP; pass[n].value = 0; ++n; }
    host->channels = 0xFu;
    host->logic = 1;
    host->op = pass[0].op;
    return n;
}

static void index_log_approximate(const struct glp_index_state *st, const char *why)
{
    static unsigned int logged[8];
    static int nlogged;
    const unsigned int key = (st->mask & INDEX_BITS) | (st->logic ? st->op << 12 : 0);
    int i;
    for (i = 0; i < nlogged; ++i) {
        if (logged[i] == key) return;
    }
    if (nlogged == 8) return;
    logged[nlogged++] = key;
    glx_log("index mask %#x, %s: first pass only (%s)", st->mask & INDEX_BITS, st->logic ? "logic op" : "copy", why);
}

static void put_word(unsigned int fn, unsigned int word) { glp_put(fn, 1)[0] = word; }

static void put_channels(unsigned int channels)
{
    unsigned int *p = glp_put(GLP_FN_glColorMask, 4);
    int k;
    for (k = 0; k < 4; ++k) p[k] = (channels >> k) & 1u;
}

static void encode_index(unsigned int *p, unsigned int i)
{
    p[0] = GLP_INDEX_R(i); p[1] = GLP_INDEX_G(i); p[2] = GLP_INDEX_B(i); p[3] = GLP_INDEX_A(i);
}

/* Returns the payload's place in the calling thread's command buffer. */
static unsigned int *put_index_color(unsigned int i)
{
    unsigned int *p = glp_put(GLP_FN_glColor4ub, 4);
    encode_index(p, i);
    return p;
}

/* An index's colour as one word of RGBA bytes (a colour array element). */
static unsigned int packed_index(unsigned int i)
{
    unsigned int w;
    unsigned char *b = (unsigned char *)&w;
    b[0] = (unsigned char)GLP_INDEX_R(i);
    b[1] = (unsigned char)GLP_INDEX_G(i);
    b[2] = (unsigned char)GLP_INDEX_B(i);
    b[3] = (unsigned char)GLP_INDEX_A(i);
    return w;
}

static void put_raw(struct glp_thread *t, const unsigned int *words, unsigned int n);

/* ---- Recordings ---------------------------------------------------------------------

   A draw that takes several passes is sent once with the first pass's
   colours, and recorded: when it ends, its commands are sent again for each
   further pass, its colours rewritten for that pass (struct glp_event). A
   display list being compiled is recorded the same way, as its shadow (see
   list_expand). Commands are recorded as they go to the host: what a buffer
   flush sends is kept first (thread_flush), so a recording may be larger
   than the command buffer. */

/* `buf` grown to hold `need` elements of `size` bytes; 0 when out of memory
   (`buf` unchanged). */
static void *grown(void *buf, unsigned int *cap, unsigned int need, size_t size)
{
    unsigned int c = *cap ? *cap : 256;
    void *p;
    if (need <= *cap) return buf;
    while (c < need) c *= 2;
    p = realloc(buf, (size_t)c * size);
    if (p) *cap = c;
    return p;
}

static void recording_start(struct glp_recording *r, const struct glp_thread *t, unsigned int limit)
{
    r->active = 1;
    r->failed = 0;
    r->start = t->cmdlen;
    r->limit = limit;
    r->nwords = 0;
    r->nevents = 0;
}

/* Keeps the thread's commands since the last keep. */
static void recording_keep(struct glp_recording *r, const struct glp_thread *t)
{
    const unsigned int n = t->cmdlen > r->start ? t->cmdlen - r->start : 0;
    unsigned int *w;
    if (!r->active || r->failed || n == 0) {
        r->start = t->cmdlen;
        return;
    }
    if (r->nwords + n > r->limit || !(w = (unsigned int *)grown(r->words, &r->capwords, r->nwords + n, 4))) {
        r->failed = 1;
        return;
    }
    r->words = w;
    memcpy(w + r->nwords, t->cmdbuf + r->start, (size_t)n * 4);
    r->nwords += n;
    r->start = t->cmdlen;
}

/* The recording's offset of `p`, a word in the thread's command buffer
   after r->start. */
static unsigned int recording_at(const struct glp_recording *r, const struct glp_thread *t, const unsigned int *p)
{
    return r->nwords + (unsigned int)(p - t->cmdbuf) - r->start;
}

static void recording_note(struct glp_recording *r, unsigned int at, unsigned int kind, unsigned int value,
                           unsigned int len)
{
    struct glp_event *e;
    if (r->failed) return;
    e = (struct glp_event *)grown(r->events, &r->capevents, r->nevents + 1, sizeof *e);
    if (!e) {
        r->failed = 1;
        return;
    }
    r->events = e;
    e += r->nevents++;
    e->at = at;
    e->kind = kind;
    e->value = value;
    e->len = len;
}

/* After a recording is used: a large buffer is not kept for the next. */
static void recording_trim(struct glp_recording *r)
{
    if (r->capwords > (256u << 10)) {
        free(r->words);
        r->words = 0;
        r->capwords = 0;
    }
    if (r->capevents > (64u << 10)) {
        free(r->events);
        r->events = 0;
        r->capevents = 0;
    }
    r->nwords = 0;
    r->nevents = 0;
}

static void recording_free(struct glp_recording *r)
{
    free(r->words);
    free(r->events);
    memset(r, 0, sizeof *r);
}

static void recordings_keep(GLXContext c, const struct glp_thread *t)
{
    recording_keep(&c->ix.group, t);
    recording_keep(&c->ix.compile, t);
}

/* The thread's buffer was emptied, or the context has a new thread. */
static void recordings_restart(GLXContext c, const struct glp_thread *t)
{
    c->ix.group.start = t->cmdlen;
    c->ix.compile.start = t->cmdlen;
}

/* The recording an index colour goes into: the list being compiled, or the
   draw recorded for further passes. */
static struct glp_recording *index_recording(struct glp_index *ix)
{
    if (ix->compile.active) return &ix->compile;
    if (ix->group.active) return &ix->group;
    return 0;
}

/* Notes `p`, a colour for index `src` just put, where a recording rewrites it. */
static void index_event(GLXContext c, const unsigned int *p, unsigned int kind, unsigned int src)
{
    struct glp_recording *r = c->index_mode ? index_recording(&c->ix) : 0;
    if (r) recording_note(r, recording_at(r, glp_self(), p), kind, src, kind == EV_COLOR4 ? 4u : 1u);
}

/* The colour value to write for index `src` now: a list being compiled
   holds the plain encoding (rewritten when it runs per pass), anything else
   the first pass's value. */
static unsigned int index_value(GLXContext c, unsigned int src)
{
    struct glp_index_host host;
    struct glp_index_pass pass[3];
    if (!c->index_mode) return src;
    if (c->ix.list) return src;
    (void)index_passes(&c->ix.state, src, &host, pass);
    return pass[0].value;
}

/* Sends whole records: through the command buffer, or straight to the host
   when they would not fit it. */
static void submit_words(struct glp_thread *t, const unsigned int *words, unsigned int n)
{
    if (n <= t->cmdcap / 2 || n <= 4096) {
        put_raw(t, words, n);
        return;
    }
    thread_flush(t);
    if (t->current) {
        t->submitted += n;
        glp_gl(GLP_OP_SUBMIT, t->current->id, words, (unsigned long)n * 4);
    }
}

/* Sends the host the parts of `want` it does not have. */
static void index_apply(struct glp_index *ix, const struct glp_index_host *want)
{
    struct glp_index_host *h = &ix->host;
    if (!h->known || h->logic != want->logic) {
        put_word(want->logic ? GLP_FN_glEnable : GLP_FN_glDisable, GL_COLOR_LOGIC_OP);
    }
    if (!h->known || h->op != want->op) put_word(GLP_FN_glLogicOp, want->op);
    if (!h->known || h->channels != want->channels) put_channels(want->channels);
    *h = *want;
}

/* Brings the host in line with the context's index state, and sets the
   colour for the current index (its first pass). Inside glBegin/glEnd the
   host state cannot change: the colour is exact only if the state fits. */
static void index_update(GLXContext c)
{
    struct glp_index *ix = &c->ix;
    struct glp_index_host want;
    struct glp_index_pass pass[3];
    const unsigned int src = ix->state.index % PALETTE_SIZE;
    int n = index_passes(&ix->state, src, &want, pass);
    if (ix->in_begin) {
        if (ix->group.active) {
            /* Another index inside a recorded block: the state, so the
               passes, cannot change until glEnd; the colour is this pass's
               value, rewritten for the others. */
            index_event(c, put_index_color(pass[0].value), EV_COLOR4, src);
            return;
        }
        if (!ix->host.known || ix->host.logic != want.logic || ix->host.op != want.op ||
            ix->host.channels != want.channels || n > 1) {
            if (c->render_mode == GL_RENDER) index_log_approximate(&ix->state, "index changed inside glBegin/glEnd");
            put_index_color(src);
            return;
        }
        put_index_color(pass[0].value);
        return;
    }
    index_apply(ix, &want);
    memcpy(ix->passes, pass, sizeof pass);
    ix->npasses = n;
    put_index_color(pass[0].value);
}

/* Raw words appended to the calling thread's command stream. */
static void put_raw(struct glp_thread *t, const unsigned int *words, unsigned int n)
{
    if (t->cmdlen + n > t->cmdcap) {
        thread_flush(t);
        if (n > t->cmdcap) {
            unsigned int cap = t->cmdcap ? t->cmdcap : 16384;
            while (cap < n) cap *= 2;
            free(t->cmdbuf);
            t->cmdbuf = (unsigned int *)malloc((size_t)cap * 4);
            if (!t->cmdbuf) {
                fprintf(stderr, "glpass: out of memory for a %u-word command\n", n);
                abort();
            }
            t->cmdcap = cap;
        }
    }
    memcpy(t->cmdbuf + t->cmdlen, words, (size_t)n * 4);
    t->cmdlen += n;
}

/* A draw starts: when the current state takes several passes, its commands
   are recorded to be sent again. Not in selection or feedback mode, where a
   pass more would be hits or tokens more. Returns 1 when it started. */
static int passes_begin(GLXContext c)
{
    struct glp_index *ix = &c->ix;
    if (!c->index_mode || ix->group.active || ix->list || ix->npasses < 2 || !ix->host.known ||
        c->render_mode != GL_RENDER) {
        return 0;
    }
    recording_start(&ix->group, glp_self(), GROUP_MAX_WORDS);
    return 1;
}

/* The depth test for passes after the first: they draw the fragments the
   first drew, at the depths it may have written. */
static GLenum replay_depth_func(GLenum func)
{
    return func == GL_LESS ? GL_LEQUAL : func == GL_GREATER ? GL_GEQUAL : func;
}

/* The recorded draw again for each further pass: its colours rewritten for
   the pass, after the pass's logic op and the colour current at its start.
   Then the host back to the first pass, for the current index. */
static void passes_replay(GLXContext c, struct glp_recording *r, int wrapped)
{
    struct glp_thread *t = glp_self();
    struct glp_index *ix = &c->ix;
    const GLenum depth = ix->state.depth_func, again = replay_depth_func(depth);
    int k;
    if (again != depth) put_word(GLP_FN_glDepthFunc, again);
    for (k = 1; k < ix->npasses; ++k) {
        unsigned int j, last = 0xFFFFFFFFu, value = 0;
        for (j = 0; j < r->nevents; ++j) {
            const struct glp_event *e = &r->events[j];
            if (e->at + e->len > r->nwords) continue;
            if (e->value != last) {
                struct glp_index_host host;
                struct glp_index_pass pass[3];
                (void)index_passes(&ix->state, e->value, &host, pass);
                last = e->value;
                value = pass[k].value;
            }
            if (e->kind == EV_COLOR4) encode_index(r->words + e->at, value);
            else if (e->kind == EV_PACKED) r->words[e->at] = packed_index(value);
        }
        if (wrapped) { /* back to the matrix the draw started from; the last pass keeps what it leaves */
            glp_put(GLP_FN_glPopMatrix, 0);
            if (k + 1 < ix->npasses) glp_put(GLP_FN_glPushMatrix, 0);
        }
        put_word(GLP_FN_glLogicOp, ix->passes[k].op);
        put_index_color(ix->passes[k].value);
        submit_words(t, r->words, r->nwords);
    }
    if (again != depth) put_word(GLP_FN_glDepthFunc, depth);
    ix->host.op = ix->passes[ix->npasses - 1].op;
    index_update(c); /* the index may have changed during the draw */
}

/* The draw ended: its other passes. `wrapped`: the caller pushed the
   matrix before it, for the passes to start from (list_expand); it is
   popped here. */
static void passes_finish(GLXContext c, int wrapped)
{
    struct glp_thread *t = glp_self();
    struct glp_index *ix = &c->ix;
    if (!ix->group.active) return;
    recording_keep(&ix->group, t);
    ix->group.active = 0;
    if (ix->group.failed || ix->group.nwords == 0) {
        if (ix->group.failed) index_log_approximate(&ix->state, "draw too large to record");
        if (wrapped) glp_put(GLP_FN_glPopMatrix, 0);
        index_update(c);
    } else {
        passes_replay(c, &ix->group, wrapped);
    }
    recording_trim(&ix->group);
}

static void passes_end(GLXContext c) { passes_finish(c, 0); }

/* While a GL_COMPILE list is built, state calls are recorded, not run: the
   list gets the plain translation (whole channels), and its effect on the
   index state is noted for glCallList. */
static int index_compiling(const struct glp_index *ix) { return ix->list && ix->list_mode == GL_COMPILE; }

/* Notes a change in the list being built. GL_COMPILE changes went to its
   record already (index_target_state); GL_COMPILE_AND_EXECUTE ones are
   copied from the context's state. */
static void index_note(struct glp_index *ix, unsigned int changed)
{
    struct glp_index_state *r = &ix->compiling.state;
    if (!ix->list) return;
    ix->compiling.changed |= changed;
    if (ix->list_mode == GL_COMPILE) return;
    if (changed & INDEX_CHANGED_MASK) r->mask = ix->state.mask;
    if (changed & INDEX_CHANGED_LOGIC) r->logic = ix->state.logic;
    if (changed & INDEX_CHANGED_OP) r->op = ix->state.op;
    if (changed & INDEX_CHANGED_INDEX) r->index = ix->state.index;
    if (changed & INDEX_CHANGED_CLEAR) r->clear = ix->state.clear;
    if (changed & INDEX_CHANGED_DEPTH) r->depth_func = ix->state.depth_func;
}

static struct glp_index_state *index_target_state(struct glp_index *ix)
{
    /* Compiling only: the change goes to the list's record, not the context. */
    return index_compiling(ix) ? &ix->compiling.state : &ix->state;
}

/* The plain translation, which lists and approximate writes use. */
static void index_plain(GLXContext c, const struct glp_index_state *st, unsigned int changed)
{
    int exact;
    if (changed & INDEX_CHANGED_MASK) put_channels(index_channels(st->mask, &exact));
    if (changed & INDEX_CHANGED_LOGIC) put_word(st->logic ? GLP_FN_glEnable : GLP_FN_glDisable, GL_COLOR_LOGIC_OP);
    if (changed & INDEX_CHANGED_OP) put_word(GLP_FN_glLogicOp, st->op);
    if (changed & INDEX_CHANGED_INDEX) {
        index_event(c, put_index_color(st->index % PALETTE_SIZE), EV_COLOR4, st->index % PALETTE_SIZE);
    }
}

static void index_changed(GLXContext c, unsigned int changed)
{
    struct glp_index *ix = &c->ix;
    index_note(ix, changed);
    if (ix->list) {
        /* Lists hold the plain translation, in either mode (with
           GL_COMPILE_AND_EXECUTE, what is sent is compiled too); the host
           state is unknown after. */
        index_plain(c, index_target_state(ix), changed);
        ix->host.known = 0;
        return;
    }
    index_update(c);
}

static void set_index(double index)
{
    GLXContext current = current_context();
    if (current && !current->index_mode) {
        glx_log("glIndex(%ld) in RGBA context %u on %#lx", (long)index, current->id, (unsigned long)current->drawable);
    }
    if (!current || !current->index_mode) return;
    index_target_state(&current->ix)->index = (unsigned int)(long)index % PALETTE_SIZE;
    index_changed(current, INDEX_CHANGED_INDEX);
}

void glp_impl_glIndexd(GLdouble c) { TRACE(glIndexd); set_index(c); }
void glp_impl_glIndexf(GLfloat c) { TRACE(glIndexf); set_index(c); }
void glp_impl_glIndexi(GLint c) { TRACE(glIndexi); set_index(c); }
void glp_impl_glIndexs(GLshort c) { TRACE(glIndexs); set_index(c); }
void glp_impl_glIndexub(GLubyte c) { TRACE(glIndexub); set_index(c); }
void glp_impl_glIndexdv(const GLdouble *c) { TRACE(glIndexdv); set_index(c[0]); }
void glp_impl_glIndexfv(const GLfloat *c) { TRACE(glIndexfv); set_index(c[0]); }
void glp_impl_glIndexiv(const GLint *c) { TRACE(glIndexiv); set_index(c[0]); }
void glp_impl_glIndexsv(const GLshort *c) { TRACE(glIndexsv); set_index(c[0]); }
void glp_impl_glIndexubv(const GLubyte *c) { TRACE(glIndexubv); set_index(c[0]); }

void glp_impl_glClearIndex(GLfloat c)
{
    GLXContext current = current_context();
    unsigned int i;
    TRACE(glClearIndex);
    if (!current || !current->index_mode) return;
    i = (unsigned int)(long)c % PALETTE_SIZE;
    index_target_state(&current->ix)->clear = i;
    index_note(&current->ix, INDEX_CHANGED_CLEAR);
    glClearColor((GLfloat)GLP_INDEX_R(i) / 255.0f, (GLfloat)GLP_INDEX_G(i) / 255.0f, (GLfloat)GLP_INDEX_B(i) / 255.0f,
                 (GLfloat)GLP_INDEX_A(i) / 255.0f);
}

/* In a colour-index context, GL_INDEX_LOGIC_OP (GL_LOGIC_OP) acts on the
   stored indices: the host's GL_COLOR_LOGIC_OP, which the index state
   drives. */
static void put_cap(unsigned int fn, GLenum cap)
{
    GLXContext current = current_context();
    if (cap == GL_INDEX_LOGIC_OP && current && current->index_mode) {
        index_target_state(&current->ix)->logic = fn == GLP_FN_glEnable;
        index_changed(current, INDEX_CHANGED_LOGIC);
        return;
    }
    put_word(fn, cap);
}

void glp_impl_glEnable(GLenum cap)
{
    TRACE(glEnable);
    put_cap(GLP_FN_glEnable, cap);
}

void glp_impl_glDisable(GLenum cap)
{
    TRACE(glDisable);
    put_cap(GLP_FN_glDisable, cap);
}

void glp_impl_glLogicOp(GLenum opcode)
{
    GLXContext current = current_context();
    TRACE(glLogicOp);
    if (!current || !current->index_mode) {
        put_word(GLP_FN_glLogicOp, opcode);
        return;
    }
    index_target_state(&current->ix)->op = opcode;
    index_changed(current, INDEX_CHANGED_OP);
}

void glp_impl_glIndexMask(GLuint mask)
{
    GLXContext current = current_context();
    TRACE(glIndexMask);
    if (!current || !current->index_mode) return;
    glx_log("glIndexMask(%#x) context %u", mask, current->id);
    index_target_state(&current->ix)->mask = mask;
    index_changed(current, INDEX_CHANGED_MASK);
}

/* glClear ignores the logic op but obeys the index mask. A mask of whole
   channels clears through the colour mask; another is cleared exactly by a
   window-sized rectangle per pass (see index_passes). */
void glp_impl_glClear(GLbitfield mask)
{
    GLXContext c = current_context();
    struct glp_index *ix;
    struct glp_index_state copy;
    struct glp_index_host want;
    struct glp_index_pass pass[3];
    unsigned int *p;
    int n, k;
    TRACE(glClear);
    if (c && (mask & GL_COLOR_BUFFER_BIT) && !index_compiling(&c->ix)) c->frame_cleared = 1;
    if (!c || !c->index_mode || !(mask & GL_COLOR_BUFFER_BIT) || c->ix.list || c->render_mode != GL_RENDER) {
        put_word(GLP_FN_glClear, mask);
        return;
    }
    ix = &c->ix;
    copy = ix->state;
    copy.logic = 0; /* a clear copies the clear index */
    n = index_passes(&copy, ix->state.clear % PALETTE_SIZE, &want, pass);
    if (!want.logic) { /* whole channels: a plain clear through the colour mask */
        if (!ix->host.known || ix->host.channels != want.channels) put_channels(want.channels);
        put_word(GLP_FN_glClear, mask);
        if (ix->host.known && ix->host.channels != want.channels) put_channels(ix->host.channels);
        if (!ix->host.known) index_update(c);
        return;
    }
    if (mask & ~GL_COLOR_BUFFER_BIT) put_word(GLP_FN_glClear, mask & ~GL_COLOR_BUFFER_BIT);
    put_word(GLP_FN_glPushAttrib, GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT | GL_POLYGON_BIT |
                                      GL_VIEWPORT_BIT | GL_TRANSFORM_BIT);
    {
        static const GLenum off[] = {GL_DEPTH_TEST, GL_STENCIL_TEST, GL_ALPHA_TEST, GL_BLEND, GL_DITHER,
                                     GL_LIGHTING, GL_FOG, GL_TEXTURE_1D, GL_TEXTURE_2D, GL_CULL_FACE,
                                     GL_POLYGON_STIPPLE, GL_CLIP_PLANE0, GL_CLIP_PLANE1, GL_CLIP_PLANE2,
                                     GL_CLIP_PLANE3, GL_CLIP_PLANE4, GL_CLIP_PLANE5};
        unsigned int i;
        for (i = 0; i < sizeof off / sizeof off[0]; ++i) put_word(GLP_FN_glDisable, off[i]);
    }
    put_word(GLP_FN_glEnable, GL_COLOR_LOGIC_OP);
    put_channels(0xFu);
    p = glp_put(GLP_FN_glPolygonMode, 2);
    p[0] = GL_FRONT_AND_BACK; p[1] = GL_FILL;
    p = glp_put(GLP_FN_glViewport, 4);
    p[0] = 0; p[1] = 0; p[2] = c->width ? c->width : 1; p[3] = c->height ? c->height : 1;
    put_word(GLP_FN_glMatrixMode, GL_PROJECTION);
    glp_put(GLP_FN_glPushMatrix, 0);
    glp_put(GLP_FN_glLoadIdentity, 0);
    put_word(GLP_FN_glMatrixMode, GL_MODELVIEW);
    glp_put(GLP_FN_glPushMatrix, 0);
    glp_put(GLP_FN_glLoadIdentity, 0);
    for (k = 0; k < n; ++k) {
        put_word(GLP_FN_glLogicOp, pass[k].op);
        put_index_color(pass[k].value);
        p = glp_put(GLP_FN_glRectf, 4);
        p[0] = glp_fbits(-1.0f); p[1] = glp_fbits(-1.0f); p[2] = glp_fbits(1.0f); p[3] = glp_fbits(1.0f);
    }
    glp_put(GLP_FN_glPopMatrix, 0);
    put_word(GLP_FN_glMatrixMode, GL_PROJECTION);
    glp_put(GLP_FN_glPopMatrix, 0);
    glp_put(GLP_FN_glPopAttrib, 0); /* back to the index state's host setting, matrix mode included */
}

/* The index state is attribute state: GL_COLOR_BUFFER_BIT (mask, logic op,
   clear index), GL_ENABLE_BIT (logic op on/off), GL_CURRENT_BIT (index). */
void glp_impl_glPushAttrib(GLbitfield mask)
{
    GLXContext c = current_context();
    TRACE(glPushAttrib);
    put_word(GLP_FN_glPushAttrib, mask);
    if (!c || !c->index_mode || c->ix.list) return;
    if (c->ix.depth < INDEX_STACK) {
        c->ix.stack[c->ix.depth].bits = mask;
        c->ix.stack[c->ix.depth].state = c->ix.state;
        c->ix.stack[c->ix.depth].host = c->ix.host;
    }
    ++c->ix.depth;
}

void glp_impl_glPopAttrib(void)
{
    GLXContext c = current_context();
    struct glp_index *ix;
    TRACE(glPopAttrib);
    glp_put(GLP_FN_glPopAttrib, 0);
    if (!c || !c->index_mode) return;
    ix = &c->ix;
    if (ix->list) { /* inside a list: its effect is unknown */
        ix->host.known = 0;
        return;
    }
    if (ix->depth <= 0) return;
    if (--ix->depth >= INDEX_STACK) {
        ix->host.known = 0;
        return;
    }
    {
        const GLbitfield bits = ix->stack[ix->depth].bits;
        const struct glp_index_state *saved = &ix->stack[ix->depth].state;
        const struct glp_index_host *host = &ix->stack[ix->depth].host;
        if (bits & GL_COLOR_BUFFER_BIT) {
            ix->state.mask = saved->mask;
            ix->state.op = saved->op;
            ix->state.clear = saved->clear;
            ix->state.logic = saved->logic;
            ix->host.op = host->op;
            ix->host.channels = host->channels;
            ix->host.logic = host->logic;
        } else if (bits & GL_ENABLE_BIT) {
            ix->state.logic = saved->logic;
            ix->host.logic = host->logic;
        }
        if (bits & GL_CURRENT_BIT) ix->state.index = saved->index;
        if (bits & GL_DEPTH_BUFFER_BIT) ix->state.depth_func = saved->depth_func;
        ix->host.known = ix->host.known && host->known;
        if (bits & (GL_COLOR_BUFFER_BIT | GL_ENABLE_BIT | GL_CURRENT_BIT)) index_update(c);
    }
}

/* ---- Display lists ---------------------------------------------------------------

   A list compiled in a colour index context holds the plain translation (its
   colours are the indices' encodings, the colour mask covers whole
   channels), and its index state changes are noted (glp_list.effect) for
   when it runs. Its commands are recorded too, as its shadow: when the
   state takes several passes, glCallList sends the shadow's commands (and
   those of the lists it calls) as a recorded draw, colours rewritten for
   each pass, instead of the call (list_expand). A list whose commands the
   passes cannot repeat runs as a call (list_scan). The records live in the
   share group's table (struct glp_lists), under the library lock; a shadow
   in use stays until its last user lets it go. */

static struct glp_lists *lists_of(GLXContext c)
{
    if (!c->ix.lists) {
        c->ix.lists = (struct glp_lists *)calloc(1, sizeof *c->ix.lists);
        if (c->ix.lists) c->ix.lists->refs = 1;
    }
    return c->ix.lists;
}

static struct glp_list *list_find(struct glp_lists *ls, GLuint id)
{
    struct glp_list *l;
    for (l = ls->bucket[id % LIST_BUCKETS]; l && l->id != id; l = l->next) {
    }
    return l;
}

static void list_unref(struct glp_lists *ls, struct glp_list *l)
{
    if (--l->refs > 0) return;
    ls->words -= l->nwords;
    free(l->words);
    free(l->events);
    free(l);
}

/* Takes `id` out of the table (lock held). */
static void list_remove(struct glp_lists *ls, GLuint id)
{
    struct glp_list **pp;
    for (pp = &ls->bucket[id % LIST_BUCKETS]; *pp; pp = &(*pp)->next) {
        if ((*pp)->id == id) {
            struct glp_list *l = *pp;
            *pp = l->next;
            list_unref(ls, l);
            return;
        }
    }
}

static void lists_release(struct glp_lists *ls)
{
    int b;
    if (!ls) return;
    glp_lock();
    if (--ls->refs > 0) {
        glp_unlock();
        return;
    }
    for (b = 0; b < LIST_BUCKETS; ++b) {
        while (ls->bucket[b]) list_remove(ls, ls->bucket[b]->id);
    }
    glp_unlock();
    free(ls);
}

/* Record `at` of `words`: its command and payload length; 0 when it runs
   past `n`. */
static unsigned int record_at(const unsigned int *words, unsigned int n, unsigned int at, unsigned int *fn,
                              unsigned int *header)
{
    unsigned int len;
    if (at >= n) return 0;
    *fn = words[at] >> 16;
    len = words[at] & 0xFFFFu;
    *header = 1;
    if (len == GLP_LONG_RECORD) {
        if (at + 1 >= n) return 0;
        len = words[at + 1];
        *header = 2;
    }
    if (len > n - at - *header) return 0;
    return *header + len;
}

/* Whether the passes can repeat the list's commands, and its calls as
   events, merged in order with its colours. The passes set the logic op,
   the colour mask and the depth function, so a list setting them runs as
   a call; so does one drawing with the raster colour, which was set before
   (glBitmap, glDrawPixels, glCopyPixels), one clearing (a clear ignores the
   logic op), or one changing the list base. Each pass starts from the
   matrix the lists started from (list_expand pushes it), so a list may
   move it (a glyph's advance), but not change the matrix mode, and its
   matrix and attribute pushes and pops must pair up. */
static int list_scan(struct glp_list *l)
{
    struct glp_event *events = 0;
    unsigned int capevents = 0, nevents = 0, at = 0, j = 0, fn, header, len;
    int depth = 0, matrices = 0, ok = 1;
    while (ok && at < l->nwords) {
        len = record_at(l->words, l->nwords, at, &fn, &header);
        if (len == 0) {
            ok = 0;
            break;
        }
        switch (fn) {
        case GLP_FN_glBitmap: case GLP_FN_glDrawPixels: case GLP_FN_glCopyPixels: case GLP_FN_glClear:
        case GLP_FN_glAccum: case GLP_FN_glListBase: case GLP_FN_glLogicOp: case GLP_FN_glColorMask:
        case GLP_FN_glDepthFunc: case GLP_FN_glMatrixMode:
            ok = 0;
            l->refused = fn;
            break;
        case GLP_FN_glPushMatrix: ++matrices; break;
        case GLP_FN_glPopMatrix: if (--matrices < 0) ok = 0, l->refused = fn; break;
        case GLP_FN_glEnable: case GLP_FN_glDisable:
            if (len > header && l->words[at + header] == GL_COLOR_LOGIC_OP) ok = 0, l->refused = fn;
            break;
        case GLP_FN_glPushAttrib: ++depth; break;
        case GLP_FN_glPopAttrib: if (--depth < 0) ok = 0, l->refused = fn; break;
        case GLP_FN_glCallList: case GLP_FN_glCallLists: {
            struct glp_event *e;
            if (len <= header) {
                ok = 0;
                break;
            }
            for (; j < l->nevents && l->events[j].at < at; ++j) { /* the colours before it */
                if (!(e = (struct glp_event *)grown(events, &capevents, nevents + 1, sizeof *e))) break;
                events = e;
                events[nevents++] = l->events[j];
            }
            if (j < l->nevents && l->events[j].at < at) {
                ok = 0;
                break;
            }
            if (!(e = (struct glp_event *)grown(events, &capevents, nevents + 1, sizeof *e))) {
                ok = 0;
                break;
            }
            events = e;
            e += nevents++;
            e->at = at;
            e->len = len;
            e->kind = fn == GLP_FN_glCallList ? EV_CALL : EV_CALLS;
            e->value = fn == GLP_FN_glCallList ? l->words[at + header] : header;
            break;
        }
        default:
            break;
        }
        at += len;
    }
    if (ok && (depth != 0 || matrices != 0)) {
        ok = 0;
        l->refused = depth != 0 ? GLP_FN_glPushAttrib : GLP_FN_glPushMatrix;
    }
    for (; ok && j < l->nevents; ++j) {
        struct glp_event *e = (struct glp_event *)grown(events, &capevents, nevents + 1, sizeof *e);
        if (!e) {
            ok = 0;
            break;
        }
        events = e;
        events[nevents++] = l->events[j];
    }
    if (!ok) {
        free(events);
        return 0;
    }
    free(l->events);
    l->events = events;
    l->nevents = nevents;
    return 1;
}

/* The ids a glCallLists record of a shadow calls: `base` plus its values. */
static unsigned int calls_count(const struct glp_list *l, const struct glp_event *e)
{
    const unsigned int n = l->words[e->at + e->value];
    return n < e->len - e->value ? n : e->len - e->value - 1;
}

static GLuint calls_id(const struct glp_list *l, const struct glp_event *e, GLuint base, unsigned int k)
{
    return base + l->words[e->at + e->value + 1 + k];
}

struct glp_need {
    struct glp_list **lists;
    unsigned int n, cap;
    GLuint refused_list; /* the list that cannot be expanded, and why (trace) */
    unsigned int refused;
};

/* Collects list `id` and those it calls, all expandable, into `need`, a
   reference taken on each (lock held). 0 when one is missing (compiled
   before the context was in colour index mode, or never), is not
   expandable, or calls itself. */
static int list_collect(struct glp_lists *ls, GLuint base, GLuint id, struct glp_need *need, int depth)
{
    struct glp_list *l = list_find(ls, id), **grown_lists;
    unsigned int j, k;
    if (!l || !l->expandable || depth > LIST_NESTING) {
        need->refused_list = id;
        need->refused = !l ? 0xFFFFu : depth > LIST_NESTING ? GLP_FN_glCallList : l->refused;
        return 0;
    }
    if (l->mark == ls->mark) return l->visit; /* 0: on the walk's path, a cycle */
    l->mark = ls->mark;
    l->visit = 0;
    for (j = 0; j < l->nevents; ++j) {
        const struct glp_event *e = &l->events[j];
        if (e->kind == EV_CALL && !list_collect(ls, base, e->value, need, depth + 1)) return 0;
        if (e->kind == EV_CALLS) {
            const unsigned int n = calls_count(l, e);
            for (k = 0; k < n; ++k) {
                if (!list_collect(ls, base, calls_id(l, e, base, k), need, depth + 1)) return 0;
            }
        }
    }
    if (!(grown_lists = (struct glp_list **)grown(need->lists, &need->cap, need->n + 1, sizeof *need->lists))) {
        return 0;
    }
    need->lists = grown_lists;
    need->lists[need->n++] = l;
    ++l->refs;
    l->visit = 1;
    return 1;
}

static struct glp_list *need_find(const struct glp_need *need, GLuint id)
{
    unsigned int k;
    for (k = 0; k < need->n; ++k) {
        if (need->lists[k]->id == id) return need->lists[k];
    }
    return 0;
}

/* Sends whole records of a shadow, `words` from its offset `offset`, with
   their colours for the first pass, noted in the draw's recording. */
static void list_emit_records(GLXContext c, const struct glp_list *l, unsigned int offset, unsigned int n,
                              unsigned int *j)
{
    struct glp_thread *t = glp_self();
    const unsigned int limit = t->cmdcap / 2 > 4096 ? t->cmdcap / 2 : 4096;
    unsigned int done = 0, fn, header;
    while (done < n) {
        unsigned int len = 0, *dst;
        while (done + len < n) { /* whole records, about half a buffer at most */
            const unsigned int rl = record_at(l->words + offset, n, done + len, &fn, &header);
            if (rl == 0) return;
            if (len > 0 && len + rl > limit) break;
            len += rl;
        }
        put_raw(t, l->words + offset + done, len);
        dst = t->cmdbuf + t->cmdlen - len;
        for (; *j < l->nevents && l->events[*j].at < offset + done + len; ++*j) {
            const struct glp_event *e = &l->events[*j];
            unsigned int *w = dst + (e->at - offset - done);
            if (e->kind == EV_COLOR4) encode_index(w, index_value(c, e->value));
            else if (e->kind == EV_PACKED) *w = packed_index(index_value(c, e->value));
            else continue;
            index_event(c, w, e->kind, e->value);
        }
        done += len;
    }
}

/* Sends list `id`'s commands, and those of the lists it calls, in place of
   the call. */
static void list_emit(GLXContext c, const struct glp_need *need, GLuint base, GLuint id, int depth)
{
    const struct glp_list *l = need_find(need, id);
    unsigned int at = 0, j = 0, k;
    if (!l || depth > LIST_NESTING) return;
    while (at < l->nwords) {
        unsigned int stop = l->nwords, next;
        for (next = j; next < l->nevents; ++next) {
            if (l->events[next].kind == EV_CALL || l->events[next].kind == EV_CALLS) {
                stop = l->events[next].at;
                break;
            }
        }
        list_emit_records(c, l, at, stop - at, &j);
        if (stop == l->nwords) break;
        {
            const struct glp_event *e = &l->events[next];
            j = next + 1;
            if (e->kind == EV_CALL) {
                list_emit(c, need, base, e->value, depth + 1);
            } else {
                const unsigned int n = calls_count(l, e);
                for (k = 0; k < n; ++k) list_emit(c, need, base, calls_id(l, e, base, k), depth + 1);
            }
            at = e->at + e->len;
        }
    }
}

static void index_after_lists(GLXContext c, const GLuint *ids, GLsizei n);

/* Runs lists `ids` as a recorded draw, once per pass, when the index state
   takes several. Returns 0 when it cannot: the caller sends the call. */
static int list_expand(GLXContext c, const GLuint *ids, GLsizei n)
{
    struct glp_index *ix = &c->ix;
    struct glp_need need;
    GLsizei k;
    unsigned int m;
    int ok = 1;
    if (!c->index_mode || ix->list || ix->group.active || ix->npasses < 2 || !ix->host.known ||
        c->render_mode != GL_RENDER || !ix->lists) {
        return 0;
    }
    memset(&need, 0, sizeof need);
    glp_lock();
    ++ix->lists->mark;
    for (k = 0; ok && k < n; ++k) ok = list_collect(ix->lists, c->list_base, ids[k], &need, 1);
    glp_unlock();
    /* The shadows collected stay while this thread uses them, even if
       another thread deletes or redefines their lists meanwhile. */
    if (ok) {
        glp_put(GLP_FN_glPushMatrix, 0); /* each pass starts from this matrix (a glyph list moves it) */
        if (!passes_begin(c)) {
            glp_put(GLP_FN_glPopMatrix, 0);
            ok = 0;
        }
    }
    if (ok) {
        for (k = 0; k < n; ++k) list_emit(c, &need, c->list_base, ids[k], 1);
        index_after_lists(c, ids, n); /* their glIndex calls */
        passes_finish(c, 1);
    }
    glp_lock();
    for (m = 0; m < need.n; ++m) list_unref(ix->lists, need.lists[m]);
    glp_unlock();
    free(need.lists);
    if (!ok) {
        static GLuint logged[16];
        static unsigned int nlogged;
        unsigned int i;
        index_log_approximate(&ix->state, "display list");
        for (i = 0; i < nlogged && logged[i] != need.refused_list; ++i) {
        }
        if (i == nlogged && nlogged < 16) {
            logged[nlogged++] = need.refused_list;
            glx_log("display list %u runs in one pass: %s %#x", need.refused_list,
                    need.refused == 0xFFFFu ? "no record of it" : need.refused ? "command" : "too large to keep",
                    need.refused);
        }
    }
    return ok;
}

void glp_impl_glNewList(GLuint list, GLenum mode)
{
    GLXContext c = current_context();
    unsigned int *p;
    TRACE(glNewList);
    p = glp_put(GLP_FN_glNewList, 2);
    p[0] = list;
    p[1] = mode;
    if (!c || !c->index_mode) return;
    c->ix.list = list;
    c->ix.list_mode = mode;
    c->ix.compiling.list = list;
    c->ix.compiling.changed = 0;
    c->ix.compiling.state = c->ix.state;
    recording_start(&c->ix.compile, glp_self(), LIST_MAX_WORDS);
}

void glp_impl_glEndList(void)
{
    GLXContext c = current_context();
    struct glp_index *ix;
    struct glp_lists *ls;
    struct glp_list *l;
    TRACE(glEndList);
    if (!c || !c->index_mode || !c->ix.list) {
        glp_put(GLP_FN_glEndList, 0);
        return;
    }
    ix = &c->ix;
    recording_keep(&ix->compile, glp_self()); /* the list's commands, without the glEndList */
    ix->compile.active = 0;
    glp_put(GLP_FN_glEndList, 0);
    l = (struct glp_list *)calloc(1, sizeof *l);
    glp_lock();
    ls = lists_of(c);
    if (l && ls) {
        l->id = ix->list;
        l->refs = 1;
        l->effect = ix->compiling;
        if (!ix->compile.failed && ls->words + ix->compile.nwords <= LISTS_MAX_WORDS) {
            /* The recording's buffers become the shadow's. */
            l->words = ix->compile.words;
            l->nwords = ix->compile.nwords;
            l->events = ix->compile.events;
            l->nevents = ix->compile.nevents;
            ix->compile.words = 0;
            ix->compile.capwords = 0;
            ix->compile.events = 0;
            ix->compile.capevents = 0;
            l->expandable = list_scan(l);
            if (!l->expandable) { /* runs as a call: the shadow is not needed */
                free(l->words);
                free(l->events);
                l->words = 0;
                l->events = 0;
                l->nwords = 0;
                l->nevents = 0;
            }
            ls->words += l->nwords;
        }
        list_remove(ls, l->id); /* a redefinition */
        l->next = ls->bucket[l->id % LIST_BUCKETS];
        ls->bucket[l->id % LIST_BUCKETS] = l;
    } else {
        free(l);
        if (ls) list_remove(ls, ix->list); /* its old record would be wrong */
    }
    glp_unlock();
    recording_trim(&ix->compile);
    ix->list = 0;
    ix->list_mode = 0;
    if (!ix->host.known) index_update(c);
}

void glp_impl_glDeleteLists(GLuint list, GLsizei range)
{
    GLXContext c = current_context();
    unsigned int *p;
    TRACE(glDeleteLists);
    p = glp_put(GLP_FN_glDeleteLists, 2);
    p[0] = list;
    p[1] = (unsigned int)range;
    if (!c || !c->ix.lists || range <= 0) return;
    glp_lock();
    {
        struct glp_lists *ls = c->ix.lists;
        int b;
        for (b = 0; b < LIST_BUCKETS; ++b) {
            struct glp_list *l = ls->bucket[b], *next;
            for (; l; l = next) {
                next = l->next;
                if (l->id >= list && l->id - list < (GLuint)range) list_remove(ls, l->id);
            }
        }
    }
    glp_unlock();
}

/* A call compiled into a list: the called lists' index state changes
   become the list's own (as they are now; GL looks the lists up when the
   list runs). With GL_COMPILE_AND_EXECUTE they apply now too. */
static void index_compile_calls(GLXContext c, const GLuint *ids, GLsizei n)
{
    struct glp_index *ix = &c->ix;
    GLsizei k;
    if (!ix->lists) return;
    glp_lock();
    for (k = 0; k < n; ++k) {
        const struct glp_list *l = list_find(ix->lists, ids[k]);
        struct glp_index_state *st = index_target_state(ix);
        const struct glp_list_effect *e;
        if (!l || !l->effect.changed) continue;
        e = &l->effect;
        if (e->changed & INDEX_CHANGED_MASK) st->mask = e->state.mask;
        if (e->changed & INDEX_CHANGED_LOGIC) st->logic = e->state.logic;
        if (e->changed & INDEX_CHANGED_OP) st->op = e->state.op;
        if (e->changed & INDEX_CHANGED_INDEX) st->index = e->state.index;
        if (e->changed & INDEX_CHANGED_CLEAR) st->clear = e->state.clear;
        if (e->changed & INDEX_CHANGED_DEPTH) st->depth_func = e->state.depth_func;
        index_note(ix, e->changed);
    }
    glp_unlock();
}

/* After a list ran: its index state changes apply, and the host state it
   left (the plain translation) is brought back in line. */
static void index_after_lists(GLXContext c, const GLuint *ids, GLsizei n)
{
    struct glp_index *ix = &c->ix;
    GLsizei k;
    if (!ix->lists) return;
    glp_lock();
    for (k = 0; k < n; ++k) {
        const struct glp_list *l = list_find(ix->lists, ids[k]);
        const struct glp_list_effect *e;
        if (!l || !l->effect.changed) continue;
        e = &l->effect;
        if (e->changed & INDEX_CHANGED_MASK) ix->state.mask = e->state.mask;
        if (e->changed & INDEX_CHANGED_LOGIC) ix->state.logic = e->state.logic;
        if (e->changed & INDEX_CHANGED_OP) ix->state.op = e->state.op;
        if (e->changed & INDEX_CHANGED_INDEX) ix->state.index = e->state.index;
        if (e->changed & INDEX_CHANGED_CLEAR) ix->state.clear = e->state.clear;
        if (e->changed & INDEX_CHANGED_DEPTH) ix->state.depth_func = e->state.depth_func;
        ix->host.known = 0;
    }
    glp_unlock();
}

/* Lists draw with the plain translation: before one runs, the host gets it
   for the current state (unless that is what it has). */
static void index_before_lists(GLXContext c)
{
    struct glp_index *ix = &c->ix;
    struct glp_index_host want;
    int exact;
    if (ix->list) return;
    want.known = 1;
    want.channels = index_channels(ix->state.mask, &exact);
    want.logic = ix->state.logic;
    want.op = ix->state.op;
    if (ix->host.known && ix->host.channels == want.channels && ix->host.logic == want.logic &&
        ix->host.op == want.op) {
        return;
    }
    index_apply(ix, &want);
    ix->npasses = 1; /* lists draw once, with the plain translation */
    put_index_color(ix->state.index % PALETTE_SIZE);
}

void glp_impl_glCallList(GLuint list)
{
    GLXContext c = current_context();
    TRACE(glCallList);
    if (c && c->index_mode && list_expand(c, &list, 1)) return;
    if (c && c->index_mode) index_before_lists(c);
    put_word(GLP_FN_glCallList, list);
    if (c && c->index_mode && c->ix.list) index_compile_calls(c, &list, 1);
    if (c && c->index_mode && !c->ix.list) {
        index_after_lists(c, &list, 1);
        index_update(c);
    }
}

void glp_impl_glDepthFunc(GLenum func)
{
    GLXContext c = current_context();
    TRACE(glDepthFunc);
    put_word(GLP_FN_glDepthFunc, func);
    if (!c || !c->index_mode) return;
    index_target_state(&c->ix)->depth_func = func;
    index_note(&c->ix, INDEX_CHANGED_DEPTH);
}

/* ---- GLX drawables ------------------------------------------------------------------- */

enum { DRAW_PIXMAP = 1, DRAW_PBUFFER };

/* Pixmaps and pbuffers; anything else is taken to be a window. */
struct glp_drawable {
    XID id;
    int kind;
    unsigned int width, height;
    VisualID visual;
    struct glp_drawable *next;
};

static struct glp_drawable *drawables;
static XID next_pbuffer = 0x7F000000; /* pbuffers have no X resource: ids from a range X does not use */

/* The list is under the library lock; an entry lives until the program
   destroys its pixmap or pbuffer, which no other thread draws to then. */
static struct glp_drawable *find_drawable(XID id)
{
    struct glp_drawable *d;
    glp_lock();
    for (d = drawables; d; d = d->next) {
        if (d->id == id) break;
    }
    glp_unlock();
    return d;
}

static struct glp_drawable *add_drawable(XID id, int kind)
{
    struct glp_drawable *d = (struct glp_drawable *)calloc(1, sizeof *d);
    if (!d) return 0;
    d->id = id;
    d->kind = kind;
    glp_lock();
    d->next = drawables;
    drawables = d;
    glp_unlock();
    return d;
}

static void remove_drawable(XID id)
{
    struct glp_drawable **pp;
    glp_lock();
    for (pp = &drawables; *pp; pp = &(*pp)->next) {
        if ((*pp)->id == id) {
            struct glp_drawable *d = *pp;
            *pp = d->next;
            free(d);
            break;
        }
    }
    glp_unlock();
}

/* ---- X errors ---------------------------------------------------------------------------- */

/* The X error handler is one per process. This library's requests fail when
   a program destroys a window it still draws to (Houdini's License
   Administrator leaves a context current on its destroyed splash window),
   and those errors must not reach the program: its handler would take them
   for its own, and Xt's default one exits. So the process's handler is
   glp_x_error, which keeps the errors this library caused, everything on its
   own connection and the few requests it sends on the program's, and passes
   the rest to the program's handler. libglpass_x routes the program's
   XSetErrorHandler calls to glpass_set_error_handler, so a handler the
   program installs later does not displace this one. A failed request on a
   window marks the window destroyed (see note_dead). */
typedef int (*x_error_handler)(Display *, XErrorEvent *);

static x_error_handler program_handler; /* the program's (Xlib's default when it set none) */
static x_error_handler xlib_default;
static int handler_installed;
static int x_error_code;                /* the last error on this library's connection */

/* Requests this library sent on the program's connection (the overlay key
   fill), by serial: their errors are this library's too. */
#define APP_REQUESTS 8
static struct {
    Display *dpy;
    unsigned long first, last;
} app_requests[APP_REQUESTS];
static int next_app_request;

static void note_app_requests(Display *dpy, unsigned long first, unsigned long last)
{
    const int k = next_app_request;
    app_requests[k].dpy = 0;
    app_requests[k].first = first;
    app_requests[k].last = last;
    app_requests[k].dpy = dpy;
    next_app_request = (k + 1) % APP_REQUESTS;
}

/* Destroyed windows, written without a lock (the handler runs inside Xlib,
   possibly in a thread holding the program's display lock while another
   holds the library lock): a ring the readers scan. */
#define DEAD_MAX 64
static volatile unsigned long dead_windows[DEAD_MAX];
static volatile int next_dead;

static void note_dead(unsigned long w)
{
    int k;
    if (!w) return;
    for (k = 0; k < DEAD_MAX; ++k) {
        if (dead_windows[k] == w) return;
    }
    k = next_dead;
    next_dead = (k + 1) % DEAD_MAX;
    dead_windows[k] = w;
}

static int window_dead(unsigned long w)
{
    int k;
    for (k = 0; w && k < DEAD_MAX; ++k) {
        if (dead_windows[k] == w) return 1;
    }
    return 0;
}

/* libX11's XSetErrorHandler, past libglpass_x's. */
static x_error_handler x11_set_error_handler(x_error_handler handler)
{
    static x_error_handler (*real)(x_error_handler);
    if (!real) {
        void *x11 = dlopen("libX11.so.1", RTLD_LAZY);
        if (!x11) x11 = dlopen("libX11.so", RTLD_LAZY);
        if (x11) real = (x_error_handler (*)(x_error_handler))dlsym(x11, "XSetErrorHandler");
        if (!real) real = XSetErrorHandler;
    }
    return real(handler);
}

static int ours(Display *dpy, const XErrorEvent *e)
{
    int k;
    if (own_display && dpy == own_display) return 1;
    for (k = 0; k < APP_REQUESTS; ++k) {
        if (app_requests[k].dpy == dpy && e->serial >= app_requests[k].first && e->serial <= app_requests[k].last) {
            return 1;
        }
    }
    return 0;
}

static int glp_x_error(Display *dpy, XErrorEvent *e)
{
    if (ours(dpy, e)) {
        if (dpy == own_display) x_error_code = e->error_code;
        if (e->error_code == BadWindow || e->error_code == BadDrawable) note_dead(e->resourceid);
        glx_log("X error %d (request %d.%d) on %#lx, from this library: ignored", (int)e->error_code,
                (int)e->request_code, (int)e->minor_code, (unsigned long)e->resourceid);
        return 0;
    }
    return (program_handler ? program_handler : xlib_default)(dpy, e);
}

static void install_error_handler(void)
{
    x_error_handler previous;
    if (handler_installed) return;
    handler_installed = 1;
    previous = x11_set_error_handler(0);           /* the program's, and Xlib's default in its place */
    xlib_default = x11_set_error_handler(glp_x_error);
    if (!program_handler && previous != glp_x_error) program_handler = previous;
    if (!program_handler) program_handler = xlib_default;
}

/* The program's XSetErrorHandler (through libglpass_x): its handler gets
   the errors that are not this library's. */
x_error_handler glpass_set_error_handler(x_error_handler handler)
{
    x_error_handler old;
    if (glp_forwarding()) { /* IRIX's OpenGL: the program's handler, as it is */
        static x_error_handler (*real)(x_error_handler);
        if (!real) {
            void *x11 = dlopen("libX11.so.1", RTLD_LAZY);
            if (!x11) x11 = dlopen("libX11.so", RTLD_LAZY);
            if (x11) real = (x_error_handler (*)(x_error_handler))dlsym(x11, "XSetErrorHandler");
        }
        return real ? real(handler) : 0;
    }
    install_error_handler();
    old = program_handler;
    program_handler = handler ? handler : xlib_default;
    return old;
}

/* libglpass_x's XDestroyWindow: nothing more is shown in, or asked of, the window. */
void glpass_window_destroyed(unsigned long w) { note_dead(w); }

/* libglpass_x's XCreateWindow: the ID is a live window again. The server
   reuses a closed connection's IDs for a new one, so a window can get the
   ID of one destroyed earlier. */
void glpass_window_created(unsigned long w)
{
    int k;
    for (k = 0; w && k < DEAD_MAX; ++k) {
        if (dead_windows[k] == w) dead_windows[k] = 0;
    }
}

/* Requests whose errors are reported, not raised: on this library's
   connection, the handler keeps them; on the program's (no second
   connection), a handler of the moment does. */
static int note_x_error(Display *dpy, XErrorEvent *event)
{
    (void)dpy;
    x_error_code = event->error_code;
    return 0;
}

static x_error_handler errors_begin(Display *dpy)
{
    XSync(dpy, False);
    x_error_code = 0;
    return dpy == own_display ? 0 : x11_set_error_handler(note_x_error);
}

static void errors_end(Display *dpy, x_error_handler previous)
{
    XSync(dpy, False);
    if (dpy != own_display) x11_set_error_handler(previous);
}

/* XGetGeometry that reports, rather than raises, an X error. */
static Status checked_geometry(Display *dpy, Drawable d, Window *root, int *x, int *y, unsigned int *w,
                               unsigned int *h, unsigned int *border, unsigned int *depth)
{
    x_error_handler previous;
    Status ok;
    previous = errors_begin(dpy);
    ok = XGetGeometry(dpy, d, root, x, y, w, h, border, depth);
    errors_end(dpy, previous);
    if (!ok || x_error_code) {
        glx_log("XGetGeometry(%#lx) failed: status %d, X error %d", (unsigned long)d, (int)ok, x_error_code);
        return 0;
    }
    return ok;
}

/* Size, and position on screen; returns 0 for drawables not on screen. */
static int drawable_geometry_unlocked(Display *dpy, GLXDrawable d, int *sx, int *sy, unsigned int *w,
                                      unsigned int *h)
{
    struct glp_drawable *pd = find_drawable(d);
    Window root, child;
    int x, y;
    unsigned int border, depth;
    *sx = 0;
    *sy = 0;
    if (pd && pd->kind == DRAW_PBUFFER) {
        *w = pd->width;
        *h = pd->height;
        return 0;
    }
    if (!checked_geometry(dpy, d, &root, &x, &y, w, h, &border, &depth)) {
        *w = *h = 1;
        return 0;
    }
    if (pd) return 0;
    XTranslateCoordinates(dpy, d, root, 0, 0, sx, sy, &child);
    return 1;
}

/* Size, and position on screen; returns 0 for drawables not on screen. */
static int drawable_geometry(Display *dpy, GLXDrawable d, int *sx, int *sy, unsigned int *w, unsigned int *h)
{
    int on_screen;
    glp_lock();
    on_screen = drawable_geometry_unlocked(xconn(dpy), d, sx, sy, w, h);
    glp_unlock();
    return on_screen;
}

static int mask_shift(unsigned long mask, int *bits)
{
    int shift = 0;
    *bits = 0;
    if (!mask) return 0;
    while (!(mask >> shift & 1)) ++shift;
    while (mask >> (shift + *bits) & 1) ++*bits;
    return shift;
}

/* Copies a pixmap drawable's host image into the X pixmap. */
static void update_pixmap_unlocked(struct glp_drawable *pd)
{
    GLXContext current = current_context();
    Display *dpy = current ? xconn(current->dpy) : 0;
    XVisualInfo templ, *vi;
    XImage *image;
    unsigned char *rgba;
    unsigned int x, y, w = pd->width, h = pd->height, a[2];
    int n = 0, rs, gs, bs, rbits, gbits, bbits;
    GC gc;
    if (!dpy || !w || !h) return;
    templ.visualid = pd->visual;
    vi = XGetVisualInfo(dpy, VisualIDMask, &templ, &n);
    if (!vi) return;
    rgba = (unsigned char *)malloc((size_t)w * h * 4);
    image = XCreateImage(dpy, vi->visual, (unsigned int)vi->depth, ZPixmap, 0, 0, w, h, 32, 0);
    if (!rgba || !image) {
        free(rgba);
        if (image) XDestroyImage(image);
        XFree(vi);
        return;
    }
    a[0] = w;
    a[1] = h;
    glp_query(GLP_FN_READ_DRAWABLE, rgba, w * h * 4, a, 2);
    rs = mask_shift(vi->red_mask, &rbits);
    gs = mask_shift(vi->green_mask, &gbits);
    bs = mask_shift(vi->blue_mask, &bbits);
    image->data = (char *)malloc((size_t)image->bytes_per_line * h);
    if (image->data) {
        for (y = 0; y < h; ++y) {
            const unsigned char *s = &rgba[(size_t)(h - 1 - y) * w * 4]; /* GL rows are bottom-up */
            for (x = 0; x < w; ++x, s += 4) {
                unsigned long pixel = (unsigned long)(s[0] >> (8 - rbits)) << rs |
                                      (unsigned long)(s[1] >> (8 - gbits)) << gs |
                                      (unsigned long)(s[2] >> (8 - bbits)) << bs;
                XPutPixel(image, (int)x, (int)y, pixel);
            }
        }
        gc = XCreateGC(dpy, pd->id, 0, 0);
        XPutImage(dpy, pd->id, gc, image, 0, 0, 0, 0, w, h);
        XFreeGC(dpy, gc);
        XFlush(dpy);
    }
    XDestroyImage(image);
    XFree(vi);
    free(rgba);
}

/* Shows a drawable's image: back buffer first copied to the front when
   `swap`. Windows get the host image composited where they show the key
   colour; pixmaps get the image copied in. */
static int is_overlay_window(GLXDrawable d);

static void update_pixmap(struct glp_drawable *pd)
{
    glp_lock();
    update_pixmap_unlocked(pd);
    glp_unlock();
}

static void present_unlocked(Display *dpy, Display *app, GLXDrawable drawable, int swap);

/* ---- Window tracking -------------------------------------------------------------------- */

/* Where each GL window is on screen, kept from the X server's events rather
   than asked for at every present. The first present of a window (or of one
   whose ancestors changed) queries it and its ancestors and selects
   StructureNotify on them on this library's connection; from then on their
   ConfigureNotify, GravityNotify, MapNotify, UnmapNotify, ReparentNotify and
   DestroyNotify events keep the position, size and visibility current. The
   events are read without waiting (XEventsQueued, QueuedAfterReading) under
   the library lock whenever a thread presents or makes a context current;
   command submission never waits for them. Selecting events on another
   client's windows changes nothing for that client. */
#define TRACKED_MAX 64
#define CHAIN_MAX 24
struct link {
    Window id;
    int x, y;            /* outer corner, relative to the parent's inside */
    unsigned int border;
    int mapped;
};
static struct tracked {
    Window window;        /* 0: free slot */
    int valid;            /* chain known (cleared by a reparent or an ancestor's destruction) */
    int nchain;
    struct link chain[CHAIN_MAX]; /* the window, then its ancestors below the root */
    unsigned int w, h;
    unsigned long since;  /* events older than the queries (lower serial) are ignored */
    int shown;            /* its image is on the emulated display */
    int moved;            /* position, size or mapping changed since it was last shown */
} tracked[TRACKED_MAX];
static int next_tracked;

static struct tracked *find_tracked(Window w)
{
    int i;
    for (i = 0; i < TRACKED_MAX; ++i) {
        if (tracked[i].window == w && w) return &tracked[i];
    }
    return 0;
}

/* Queries `w` and its ancestors (round trips) and starts tracking them. */
static struct tracked *track_window(Display *dpy, Window w)
{
    struct tracked *t = find_tracked(w);
    x_error_handler previous;
    Window cur = w, root = 0, parent = 0, *children = 0;
    unsigned int nchildren;
    int n = 0;
    if (!t) {
        t = &tracked[next_tracked];
        next_tracked = (next_tracked + 1) % TRACKED_MAX; /* full: the oldest one goes */
        memset(t, 0, sizeof *t);
    }
    previous = errors_begin(dpy);
    t->since = NextRequest(dpy);
    while (n < CHAIN_MAX) {
        struct link *l = &t->chain[n];
        XWindowAttributes wa;
        /* Selected before the query: no change can fall between the two. */
        XSelectInput(dpy, cur, StructureNotifyMask);
        if (!XGetWindowAttributes(dpy, cur, &wa)) break;
        l->id = cur;
        l->x = wa.x;
        l->y = wa.y;
        l->border = (unsigned int)wa.border_width;
        l->mapped = wa.map_state != IsUnmapped;
        if (n == 0) {
            t->w = (unsigned int)wa.width;
            t->h = (unsigned int)wa.height;
        }
        ++n;
        if (!XQueryTree(dpy, cur, &root, &parent, &children, &nchildren)) break;
        if (children) XFree(children);
        if (!parent || parent == root) break;
        cur = parent;
    }
    errors_end(dpy, previous);
    if (x_error_code || n == 0 || (parent != root && n == CHAIN_MAX)) {
        glx_log("window %#lx: cannot track (X error %d, %d ancestors)", (unsigned long)w, x_error_code, n);
        t->window = 0;
        return 0;
    }
    t->window = w;
    t->nchain = n;
    t->valid = 1;
    t->moved = 1;
    return t;
}

static int tracked_viewable(const struct tracked *t)
{
    int k;
    if (!t->valid) return 0;
    for (k = 0; k < t->nchain; ++k) {
        if (!t->chain[k].mapped) return 0;
    }
    return 1;
}

/* The inside's top-left corner in root coordinates. */
static void tracked_origin(const struct tracked *t, int *sx, int *sy)
{
    int k;
    *sx = 0;
    *sy = 0;
    for (k = 0; k < t->nchain; ++k) {
        *sx += t->chain[k].x + (int)t->chain[k].border;
        *sy += t->chain[k].y + (int)t->chain[k].border;
    }
}

static void track_event(const XEvent *e)
{
    int i, k;
    Window w;
    switch (e->type) {
    case ConfigureNotify: w = e->xconfigure.window; break;
    case GravityNotify: w = e->xgravity.window; break;
    case MapNotify: w = e->xmap.window; break;
    case UnmapNotify: w = e->xunmap.window; break;
    case ReparentNotify: w = e->xreparent.window; break;
    case DestroyNotify: w = e->xdestroywindow.window; break;
    default: return;
    }
    /* A window manager's synthetic ConfigureNotify gives root coordinates;
       the real one, which follows the frame's own, is what the chain uses. */
    if (e->xany.send_event) return;
    for (i = 0; i < TRACKED_MAX; ++i) {
        struct tracked *t = &tracked[i];
        if (!t->window || e->xany.serial < t->since) continue;
        for (k = 0; k < t->nchain; ++k) {
            struct link *l = &t->chain[k];
            if (l->id != w) continue;
            switch (e->type) {
            case ConfigureNotify:
                l->x = e->xconfigure.x;
                l->y = e->xconfigure.y;
                l->border = (unsigned int)e->xconfigure.border_width;
                if (k == 0) {
                    t->w = (unsigned int)e->xconfigure.width;
                    t->h = (unsigned int)e->xconfigure.height;
                }
                break;
            case GravityNotify:
                l->x = e->xgravity.x;
                l->y = e->xgravity.y;
                break;
            case MapNotify: l->mapped = 1; break;
            case UnmapNotify: l->mapped = 0; break;
            case ReparentNotify: t->valid = 0; break; /* a new chain: queried at the next present */
            case DestroyNotify:
                t->valid = 0;
                l->mapped = 0;
                if (k == 0) note_dead(w);
                break;
            }
            t->moved = 1;
        }
    }
}

/* Sends a window's image again, at its tracked place (its front buffer, no
   key fill: the window's pixels move with it), or hides it. */
static void reshow(struct tracked *t, unsigned int context, int show)
{
    unsigned int req[7];
    int sx, sy;
    memset(req, 0, sizeof req);
    req[0] = (unsigned int)t->window;
    if (show) {
        tracked_origin(t, &sx, &sy);
        req[1] = (unsigned int)sx; req[2] = (unsigned int)sy;
        req[3] = t->w; req[4] = t->h;
        req[5] = is_overlay_window(t->window) ? GLP_OVERLAY_KEY : GLP_DEFAULT_KEY;
    } else {
        req[6] = GLP_SWAP_HIDE;
    }
    glp_gl(GLP_OP_SWAP, context, req, sizeof req);
}

/* Reads the events that have arrived, without waiting, and brings the
   emulated display along: a shown window that is no longer viewable (it, or
   an ancestor, was unmapped or destroyed) has its image taken down, and one
   that moved is shown at its new place. Under the library lock. */
static void track_events(Display *dpy, unsigned int context)
{
    XEvent e;
    int i;
    if (!dpy) return;
    while (XEventsQueued(dpy, QueuedAfterReading) > 0) {
        XNextEvent(dpy, &e);
        track_event(&e);
    }
    for (i = 0; i < TRACKED_MAX; ++i) { /* destroyed without an event read yet */
        struct tracked *t = &tracked[i];
        if (t->window && t->valid && window_dead(t->window)) {
            t->valid = 0;
            t->chain[0].mapped = 0;
            t->moved = 1;
        }
    }
    for (i = 0; i < TRACKED_MAX; ++i) {
        struct tracked *t = &tracked[i];
        if (!t->window || !t->moved) continue;
        if (!tracked_viewable(t)) {
            if (t->shown) {
                glx_log("window %#lx hidden", (unsigned long)t->window);
                reshow(t, context, 0);
                t->shown = 0;
            }
            t->moved = 0;
            continue;
        }
        if (t->shown) {
            int sx, sy;
            tracked_origin(t, &sx, &sy);
            glx_log("window %#lx moved to %d,%d (%ux%u)", (unsigned long)t->window, sx, sy, t->w, t->h);
            reshow(t, context, 1);
        }
        t->moved = 0;
    }
}

/* Size and screen position of window `d` (this library's connection, under
   the lock). From the tracked state; the server is asked only for a window
   not tracked yet, or whose ancestors changed, unless `trust_stale` (overlay
   windows, drawn while the program may hold a server grab: the last known
   place, if any, rather than a round trip that would wait for the grab).
   Returns 1 when the window is viewable, 0 when it is not, -1 when unknown. */
static int window_geometry(Display *dpy, GLXDrawable d, int trust_stale, int *sx, int *sy, unsigned int *w,
                           unsigned int *h)
{
    struct tracked *t = find_tracked(d);
    *sx = *sy = 0;
    if (window_dead(d)) { /* destroyed: no request for it */
        *w = *h = 1;
        return -1;
    }
    if (!t || !(t->valid || (trust_stale && t->nchain > 0))) t = track_window(dpy, d);
    if (!t) {
        *w = *h = 1;
        return -1;
    }
    tracked_origin(t, sx, sy);
    *w = t->w;
    *h = t->h;
    if (!t->valid) return 1; /* stale, trusted */
    return tracked_viewable(t);
}

static void present(Display *dpy, GLXDrawable drawable, int swap)
{
    if (!dpy) return;
    glp_lock();
    present_unlocked(xconn(dpy), dpy, drawable, swap);
    glp_unlock();
}

/* `dpy` is this library's connection, `app` the program's. */
static void present_unlocked(Display *dpy, Display *app, GLXDrawable drawable, int swap)
{
    GLXContext current = current_context();
    struct glp_drawable *pd = find_drawable(drawable);
    unsigned int req[7];
    int sx, sy;
    unsigned int w, h;
    if (!dpy) return;
    glp_flush(); /* the commands still run: their state (textures, lists) outlives the window */
    if (!pd && window_dead(drawable)) return; /* destroyed: nothing to show, no request for it */
    memset(req, 0, sizeof req);
    req[0] = (unsigned int)drawable;
    if (pd) {
        req[6] = (swap ? GLP_SWAP_BACK_TO_FRONT : 0) | GLP_SWAP_NO_DISPLAY;
        glp_gl(GLP_OP_SWAP, current ? current->id : 0, req, sizeof req);
        if (pd->kind == DRAW_PIXMAP && current && current->drawable == drawable) update_pixmap(pd);
        return;
    }
    /* The window's place comes from the X server's events (see Window
       tracking); overlay windows (menus, marquee), presented while the
       program may hold a server grab, keep their last known place. */
    track_events(dpy, current ? current->id : 0);
    switch (window_geometry(dpy, drawable, is_overlay_window(drawable), &sx, &sy, &w, &h)) {
    case -1:
        glx_log("present(%#lx): not a window on screen", (unsigned long)drawable);
        return;
    case 0:
        glx_log("present(%#lx): not viewable", (unsigned long)drawable);
        return;
    }
    glx_log("present(%#lx) %ux%u at %d,%d%s", (unsigned long)drawable, w, h, sx, sy, swap ? " swap" : "");
    /* Paint the key colour: the host image shows wherever it survives. */
    if (is_overlay_window(drawable)) {
        /* Overlay windows (in the popup planes) get the overlay key there.
           Through the program's connection, without a round trip: these are
           drawn while the program may hold a server grab (Maya's menus), and
           the program draws from the thread that uses its connection. */
        XGCValues values;
        GC gc;
        unsigned long first;
        values.foreground = GLP_OVERLAY_KEY;
        values.plane_mask = AllPlanes;
        values.function = GXcopy;
        first = NextRequest(app);
        gc = XCreateGC(app, drawable, GCForeground | GCPlaneMask | GCFunction, &values);
        XFillRectangle(app, drawable, gc, 0, 0, w, h);
        XFreeGC(app, gc);
        note_app_requests(app, first, NextRequest(app) - 1); /* their errors are this library's */
        XFlush(app);
    } else {
        /* A GC made for this drawable: the server may tie a GC to the visual
           it was created on. No round trip (XFlush): the program may hold a
           server grab, which would hold up a reply on this connection. */
        XGCValues values;
        GC gc;
        values.foreground = GLP_DEFAULT_KEY;
        values.plane_mask = AllPlanes;
        values.function = GXcopy;
        gc = XCreateGC(dpy, drawable, GCForeground | GCPlaneMask | GCFunction, &values);
        XFillRectangle(dpy, drawable, gc, 0, 0, w, h);
        XFreeGC(dpy, gc);
        XFlush(dpy);
    }
    req[1] = (unsigned int)sx; req[2] = (unsigned int)sy;
    req[3] = w; req[4] = h; req[6] = swap ? GLP_SWAP_BACK_TO_FRONT : 0;
    req[5] = is_overlay_window(drawable) ? GLP_OVERLAY_KEY : GLP_DEFAULT_KEY;
    glp_gl(GLP_OP_SWAP, current ? current->id : 0, req, sizeof req);
    {
        struct tracked *t = find_tracked(drawable);
        if (t) {
            t->shown = 1;
            t->moved = 0;
        }
    }
    if (current && current->drawable == drawable && (w != current->width || h != current->height)) {
        glXMakeCurrent(current->dpy, drawable, current); /* window resized: resize the host framebuffer */
    }
}

/* Drawables that have had a glXSwapBuffers: the others are taken for
   single-buffered ones. Every visual here is double-buffered, so a program
   that asked for a single-buffered one draws into the back buffer and only
   calls glFlush (Maya's marking menus); its image is shown then. */
#define SWAPPED_MAX 64
static GLXDrawable swapped[SWAPPED_MAX];
static int nswapped;

static int has_swapped(GLXDrawable d)
{
    int i, found = 0;
    glp_lock();
    for (i = 0; i < nswapped && !found; ++i) found = swapped[i] == d;
    glp_unlock();
    return found;
}

static void note_swapped(GLXDrawable d)
{
    glp_lock();
    if (!has_swapped(d)) {
        if (nswapped == SWAPPED_MAX) {
            memmove(swapped, swapped + 1, sizeof swapped[0] * (SWAPPED_MAX - 1)); /* forget the oldest */
            --nswapped;
        }
        swapped[nswapped++] = d;
    }
    glp_unlock();
}

/* Windows of the overlay planes' visual (noted when a context of it is made
   current on them): presented without the key fill, which belongs to the
   GL window below in the main planes. */
#define OVERLAY_WINDOWS_MAX 16
static GLXDrawable overlay_windows[OVERLAY_WINDOWS_MAX];
static int noverlay_windows;

static int is_overlay_window(GLXDrawable d)
{
    int i, found = 0;
    glp_lock();
    for (i = 0; i < noverlay_windows && !found; ++i) found = overlay_windows[i] == d;
    glp_unlock();
    return found;
}

static void note_overlay_window(GLXDrawable d)
{
    glp_lock();
    if (!is_overlay_window(d)) {
        if (noverlay_windows == OVERLAY_WINDOWS_MAX) {
            memmove(overlay_windows, overlay_windows + 1, sizeof overlay_windows[0] * (OVERLAY_WINDOWS_MAX - 1));
            --noverlay_windows;
        }
        overlay_windows[noverlay_windows++] = d;
    }
    glp_unlock();
}

/* glFlush and glFinish show what was drawn when it is meant to be seen now:
   drawing into the front buffer, or into a never-swapped drawable. */
/* A flush also shows drawing done on top of the last swapped frame without
   clearing it: Maya's selection rectangle is drawn so, into the back buffer
   of its double-buffered view, with only a glFlush after each mouse move.
   A frame that starts with a clear is shown at its swap only. */
static void present_flushed(void)
{
    struct glp_thread *t = glp_self();
    GLXContext current = t->current;
    if (!current || !current->drawable || window_dead(current->drawable)) return;
    load_palette(current->dpy, current, current->drawable);
    if (draws_front(current->draw_buffer)) {
        present(current->dpy, current->drawable, 0);
    } else if (!has_swapped(current->drawable)) {
        present(current->dpy, current->drawable, 1);
    } else if (!current->frame_cleared && t->submitted > current->shown_words) {
        glx_log("present(%#lx): drawn over the last frame", (unsigned long)current->drawable);
        present(current->dpy, current->drawable, 1);
    }
    current->shown_words = t->submitted;
}

void glp_impl_glFlush(void)
{
    GLXContext current = current_context();
    TRACE(glFlush);
    glp_flush();
    if (current) glx_log("glFlush (context %u, draw buffer %#x)", current->id, current->draw_buffer);
    present_flushed();
}

void glp_impl_glFinish(void)
{
    GLXContext current = current_context();
    TRACE(glFinish);
    glp_flush();
    if (current) glx_log("glFinish (context %u, draw buffer %#x)", current->id, current->draw_buffer);
    if (current) glp_gl(GLP_OP_FINISH, current->id, 0, 0);
    present_flushed();
}

/* ---- GLX visuals and framebuffer configurations ------------------------------------------ */

struct __GLXFBConfigRec {
    VisualID visual;
    int screen;
};

/* The X server's overlay-plane visual (SERVER_OVERLAY_VISUALS, layer 1),
   and its transparent pixel; 0 if there is none. GL overlay windows (Maya's
   menus, marquee, highlights) use it: the host renders them, and shows them
   above the GL image of the window below, their transparent index unseen. */
static VisualID overlay_visual_id;
static int overlay_transparent;
static int overlay_depth; /* its planes: 2 on the XL8 (the popup planes) */

static VisualID overlay_visual(Display *dpy, int screen)
{
    static int looked;
    Atom atom, type;
    int format;
    unsigned long n = 0, after, i;
    unsigned char *data = 0;
    const char *enable = getenv("GLPASS_OVERLAY");
    if (looked) return overlay_visual_id;
    looked = 1;
    /* Off unless GLPASS_OVERLAY=1: offered overlay planes, Maya also moves
       its Motif menus into them, which the emulated XL8 does not show, and
       its marking menus then wait forever. Without them it draws its menus
       in the main planes, which the colour-index emulation handles. */
    if (!enable || *enable != '1') return 0;
    atom = XInternAtom(dpy, "SERVER_OVERLAY_VISUALS", True);
    if (atom == None) return 0;
    if (XGetWindowProperty(dpy, RootWindow(dpy, screen), atom, 0, 256, False, AnyPropertyType, &type, &format, &n,
                           &after, &data) != Success || !data) {
        return 0;
    }
    if (format == 32) {
        const long *entry = (const long *)data; /* visual, transparent type, value, layer */
        for (i = 0; i + 4 <= n; i += 4) {
            if (entry[i + 3] == 1) {
                overlay_visual_id = (VisualID)entry[i];
                overlay_transparent = entry[i + 1] == 1 ? (int)entry[i + 2] : -1;
                break;
            }
        }
    }
    XFree(data);
    if (overlay_visual_id) {
        XVisualInfo templ, *vi;
        int count = 0;
        templ.visualid = overlay_visual_id;
        vi = XGetVisualInfo(dpy, VisualIDMask, &templ, &count);
        overlay_depth = vi && count > 0 ? vi->depth : 2;
        if (vi) XFree(vi);
    }
    glx_log("overlay visual %#lx, depth %d, transparent index %d", (unsigned long)overlay_visual_id, overlay_depth,
            overlay_transparent);
    return overlay_visual_id;
}

static int is_overlay_visual(VisualID id) { return id != 0 && id == overlay_visual_id; }

static int visual_supported(const XVisualInfo *vi)
{
    /* The window only carries the key colour; the host renders in RGBA. */
    return ((vi->class == TrueColor || vi->class == PseudoColor) && vi->depth >= 8) ||
           is_overlay_visual(vi->visualid);
}

/* What the overlay visual offers: single-buffered colour index, level 1. */
static int overlay_config_value(int attribute, int *value)
{
    switch (attribute) {
    case GLX_USE_GL: *value = 1; break;
    case GLX_BUFFER_SIZE: *value = overlay_depth; break; /* the visual's depth: windows are made with it */
    case GLX_LEVEL: *value = 1; break;
    case GLX_RGBA: case GLX_DOUBLEBUFFER: case GLX_STEREO: case GLX_AUX_BUFFERS: *value = 0; break;
    case GLX_RED_SIZE: case GLX_GREEN_SIZE: case GLX_BLUE_SIZE: case GLX_ALPHA_SIZE: *value = 0; break;
    case GLX_DEPTH_SIZE: case GLX_STENCIL_SIZE: *value = 0; break;
    case GLX_ACCUM_RED_SIZE: case GLX_ACCUM_GREEN_SIZE: case GLX_ACCUM_BLUE_SIZE: case GLX_ACCUM_ALPHA_SIZE:
        *value = 0; break;
    case GLX_X_VISUAL_TYPE_EXT: *value = GLX_PSEUDO_COLOR_EXT; break;
    case GLX_VISUAL_CAVEAT_EXT: *value = GLX_NONE_EXT; break;
    case GLX_TRANSPARENT_TYPE_EXT: *value = overlay_transparent >= 0 ? GLX_TRANSPARENT_INDEX_EXT : GLX_NONE_EXT; break;
    case GLX_TRANSPARENT_INDEX_VALUE_EXT: *value = overlay_transparent >= 0 ? overlay_transparent : 0; break;
    case GLX_SAMPLES_SGIS: case GLX_SAMPLE_BUFFERS_SGIS: *value = 0; break;
    default: return GLX_BAD_ATTRIBUTE;
    }
    return 0;
}

Bool glp_impl_glXQueryExtension(Display *dpy, int *error, int *event)
{
    (void)dpy;
    if (error) *error = 0;
    if (event) *event = 0;
    return glp_available() ? True : False;
}

Bool glp_impl_glXQueryVersion(Display *dpy, int *major, int *minor)
{
    (void)dpy;
    if (major) *major = 1;
    if (minor) *minor = 3;
    return True;
}

/* What every visual offers: RGBA8, double-buffered, depth 24, stencil 8. */
static int config_value(int attribute, int *value)
{
    switch (attribute) {
    case GLX_USE_GL: case GLX_RGBA: case GLX_DOUBLEBUFFER: case GLX_X_RENDERABLE: *value = 1; break;
    case GLX_BUFFER_SIZE: *value = 32; break;
    case GLX_LEVEL: case GLX_STEREO: case GLX_AUX_BUFFERS: *value = 0; break;
    case GLX_RED_SIZE: case GLX_GREEN_SIZE: case GLX_BLUE_SIZE: case GLX_ALPHA_SIZE: *value = 8; break;
    case GLX_DEPTH_SIZE: *value = 24; break;
    case GLX_STENCIL_SIZE: *value = 8; break;
    case GLX_ACCUM_RED_SIZE: case GLX_ACCUM_GREEN_SIZE: case GLX_ACCUM_BLUE_SIZE: case GLX_ACCUM_ALPHA_SIZE:
        *value = 0; break;
    case GLX_X_VISUAL_TYPE_EXT: *value = GLX_TRUE_COLOR_EXT; break;
    case GLX_VISUAL_CAVEAT_EXT: case GLX_TRANSPARENT_TYPE_EXT: *value = GLX_NONE_EXT; break;
    case GLX_SAMPLES_SGIS: case GLX_SAMPLE_BUFFERS_SGIS: *value = 0; break;
    case GLX_DRAWABLE_TYPE: *value = GLX_WINDOW_BIT | GLX_PIXMAP_BIT | GLX_PBUFFER_BIT; break;
    case GLX_RENDER_TYPE: *value = GLX_RGBA_BIT; break;
    case GLX_MAX_PBUFFER_WIDTH: case GLX_MAX_PBUFFER_HEIGHT: *value = 2048; break;
    case GLX_MAX_PBUFFER_PIXELS: *value = 2048 * 2048; break;
    default: return GLX_BAD_ATTRIBUTE;
    }
    return 0;
}

int glp_impl_glXGetConfig(Display *dpy, XVisualInfo *vis, int attribute, int *value)
{
    (void)dpy;
    if (!vis || !value) return GLX_BAD_VISUAL;
    glx_log("glXGetConfig(visual %#lx, %#x)", (unsigned long)vis->visualid, (unsigned int)attribute);
    if (!visual_supported(vis)) {
        if (attribute != GLX_USE_GL) return GLX_BAD_VISUAL;
        *value = 0;
        return 0;
    }
    if (attribute == GLX_VISUAL_ID_EXT || attribute == GLX_FBCONFIG_ID) {
        *value = (int)vis->visualid;
        return 0;
    }
    if (is_overlay_visual(vis->visualid)) return overlay_config_value(attribute, value);
    return config_value(attribute, value);
}

/* Checks an attribute list against the offered configuration. In GLX 1.0
   lists the boolean attributes take no value; in FBConfig lists all do. */
static int attributes_match(const int *attributes, int fbconfig)
{
    const int *a;
    int rgba = fbconfig, level = 0; /* glXChooseFBConfig defaults to RGBA */
    if (!attributes) return 1;
    for (a = attributes; *a != None; ++a) {
        int attribute = *a, value = 1;
        if (fbconfig || !(attribute == GLX_USE_GL || attribute == GLX_RGBA || attribute == GLX_DOUBLEBUFFER ||
                          attribute == GLX_STEREO)) {
            if (a[1] == None) break; /* a value-less attribute at the end */
            value = *++a;
        }
        switch (attribute) {
        case GLX_RGBA: rgba = value != 0; break;
        case GLX_BUFFER_SIZE: break;
        case GLX_RENDER_TYPE: rgba = (value & GLX_RGBA_BIT) != 0; break;
        case GLX_LEVEL:
            if (value == 1 && !fbconfig) level = 1;
            else if (value != 0) return 0;
            break;
        case GLX_STEREO: if (value && value != (int)GLX_DONT_CARE) return 0; break;
        case GLX_X_VISUAL_TYPE_EXT:
            if (value != (int)GLX_TRUE_COLOR_EXT && value != (int)GLX_DONT_CARE) return 0;
            break;
        case GLX_TRANSPARENT_TYPE_EXT:
            if (value != (int)GLX_NONE_EXT && value != (int)GLX_DONT_CARE && value != (int)GLX_TRANSPARENT_INDEX_EXT) {
                return 0;
            }
            break;
        default: break; /* sizes are met, or ignored (accumulation, multisampling) */
        }
    }
    if (level == 1) return 3; /* the overlay planes */
    /* Colour-index requests (Maya asks for a 12-bit double-buffered one for
       its 3D views) get an 8-bit PseudoColor visual, so the program's
       colour-index colormaps and windows work; the host emulates colour
       index. Overlay requests (level 1) get the overlay planes' visual. */
    return rgba ? 1 : 2;
}

static XVisualInfo *gl_visuals(Display *dpy, int screen, int *count, int index)
{
    static int listed;
    XVisualInfo templ;
    (void)overlay_visual(dpy, screen);
    if (!listed) {
        XVisualInfo *all;
        int n = 0, i;
        listed = 1;
        memset(&templ, 0, sizeof templ);
        templ.screen = screen;
        all = XGetVisualInfo(dpy, VisualScreenMask, &templ, &n);
        glx_log("default visual %#lx", (unsigned long)XVisualIDFromVisual(DefaultVisual(dpy, screen)));
        for (i = 0; all && i < n; ++i) {
            glx_log("X visual %#lx class %d depth %d", (unsigned long)all[i].visualid, all[i].class, all[i].depth);
        }
        if (all) XFree(all);
    }
    /* The screen's default visual when it is 8-bit PseudoColor (the XL8's),
       for RGBA too: the host renders in RGBA whatever the visual, and the
       program's X widgets inside its GL windows (Maya's menus) then see the
       colours of the default colormap their pixels come from. */
    {
        Visual *dv = DefaultVisual(dpy, screen);
        memset(&templ, 0, sizeof templ);
        templ.visualid = XVisualIDFromVisual(dv);
        if (dv->class == PseudoColor && DefaultDepth(dpy, screen) == 8) {
            XVisualInfo *vi = XGetVisualInfo(dpy, VisualIDMask, &templ, count);
            if (vi) return vi;
        }
    }
    memset(&templ, 0, sizeof templ);
    templ.screen = screen;
    templ.depth = 8;
    templ.class = index ? PseudoColor : TrueColor;
    {
        XVisualInfo *vi = XGetVisualInfo(dpy, VisualScreenMask | VisualDepthMask | VisualClassMask, &templ, count);
        if (vi || !index) return vi;
    }
    templ.class = TrueColor;
    return XGetVisualInfo(dpy, VisualScreenMask | VisualDepthMask | VisualClassMask, &templ, count);
}

/* RGBA and colour-index requests may get the same X visual: which one a
   visual handed out was for, so contexts created with it take that mode. */
#define CHOSEN_MAX 64
static struct {
    const XVisualInfo *vi;
    int index;
} chosen[CHOSEN_MAX];
static int next_chosen;

static void note_chosen(const XVisualInfo *vi, int index)
{
    if (!vi) return;
    glp_lock();
    chosen[next_chosen].vi = vi;
    chosen[next_chosen].index = index;
    next_chosen = (next_chosen + 1) % CHOSEN_MAX;
    glp_unlock();
}

/* 1 colour index, 0 RGBA, -1 not handed out here. */
static int chosen_mode(const XVisualInfo *vi)
{
    int i, mode = -1;
    glp_lock();
    for (i = 0; i < CHOSEN_MAX && mode < 0; ++i) {
        if (vi && chosen[i].vi == vi) mode = chosen[i].index;
    }
    glp_unlock();
    return mode;
}

XVisualInfo *glp_impl_glXChooseVisual(Display *dpy, int screen, int *attributes)
{
    int count = 0;
    XVisualInfo *vi;
    static int overlay_logged;
    int overlay = 0, match;
    const int *a;
    for (a = attributes; a && *a != None; ++a) {
        if (*a == GLX_LEVEL && a[1] != 0) overlay = 1;
    }
    if (!overlay || overlay_logged++ < 4) log_attributes("glXChooseVisual", attributes);
    match = attributes_match(attributes, 0);
    if (match == 3) {
        XVisualInfo templ;
        int wanted = 0;
        templ.visualid = overlay_visual(dpy, screen);
        /* GLX_BUFFER_SIZE is a minimum, as in IRIX's GLX: the XL8's overlay
           planes are 2 bits, so a request for more finds none. (Maya asks
           for 8-bit overlay planes to put its Motif menus in; offered the
           2-bit visual for that, it posted menus it could not create.) */
        for (a = attributes; a && *a != None; ++a) {
            const int attribute = *a;
            if (attribute == GLX_USE_GL || attribute == GLX_RGBA || attribute == GLX_DOUBLEBUFFER ||
                attribute == GLX_STEREO || a[1] == None) {
                continue; /* GLX 1.0 booleans take no value */
            }
            if (attribute == GLX_BUFFER_SIZE) wanted = a[1];
            ++a;
        }
        if (templ.visualid && wanted > overlay_depth) {
            if (overlay_logged <= 4) glx_log("  -> no overlay visual of %d bits (%d)", wanted, overlay_depth);
            return 0;
        }
        vi = templ.visualid ? XGetVisualInfo(dpy, VisualIDMask, &templ, &count) : 0;
        if (overlay_logged <= 4) glx_log("  -> overlay visual %#lx", vi ? (unsigned long)vi->visualid : 0ul);
        return vi;
    }
    if (!match) {
        if (!overlay || overlay_logged <= 4) glx_log("  -> no visual");
        return 0;
    }
    vi = gl_visuals(dpy, screen, &count, match == 2);
    glx_log("  -> visual %#lx", vi ? (unsigned long)vi->visualid : 0ul);
    note_chosen(vi, match == 2);
    return vi;
}

GLXFBConfig *glp_impl_glXGetFBConfigs(Display *dpy, int screen, int *nelements)
{
    int count = 0, i;
    XVisualInfo *vi = gl_visuals(dpy, screen, &count, 0);
    GLXFBConfig *list;
    if (nelements) *nelements = 0;
    if (!vi || count <= 0) return 0;
    list = (GLXFBConfig *)malloc(sizeof *list * (size_t)count);
    for (i = 0; list && i < count; ++i) {
        list[i] = (GLXFBConfig)malloc(sizeof **list);
        list[i]->visual = vi[i].visualid;
        list[i]->screen = screen;
    }
    XFree(vi);
    if (list && nelements) *nelements = count;
    return list;
}

GLXFBConfig *glp_impl_glXChooseFBConfig(Display *dpy, int screen, int *attributes, int *nitems)
{
    log_attributes("glXChooseFBConfig", attributes);
    if (nitems) *nitems = 0;
    if (!attributes_match(attributes, 1)) return 0;
    return glXGetFBConfigs(dpy, screen, nitems);
}

GLXFBConfigSGIX *glp_impl_glXChooseFBConfigSGIX(Display *dpy, int screen, int *attributes, int *nitems)
{
    return glXChooseFBConfig(dpy, screen, attributes, nitems);
}

int glp_impl_glXGetFBConfigAttrib(Display *dpy, GLXFBConfig config, int attribute, int *value)
{
    (void)dpy;
    if (!config || !value) return GLX_BAD_ATTRIBUTE;
    if (attribute == GLX_VISUAL_ID || attribute == GLX_FBCONFIG_ID) {
        *value = (int)config->visual;
        return 0;
    }
    return config_value(attribute, value);
}

int glp_impl_glXGetFBConfigAttribSGIX(Display *dpy, GLXFBConfigSGIX config, int attribute, int *value)
{
    return glXGetFBConfigAttrib(dpy, config, attribute, value);
}

XVisualInfo *glp_impl_glXGetVisualFromFBConfig(Display *dpy, GLXFBConfig config)
{
    XVisualInfo templ;
    int count = 0;
    if (!config) return 0;
    templ.visualid = config->visual;
    return XGetVisualInfo(dpy, VisualIDMask, &templ, &count);
}

XVisualInfo *glp_impl_glXGetVisualFromFBConfigSGIX(Display *dpy, GLXFBConfigSGIX config)
{
    return glXGetVisualFromFBConfig(dpy, config);
}

GLXFBConfigSGIX glp_impl_glXGetFBConfigFromVisualSGIX(Display *dpy, XVisualInfo *vis)
{
    GLXFBConfig config;
    (void)dpy;
    if (!vis || !visual_supported(vis)) return 0;
    config = (GLXFBConfig)malloc(sizeof *config);
    if (!config) return 0;
    config->visual = vis->visualid;
    config->screen = vis->screen;
    return config;
}

/* ---- GLX contexts --------------------------------------------------------------------------- */

static GLXContext new_context(Display *dpy, int screen, VisualID visual, GLXContext share)
{
    GLXContext c;
    long id;
    if (!glp_available()) return 0;
    id = glp_gl(GLP_OP_CREATE_CONTEXT, 0, 0, share ? share->id : 0);
    if (id <= 0) return 0;
    c = (GLXContext)calloc(1, sizeof *c);
    if (!c) return 0;
    c->id = (unsigned int)id;
    c->dpy = dpy;
    c->screen = screen;
    c->visual = visual;
    c->share = share;
    context_defaults(c);
    if (share) { /* one list table for the share group */
        glp_lock();
        c->ix.lists = lists_of(share);
        if (c->ix.lists) ++c->ix.lists->refs;
        glp_unlock();
    }
    {
        XVisualInfo templ, *vi;
        int n = 0;
        templ.visualid = visual;
        vi = visual ? XGetVisualInfo(dpy, VisualIDMask, &templ, &n) : 0;
        if (vi) {
            c->index_mode = vi->class == PseudoColor || vi->class == StaticColor;
            c->pseudo = vi->class == PseudoColor;
            c->overlay = is_overlay_visual(vi->visualid);
            XFree(vi);
        }
    }
    glx_log("context %u: %s%s", c->id, c->index_mode ? "colour index (emulated)" : "RGBA",
            c->overlay ? ", overlay planes" : "");
    return c;
}

GLXContext glp_impl_glXCreateContext(Display *dpy, XVisualInfo *vis, GLXContext share, Bool direct)
{
    GLXContext c;
    int mode = chosen_mode(vis);
    (void)direct;
    glx_log("glXCreateContext(visual %#lx, share %u)", vis ? (unsigned long)vis->visualid : 0ul,
            share ? share->id : 0u);
    c = new_context(dpy, vis ? vis->screen : DefaultScreen(dpy), vis ? vis->visualid : 0, share);
    if (c && mode >= 0 && !c->overlay && c->index_mode != mode) {
        c->index_mode = mode;
        glx_log("context %u: %s (as requested)", c->id, mode ? "colour index (emulated)" : "RGBA");
    }
    return c;
}

GLXContext glp_impl_glXCreateNewContext(Display *dpy, GLXFBConfig config, int render_type, GLXContext share, Bool direct)
{
    GLXContext c;
    (void)direct;
    c = new_context(dpy, config ? config->screen : DefaultScreen(dpy), config ? config->visual : 0, share);
    if (c && !c->overlay) c->index_mode = render_type != GLX_RGBA_TYPE; /* the mode asked for */
    return c;
}

GLXContext glp_impl_glXCreateContextWithConfigSGIX(Display *dpy, GLXFBConfigSGIX config, int render_type, GLXContext share,
                                          Bool direct)
{
    return glXCreateNewContext(dpy, config, render_type, share, direct);
}

/* ---- Context ownership: GLX makes a context current in one thread at most --------- */

/* Whether thread state `o` still belongs to a running thread: the calling
   process's POSIX threads release their contexts as they exit (thread_free);
   a sproc is checked by its pid. */
static int owner_alive(const struct glp_thread *o)
{
    if (!o) return 0;
    if (o->pid == thread_pid()) return 1;
    return !(kill(o->pid, 0) < 0 && errno == ESRCH);
}

static void destroy_now(GLXContext c)
{
    glp_gl(GLP_OP_DESTROY_CONTEXT, c->id, 0, c->id);
    recording_free(&c->ix.group);
    recording_free(&c->ix.compile);
    lists_release(c->ix.lists);
    free(c);
}

/* Claims `c` for `t`; 0 when it is current in another running thread
   (GLX: BadAccess). A thread that died with it current loses it. */
static int context_claim(struct glp_thread *t, GLXContext c)
{
    int ok;
    glp_lock();
    ok = !c->owner || c->owner == t || !owner_alive(c->owner);
    if (ok) c->owner = t;
    glp_unlock();
    return ok;
}

/* `t` no longer has `c` current: a destroy requested meanwhile happens now. */
static void context_release(struct glp_thread *t, GLXContext c)
{
    int destroy;
    if (!c) return;
    glp_lock();
    if (c->owner == t) c->owner = 0;
    destroy = c->destroy_pending && !c->owner;
    glp_unlock();
    if (destroy) {
        glx_log("context %u: destroyed now that it is released", c->id);
        destroy_now(c);
    }
}

/* As GLX: a context current in another thread is destroyed when that thread
   releases it (its state stays valid until then); otherwise at once. */
void glp_impl_glXDestroyContext(Display *dpy, GLXContext c)
{
    struct glp_thread *t = glp_self();
    int deferred;
    (void)dpy;
    if (!c) return;
    if (c == t->current) {
        thread_flush(t);
        t->current = 0;
    }
    glp_lock();
    if (c->owner == t) c->owner = 0;
    deferred = c->owner != 0 && owner_alive(c->owner);
    if (deferred) c->destroy_pending = 1;
    glp_unlock();
    if (deferred) {
        glx_log("context %u: current in another thread, destroyed when released", c->id);
        return;
    }
    destroy_now(c);
}

void glp_impl_glXCopyContext(Display *dpy, GLXContext src, GLXContext dst, unsigned long mask)
{
    static int logged;
    (void)dpy; (void)src; (void)dst; (void)mask;
    glp_unimplemented("glXCopyContext", &logged);
}

/* RGBA and colour-index windows share the PseudoColor visual: a colour-index
   window has a colormap of the program's own, an RGBA one the default
   colormap. Noted per window. */
#define WINDOW_MODES_MAX 64
static struct {
    GLXDrawable drawable;
    int index;
} window_modes[WINDOW_MODES_MAX];
static int next_window_mode;

static int window_is_index(Display *app, GLXDrawable drawable)
{
    XWindowAttributes wa;
    Display *dpy;
    int i, index = -1;
    glp_lock();
    for (i = 0; i < WINDOW_MODES_MAX && index < 0; ++i) {
        if (window_modes[i].drawable == drawable && drawable) index = window_modes[i].index;
    }
    if (index < 0) {
        dpy = xconn(app);
        index = XGetWindowAttributes(dpy, drawable, &wa) && wa.colormap != None &&
                wa.colormap != DefaultColormap(dpy, DefaultScreen(dpy));
        window_modes[next_window_mode].drawable = drawable;
        window_modes[next_window_mode].index = index;
        next_window_mode = (next_window_mode + 1) % WINDOW_MODES_MAX;
    }
    glp_unlock();
    return index;
}

Bool glp_impl_glXMakeCurrent(Display *dpy, GLXDrawable drawable, GLXContext c)
{
    struct glp_thread *t = glp_self();
    GLXContext current = t->current;
    unsigned int req[4];
    int sx, sy;
    struct glp_drawable *pd;
    if (c && drawable && window_dead(drawable) && !find_drawable(drawable)) {
        /* Destroyed: GLX fails with GLXBadDrawable; the current context stays. */
        glx_log("glXMakeCurrent(%#lx, context %u): the window was destroyed", (unsigned long)drawable, c->id);
        return False;
    }
    if (c && drawable && c != current && !context_claim(t, c)) {
        /* GLX: BadAccess; the current context stays. */
        glx_log("glXMakeCurrent(%#lx, context %u): current in another thread", (unsigned long)drawable, c->id);
        return False;
    }
    glx_log("glXMakeCurrent(%#lx, context %u) [context %u drew %lu words]", (unsigned long)drawable,
            c ? c->id : 0u, current ? current->id : 0u, (thread_flush(t), t->submitted));
    t->submitted = 0;
    if (current && current->drawable && (pd = find_drawable(current->drawable)) != 0 && pd->kind == DRAW_PIXMAP &&
        (current != c || current->drawable != drawable)) {
        glp_flush();
        update_pixmap(pd); /* leaving a pixmap: copy out what was drawn */
    } else if (current && current->drawable && (current != c || current->drawable != drawable)) {
        /* glXMakeCurrent flushes the context it leaves: what it drew into a
           single-buffered window or the front buffer is to be seen now
           (Maya's overlay drawing ends so, without a glFlush). */
        glp_flush();
        present_flushed();
    }
    thread_flush(t);
    if (!c || !drawable) {
        /* Releases this thread's context only: other threads keep theirs. */
        if (current) {
            req[0] = current->id; req[1] = 0; req[2] = 0; req[3] = 0;
            glp_gl(GLP_OP_MAKE_CURRENT, current->id, req, sizeof req);
        }
        t->current = 0;
        context_release(t, current);
        return True;
    }
    XFlush(dpy); /* as GLX does: the program's new windows exist for this library's connection */
    pd = find_drawable(drawable);
    if (pd) {
        drawable_geometry(dpy, drawable, &sx, &sy, &c->width, &c->height);
    } else {
        /* Overlay windows may be made current under a server grab (see present). */
        Display *own;
        glp_lock();
        own = xconn(dpy);
        track_events(own, current ? current->id : 0);
        window_geometry(own, drawable, c->overlay, &sx, &sy, &c->width, &c->height);
        glp_unlock();
    }
    if (pd) {
        pd->width = c->width;
        pd->height = c->height;
    }
    req[0] = c->id; req[1] = (unsigned int)drawable; req[2] = c->width; req[3] = c->height;
    if (glp_gl(GLP_OP_MAKE_CURRENT, c->id, req, sizeof req) != 0) {
        t->current = 0;
        if (current != c) context_release(t, current);
        context_release(t, c);
        return False;
    }
    if (c->drawable != drawable) c->frame_cleared = 0; /* another drawable: its frame is not known */
    c->shown_words = 0;
    c->dpy = dpy;
    c->drawable = drawable;
    t->current = c;
    recordings_restart(c, t); /* a list being compiled goes on in this thread's buffer */
    if (current && current != c) context_release(t, current); /* this thread left it */
    if (c->pseudo && !c->overlay && !find_drawable(drawable)) {
        int index = window_is_index(dpy, drawable);
        if (index != c->index_mode) {
            c->index_mode = index;
            glx_log("context %u on %#lx: %s", c->id, (unsigned long)drawable,
                    index ? "colour index (emulated)" : "RGBA");
        }
    }
    if (c->overlay) note_overlay_window(drawable);
    load_palette(dpy, c, drawable);
    return True;
}

Bool glp_impl_glXMakeContextCurrent(Display *dpy, GLXDrawable draw, GLXDrawable read, GLXContext c)
{
    (void)read;
    return glXMakeCurrent(dpy, draw, c);
}

Bool glp_impl_glXMakeCurrentReadSGI(Display *dpy, GLXDrawable draw, GLXDrawable read, GLXContext c)
{
    return glXMakeContextCurrent(dpy, draw, read, c);
}

void glp_impl_glXSwapBuffers(Display *dpy, GLXDrawable drawable)
{
    struct glp_thread *t = glp_self();
    GLXContext current = t->current;
    thread_flush(t);
    if (window_dead(drawable) && !find_drawable(drawable)) { /* destroyed: a swap that shows nothing */
        static unsigned long logged;
        if (logged != drawable) {
            logged = drawable;
            glx_log("glXSwapBuffers(%#lx): the window was destroyed; not shown", (unsigned long)drawable);
        }
        t->submitted = 0;
        return;
    }
    if (dpy) XFlush(dpy); /* as GLX does (see glXMakeCurrent) */
    glx_log("glXSwapBuffers(%p, %#lx) [%lu words]", (void *)dpy, (unsigned long)drawable, t->submitted);
    note_swapped(drawable);
    t->submitted = 0;
    if (current && current->drawable == drawable) {
        current->frame_cleared = 0;
        current->shown_words = 0;
    }
    if (current && current->drawable == drawable) load_palette(current->dpy, current, drawable);
    present(dpy ? dpy : (current ? current->dpy : 0), drawable, 1);
}

GLXContext glp_impl_glXGetCurrentContext(void) { return current_context(); }

GLXDrawable glp_impl_glXGetCurrentDrawable(void)
{
    GLXContext current = current_context();
    return current ? current->drawable : 0;
}

GLXDrawable glp_impl_glXGetCurrentReadDrawable(void) { return glXGetCurrentDrawable(); }
GLXDrawable glp_impl_glXGetCurrentReadDrawableSGI(void) { return glXGetCurrentDrawable(); }
GLXDrawable glp_impl_glXGetCurrentDrawableEXT(void) { return glXGetCurrentDrawable(); }
Display *glp_impl_glXGetCurrentDisplay(void)
{
    GLXContext current = current_context();
    return current ? current->dpy : 0;
}

Display *glp_impl_glXGetCurrentDisplayEXT(void) { return glXGetCurrentDisplay(); }
Bool glp_impl_glXIsDirect(Display *dpy, GLXContext c) { (void)dpy; (void)c; return True; }
void glp_impl_glXWaitGL(void) { glFinish(); }
void glp_impl_glXWaitX(void)
{
    GLXContext current = current_context();
    if (current) XSync(current->dpy, False);
}

int glp_impl_glXQueryContext(Display *dpy, GLXContext c, int attribute, int *value)
{
    (void)dpy;
    if (!c || !value) return GLX_BAD_CONTEXT;
    switch (attribute) {
    case GLX_FBCONFIG_ID: *value = (int)c->visual; return 0;
    case GLX_RENDER_TYPE: *value = GLX_RGBA_TYPE; return 0;
    case GLX_SCREEN: *value = c->screen; return 0;
    default: return GLX_BAD_ATTRIBUTE;
    }
}

int glp_impl_glXQueryContextInfoEXT(Display *dpy, GLXContext c, int attribute, int *value)
{
    (void)dpy;
    if (!c || !value) return GLX_BAD_CONTEXT;
    switch (attribute) {
    case GLX_SHARE_CONTEXT_EXT: *value = c->share ? (int)c->share->id : 0; return 0;
    case GLX_VISUAL_ID_EXT: *value = (int)c->visual; return 0;
    case GLX_SCREEN_EXT: *value = c->screen; return 0;
    default: return GLX_BAD_ATTRIBUTE;
    }
}

GLXContextID glp_impl_glXGetContextIDEXT(const GLXContext c) { return c ? c->id : 0; }
GLXContext glp_impl_glXImportContextEXT(Display *dpy, GLXContextID id) { (void)dpy; (void)id; return 0; }
void glp_impl_glXFreeContextEXT(Display *dpy, GLXContext c) { (void)dpy; (void)c; }

/* ---- GLX windows, pixmaps, pbuffers --------------------------------------------------- */

GLXWindow glp_impl_glXCreateWindow(Display *dpy, GLXFBConfig config, Window win, int *attributes)
{
    (void)dpy; (void)config; (void)attributes;
    return win;
}

void glp_impl_glXDestroyWindow(Display *dpy, GLXWindow win) { (void)dpy; (void)win; }

static GLXPixmap new_pixmap(VisualID visual, Pixmap pixmap)
{
    struct glp_drawable *d = add_drawable(pixmap, DRAW_PIXMAP);
    glx_log("GLX pixmap %#lx", (unsigned long)pixmap);
    if (!d) return 0;
    d->visual = visual;
    return pixmap;
}

GLXPixmap glp_impl_glXCreateGLXPixmap(Display *dpy, XVisualInfo *vis, Pixmap pixmap)
{
    (void)dpy;
    return new_pixmap(vis ? vis->visualid : 0, pixmap);
}

GLXPixmap glp_impl_glXCreatePixmap(Display *dpy, GLXFBConfig config, Pixmap pixmap, int *attributes)
{
    (void)dpy; (void)attributes;
    return new_pixmap(config ? config->visual : 0, pixmap);
}

GLXPixmap glp_impl_glXCreateGLXPixmapWithConfigSGIX(Display *dpy, GLXFBConfigSGIX config, Pixmap pixmap)
{
    return glXCreatePixmap(dpy, config, pixmap, 0);
}

void glp_impl_glXDestroyGLXPixmap(Display *dpy, GLXPixmap pix) { (void)dpy; remove_drawable(pix); }
void glp_impl_glXDestroyPixmap(Display *dpy, GLXPixmap pix) { (void)dpy; remove_drawable(pix); }

static XID new_pbuffer(unsigned int width, unsigned int height)
{
    struct glp_drawable *d;
    glp_lock();
    d = add_drawable(next_pbuffer, DRAW_PBUFFER);
    if (d) ++next_pbuffer;
    glp_unlock();
    if (!d) return 0;
    d->width = width ? width : 1;
    d->height = height ? height : 1;
    return d->id;
}

GLXPbuffer glp_impl_glXCreatePbuffer(Display *dpy, GLXFBConfig config, int *attributes)
{
    unsigned int width = 0, height = 0;
    const int *a;
    (void)dpy; (void)config;
    for (a = attributes; a && *a != None; a += 2) {
        if (a[0] == GLX_PBUFFER_WIDTH) width = (unsigned int)a[1];
        if (a[0] == GLX_PBUFFER_HEIGHT) height = (unsigned int)a[1];
    }
    return new_pbuffer(width, height);
}

GLXPbufferSGIX glp_impl_glXCreateGLXPbufferSGIX(Display *dpy, GLXFBConfigSGIX config, unsigned int width,
                                        unsigned int height, int *attributes)
{
    (void)dpy; (void)config; (void)attributes;
    return new_pbuffer(width, height);
}

void glp_impl_glXDestroyPbuffer(Display *dpy, GLXPbuffer pbuf) { (void)dpy; remove_drawable(pbuf); }
void glp_impl_glXDestroyGLXPbufferSGIX(Display *dpy, GLXPbufferSGIX pbuf) { (void)dpy; remove_drawable(pbuf); }

void glp_impl_glXQueryDrawable(Display *dpy, GLXDrawable draw, int attribute, unsigned int *value)
{
    GLXContext current = current_context();
    int sx, sy;
    unsigned int w = 0, h = 0;
    if (!value) return;
    drawable_geometry(dpy, draw, &sx, &sy, &w, &h);
    switch (attribute) {
    case GLX_WIDTH: *value = w; break;
    case GLX_HEIGHT: *value = h; break;
    case GLX_PRESERVED_CONTENTS: case GLX_LARGEST_PBUFFER: *value = 1; break;
    case GLX_FBCONFIG_ID: *value = current ? (unsigned int)current->visual : 0; break;
    default: *value = 0; break;
    }
}

int glp_impl_glXQueryGLXPbufferSGIX(Display *dpy, GLXPbufferSGIX pbuf, int attribute, unsigned int *value)
{
    glXQueryDrawable(dpy, pbuf, attribute, value);
    return 0;
}

/* No buffer-clobber events: the host buffers are never damaged. */
void glp_impl_glXSelectEvent(Display *dpy, GLXDrawable drawable, unsigned long mask) { (void)dpy; (void)drawable; (void)mask; }
void glp_impl_glXGetSelectedEvent(Display *dpy, GLXDrawable drawable, unsigned long *mask) { (void)dpy; (void)drawable; if (mask) *mask = 0; }
void glp_impl_glXSelectEventSGIX(Display *dpy, GLXDrawable drawable, unsigned long mask) { glXSelectEvent(dpy, drawable, mask); }
void glp_impl_glXGetSelectedEventSGIX(Display *dpy, GLXDrawable drawable, unsigned long *mask) { glXGetSelectedEvent(dpy, drawable, mask); }

/* ---- GLX strings, procedures, sync -------------------------------------------------------------- */

static const char glx_extensions[] =
    "GLX_EXT_visual_info GLX_EXT_import_context GLX_SGI_make_current_read GLX_SGI_swap_control "
    "GLX_SGI_video_sync GLX_SGIX_fbconfig GLX_SGIX_pbuffer";

const char *glp_impl_glXGetClientString(Display *dpy, int name)
{
    (void)dpy;
    switch (name) {
    case GLX_VENDOR: return "irix-emu glpass";
    case GLX_VERSION: return "1.3";
    case GLX_EXTENSIONS: return glx_extensions;
    default: return 0;
    }
}

const char *glp_impl_glXQueryServerString(Display *dpy, int screen, int name)
{
    (void)screen;
    return glXGetClientString(dpy, name);
}

const char *glp_impl_glXQueryExtensionsString(Display *dpy, int screen)
{
    (void)dpy; (void)screen;
    return glx_extensions;
}

glp_proc glp_impl_glXGetProcAddress(const GLubyte *name)
{
    static void *self;
    if (!self) self = dlopen("libGL.so", RTLD_LAZY);
    return self ? (void (*)(void))dlsym(self, (const char *)name) : 0;
}

glp_proc glp_impl_glXGetProcAddressARB(const GLubyte *name) { return glXGetProcAddress(name); }

static unsigned int video_count;

int glp_impl_glXGetVideoSyncSGI(unsigned int *count)
{
    glp_lock();
    ++video_count;
    if (count) *count = video_count;
    glp_unlock();
    return 0;
}

int glp_impl_glXWaitVideoSyncSGI(int divisor, int remainder, unsigned int *count)
{
    glp_lock();
    ++video_count;
    if (divisor > 0 && remainder >= 0 && remainder < divisor) {
        while ((int)(video_count % (unsigned int)divisor) != remainder) ++video_count;
    }
    if (count) *count = video_count;
    glp_unlock();
    return 0;
}

int glp_impl_glXSwapIntervalSGI(int interval) { (void)interval; return 0; }

/* Large-system features (channels, hyperpipes, swap groups, video sources) do not exist here. */
int glp_impl_glXBindChannelToWindowSGIX(Display *dpy, int screen, int channel, Window window) { (void)dpy; (void)screen; (void)channel; (void)window; return 0; }
int glp_impl_glXQueryChannelDeltasSGIX(Display *dpy, int screen, int channel, int *dx, int *dy, int *dw, int *dh) { (void)dpy; (void)screen; (void)channel; (void)dx; (void)dy; (void)dw; (void)dh; return 0; }
int glp_impl_glXChannelRectSGIX(Display *dpy, int screen, int channel, int x, int y, int w, int h) { (void)dpy; (void)screen; (void)channel; (void)x; (void)y; (void)w; (void)h; return 0; }
int glp_impl_glXQueryChannelRectSGIX(Display *dpy, int screen, int channel, int *x, int *y, int *w, int *h) { (void)dpy; (void)screen; (void)channel; (void)x; (void)y; (void)w; (void)h; return 0; }
int glp_impl_glXChannelRectSyncSGIX(Display *dpy, int screen, int channel, GLenum synctype) { (void)dpy; (void)screen; (void)channel; (void)synctype; return 0; }
GLXHyperpipeNetworkSGIX *glp_impl_glXQueryHyperpipeNetworkSGIX(Display *dpy, int *npipes) { (void)dpy; if (npipes) *npipes = 0; return 0; }
int glp_impl_glXHyperpipeConfigSGIX(Display *dpy, int networkId, int npipes, GLXHyperpipeConfigSGIX *cfg, int *hpId) { (void)dpy; (void)networkId; (void)npipes; (void)cfg; (void)hpId; return GLX_BAD_CONTEXT; }
int glp_impl_glXDestroyHyperpipeConfigSGIX(Display *dpy, int hpId) { (void)dpy; (void)hpId; return GLX_BAD_CONTEXT; }
GLXHyperpipeConfigSGIX *glp_impl_glXQueryHyperpipeConfigSGIX(Display *dpy, int hpId, int *npipes) { (void)dpy; (void)hpId; if (npipes) *npipes = 0; return 0; }
int glp_impl_glXBindHyperpipeSGIX(Display *dpy, int hpId) { (void)dpy; (void)hpId; return GLX_BAD_CONTEXT; }
int glp_impl_glXQueryHyperpipeBestAttribSGIX(Display *dpy, int timeSlice, int attrib, int size, void *attribList, void *returnAttribList) { (void)dpy; (void)timeSlice; (void)attrib; (void)size; (void)attribList; (void)returnAttribList; return GLX_BAD_CONTEXT; }
int glp_impl_glXQueryHyperpipeAttribSGIX(Display *dpy, int timeSlice, int attrib, int size, void *returnAttribList) { (void)dpy; (void)timeSlice; (void)attrib; (void)size; (void)returnAttribList; return GLX_BAD_CONTEXT; }
int glp_impl_glXHyperpipeAttribSGIX(Display *dpy, int timeSlice, int attrib, int size, void *attribList) { (void)dpy; (void)timeSlice; (void)attrib; (void)size; (void)attribList; return GLX_BAD_CONTEXT; }
void glp_impl_glXJoinSwapGroupSGIX(Display *dpy, GLXDrawable drawable, GLXDrawable member) { (void)dpy; (void)drawable; (void)member; }
void glp_impl_glXBindSwapBarrierSGIX(Display *dpy, GLXDrawable drawable, int barrier) { (void)dpy; (void)drawable; (void)barrier; }
Bool glp_impl_glXQueryMaxSwapBarriersSGIX(Display *dpy, int screen, int *max) { (void)dpy; (void)screen; if (max) *max = 0; return False; }
/* glx.h declares these only with the video and media library headers; their
   argument types are opaque here. */
GLXVideoSourceSGIX glp_impl_glXCreateGLXVideoSourceSGIX(Display *dpy, int screen, void *server, int path, int nodeClass, int node) { (void)dpy; (void)screen; (void)server; (void)path; (void)nodeClass; (void)node; return 0; }
void glp_impl_glXDestroyGLXVideoSourceSGIX(Display *dpy, GLXVideoSourceSGIX source) { (void)dpy; (void)source; }
Bool glp_impl_glXAssociateDMPbufferSGIX(Display *dpy, GLXPbufferSGIX pbuffer, void *params, void *buffer) { (void)dpy; (void)pbuffer; (void)params; (void)buffer; return False; }

/* ---- glXUseXFont ---------------------------------------------------------------------------------- */

/* One glBitmap display list per glyph, drawn by X into a bitmap (the method
   of Mesa's xfonts.c). */
void glp_impl_glXUseXFont(Font font, int first, int count, int listBase)
{
    GLXContext current = current_context();
    Display *dpy = current ? current->dpy : 0;
    XFontStruct *fs;
    Pixmap pixmap;
    GC gc;
    XGCValues values;
    unsigned int maxw, maxh;
    unsigned char *bits;
    struct glp_pixelstore saved;
    int i;
    if (!dpy) return;
    fs = XQueryFont(dpy, font);
    if (!fs) return;
    maxw = (unsigned int)(fs->max_bounds.rbearing - fs->min_bounds.lbearing);
    maxh = (unsigned int)(fs->max_bounds.ascent + fs->max_bounds.descent);
    if (maxw == 0) maxw = 1;
    if (maxh == 0) maxh = 1;
    pixmap = XCreatePixmap(dpy, RootWindow(dpy, current->screen), maxw, maxh, 1);
    values.foreground = 0;
    values.background = 0;
    values.font = font;
    gc = XCreateGC(dpy, pixmap, GCForeground | GCBackground | GCFont, &values);
    bits = (unsigned char *)malloc(((maxw + 7) / 8) * maxh);

    saved = ctx()->client.unpack;
    glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
    glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    for (i = 0; i < count; ++i) {
        unsigned int c = (unsigned int)(first + i);
        const XCharStruct *cs = 0;
        int width, height;
        if (!fs->per_char) {
            cs = &fs->max_bounds;
        } else if (c >= fs->min_char_or_byte2 && c <= fs->max_char_or_byte2) {
            cs = &fs->per_char[c - fs->min_char_or_byte2];
        }
        glNewList((GLuint)(listBase + i), GL_COMPILE);
        if (cs) {
            width = cs->rbearing - cs->lbearing;
            height = cs->ascent + cs->descent;
            if (width > 0 && height > 0 && bits) {
                XImage *image;
                char ch = (char)c;
                int x, y, rowbytes = (width + 7) / 8;
                XSetForeground(dpy, gc, 0);
                XFillRectangle(dpy, pixmap, gc, 0, 0, maxw, maxh);
                XSetForeground(dpy, gc, 1);
                XDrawString(dpy, pixmap, gc, -cs->lbearing, cs->ascent, &ch, 1);
                image = XGetImage(dpy, pixmap, 0, 0, (unsigned int)width, (unsigned int)height, 1, XYPixmap);
                memset(bits, 0, (size_t)rowbytes * (size_t)height);
                if (image) {
                    for (y = 0; y < height; ++y) {
                        unsigned char *row = bits + (size_t)(height - 1 - y) * (size_t)rowbytes; /* bottom-up */
                        for (x = 0; x < width; ++x) {
                            if (XGetPixel(image, x, y)) row[x / 8] |= (unsigned char)(0x80 >> (x % 8));
                        }
                    }
                    XDestroyImage(image);
                }
                glBitmap(width, height, (GLfloat)-cs->lbearing, (GLfloat)cs->descent, (GLfloat)cs->width, 0.0f,
                         bits);
            } else {
                glBitmap(0, 0, 0.0f, 0.0f, (GLfloat)cs->width, 0.0f, 0);
            }
        }
        glEndList();
    }

    glPixelStorei(GL_UNPACK_SWAP_BYTES, saved.swap);
    glPixelStorei(GL_UNPACK_LSB_FIRST, saved.lsbfirst);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, saved.rowlength);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, saved.skiprows);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, saved.skippixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, saved.alignment);
    free(bits);
    XFreeGC(dpy, gc);
    XFreePixmap(dpy, pixmap);
    XFreeFontInfo(0, fs, 1);
}
