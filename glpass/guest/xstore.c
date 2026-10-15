/*
 * libglpass_x.so: notes the colours programs store in X colormaps, for the
 * colour-index emulation of libGL.so (glpass.c), and lets those programs draw
 * with X in their 12-bit colours.
 *
 * Programs written for 12-bit colour index (Maya) store colours at pixels
 * the server's 8-bit colormaps do not have; the server refuses them, but the
 * emulation needs them. This library defines XStoreColors, XStoreColor and
 * XStoreNamedColor, notes each colour, then calls libX11's function.
 *
 * Such programs also draw with X using those pixels (Maya's marking menus and
 * sliders when there are no overlay planes), of which the server keeps only
 * the low 8 bits. So the calls that hand pixel values to X (GC foreground and
 * background, window background and border) are defined here too: a 12-bit
 * pixel gets an 8-bit cell of its own in the colormap, which receives the
 * pixel's colour (and its later changes), and X is given that cell; in a
 * colormap holding the default colormap's colours (see XCreateColormap
 * below), the nearest of those instead.
 *
 * The library must come before libX11 in the symbol search, so it is
 * preloaded:
 *   _RLDN32_LIST=/usr/local/irix-emu/glpass/libglpass_x.so:DEFAULT
 * It links nothing but libc, so the programs that do not use X (every
 * process inherits the variable) load it harmlessly.
 */
#include <X11/Xlib.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glpass_xstore.h"

static struct glpass_colormap *colormaps;
static unsigned long generation = 1;

unsigned long glpass_colormap_generation(void) { return generation; }

struct glpass_colormap *glpass_find_colormap(unsigned long id)
{
    struct glpass_colormap *m;
    for (m = colormaps; m; m = m->next) {
        if (m->id == id) return m;
    }
    return 0;
}

/* libX11's own function `name`. */
static void *x11_function(const char *name)
{
    static void *x11;
    if (!x11) x11 = dlopen("libX11.so.1", RTLD_LAZY);
    if (!x11) x11 = dlopen("libX11.so", RTLD_LAZY);
    return x11 ? dlsym(x11, name) : 0;
}

/* ---- 8-bit cells for 12-bit pixels ------------------------------------------ */

struct cells {
    unsigned long colormap;
    Display *dpy;
    unsigned char own[256];                   /* cells the program stored itself */
    unsigned char taken[256];                 /* cells given to high pixels */
    unsigned short cell[GLPASS_PALETTE_SIZE]; /* high pixel -> cell (0: none) */
    int mirrored;                             /* holds the default colormap's colours */
    unsigned char mirror[256][3];
    struct cells *next;
};

static struct cells *cell_maps;

static struct cells *cells_of(unsigned long colormap, int create)
{
    struct cells *c;
    for (c = cell_maps; c; c = c->next) {
        if (c->colormap == colormap) return c;
    }
    if (!create) return 0;
    c = (struct cells *)calloc(1, sizeof *c);
    if (!c) return 0;
    c->colormap = colormap;
    c->next = cell_maps;
    cell_maps = c;
    return c;
}

/* Stores high pixel `pixel`'s colour into its cell. */
static void store_cell(struct cells *c, const struct glpass_colormap *m, unsigned long pixel)
{
    static int (*real)(Display *, Colormap, XColor *);
    XColor color;
    if (!c->dpy || !c->cell[pixel]) return;
    if (!real) real = (int (*)(Display *, Colormap, XColor *))x11_function("XStoreColor");
    if (!real) return;
    color.pixel = c->cell[pixel];
    color.red = (unsigned short)(m->rgb[pixel][0] * 257u);
    color.green = (unsigned short)(m->rgb[pixel][1] * 257u);
    color.blue = (unsigned short)(m->rgb[pixel][2] * 257u);
    color.flags = DoRed | DoGreen | DoBlue;
    real(c->dpy, (Colormap)c->colormap, &color);
}

/* The pixel to hand X for `pixel`: an 8-bit cell for a 12-bit pixel the
   program stored a colour at, the pixel itself otherwise. */
/* GLPASS_TRACE=1: each (function, pixel) the program hands X, once. */
static void trace_pixel(const char *function, unsigned long pixel)
{
    static int enabled = -1;
    static char seen[64][48];
    static int nseen;
    char key[48];
    int i;
    if (enabled < 0) {
        const char *v = getenv("GLPASS_TRACE");
        enabled = v && *v && *v != '0';
    }
    if (!enabled) return;
    sprintf(key, "%.24s %lu", function, pixel);
    for (i = 0; i < nseen; ++i) {
        if (strcmp(seen[i], key) == 0) return;
    }
    if (nseen < 64) strcpy(seen[nseen++], key);
    fprintf(stderr, "glpass_x: %s pixel %lu\n", function, pixel);
}

static unsigned long remap(Display *dpy, unsigned long pixel)
{
    struct glpass_colormap *m;
    struct cells *c;
    int i;
    if (pixel < 256 || pixel >= GLPASS_PALETTE_SIZE) return pixel;
    for (m = colormaps; m; m = m->next) { /* the newest colormap holding it */
        if (m->set[pixel]) break;
    }
    if (!m) return pixel;
    c = cells_of(m->id, 1);
    if (!c) return pixel & 0xFF;
    if (!c->dpy) c->dpy = dpy;
    if (c->mirrored) { /* the cells are the default colormap's: the nearest colour */
        long best = -1;
        int found = 0;
        for (i = 0; i < 256; ++i) {
            long dr = (long)c->mirror[i][0] - m->rgb[pixel][0];
            long dg = (long)c->mirror[i][1] - m->rgb[pixel][1];
            long db = (long)c->mirror[i][2] - m->rgb[pixel][2];
            long d = dr * dr + dg * dg + db * db;
            if (best < 0 || d < best) {
                best = d;
                found = i;
            }
        }
        return (unsigned long)found;
    }
    if (!c->cell[pixel]) {
        for (i = 255; i > 0; --i) { /* from the top: programs use the low cells */
            if (!c->own[i] && !c->taken[i]) break;
        }
        if (i == 0) return pixel & 0xFF; /* none left */
        c->taken[i] = 1;
        c->cell[pixel] = (unsigned short)i;
        store_cell(c, m, pixel);
    }
    return c->cell[pixel];
}

/* ---- Colour stores --------------------------------------------------------------- */

static void note_color(Colormap id, const XColor *color)
{
    struct glpass_colormap *m;
    struct cells *c;
    if (color->pixel >= GLPASS_PALETTE_SIZE) return;
    m = glpass_find_colormap(id);
    if (!m) {
        m = (struct glpass_colormap *)calloc(1, sizeof *m);
        if (!m) return;
        m->id = id;
        m->next = colormaps;
        colormaps = m;
    }
    if (color->flags & DoRed) m->rgb[color->pixel][0] = (unsigned char)(color->red >> 8);
    if (color->flags & DoGreen) m->rgb[color->pixel][1] = (unsigned char)(color->green >> 8);
    if (color->flags & DoBlue) m->rgb[color->pixel][2] = (unsigned char)(color->blue >> 8);
    m->set[color->pixel] = 1;
    ++generation;
    c = cells_of(id, color->pixel < 256);
    if (!c) return;
    if (color->pixel < 256) {
        c->own[color->pixel] = 1; /* the program's own cell: not for high pixels */
    } else {
        store_cell(c, m, color->pixel); /* a high pixel's cell follows its colour */
    }
}

int XStoreColors(Display *dpy, Colormap colormap, XColor *colors, int ncolors)
{
    static int (*real)(Display *, Colormap, XColor *, int);
    int i;
    for (i = 0; i < ncolors; ++i) note_color(colormap, &colors[i]);
    if (!real) real = (int (*)(Display *, Colormap, XColor *, int))x11_function("XStoreColors");
    return real ? real(dpy, colormap, colors, ncolors) : 0;
}

int XStoreColor(Display *dpy, Colormap colormap, XColor *color)
{
    static int (*real)(Display *, Colormap, XColor *);
    note_color(colormap, color);
    if (!real) real = (int (*)(Display *, Colormap, XColor *))x11_function("XStoreColor");
    return real ? real(dpy, colormap, color) : 0;
}

int XStoreNamedColor(Display *dpy, Colormap colormap, _Xconst char *name, unsigned long pixel, int flags)
{
    static int (*real)(Display *, Colormap, _Xconst char *, unsigned long, int);
    static Status (*parse)(Display *, Colormap, _Xconst char *, XColor *);
    XColor color;
    if (!parse) parse = (Status (*)(Display *, Colormap, _Xconst char *, XColor *))x11_function("XParseColor");
    if (parse && parse(dpy, colormap, name, &color)) {
        color.pixel = pixel;
        color.flags = (char)flags;
        note_color(colormap, &color);
    }
    if (!real) {
        real = (int (*)(Display *, Colormap, _Xconst char *, unsigned long, int))x11_function("XStoreNamedColor");
    }
    return real ? real(dpy, colormap, name, pixel, flags) : 0;
}

/* ---- Pixel values given to X ---------------------------------------------------- */

int XSetForeground(Display *dpy, GC gc, unsigned long pixel)
{
    static int (*real)(Display *, GC, unsigned long);
    if (!real) real = (int (*)(Display *, GC, unsigned long))x11_function("XSetForeground");
    trace_pixel("XSetForeground", pixel);
    return real ? real(dpy, gc, remap(dpy, pixel)) : 0;
}

int XSetBackground(Display *dpy, GC gc, unsigned long pixel)
{
    static int (*real)(Display *, GC, unsigned long);
    if (!real) real = (int (*)(Display *, GC, unsigned long))x11_function("XSetBackground");
    trace_pixel("XSetBackground", pixel);
    return real ? real(dpy, gc, remap(dpy, pixel)) : 0;
}

int XSetState(Display *dpy, GC gc, unsigned long foreground, unsigned long background, int function,
              unsigned long plane_mask)
{
    static int (*real)(Display *, GC, unsigned long, unsigned long, int, unsigned long);
    if (!real) {
        real = (int (*)(Display *, GC, unsigned long, unsigned long, int, unsigned long))x11_function("XSetState");
    }
    return real ? real(dpy, gc, remap(dpy, foreground), remap(dpy, background), function, plane_mask) : 0;
}

static XGCValues *remap_gc_values(Display *dpy, unsigned long mask, XGCValues *values, XGCValues *copy)
{
    if (!values || !(mask & (GCForeground | GCBackground))) return values;
    if (mask & GCForeground) trace_pixel("GC foreground", values->foreground);
    if (mask & GCBackground) trace_pixel("GC background", values->background);
    *copy = *values;
    if (mask & GCForeground) copy->foreground = remap(dpy, values->foreground);
    if (mask & GCBackground) copy->background = remap(dpy, values->background);
    return copy;
}

int XChangeGC(Display *dpy, GC gc, unsigned long mask, XGCValues *values)
{
    static int (*real)(Display *, GC, unsigned long, XGCValues *);
    XGCValues copy;
    if (!real) real = (int (*)(Display *, GC, unsigned long, XGCValues *))x11_function("XChangeGC");
    return real ? real(dpy, gc, mask, remap_gc_values(dpy, mask, values, &copy)) : 0;
}

GC XCreateGC(Display *dpy, Drawable d, unsigned long mask, XGCValues *values)
{
    static GC (*real)(Display *, Drawable, unsigned long, XGCValues *);
    XGCValues copy;
    if (!real) real = (GC (*)(Display *, Drawable, unsigned long, XGCValues *))x11_function("XCreateGC");
    return real ? real(dpy, d, mask, remap_gc_values(dpy, mask, values, &copy)) : 0;
}

int XSetWindowBackground(Display *dpy, Window w, unsigned long pixel)
{
    static int (*real)(Display *, Window, unsigned long);
    if (!real) real = (int (*)(Display *, Window, unsigned long))x11_function("XSetWindowBackground");
    trace_pixel("XSetWindowBackground", pixel);
    return real ? real(dpy, w, remap(dpy, pixel)) : 0;
}

int XSetWindowBorder(Display *dpy, Window w, unsigned long pixel)
{
    static int (*real)(Display *, Window, unsigned long);
    if (!real) real = (int (*)(Display *, Window, unsigned long))x11_function("XSetWindowBorder");
    trace_pixel("XSetWindowBorder", pixel);
    return real ? real(dpy, w, remap(dpy, pixel)) : 0;
}

static XSetWindowAttributes *remap_window_values(Display *dpy, unsigned long mask, XSetWindowAttributes *values,
                                                 XSetWindowAttributes *copy)
{
    if (!values || !(mask & (CWBackPixel | CWBorderPixel))) return values;
    if (mask & CWBackPixel) trace_pixel("window background", values->background_pixel);
    *copy = *values;
    if (mask & CWBackPixel) copy->background_pixel = remap(dpy, values->background_pixel);
    if (mask & CWBorderPixel) copy->border_pixel = remap(dpy, values->border_pixel);
    return copy;
}

int XChangeWindowAttributes(Display *dpy, Window w, unsigned long mask, XSetWindowAttributes *values)
{
    static int (*real)(Display *, Window, unsigned long, XSetWindowAttributes *);
    XSetWindowAttributes copy;
    if (!real) {
        real = (int (*)(Display *, Window, unsigned long, XSetWindowAttributes *))x11_function(
            "XChangeWindowAttributes");
    }
    return real ? real(dpy, w, mask, remap_window_values(dpy, mask, values, &copy)) : 0;
}

Window XCreateWindow(Display *dpy, Window parent, int x, int y, unsigned int width, unsigned int height,
                     unsigned int border_width, int depth, unsigned int class_, Visual *visual, unsigned long mask,
                     XSetWindowAttributes *values)
{
    static Window (*real)(Display *, Window, int, int, unsigned int, unsigned int, unsigned int, int, unsigned int,
                          Visual *, unsigned long, XSetWindowAttributes *);
    XSetWindowAttributes copy;
    if (!real) {
        real = (Window (*)(Display *, Window, int, int, unsigned int, unsigned int, unsigned int, int, unsigned int,
                           Visual *, unsigned long, XSetWindowAttributes *))x11_function("XCreateWindow");
    }
    return real ? real(dpy, parent, x, y, width, height, border_width, depth, class_, visual, mask,
                       remap_window_values(dpy, mask, values, &copy))
                : 0;
}

Window XCreateSimpleWindow(Display *dpy, Window parent, int x, int y, unsigned int width, unsigned int height,
                           unsigned int border_width, unsigned long border, unsigned long background)
{
    static Window (*real)(Display *, Window, int, int, unsigned int, unsigned int, unsigned int, unsigned long,
                          unsigned long);
    if (!real) {
        real = (Window (*)(Display *, Window, int, int, unsigned int, unsigned int, unsigned int, unsigned long,
                           unsigned long))x11_function("XCreateSimpleWindow");
    }
    return real ? real(dpy, parent, x, y, width, height, border_width, remap(dpy, border), remap(dpy, background)) : 0;
}

/* A program's own PseudoColor colormap (Maya's colour-index one) starts with
   the default colormap's colours: the program's X widgets inside its GL
   windows inherit it, and draw with pixels from the default colormap. One
   created without cells for the default visual is the default colormap. */
Colormap XCreateColormap(Display *dpy, Window w, Visual *visual, int alloc)
{
    static Colormap (*real)(Display *, Window, Visual *, int);
    static int (*query)(Display *, Colormap, XColor *, int);
    static int (*store)(Display *, Colormap, XColor *, int);
    Colormap colormap;
    if (!real) real = (Colormap (*)(Display *, Window, Visual *, int))x11_function("XCreateColormap");
    if (!real) return None;
    /* A colormap with no cells of its own for the default visual (a GL
       window's): the default colormap itself, whose colours its widgets use.
       (Freeing the default colormap does nothing.) */
    if (alloc == AllocNone && visual == DefaultVisual(dpy, DefaultScreen(dpy))) {
        return DefaultColormap(dpy, DefaultScreen(dpy));
    }
    colormap = real(dpy, w, visual, alloc);
    if (colormap != None && alloc == AllocAll && visual && visual->class == PseudoColor && visual->map_entries == 256) {
        XColor colors[256];
        struct cells *c = cells_of(colormap, 1);
        int i;
        if (!query) query = (int (*)(Display *, Colormap, XColor *, int))x11_function("XQueryColors");
        if (!store) store = (int (*)(Display *, Colormap, XColor *, int))x11_function("XStoreColors");
        if (query && store && c) {
            for (i = 0; i < 256; ++i) colors[i].pixel = (unsigned long)i;
            query(dpy, DefaultColormap(dpy, DefaultScreen(dpy)), colors, 256);
            for (i = 0; i < 256; ++i) {
                colors[i].flags = DoRed | DoGreen | DoBlue;
                c->mirror[i][0] = (unsigned char)(colors[i].red >> 8);
                c->mirror[i][1] = (unsigned char)(colors[i].green >> 8);
                c->mirror[i][2] = (unsigned char)(colors[i].blue >> 8);
            }
            store(dpy, colormap, colors, 256);
            c->mirrored = 1;
            c->dpy = dpy;
        }
    }
    return colormap;
}

/* ---- GLPASS_TRACE=1: the program's line and rectangle drawing ------------------- */

static void trace_draw(Display *dpy, const char *what, Drawable d, GC gc, int w, int h)
{
    static int enabled = -1, count;
    static Status (*get)(Display *, GC, unsigned long, XGCValues *);
    XGCValues v;
    if (enabled < 0) {
        const char *e = getenv("GLPASS_TRACE");
        enabled = e && *e && *e != '0';
    }
    if (!enabled || count >= 400) return;
    if (!get) get = (Status (*)(Display *, GC, unsigned long, XGCValues *))x11_function("XGetGCValues");
    v.function = -1;
    v.foreground = 0;
    v.plane_mask = 0;
    v.subwindow_mode = 0;
    if (get) get(dpy, gc, GCFunction | GCForeground | GCPlaneMask | GCSubwindowMode, &v);
    if (v.function == GXcopy) return; /* ordinary drawing: only rubber bands and the like */
    ++count;
    fprintf(stderr, "glpass_x: %s on %#lx %dx%d function %d foreground %lu planes %#lx subwindows %d\n", what,
            (unsigned long)d, w, h, v.function, v.foreground, v.plane_mask, v.subwindow_mode);
}

int XDrawRectangle(Display *dpy, Drawable d, GC gc, int x, int y, unsigned int w, unsigned int h)
{
    static int (*real)(Display *, Drawable, GC, int, int, unsigned int, unsigned int);
    if (!real) real = (int (*)(Display *, Drawable, GC, int, int, unsigned int, unsigned int))x11_function("XDrawRectangle");
    trace_draw(dpy, "XDrawRectangle", d, gc, (int)w, (int)h);
    return real ? real(dpy, d, gc, x, y, w, h) : 0;
}

int XDrawLine(Display *dpy, Drawable d, GC gc, int x1, int y1, int x2, int y2)
{
    static int (*real)(Display *, Drawable, GC, int, int, int, int);
    if (!real) real = (int (*)(Display *, Drawable, GC, int, int, int, int))x11_function("XDrawLine");
    trace_draw(dpy, "XDrawLine", d, gc, x2 - x1, y2 - y1);
    return real ? real(dpy, d, gc, x1, y1, x2, y2) : 0;
}

int XDrawLines(Display *dpy, Drawable d, GC gc, XPoint *points, int n, int mode)
{
    static int (*real)(Display *, Drawable, GC, XPoint *, int, int);
    if (!real) real = (int (*)(Display *, Drawable, GC, XPoint *, int, int))x11_function("XDrawLines");
    trace_draw(dpy, "XDrawLines", d, gc, n, mode);
    return real ? real(dpy, d, gc, points, n, mode) : 0;
}

int XDrawSegments(Display *dpy, Drawable d, GC gc, XSegment *segments, int n)
{
    static int (*real)(Display *, Drawable, GC, XSegment *, int);
    if (!real) real = (int (*)(Display *, Drawable, GC, XSegment *, int))x11_function("XDrawSegments");
    trace_draw(dpy, "XDrawSegments", d, gc, n, 0);
    return real ? real(dpy, d, gc, segments, n) : 0;
}

int XFillRectangle(Display *dpy, Drawable d, GC gc, int x, int y, unsigned int w, unsigned int h)
{
    static int (*real)(Display *, Drawable, GC, int, int, unsigned int, unsigned int);
    if (!real) real = (int (*)(Display *, Drawable, GC, int, int, unsigned int, unsigned int))x11_function("XFillRectangle");
    trace_draw(dpy, "XFillRectangle", d, gc, (int)w, (int)h);
    return real ? real(dpy, d, gc, x, y, w, h) : 0;
}
