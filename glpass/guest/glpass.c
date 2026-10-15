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
#include <stdarg.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef __sgi
#include <abi_mutex.h>
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

static int glp_state = -1; /* -1 unknown, 0 unavailable, 1 available */

static int glp_available(void)
{
    if (glp_state < 0) {
        long version = 0, hello = -1;
        if (glp_call(GLP_SERVICE_PING, 0, 0, 0, &version) == (long)GLP_PING_MAGIC) {
            hello = glp_call(GLP_SERVICE_GL, GLP_OP_HELLO, 0, GLP_FN_TABLE_ID, 0);
        }
        glp_state = hello == (long)GLP_VERSION;
        if (hello == GLP_ERR_TABLE) {
            fprintf(stderr, "glpass: this library and the emulator come from different glpass sources: "
                            "rebuild it in IRIX (see glpass/README.md)\n");
        } else if (!glp_state) {
            fprintf(stderr, "glpass: no host GL pass-through in this emulator\n");
        }
    }
    return glp_state;
}

/* ---- Diagnostics ------------------------------------------------------------ */

int glp_trace = -1;
static unsigned char traced[GLP_FN_END];

void glp_trace_first(unsigned int fn, const char *name)
{
    if (glp_trace < 0) {
        const char *v = getenv("GLPASS_TRACE");
        glp_trace = v && *v && *v != '0';
    }
    if (!glp_trace || fn >= GLP_FN_END || traced[fn]) return;
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

void glp_unimplemented(const char *name)
{
    /* Few programs call these; a short list of names is enough. */
    static const char *seen[64];
    static int nseen;
    int i;
    for (i = 0; i < nseen; ++i) {
        if (seen[i] == name) return;
    }
    if (nseen < 64) seen[nseen++] = name;
    fprintf(stderr, "glpass: %s is not supported (ignored)\n", name);
}

#define TRACE(name) GLP_TRACE(GLP_FN_##name, #name)

/* ---- Contexts and client state ------------------------------------------------ */

enum { ARR_VERTEX, ARR_NORMAL, ARR_COLOR, ARR_INDEX, ARR_TEXCOORD, ARR_EDGEFLAG, ARR_COUNT };

struct glp_array {
    int enabled;
    int size;
    GLenum type;
    int stride;
    const void *pointer;
};

struct glp_pixelstore {
    int swap, lsbfirst, rowlength, skiprows, skippixels, alignment;
};

struct glp_client {
    struct glp_array arrays[ARR_COUNT];
    struct glp_pixelstore pack, unpack;
};

#define CLIENT_STACK 16

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
};

static GLXContext current;
static struct __GLXcontextRec no_context; /* client state when nothing is current */

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
    c->draw_buffer = GL_BACK; /* every visual is double-buffered */
    c->read_buffer = GL_BACK;
}

static GLXContext ctx(void)
{
    if (current) return current;
    if (!no_context.render_mode) context_defaults(&no_context);
    return &no_context;
}

/* ---- Command buffer ----------------------------------------------------------- */

static unsigned int *cmdbuf;
static unsigned int cmdcap, cmdlen;

static unsigned long submitted; /* words sent for the current context (trace) */

static void glp_flush(void)
{
    if (cmdlen > 0 && current) submitted += cmdlen;
    if (cmdlen > 0 && current) glp_call(GLP_SERVICE_GL, GLP_OP_SUBMIT, cmdbuf, (unsigned long)cmdlen * 4, 0);
    cmdlen = 0;
}

static void begin_materialize(void);
static int begin_pending; /* a glBegin held back while only glArrayElement follows */

unsigned int *glp_put(unsigned int cmd, unsigned int words)
{
    unsigned int header = words < GLP_LONG_RECORD ? 1 : 2;
    unsigned int *p;
    if (begin_pending) begin_materialize(); /* another command inside glBegin/glEnd */
    if (cmdlen + header + words > cmdcap) {
        glp_flush();
        if (header + words > cmdcap) {
            unsigned int cap = cmdcap ? cmdcap : 16384;
            while (cap < header + words) cap *= 2;
            free(cmdbuf);
            cmdbuf = (unsigned int *)malloc((size_t)cap * 4);
            if (!cmdbuf) {
                fprintf(stderr, "glpass: out of memory for a %u-word command\n", words);
                abort();
            }
            cmdcap = cap;
        }
    }
    p = &cmdbuf[cmdlen];
    if (header == 1) {
        p[0] = (cmd << 16) | words;
    } else {
        p[0] = (cmd << 16) | GLP_LONG_RECORD;
        p[1] = words;
    }
    cmdlen += header + words;
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
    if (!current) return 0;
    req = nargs + 3 <= 32 ? local : (unsigned int *)malloc((nargs + 3) * 4);
    if (!req) return 0;
    glp_flush();
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
    result = glp_call(GLP_SERVICE_GL, GLP_OP_QUERY, req, (nargs + 3) * 4, 0);
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

void glPixelStorei(GLenum pname, GLint param)
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
    default: break;
    }
    p = glp_put(GLP_FN_glPixelStorei, 2);
    p[0] = pname;
    p[1] = (unsigned int)param;
}

void glPixelStoref(GLenum pname, GLfloat param) { glPixelStorei(pname, (GLint)(param + (param < 0 ? -0.5f : 0.5f))); }

void glTexImage1D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLint border, GLenum format,
                  GLenum type, const GLvoid *pixels)
{
    unsigned int a[7];
    TRACE(glTexImage1D);
    a[0] = target; a[1] = (unsigned int)level; a[2] = (unsigned int)internalformat; a[3] = (unsigned int)width;
    a[4] = (unsigned int)border; a[5] = format; a[6] = type;
    put_image(GLP_FN_glTexImage1D, a, 7, pixels, image_bytes(&ctx()->client.unpack, width, 1, format, type));
}

void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
                  GLenum format, GLenum type, const GLvoid *pixels)
{
    unsigned int a[8];
    TRACE(glTexImage2D);
    a[0] = target; a[1] = (unsigned int)level; a[2] = (unsigned int)internalformat; a[3] = (unsigned int)width;
    a[4] = (unsigned int)height; a[5] = (unsigned int)border; a[6] = format; a[7] = type;
    put_image(GLP_FN_glTexImage2D, a, 8, pixels, image_bytes(&ctx()->client.unpack, width, height, format, type));
}

void glTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type,
                     const GLvoid *pixels)
{
    unsigned int a[6];
    TRACE(glTexSubImage1D);
    a[0] = target; a[1] = (unsigned int)level; a[2] = (unsigned int)xoffset; a[3] = (unsigned int)width;
    a[4] = format; a[5] = type;
    put_image(GLP_FN_glTexSubImage1D, a, 6, pixels, image_bytes(&ctx()->client.unpack, width, 1, format, type));
}

void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                     GLenum format, GLenum type, const GLvoid *pixels)
{
    unsigned int a[8];
    TRACE(glTexSubImage2D);
    a[0] = target; a[1] = (unsigned int)level; a[2] = (unsigned int)xoffset; a[3] = (unsigned int)yoffset;
    a[4] = (unsigned int)width; a[5] = (unsigned int)height; a[6] = format; a[7] = type;
    put_image(GLP_FN_glTexSubImage2D, a, 8, pixels, image_bytes(&ctx()->client.unpack, width, height, format, type));
}

void glTexSubImage1DEXT(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type,
                        const GLvoid *pixels)
{
    glTexSubImage1D(target, level, xoffset, width, format, type, pixels);
}

void glTexSubImage2DEXT(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                        GLenum format, GLenum type, const GLvoid *pixels)
{
    glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
}

void glDrawPixels(GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels)
{
    unsigned int a[4];
    TRACE(glDrawPixels);
    a[0] = (unsigned int)width; a[1] = (unsigned int)height; a[2] = format; a[3] = type;
    put_image(GLP_FN_glDrawPixels, a, 4, pixels, image_bytes(&ctx()->client.unpack, width, height, format, type));
}

void glBitmap(GLsizei width, GLsizei height, GLfloat xorig, GLfloat yorig, GLfloat xmove, GLfloat ymove,
              const GLubyte *bitmap)
{
    unsigned int a[6];
    TRACE(glBitmap);
    a[0] = (unsigned int)width; a[1] = (unsigned int)height; a[2] = glp_fbits(xorig); a[3] = glp_fbits(yorig);
    a[4] = glp_fbits(xmove); a[5] = glp_fbits(ymove);
    put_image(GLP_FN_glBitmap, a, 6, bitmap,
              image_bytes(&ctx()->client.unpack, width, height, GL_COLOR_INDEX, GL_BITMAP));
}

void glPolygonStipple(const GLubyte *mask)
{
    TRACE(glPolygonStipple);
    put_image(GLP_FN_glPolygonStipple, 0, 0, mask,
              image_bytes(&ctx()->client.unpack, 32, 32, GL_COLOR_INDEX, GL_BITMAP));
}

/* ---- Lists, textures, evaluators ------------------------------------------------- */

void glCallLists(GLsizei n, GLenum type, const GLvoid *lists)
{
    const unsigned char *b = (const unsigned char *)lists;
    unsigned int *p;
    GLsizei i;
    TRACE(glCallLists);
    if (n <= 0 || !lists) return;
    /* Sent as ints, so the host needs no byte order handling. */
    p = glp_put(GLP_FN_glCallLists, 1 + (unsigned int)n);
    p[0] = (unsigned int)n;
    for (i = 0; i < n; ++i) {
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
        p[1 + i] = (unsigned int)v;
    }
}

void glDeleteTextures(GLsizei n, const GLuint *textures)
{
    unsigned int *p;
    TRACE(glDeleteTextures);
    if (n <= 0 || !textures) return;
    p = glp_put(GLP_FN_glDeleteTextures, 1 + (unsigned int)n);
    p[0] = (unsigned int)n;
    memcpy(p + 1, textures, (size_t)n * 4);
}

void glDeleteTexturesEXT(GLsizei n, const GLuint *textures) { glDeleteTextures(n, textures); }

void glPrioritizeTextures(GLsizei n, const GLuint *textures, const GLclampf *priorities)
{
    unsigned int *p;
    TRACE(glPrioritizeTextures);
    if (n <= 0 || !textures || !priorities) return;
    p = glp_put(GLP_FN_glPrioritizeTextures, 1 + 2 * (unsigned int)n);
    p[0] = (unsigned int)n;
    memcpy(p + 1, textures, (size_t)n * 4);
    memcpy(p + 1 + n, priorities, (size_t)n * 4);
}

void glPrioritizeTexturesEXT(GLsizei n, const GLuint *textures, const GLclampf *priorities)
{
    glPrioritizeTextures(n, textures, priorities);
}

void glPolygonOffsetEXT(GLfloat factor, GLfloat bias)
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

void glMap1f(GLenum target, GLfloat u1, GLfloat u2, GLint stride, GLint order, const GLfloat *points)
{
    TRACE(glMap1f);
    put_map(GLP_FN_glMap1f, target, 0, u1, u2, 0, 1, stride, order, 0, 1, points);
}

void glMap1d(GLenum target, GLdouble u1, GLdouble u2, GLint stride, GLint order, const GLdouble *points)
{
    TRACE(glMap1d);
    put_map(GLP_FN_glMap1d, target, 1, u1, u2, 0, 1, stride, order, 0, 1, points);
}

void glMap2f(GLenum target, GLfloat u1, GLfloat u2, GLint ustride, GLint uorder, GLfloat v1, GLfloat v2,
             GLint vstride, GLint vorder, const GLfloat *points)
{
    TRACE(glMap2f);
    put_map(GLP_FN_glMap2f, target, 0, u1, u2, v1, v2, ustride, uorder, vstride, vorder, points);
}

void glMap2d(GLenum target, GLdouble u1, GLdouble u2, GLint ustride, GLint uorder, GLdouble v1, GLdouble v2,
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

void glPixelMapfv(GLenum map, GLint mapsize, const GLfloat *values) { TRACE(glPixelMapfv); pixel_map(GLP_FN_glPixelMapfv, map, mapsize, values, 0); }
void glPixelMapuiv(GLenum map, GLint mapsize, const GLuint *values) { TRACE(glPixelMapuiv); pixel_map(GLP_FN_glPixelMapuiv, map, mapsize, values, 0); }
void glPixelMapusv(GLenum map, GLint mapsize, const GLushort *values) { TRACE(glPixelMapusv); pixel_map(GLP_FN_glPixelMapusv, map, mapsize, values, 1); }

/* ---- Client vertex arrays ------------------------------------------------------------ */

static int array_index(GLenum array)
{
    switch (array) {
    case GL_VERTEX_ARRAY: return ARR_VERTEX;
    case GL_NORMAL_ARRAY: return ARR_NORMAL;
    case GL_COLOR_ARRAY: return ARR_COLOR;
    case GL_INDEX_ARRAY: return ARR_INDEX;
    case GL_TEXTURE_COORD_ARRAY: return ARR_TEXCOORD;
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

void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) { TRACE(glVertexPointer); set_array(ARR_VERTEX, size, type, stride, pointer); }
void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *pointer) { TRACE(glNormalPointer); set_array(ARR_NORMAL, 3, type, stride, pointer); }
void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) { TRACE(glColorPointer); set_array(ARR_COLOR, size, type, stride, pointer); }
void glIndexPointer(GLenum type, GLsizei stride, const GLvoid *pointer) { TRACE(glIndexPointer); set_array(ARR_INDEX, 1, type, stride, pointer); }
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) { TRACE(glTexCoordPointer); set_array(ARR_TEXCOORD, size, type, stride, pointer); }
void glEdgeFlagPointer(GLsizei stride, const GLboolean *pointer) { TRACE(glEdgeFlagPointer); set_array(ARR_EDGEFLAG, 1, GL_UNSIGNED_BYTE, stride, pointer); }

void glVertexPointerEXT(GLint size, GLenum type, GLsizei stride, GLsizei count, const GLvoid *pointer) { (void)count; glVertexPointer(size, type, stride, pointer); }
void glNormalPointerEXT(GLenum type, GLsizei stride, GLsizei count, const GLvoid *pointer) { (void)count; glNormalPointer(type, stride, pointer); }
void glColorPointerEXT(GLint size, GLenum type, GLsizei stride, GLsizei count, const GLvoid *pointer) { (void)count; glColorPointer(size, type, stride, pointer); }
void glIndexPointerEXT(GLenum type, GLsizei stride, GLsizei count, const GLvoid *pointer) { (void)count; glIndexPointer(type, stride, pointer); }
void glTexCoordPointerEXT(GLint size, GLenum type, GLsizei stride, GLsizei count, const GLvoid *pointer) { (void)count; glTexCoordPointer(size, type, stride, pointer); }
void glEdgeFlagPointerEXT(GLsizei stride, GLsizei count, const GLboolean *pointer) { (void)count; glEdgeFlagPointer(stride, pointer); }

void glEnableClientState(GLenum array)
{
    int i = array_index(array);
    TRACE(glEnableClientState);
    if (i >= 0) ctx()->client.arrays[i].enabled = 1;
}

void glDisableClientState(GLenum array)
{
    int i = array_index(array);
    TRACE(glDisableClientState);
    if (i >= 0) ctx()->client.arrays[i].enabled = 0;
}

void glInterleavedArrays(GLenum format, GLsizei stride, const GLvoid *pointer)
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
        arr[ARR_TEXCOORD].enabled = table[i].st != 0;
        if (table[i].st) set_array(ARR_TEXCOORD, table[i].st, GL_FLOAT, stride, base);
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

/* Record GLP_FN_CLIENT_DRAW:
     kind (GLP_DRAW_*), mode, count, narrays,
     per array: which (0 vertex, 1 normal, 2 colour, 3 index, 4 texcoord,
       5 edge flag), size, type, bytes, then elements first..first+count-1
       packed (big-endian, as in guest memory),
     GLP_DRAW_ELEMENTS: nindices, then indices relative to `first`. */
static void client_draw(unsigned int kind, GLenum mode, GLint first, GLsizei count, const unsigned int *indices,
                        GLsizei nindices)
{
    struct glp_array *arr = ctx()->client.arrays;
    unsigned int words = 4, *p, at;
    int i, narrays = 0;
    GLsizei e;
    if (count <= 0 || (!arr[ARR_VERTEX].enabled && kind != GLP_DRAW_ELEMENT)) return;
    for (i = 0; i < ARR_COUNT; ++i) {
        if (arr[i].enabled && arr[i].pointer) {
            ++narrays;
            words += 4 + words_for((unsigned int)(array_elem_bytes(&arr[i]) * count));
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
        if (!a->enabled || !a->pointer) continue;
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

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    TRACE(glDrawArrays);
    client_draw(GLP_DRAW_ARRAYS, mode, first, count, 0, 0);
}

void glDrawArraysEXT(GLenum mode, GLint first, GLsizei count) { glDrawArrays(mode, first, count); }

/* glBegin ... glArrayElement* ... glEnd (Quake III draws so) becomes one
   indexed draw: the glBegin is held back and the elements collected; any
   other command in between sends the glBegin and the elements as they were. */
static GLenum begin_mode;
static unsigned int *begin_elements;
static unsigned int begin_count, begin_capacity;

static void begin_materialize(void)
{
    unsigned int i, count = begin_count;
    begin_pending = 0;
    glp_put(GLP_FN_glBegin, 1)[0] = begin_mode;
    begin_count = 0;
    for (i = 0; i < count; ++i) client_draw(GLP_DRAW_ELEMENT, 0, (GLint)begin_elements[i], 1, 0, 0);
}

void glBegin(GLenum mode)
{
    TRACE(glBegin);
    if (begin_pending) begin_materialize();
    begin_mode = mode;
    begin_count = 0;
    begin_pending = 1;
}

void glEnd(void)
{
    struct glp_array *arr = ctx()->client.arrays;
    unsigned int i, lo = 0xFFFFFFFFu, hi = 0;
    TRACE(glEnd);
    if (!begin_pending) {
        glp_put(GLP_FN_glEnd, 0);
        return;
    }
    if (begin_count == 0) { /* glBegin; glEnd: nothing drawn */
        begin_pending = 0;
        return;
    }
    if (!arr[ARR_VERTEX].enabled || !arr[ARR_VERTEX].pointer) {
        begin_materialize();
        glp_put(GLP_FN_glEnd, 0);
        return;
    }
    begin_pending = 0;
    for (i = 0; i < begin_count; ++i) {
        if (begin_elements[i] < lo) lo = begin_elements[i];
        if (begin_elements[i] > hi) hi = begin_elements[i];
    }
    for (i = 0; i < begin_count; ++i) begin_elements[i] -= lo;
    client_draw(GLP_DRAW_ELEMENTS, begin_mode, (GLint)lo, (GLsizei)(hi - lo + 1), begin_elements,
                (GLsizei)begin_count);
    begin_count = 0;
}

void glArrayElement(GLint i)
{
    TRACE(glArrayElement);
    if (begin_pending && i >= 0) {
        if (begin_count == begin_capacity) {
            unsigned int capacity = begin_capacity ? begin_capacity * 2 : 1024;
            unsigned int *grown = (unsigned int *)realloc(begin_elements, capacity * sizeof *grown);
            if (!grown) {
                begin_materialize();
                client_draw(GLP_DRAW_ELEMENT, 0, i, 1, 0, 0);
                return;
            }
            begin_elements = grown;
            begin_capacity = capacity;
        }
        begin_elements[begin_count++] = (unsigned int)i;
        return;
    }
    client_draw(GLP_DRAW_ELEMENT, 0, i, 1, 0, 0);
}

void glArrayElementEXT(GLint i) { glArrayElement(i); }

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
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

void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const GLvoid *indices)
{
    (void)start; (void)end;
    glDrawElements(mode, count, type, indices);
}

void glDrawRangeElementsEXT(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const GLvoid *indices)
{
    glDrawRangeElements(mode, start, end, count, type, indices);
}

void glPushClientAttrib(GLbitfield mask)
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

void glPopClientAttrib(void)
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

void glDrawBuffer(GLenum mode)
{
    TRACE(glDrawBuffer);
    if (current) glx_log("glDrawBuffer(%#x) context %u", mode, current->id);
    ctx()->draw_buffer = mode;
    glp_put(GLP_FN_glDrawBuffer, 1)[0] = mode;
}

void glReadBuffer(GLenum mode)
{
    TRACE(glReadBuffer);
    if (current) glx_log("glReadBuffer(%#x) context %u", mode, current->id);
    ctx()->read_buffer = mode;
    glp_put(GLP_FN_glReadBuffer, 1)[0] = mode;
}

void glSelectBuffer(GLsizei size, GLuint *buffer)
{
    GLXContext c = ctx();
    TRACE(glSelectBuffer);
    c->select_buffer = buffer;
    c->select_size = size;
    glp_put(GLP_FN_glSelectBuffer, 1)[0] = (unsigned int)size;
}

void glFeedbackBuffer(GLsizei size, GLenum type, GLfloat *buffer)
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

GLint glRenderMode(GLenum mode)
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

GLenum glGetError(void)
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
    case GL_DRAW_BUFFER: *v = c->draw_buffer; return 1;
    case GL_READ_BUFFER: *v = c->read_buffer; return 1;
    case GL_RENDER_MODE: *v = c->render_mode; return 1;
    case GL_DOUBLEBUFFER: *v = 1; return 1;
    case GL_INDEX_MODE: *v = c->index_mode; return 1;
    case GL_RGBA_MODE: *v = !c->index_mode; return 1;
    case GL_STEREO: *v = 0; return 1;
    case GL_CLIENT_ATTRIB_STACK_DEPTH: *v = c->client_depth; return 1;
    case GL_MAX_CLIENT_ATTRIB_STACK_DEPTH: *v = CLIENT_STACK; return 1;
    case GL_SELECTION_BUFFER_SIZE: *v = c->select_size; return 1;
    case GL_FEEDBACK_BUFFER_SIZE: *v = c->feedback_size; return 1;
    case GL_VERTEX_ARRAY: case GL_NORMAL_ARRAY: case GL_COLOR_ARRAY: case GL_INDEX_ARRAY:
    case GL_TEXTURE_COORD_ARRAY: case GL_EDGE_FLAG_ARRAY:
        *v = cl->arrays[array_index(pname)].enabled; return 1;
    case GL_VERTEX_ARRAY_SIZE: *v = cl->arrays[ARR_VERTEX].size; return 1;
    case GL_COLOR_ARRAY_SIZE: *v = cl->arrays[ARR_COLOR].size; return 1;
    case GL_TEXTURE_COORD_ARRAY_SIZE: *v = cl->arrays[ARR_TEXCOORD].size; return 1;
    case GL_VERTEX_ARRAY_TYPE: i = ARR_VERTEX; break;
    case GL_NORMAL_ARRAY_TYPE: i = ARR_NORMAL; break;
    case GL_COLOR_ARRAY_TYPE: i = ARR_COLOR; break;
    case GL_INDEX_ARRAY_TYPE: i = ARR_INDEX; break;
    case GL_TEXTURE_COORD_ARRAY_TYPE: i = ARR_TEXCOORD; break;
    case GL_VERTEX_ARRAY_STRIDE: *v = cl->arrays[ARR_VERTEX].stride; return 1;
    case GL_NORMAL_ARRAY_STRIDE: *v = cl->arrays[ARR_NORMAL].stride; return 1;
    case GL_COLOR_ARRAY_STRIDE: *v = cl->arrays[ARR_COLOR].stride; return 1;
    case GL_INDEX_ARRAY_STRIDE: *v = cl->arrays[ARR_INDEX].stride; return 1;
    case GL_TEXTURE_COORD_ARRAY_STRIDE: *v = cl->arrays[ARR_TEXCOORD].stride; return 1;
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

void glGetBooleanv(GLenum pname, GLboolean *params)
{
    double v;
    TRACE(glGetBooleanv);
    if (client_get(pname, &v)) { params[0] = v != 0; return; }
    get_values(GLP_FN_glGetBooleanv, pname, params, 1);
}

void glGetIntegerv(GLenum pname, GLint *params)
{
    double v;
    TRACE(glGetIntegerv);
    if (client_get(pname, &v)) { params[0] = (GLint)v; return; }
    get_values(GLP_FN_glGetIntegerv, pname, params, 4);
}

void glGetFloatv(GLenum pname, GLfloat *params)
{
    double v;
    TRACE(glGetFloatv);
    if (client_get(pname, &v)) { params[0] = (GLfloat)v; return; }
    get_values(GLP_FN_glGetFloatv, pname, params, 4);
}

void glGetDoublev(GLenum pname, GLdouble *params)
{
    double v;
    TRACE(glGetDoublev);
    if (client_get(pname, &v)) { params[0] = v; return; }
    get_values(GLP_FN_glGetDoublev, pname, params, 8);
}

GLboolean glIsEnabled(GLenum cap)
{
    unsigned int arg = cap;
    int i = array_index(cap);
    TRACE(glIsEnabled);
    if (i >= 0) return (GLboolean)ctx()->client.arrays[i].enabled;
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
    void name(GLenum a, GLenum pname, T *params)                                                   \
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

void glGetTexLevelParameterfv(GLenum target, GLint level, GLenum pname, GLfloat *params)
{
    unsigned int a[3];
    TRACE(glGetTexLevelParameterfv);
    a[0] = target; a[1] = (unsigned int)level; a[2] = pname;
    if (params) glp_query(GLP_FN_glGetTexLevelParameterfv, params, 4, a, 3);
}

void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params)
{
    unsigned int a[3];
    TRACE(glGetTexLevelParameteriv);
    a[0] = target; a[1] = (unsigned int)level; a[2] = pname;
    if (params) glp_query(GLP_FN_glGetTexLevelParameteriv, params, 4, a, 3);
}

void glGetClipPlane(GLenum plane, GLdouble *equation)
{
    unsigned int arg = plane;
    TRACE(glGetClipPlane);
    if (equation) glp_query(GLP_FN_glGetClipPlane, equation, 32, &arg, 1);
}

/* Image reads: the host fills the program's buffer following the pack state. */
void glGetPolygonStipple(GLubyte *mask)
{
    TRACE(glGetPolygonStipple);
    if (mask) {
        glp_query(GLP_FN_glGetPolygonStipple, mask,
                  image_bytes(&ctx()->client.pack, 32, 32, GL_COLOR_INDEX, GL_BITMAP), 0, 0);
    }
}

void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels)
{
    unsigned int a[6], bytes = image_bytes(&ctx()->client.pack, width, height, format, type);
    TRACE(glReadPixels);
    if (!pixels || !bytes) return;
    a[0] = (unsigned int)x; a[1] = (unsigned int)y; a[2] = (unsigned int)width; a[3] = (unsigned int)height;
    a[4] = format; a[5] = type;
    glp_query(GLP_FN_glReadPixels, pixels, bytes, a, 6);
}

void glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid *pixels)
{
    GLint width = 0, height = 1;
    unsigned int a[4], bytes;
    TRACE(glGetTexImage);
    glGetTexLevelParameteriv(target, level, GL_TEXTURE_WIDTH, &width);
    if (target != GL_TEXTURE_1D) glGetTexLevelParameteriv(target, level, GL_TEXTURE_HEIGHT, &height);
    bytes = image_bytes(&ctx()->client.pack, width, height, format, type);
    if (!pixels || !bytes) return;
    a[0] = target; a[1] = (unsigned int)level; a[2] = format; a[3] = type;
    glp_query(GLP_FN_glGetTexImage, pixels, bytes, a, 4);
}

void glGetPointerv(GLenum pname, GLvoid **params)
{
    GLXContext c = ctx();
    int i = -1;
    TRACE(glGetPointerv);
    switch (pname) {
    case GL_VERTEX_ARRAY_POINTER: i = ARR_VERTEX; break;
    case GL_NORMAL_ARRAY_POINTER: i = ARR_NORMAL; break;
    case GL_COLOR_ARRAY_POINTER: i = ARR_COLOR; break;
    case GL_INDEX_ARRAY_POINTER: i = ARR_INDEX; break;
    case GL_TEXTURE_COORD_ARRAY_POINTER: i = ARR_TEXCOORD; break;
    case GL_EDGE_FLAG_ARRAY_POINTER: i = ARR_EDGEFLAG; break;
    case GL_SELECTION_BUFFER_POINTER: *params = c->select_buffer; return;
    case GL_FEEDBACK_BUFFER_POINTER: *params = c->feedback_buffer; return;
    default: *params = 0; return;
    }
    *params = (GLvoid *)c->client.arrays[i].pointer;
}

void glGetPointervEXT(GLenum pname, GLvoid **params) { glGetPointerv(pname, params); }

GLboolean glIsList(GLuint list)
{
    TRACE(glIsList);
    return (GLboolean)glp_query(GLP_FN_glIsList, 0, 0, &list, 1);
}

GLboolean glIsTexture(GLuint texture)
{
    TRACE(glIsTexture);
    return (GLboolean)glp_query(GLP_FN_glIsTexture, 0, 0, &texture, 1);
}

GLboolean glIsTextureEXT(GLuint texture) { return glIsTexture(texture); }

GLuint glGenLists(GLsizei range)
{
    unsigned int arg = (unsigned int)range;
    TRACE(glGenLists);
    return (GLuint)glp_query(GLP_FN_glGenLists, 0, 0, &arg, 1);
}

void glGenTextures(GLsizei n, GLuint *textures)
{
    unsigned int arg = (unsigned int)n;
    TRACE(glGenTextures);
    if (n > 0 && textures) glp_query(GLP_FN_glGenTextures, textures, (unsigned int)n * 4, &arg, 1);
}

void glGenTexturesEXT(GLsizei n, GLuint *textures) { glGenTextures(n, textures); }

GLboolean glAreTexturesResident(GLsizei n, const GLuint *textures, GLboolean *residences)
{
    TRACE(glAreTexturesResident);
    if (n <= 0 || !textures || !residences) return GL_TRUE;
    return (GLboolean)glp_query(GLP_FN_glAreTexturesResident, residences, (unsigned int)n, textures,
                                (unsigned int)n);
}

GLboolean glAreTexturesResidentEXT(GLsizei n, const GLuint *textures, GLboolean *residences)
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
    "GL_EXT_draw_range_elements";

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

const GLubyte *glGetString(GLenum name)
{
    static char renderer[256], host_list[8192], extensions[2048];
    TRACE(glGetString);
    switch (name) {
    case GL_VENDOR:
        return (const GLubyte *)"irix-emu";
    case GL_VERSION:
        return (const GLubyte *)"1.1 glpass";
    case GL_RENDERER:
        if (!renderer[0] && current) {
            char host[200];
            unsigned int arg = GL_RENDERER;
            memset(host, 0, sizeof host);
            glp_query(GLP_FN_glGetString, host, sizeof host - 1, &arg, 1);
            snprintf(renderer, sizeof renderer, "glpass (%s)", host);
        }
        return (const GLubyte *)(renderer[0] ? renderer : "glpass");
    case GL_EXTENSIONS:
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
   sprocs), and Xlib connections are not thread-safe. A lock, which the
   owning process may take again, keeps this connection to one thread. */
#ifdef __sgi
static abilock_t xlock;
static int xlock_ready;
#endif
static pid_t xlock_owner;
static int xlock_depth;

static void xlock_acquire(void)
{
#ifdef __sgi
    if (!xlock_ready) {
        init_lock(&xlock);
        xlock_ready = 1;
    }
    if (xlock_owner == getpid()) {
        ++xlock_depth;
        return;
    }
    spin_lock(&xlock);
#endif
    xlock_owner = getpid();
    xlock_depth = 1;
}

static void xlock_release(void)
{
    if (--xlock_depth > 0) return;
    xlock_owner = 0;
#ifdef __sgi
    release_lock(&xlock);
#endif
}

/* The library's connection to the server `app` is connected to. */
static Display *xconn(Display *app)
{
    static Display *own;
    static int failed;
    if (!own && !failed && app) {
        own = XOpenDisplay(DisplayString(app));
        if (!own) {
            failed = 1;
            fprintf(stderr, "glpass: cannot open a second X connection; using the program's\n");
        }
    }
    return own ? own : app;
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
static void load_palette(Display *dpy, GLXContext c, GLXDrawable drawable)
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
        xlock_acquire();
        if (!XGetWindowAttributes(dpy, drawable, &wa) || wa.colormap == None) {
            xlock_release();
            return;
        }
        n = wa.visual && wa.visual->map_entries > 0 && wa.visual->map_entries <= 256 ? wa.visual->map_entries : 256;
        for (i = 0; i < n; ++i) colors[i].pixel = (unsigned long)i;
        XQueryColors(dpy, wa.colormap, colors, n);
        xlock_release();
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

static void set_index(double index)
{
    unsigned int i;
    if (current && !current->index_mode) {
        glx_log("glIndex(%ld) in RGBA context %u on %#lx", (long)index, current->id, (unsigned long)current->drawable);
    }
    if (!current || !current->index_mode) return;
    i = (unsigned int)(long)index % PALETTE_SIZE;
    glColor4ub((GLubyte)GLP_INDEX_R(i), (GLubyte)GLP_INDEX_G(i), (GLubyte)GLP_INDEX_B(i), (GLubyte)GLP_INDEX_A(i));
}

void glIndexd(GLdouble c) { TRACE(glIndexd); set_index(c); }
void glIndexf(GLfloat c) { TRACE(glIndexf); set_index(c); }
void glIndexi(GLint c) { TRACE(glIndexi); set_index(c); }
void glIndexs(GLshort c) { TRACE(glIndexs); set_index(c); }
void glIndexub(GLubyte c) { TRACE(glIndexub); set_index(c); }
void glIndexdv(const GLdouble *c) { TRACE(glIndexdv); set_index(c[0]); }
void glIndexfv(const GLfloat *c) { TRACE(glIndexfv); set_index(c[0]); }
void glIndexiv(const GLint *c) { TRACE(glIndexiv); set_index(c[0]); }
void glIndexsv(const GLshort *c) { TRACE(glIndexsv); set_index(c[0]); }
void glIndexubv(const GLubyte *c) { TRACE(glIndexubv); set_index(c[0]); }

void glClearIndex(GLfloat c)
{
    unsigned int i;
    TRACE(glClearIndex);
    if (!current || !current->index_mode) return;
    i = (unsigned int)(long)c % PALETTE_SIZE;
    glClearColor((GLfloat)GLP_INDEX_R(i) / 255.0f, (GLfloat)GLP_INDEX_G(i) / 255.0f, (GLfloat)GLP_INDEX_B(i) / 255.0f,
                 (GLfloat)GLP_INDEX_A(i) / 255.0f);
}

/* In a colour-index context, GL_INDEX_LOGIC_OP (GL_LOGIC_OP) acts on the
   stored indices: the RGBA host buffer needs GL_COLOR_LOGIC_OP for that. */
static void put_cap(unsigned int fn, GLenum cap)
{
    glp_put(fn, 1)[0] = cap;
    if (cap == GL_INDEX_LOGIC_OP && current && current->index_mode) glp_put(fn, 1)[0] = GL_COLOR_LOGIC_OP;
}

void glEnable(GLenum cap)
{
    TRACE(glEnable);
    put_cap(GLP_FN_glEnable, cap);
}

void glDisable(GLenum cap)
{
    TRACE(glDisable);
    put_cap(GLP_FN_glDisable, cap);
}

/* The index write mask, per group of planes stored in one channel. */
void glIndexMask(GLuint mask)
{
    unsigned int *p;
    TRACE(glIndexMask);
    if (!current || !current->index_mode) return;
    glx_log("glIndexMask(%#x) context %u", mask, current->id);
    p = glp_put(GLP_FN_glColorMask, 4);
    p[0] = (mask & 0x0FFu) != 0;
    p[1] = (mask & 0x300u) != 0;
    p[2] = (mask & 0x400u) != 0;
    p[3] = (mask & 0x800u) != 0;
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

static struct glp_drawable *find_drawable(XID id)
{
    struct glp_drawable *d;
    for (d = drawables; d; d = d->next) {
        if (d->id == id) return d;
    }
    return 0;
}

static struct glp_drawable *add_drawable(XID id, int kind)
{
    struct glp_drawable *d = (struct glp_drawable *)calloc(1, sizeof *d);
    if (!d) return 0;
    d->id = id;
    d->kind = kind;
    d->next = drawables;
    drawables = d;
    return d;
}

static void remove_drawable(XID id)
{
    struct glp_drawable **pp;
    for (pp = &drawables; *pp; pp = &(*pp)->next) {
        if ((*pp)->id == id) {
            struct glp_drawable *d = *pp;
            *pp = d->next;
            free(d);
            return;
        }
    }
}

static int x_error_code;

static int note_x_error(Display *dpy, XErrorEvent *event)
{
    (void)dpy;
    x_error_code = event->error_code;
    return 0;
}

/* XGetGeometry that reports, rather than raises, an X error. */
static Status checked_geometry(Display *dpy, Drawable d, Window *root, int *x, int *y, unsigned int *w,
                               unsigned int *h, unsigned int *border, unsigned int *depth)
{
    int (*previous)(Display *, XErrorEvent *);
    Status ok;
    XSync(dpy, False);
    x_error_code = 0;
    previous = XSetErrorHandler(note_x_error);
    ok = XGetGeometry(dpy, d, root, x, y, w, h, border, depth);
    XSync(dpy, False);
    XSetErrorHandler(previous);
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
    xlock_acquire();
    on_screen = drawable_geometry_unlocked(xconn(dpy), d, sx, sy, w, h);
    xlock_release();
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
    xlock_acquire();
    update_pixmap_unlocked(pd);
    xlock_release();
}

static void present_unlocked(Display *dpy, Display *app, GLXDrawable drawable, int swap);

/* Window positions and sizes last queried, by window. */
#define GEOMETRY_KNOWN 32
static struct {
    GLXDrawable drawable;
    int x, y;
    unsigned int w, h;
    double when; /* seconds */
} geometry_cache[GEOMETRY_KNOWN];

static double now_seconds(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}
static int next_geometry_slot;

/* The noted geometry of `d`, if noted less than `max_age` seconds ago. */
static int cached_geometry_within(GLXDrawable d, int *x, int *y, unsigned int *w, unsigned int *h, double max_age)
{
    int i;
    for (i = 0; i < GEOMETRY_KNOWN; ++i) {
        if (geometry_cache[i].drawable == d && d && now_seconds() - geometry_cache[i].when <= max_age) {
            *x = geometry_cache[i].x;
            *y = geometry_cache[i].y;
            *w = geometry_cache[i].w;
            *h = geometry_cache[i].h;
            return 1;
        }
    }
    return 0;
}

static void note_geometry(GLXDrawable d, int x, int y, unsigned int w, unsigned int h)
{
    int i, slot = -1;
    for (i = 0; i < GEOMETRY_KNOWN; ++i) {
        if (geometry_cache[i].drawable == d) slot = i;
    }
    if (slot < 0) {
        slot = next_geometry_slot;
        next_geometry_slot = (next_geometry_slot + 1) % GEOMETRY_KNOWN;
    }
    geometry_cache[slot].drawable = d;
    geometry_cache[slot].x = x;
    geometry_cache[slot].y = y;
    geometry_cache[slot].w = w;
    geometry_cache[slot].h = h;
    geometry_cache[slot].when = now_seconds();
}

static int cached_geometry(GLXDrawable d, int *x, int *y, unsigned int *w, unsigned int *h)
{
    return cached_geometry_within(d, x, y, w, h, 1e30);
}

static void present(Display *dpy, GLXDrawable drawable, int swap)
{
    if (!dpy) return;
    xlock_acquire();
    present_unlocked(xconn(dpy), dpy, drawable, swap);
    xlock_release();
}

/* `dpy` is this library's connection, `app` the program's. */
static void present_unlocked(Display *dpy, Display *app, GLXDrawable drawable, int swap)
{
    struct glp_drawable *pd = find_drawable(drawable);
    unsigned int req[7];
    int sx, sy;
    unsigned int w, h;
    if (!dpy) return;
    glp_flush();
    memset(req, 0, sizeof req);
    req[0] = (unsigned int)drawable;
    if (pd) {
        req[6] = (swap ? GLP_SWAP_BACK_TO_FRONT : 0) | GLP_SWAP_NO_DISPLAY;
        glp_call(GLP_SERVICE_GL, GLP_OP_SWAP, req, sizeof req, 0);
        if (pd->kind == DRAW_PIXMAP && current && current->drawable == drawable) update_pixmap(pd);
        return;
    }
    /* Overlay windows (menus, marquee) present while the program may hold a
       server grab, during which this library's connection gets no replies:
       they use the position noted at their last ordinary query. */
    /* Other windows reuse it for a quarter of a second: fewer round trips,
       and a move or resize shows a few frames later at most. */
    if (!(is_overlay_window(drawable) ? cached_geometry(drawable, &sx, &sy, &w, &h)
                                      : cached_geometry_within(drawable, &sx, &sy, &w, &h, 0.25))) {
        if (!drawable_geometry(dpy, drawable, &sx, &sy, &w, &h)) {
            glx_log("present(%#lx): not a window on screen", (unsigned long)drawable);
            return;
        }
        note_geometry(drawable, sx, sy, w, h);
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
        values.foreground = GLP_OVERLAY_KEY;
        values.plane_mask = AllPlanes;
        values.function = GXcopy;
        gc = XCreateGC(app, drawable, GCForeground | GCPlaneMask | GCFunction, &values);
        XFillRectangle(app, drawable, gc, 0, 0, w, h);
        XFreeGC(app, gc);
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
    glp_call(GLP_SERVICE_GL, GLP_OP_SWAP, req, sizeof req, 0);
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
    int i;
    for (i = 0; i < nswapped; ++i) {
        if (swapped[i] == d) return 1;
    }
    return 0;
}

static void note_swapped(GLXDrawable d)
{
    if (has_swapped(d)) return;
    if (nswapped == SWAPPED_MAX) {
        memmove(swapped, swapped + 1, sizeof swapped[0] * (SWAPPED_MAX - 1)); /* forget the oldest */
        --nswapped;
    }
    swapped[nswapped++] = d;
}

/* Windows of the overlay planes' visual (noted when a context of it is made
   current on them): presented without the key fill, which belongs to the
   GL window below in the main planes. */
#define OVERLAY_WINDOWS_MAX 16
static GLXDrawable overlay_windows[OVERLAY_WINDOWS_MAX];
static int noverlay_windows;

static int is_overlay_window(GLXDrawable d)
{
    int i;
    for (i = 0; i < noverlay_windows; ++i) {
        if (overlay_windows[i] == d) return 1;
    }
    return 0;
}

static void note_overlay_window(GLXDrawable d)
{
    if (is_overlay_window(d)) return;
    if (noverlay_windows == OVERLAY_WINDOWS_MAX) {
        memmove(overlay_windows, overlay_windows + 1, sizeof overlay_windows[0] * (OVERLAY_WINDOWS_MAX - 1));
        --noverlay_windows;
    }
    overlay_windows[noverlay_windows++] = d;
}

/* glFlush and glFinish show what was drawn when it is meant to be seen now:
   drawing into the front buffer, or into a never-swapped drawable. */
static void present_flushed(void)
{
    if (!current || !current->drawable) return;
    load_palette(current->dpy, current, current->drawable);
    if (draws_front(current->draw_buffer)) {
        present(current->dpy, current->drawable, 0);
    } else if (!has_swapped(current->drawable)) {
        present(current->dpy, current->drawable, 1);
    }
}

void glFlush(void)
{
    TRACE(glFlush);
    glp_flush();
    if (current) glx_log("glFlush (context %u, draw buffer %#x)", current->id, current->draw_buffer);
    present_flushed();
}

void glFinish(void)
{
    TRACE(glFinish);
    glp_flush();
    if (current) glx_log("glFinish (context %u, draw buffer %#x)", current->id, current->draw_buffer);
    if (current) glp_call(GLP_SERVICE_GL, GLP_OP_FINISH, 0, 0, 0);
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
    glx_log("overlay visual %#lx, transparent index %d", (unsigned long)overlay_visual_id, overlay_transparent);
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
    case GLX_BUFFER_SIZE: *value = 8; break;
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

Bool glXQueryExtension(Display *dpy, int *error, int *event)
{
    (void)dpy;
    if (error) *error = 0;
    if (event) *event = 0;
    return glp_available() ? True : False;
}

Bool glXQueryVersion(Display *dpy, int *major, int *minor)
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

int glXGetConfig(Display *dpy, XVisualInfo *vis, int attribute, int *value)
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
    chosen[next_chosen].vi = vi;
    chosen[next_chosen].index = index;
    next_chosen = (next_chosen + 1) % CHOSEN_MAX;
}

/* 1 colour index, 0 RGBA, -1 not handed out here. */
static int chosen_mode(const XVisualInfo *vi)
{
    int i;
    for (i = 0; i < CHOSEN_MAX; ++i) {
        if (vi && chosen[i].vi == vi) return chosen[i].index;
    }
    return -1;
}

XVisualInfo *glXChooseVisual(Display *dpy, int screen, int *attributes)
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
        templ.visualid = overlay_visual(dpy, screen);
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

GLXFBConfig *glXGetFBConfigs(Display *dpy, int screen, int *nelements)
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

GLXFBConfig *glXChooseFBConfig(Display *dpy, int screen, int *attributes, int *nitems)
{
    log_attributes("glXChooseFBConfig", attributes);
    if (nitems) *nitems = 0;
    if (!attributes_match(attributes, 1)) return 0;
    return glXGetFBConfigs(dpy, screen, nitems);
}

GLXFBConfigSGIX *glXChooseFBConfigSGIX(Display *dpy, int screen, int *attributes, int *nitems)
{
    return glXChooseFBConfig(dpy, screen, attributes, nitems);
}

int glXGetFBConfigAttrib(Display *dpy, GLXFBConfig config, int attribute, int *value)
{
    (void)dpy;
    if (!config || !value) return GLX_BAD_ATTRIBUTE;
    if (attribute == GLX_VISUAL_ID || attribute == GLX_FBCONFIG_ID) {
        *value = (int)config->visual;
        return 0;
    }
    return config_value(attribute, value);
}

int glXGetFBConfigAttribSGIX(Display *dpy, GLXFBConfigSGIX config, int attribute, int *value)
{
    return glXGetFBConfigAttrib(dpy, config, attribute, value);
}

XVisualInfo *glXGetVisualFromFBConfig(Display *dpy, GLXFBConfig config)
{
    XVisualInfo templ;
    int count = 0;
    if (!config) return 0;
    templ.visualid = config->visual;
    return XGetVisualInfo(dpy, VisualIDMask, &templ, &count);
}

XVisualInfo *glXGetVisualFromFBConfigSGIX(Display *dpy, GLXFBConfigSGIX config)
{
    return glXGetVisualFromFBConfig(dpy, config);
}

GLXFBConfigSGIX glXGetFBConfigFromVisualSGIX(Display *dpy, XVisualInfo *vis)
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
    id = glp_call(GLP_SERVICE_GL, GLP_OP_CREATE_CONTEXT, 0, share ? share->id : 0, 0);
    if (id <= 0) return 0;
    c = (GLXContext)calloc(1, sizeof *c);
    if (!c) return 0;
    c->id = (unsigned int)id;
    c->dpy = dpy;
    c->screen = screen;
    c->visual = visual;
    c->share = share;
    context_defaults(c);
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

GLXContext glXCreateContext(Display *dpy, XVisualInfo *vis, GLXContext share, Bool direct)
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

GLXContext glXCreateNewContext(Display *dpy, GLXFBConfig config, int render_type, GLXContext share, Bool direct)
{
    GLXContext c;
    (void)direct;
    c = new_context(dpy, config ? config->screen : DefaultScreen(dpy), config ? config->visual : 0, share);
    if (c && !c->overlay) c->index_mode = render_type != GLX_RGBA_TYPE; /* the mode asked for */
    return c;
}

GLXContext glXCreateContextWithConfigSGIX(Display *dpy, GLXFBConfigSGIX config, int render_type, GLXContext share,
                                          Bool direct)
{
    return glXCreateNewContext(dpy, config, render_type, share, direct);
}

void glXDestroyContext(Display *dpy, GLXContext c)
{
    (void)dpy;
    if (!c) return;
    if (c == current) {
        glp_flush();
        current = 0;
    }
    glp_call(GLP_SERVICE_GL, GLP_OP_DESTROY_CONTEXT, 0, c->id, 0);
    free(c);
}

void glXCopyContext(Display *dpy, GLXContext src, GLXContext dst, unsigned long mask)
{
    (void)dpy; (void)src; (void)dst; (void)mask;
    glp_unimplemented("glXCopyContext");
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
    int i, index;
    for (i = 0; i < WINDOW_MODES_MAX; ++i) {
        if (window_modes[i].drawable == drawable && drawable) return window_modes[i].index;
    }
    xlock_acquire();
    dpy = xconn(app);
    index = XGetWindowAttributes(dpy, drawable, &wa) && wa.colormap != None &&
            wa.colormap != DefaultColormap(dpy, DefaultScreen(dpy));
    xlock_release();
    window_modes[next_window_mode].drawable = drawable;
    window_modes[next_window_mode].index = index;
    next_window_mode = (next_window_mode + 1) % WINDOW_MODES_MAX;
    return index;
}

Bool glXMakeCurrent(Display *dpy, GLXDrawable drawable, GLXContext c)
{
    unsigned int req[4];
    int sx, sy;
    struct glp_drawable *pd;
    glx_log("glXMakeCurrent(%#lx, context %u) [context %u drew %lu words]", (unsigned long)drawable,
            c ? c->id : 0u, current ? current->id : 0u, (glp_flush(), submitted));
    submitted = 0;
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
    glp_flush();
    if (!c || !drawable) {
        memset(req, 0, sizeof req);
        glp_call(GLP_SERVICE_GL, GLP_OP_MAKE_CURRENT, req, sizeof req, 0);
        current = 0;
        return True;
    }
    XFlush(dpy); /* as GLX does: the program's new windows exist for this library's connection */
    /* Overlay windows may be made current under a server grab (see present). */
    if (!(c->overlay && cached_geometry(drawable, &sx, &sy, &c->width, &c->height))) {
        if (drawable_geometry(dpy, drawable, &sx, &sy, &c->width, &c->height)) {
            note_geometry(drawable, sx, sy, c->width, c->height);
        }
    }
    pd = find_drawable(drawable);
    if (pd) {
        pd->width = c->width;
        pd->height = c->height;
    }
    req[0] = c->id; req[1] = (unsigned int)drawable; req[2] = c->width; req[3] = c->height;
    if (glp_call(GLP_SERVICE_GL, GLP_OP_MAKE_CURRENT, req, sizeof req, 0) != 0) return False;
    c->dpy = dpy;
    c->drawable = drawable;
    current = c;
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

Bool glXMakeContextCurrent(Display *dpy, GLXDrawable draw, GLXDrawable read, GLXContext c)
{
    (void)read;
    return glXMakeCurrent(dpy, draw, c);
}

Bool glXMakeCurrentReadSGI(Display *dpy, GLXDrawable draw, GLXDrawable read, GLXContext c)
{
    return glXMakeContextCurrent(dpy, draw, read, c);
}

void glXSwapBuffers(Display *dpy, GLXDrawable drawable)
{
    glp_flush();
    if (dpy) XFlush(dpy); /* as GLX does (see glXMakeCurrent) */
    glx_log("glXSwapBuffers(%p, %#lx) [%lu words]", (void *)dpy, (unsigned long)drawable, submitted);
    note_swapped(drawable);
    submitted = 0;
    if (current && current->drawable == drawable) load_palette(current->dpy, current, drawable);
    present(dpy ? dpy : (current ? current->dpy : 0), drawable, 1);
}

GLXContext glXGetCurrentContext(void) { return current; }
GLXDrawable glXGetCurrentDrawable(void) { return current ? current->drawable : 0; }
GLXDrawable glXGetCurrentReadDrawable(void) { return glXGetCurrentDrawable(); }
GLXDrawable glXGetCurrentReadDrawableSGI(void) { return glXGetCurrentDrawable(); }
GLXDrawable glXGetCurrentDrawableEXT(void) { return glXGetCurrentDrawable(); }
Display *glXGetCurrentDisplay(void) { return current ? current->dpy : 0; }
Display *glXGetCurrentDisplayEXT(void) { return glXGetCurrentDisplay(); }
Bool glXIsDirect(Display *dpy, GLXContext c) { (void)dpy; (void)c; return True; }
void glXWaitGL(void) { glFinish(); }
void glXWaitX(void) { if (current) XSync(current->dpy, False); }

int glXQueryContext(Display *dpy, GLXContext c, int attribute, int *value)
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

int glXQueryContextInfoEXT(Display *dpy, GLXContext c, int attribute, int *value)
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

GLXContextID glXGetContextIDEXT(const GLXContext c) { return c ? c->id : 0; }
GLXContext glXImportContextEXT(Display *dpy, GLXContextID id) { (void)dpy; (void)id; return 0; }
void glXFreeContextEXT(Display *dpy, GLXContext c) { (void)dpy; (void)c; }

/* ---- GLX windows, pixmaps, pbuffers --------------------------------------------------- */

GLXWindow glXCreateWindow(Display *dpy, GLXFBConfig config, Window win, int *attributes)
{
    (void)dpy; (void)config; (void)attributes;
    return win;
}

void glXDestroyWindow(Display *dpy, GLXWindow win) { (void)dpy; (void)win; }

static GLXPixmap new_pixmap(VisualID visual, Pixmap pixmap)
{
    struct glp_drawable *d = add_drawable(pixmap, DRAW_PIXMAP);
    glx_log("GLX pixmap %#lx", (unsigned long)pixmap);
    if (!d) return 0;
    d->visual = visual;
    return pixmap;
}

GLXPixmap glXCreateGLXPixmap(Display *dpy, XVisualInfo *vis, Pixmap pixmap)
{
    (void)dpy;
    return new_pixmap(vis ? vis->visualid : 0, pixmap);
}

GLXPixmap glXCreatePixmap(Display *dpy, GLXFBConfig config, Pixmap pixmap, int *attributes)
{
    (void)dpy; (void)attributes;
    return new_pixmap(config ? config->visual : 0, pixmap);
}

GLXPixmap glXCreateGLXPixmapWithConfigSGIX(Display *dpy, GLXFBConfigSGIX config, Pixmap pixmap)
{
    return glXCreatePixmap(dpy, config, pixmap, 0);
}

void glXDestroyGLXPixmap(Display *dpy, GLXPixmap pix) { (void)dpy; remove_drawable(pix); }
void glXDestroyPixmap(Display *dpy, GLXPixmap pix) { (void)dpy; remove_drawable(pix); }

static XID new_pbuffer(unsigned int width, unsigned int height)
{
    struct glp_drawable *d = add_drawable(next_pbuffer, DRAW_PBUFFER);
    if (!d) return 0;
    ++next_pbuffer;
    d->width = width ? width : 1;
    d->height = height ? height : 1;
    return d->id;
}

GLXPbuffer glXCreatePbuffer(Display *dpy, GLXFBConfig config, int *attributes)
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

GLXPbufferSGIX glXCreateGLXPbufferSGIX(Display *dpy, GLXFBConfigSGIX config, unsigned int width,
                                        unsigned int height, int *attributes)
{
    (void)dpy; (void)config; (void)attributes;
    return new_pbuffer(width, height);
}

void glXDestroyPbuffer(Display *dpy, GLXPbuffer pbuf) { (void)dpy; remove_drawable(pbuf); }
void glXDestroyGLXPbufferSGIX(Display *dpy, GLXPbufferSGIX pbuf) { (void)dpy; remove_drawable(pbuf); }

void glXQueryDrawable(Display *dpy, GLXDrawable draw, int attribute, unsigned int *value)
{
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

int glXQueryGLXPbufferSGIX(Display *dpy, GLXPbufferSGIX pbuf, int attribute, unsigned int *value)
{
    glXQueryDrawable(dpy, pbuf, attribute, value);
    return 0;
}

/* No buffer-clobber events: the host buffers are never damaged. */
void glXSelectEvent(Display *dpy, GLXDrawable drawable, unsigned long mask) { (void)dpy; (void)drawable; (void)mask; }
void glXGetSelectedEvent(Display *dpy, GLXDrawable drawable, unsigned long *mask) { (void)dpy; (void)drawable; if (mask) *mask = 0; }
void glXSelectEventSGIX(Display *dpy, GLXDrawable drawable, unsigned long mask) { glXSelectEvent(dpy, drawable, mask); }
void glXGetSelectedEventSGIX(Display *dpy, GLXDrawable drawable, unsigned long *mask) { glXGetSelectedEvent(dpy, drawable, mask); }

/* ---- GLX strings, procedures, sync -------------------------------------------------------------- */

static const char glx_extensions[] =
    "GLX_EXT_visual_info GLX_EXT_import_context GLX_SGI_make_current_read GLX_SGI_swap_control "
    "GLX_SGI_video_sync GLX_SGIX_fbconfig GLX_SGIX_pbuffer";

const char *glXGetClientString(Display *dpy, int name)
{
    (void)dpy;
    switch (name) {
    case GLX_VENDOR: return "irix-emu glpass";
    case GLX_VERSION: return "1.3";
    case GLX_EXTENSIONS: return glx_extensions;
    default: return 0;
    }
}

const char *glXQueryServerString(Display *dpy, int screen, int name)
{
    (void)screen;
    return glXGetClientString(dpy, name);
}

const char *glXQueryExtensionsString(Display *dpy, int screen)
{
    (void)dpy; (void)screen;
    return glx_extensions;
}

void (*glXGetProcAddress(const GLubyte *name))(void)
{
    static void *self;
    if (!self) self = dlopen("libGL.so", RTLD_LAZY);
    return self ? (void (*)(void))dlsym(self, (const char *)name) : 0;
}

void (*glXGetProcAddressARB(const GLubyte *name))(void) { return glXGetProcAddress(name); }

static unsigned int video_count;

int glXGetVideoSyncSGI(unsigned int *count)
{
    if (count) *count = ++video_count;
    return 0;
}

int glXWaitVideoSyncSGI(int divisor, int remainder, unsigned int *count)
{
    ++video_count;
    if (divisor > 0 && remainder >= 0 && remainder < divisor) {
        while ((int)(video_count % (unsigned int)divisor) != remainder) ++video_count;
    }
    if (count) *count = video_count;
    return 0;
}

int glXSwapIntervalSGI(int interval) { (void)interval; return 0; }

/* Large-system features (channels, hyperpipes, swap groups, video sources) do not exist here. */
int glXBindChannelToWindowSGIX(Display *dpy, int screen, int channel, Window window) { (void)dpy; (void)screen; (void)channel; (void)window; return 0; }
int glXQueryChannelDeltasSGIX(Display *dpy, int screen, int channel, int *dx, int *dy, int *dw, int *dh) { (void)dpy; (void)screen; (void)channel; (void)dx; (void)dy; (void)dw; (void)dh; return 0; }
int glXChannelRectSGIX(Display *dpy, int screen, int channel, int x, int y, int w, int h) { (void)dpy; (void)screen; (void)channel; (void)x; (void)y; (void)w; (void)h; return 0; }
int glXQueryChannelRectSGIX(Display *dpy, int screen, int channel, int *x, int *y, int *w, int *h) { (void)dpy; (void)screen; (void)channel; (void)x; (void)y; (void)w; (void)h; return 0; }
int glXChannelRectSyncSGIX(Display *dpy, int screen, int channel, GLenum synctype) { (void)dpy; (void)screen; (void)channel; (void)synctype; return 0; }
GLXHyperpipeNetworkSGIX *glXQueryHyperpipeNetworkSGIX(Display *dpy, int *npipes) { (void)dpy; if (npipes) *npipes = 0; return 0; }
int glXHyperpipeConfigSGIX(Display *dpy, int networkId, int npipes, GLXHyperpipeConfigSGIX *cfg, int *hpId) { (void)dpy; (void)networkId; (void)npipes; (void)cfg; (void)hpId; return GLX_BAD_CONTEXT; }
int glXDestroyHyperpipeConfigSGIX(Display *dpy, int hpId) { (void)dpy; (void)hpId; return GLX_BAD_CONTEXT; }
GLXHyperpipeConfigSGIX *glXQueryHyperpipeConfigSGIX(Display *dpy, int hpId, int *npipes) { (void)dpy; (void)hpId; if (npipes) *npipes = 0; return 0; }
int glXBindHyperpipeSGIX(Display *dpy, int hpId) { (void)dpy; (void)hpId; return GLX_BAD_CONTEXT; }
int glXQueryHyperpipeBestAttribSGIX(Display *dpy, int timeSlice, int attrib, int size, void *attribList, void *returnAttribList) { (void)dpy; (void)timeSlice; (void)attrib; (void)size; (void)attribList; (void)returnAttribList; return GLX_BAD_CONTEXT; }
int glXQueryHyperpipeAttribSGIX(Display *dpy, int timeSlice, int attrib, int size, void *returnAttribList) { (void)dpy; (void)timeSlice; (void)attrib; (void)size; (void)returnAttribList; return GLX_BAD_CONTEXT; }
int glXHyperpipeAttribSGIX(Display *dpy, int timeSlice, int attrib, int size, void *attribList) { (void)dpy; (void)timeSlice; (void)attrib; (void)size; (void)attribList; return GLX_BAD_CONTEXT; }
void glXJoinSwapGroupSGIX(Display *dpy, GLXDrawable drawable, GLXDrawable member) { (void)dpy; (void)drawable; (void)member; }
void glXBindSwapBarrierSGIX(Display *dpy, GLXDrawable drawable, int barrier) { (void)dpy; (void)drawable; (void)barrier; }
Bool glXQueryMaxSwapBarriersSGIX(Display *dpy, int screen, int *max) { (void)dpy; (void)screen; if (max) *max = 0; return False; }
/* glx.h declares these only with the video and media library headers; their
   argument types are opaque here. */
GLXVideoSourceSGIX glXCreateGLXVideoSourceSGIX(Display *dpy, int screen, void *server, int path, int nodeClass, int node) { (void)dpy; (void)screen; (void)server; (void)path; (void)nodeClass; (void)node; return 0; }
void glXDestroyGLXVideoSourceSGIX(Display *dpy, GLXVideoSourceSGIX source) { (void)dpy; (void)source; }
Bool glXAssociateDMPbufferSGIX(Display *dpy, GLXPbufferSGIX pbuffer, void *params, void *buffer) { (void)dpy; (void)pbuffer; (void)params; (void)buffer; return False; }

/* ---- glXUseXFont ---------------------------------------------------------------------------------- */

/* One glBitmap display list per glyph, drawn by X into a bitmap (the method
   of Mesa's xfonts.c). */
void glXUseXFont(Font font, int first, int count, int listBase)
{
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
