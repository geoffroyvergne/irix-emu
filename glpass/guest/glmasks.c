/*
 * glmasks: colour index writes under index masks that the host needs
 * several passes for (Maya's 0x83e and 0xfc0), checked pixel by pixel
 * against a model of what colour index GL stores:
 *   block      glIndex inside glBegin/glEnd, to indices that alone would take
 *              other passes (all masked bits set, all clear)
 *   flushes    4000 points in blocks of 25, an index each: blocks cross
 *              command buffer flushes
 *   depth      GL_LESS depth test: the passes after the first still draw
 *   arrays     index arrays, glDrawArrays and glDrawElements; the colour
 *              array, which colour index mode ignores, is enabled with junk
 *   lists      glIndex inside lists, a nested glCallList, glCallLists, and
 *              the index the lists leave current
 *   logic      GL_OR_REVERSE (an AND, an OR and an XOR pass) in a list
 *   deleted    a deleted list's glIndex is not applied when it is called
 *   feedback   one point gives one token, not one per pass
 *   LD_LIBRARYN32_PATH=<glpass dir> ./glmasks
 * The program reads the window back as RGBA, which in glpass is the stored
 * index's encoding (R bits 0-7, G 8-9, B 10, A 11). Prints a line per check
 * and exits 0 when all pass.
 */
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>

#include <stdio.h>
#include <string.h>

#define W 100
#define H 100
#define BITS 0xFFFu

static unsigned int model[H][W];
static int failures;

static unsigned int logic(GLenum op, unsigned int s, unsigned int d)
{
    switch (op) {
    case GL_CLEAR: return 0;
    case GL_AND: return s & d;
    case GL_AND_REVERSE: return s & ~d;
    case GL_COPY: return s;
    case GL_AND_INVERTED: return ~s & d;
    case GL_NOOP: return d;
    case GL_XOR: return s ^ d;
    case GL_OR: return s | d;
    case GL_NOR: return ~(s | d);
    case GL_EQUIV: return ~(s ^ d);
    case GL_INVERT: return ~d;
    case GL_OR_REVERSE: return s | ~d;
    case GL_COPY_INVERTED: return ~s;
    case GL_OR_INVERTED: return ~s | d;
    case GL_NAND: return ~(s & d);
    default: return BITS; /* GL_SET */
    }
}

/* What a write of index `s` stores over `d` under `mask` and `op`. */
static unsigned int stored(unsigned int s, unsigned int d, unsigned int mask, GLenum op)
{
    return ((d & ~mask) | (logic(op, s, d) & mask)) & BITS;
}

static void model_fill(int x0, int y0, int x1, int y1, unsigned int s, unsigned int mask, GLenum op)
{
    int x, y;
    for (y = y0; y < y1; ++y)
        for (x = x0; x < x1; ++x) model[y][x] = stored(s, model[y][x], mask, op);
}

/* Clears the window and the model to index `d`. */
static void start(unsigned int d)
{
    int x, y;
    glDisable(GL_INDEX_LOGIC_OP);
    glIndexMask(BITS);
    glClearIndex((GLfloat)d);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    for (y = 0; y < H; ++y)
        for (x = 0; x < W; ++x) model[y][x] = d;
}

static void check(const char *what)
{
    static GLubyte px[H][W][4];
    char detail[200];
    int x, y, wrong = 0, fx = -1, fy = -1;
    unsigned int got = 0, raw = 0;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (y = 0; y < H; ++y) {
        for (x = 0; x < W; ++x) {
            const GLubyte *p = px[y][x];
            const unsigned int index = p[0] | (p[1] & 3u) << 8 | (p[2] & 1u) << 10 | (p[3] & 1u) << 11;
            if (index != model[y][x]) {
                if (!wrong) {
                    fx = x;
                    fy = y;
                    got = index;
                    raw = (unsigned int)p[0] << 24 | (unsigned int)p[1] << 16 | (unsigned int)p[2] << 8 | p[3];
                }
                ++wrong;
            }
        }
    }
    detail[0] = 0;
    if (wrong) {
        sprintf(detail, "%d pixels wrong; first at %d,%d: %#x (RGBA %08x), want %#x", wrong, fx, fy, got, raw,
                model[fy][fx]);
    }
    printf("glmasks: %-9s %s%s%s\n", what, wrong ? "FAIL" : "PASS", wrong ? ": " : "", detail);
    if (wrong) ++failures;
}

static void box(int x0, int y0, int x1, int y1)
{
    glVertex2i(x0, y0);
    glVertex2i(x1, y0);
    glVertex2i(x1, y1);
    glVertex2i(x0, y1);
}

static void check_block(void)
{
    const unsigned int mask = 0x83e, d = 0x7c3;
    start(d);
    glIndexMask(mask);
    /* Starting on an index taking both passes, then one with every masked
       bit set (an OR alone would do), then one with none (an AND alone). */
    glIndexi(0x0a9);
    glBegin(GL_QUADS);
    box(0, 0, 30, 30);
    glIndexi(0xfff);
    box(30, 0, 60, 30);
    glIndexi(0x000);
    box(60, 0, 90, 30);
    glEnd();
    model_fill(0, 0, 30, 30, 0x0a9, mask, GL_COPY);
    model_fill(30, 0, 60, 30, 0xfff, mask, GL_COPY);
    model_fill(60, 0, 90, 30, 0x000, mask, GL_COPY);
    /* The other way round: starting on a one-pass index. */
    glIndexi(0xfff);
    glBegin(GL_QUADS);
    box(0, 30, 30, 60);
    glIndexi(0x5a5);
    box(30, 30, 60, 60);
    glEnd();
    model_fill(0, 30, 30, 60, 0xfff, mask, GL_COPY);
    model_fill(30, 30, 60, 60, 0x5a5, mask, GL_COPY);
    check("block");
}

static void check_flushes(void)
{
    const unsigned int mask = 0xfc0;
    int k;
    start(0x2b7);
    glIndexMask(mask);
    for (k = 0; k < 4000; ++k) {
        const int x = k % W, y = 50 + k / W;
        const unsigned int s = (unsigned int)(k * 37 + 11) & BITS;
        if (k % 25 == 0) glBegin(GL_POINTS);
        glIndexi((GLint)s);
        glVertex2f((GLfloat)x + 0.5f, (GLfloat)y + 0.5f);
        if (k % 25 == 24) glEnd();
        model_fill(x, y, x + 1, y + 1, s, mask, GL_COPY);
    }
    check("flushes");
}

static void check_depth(void)
{
    const unsigned int mask = 0x83e;
    start(0x123);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glIndexMask(mask);
    glIndexi(0x6d9);
    glBegin(GL_QUADS);
    box(10, 10, 60, 60);
    glEnd();
    model_fill(10, 10, 60, 60, 0x6d9, mask, GL_COPY);
    /* Behind it: fails the depth test in every pass. */
    glIndexi(0xfff);
    glBegin(GL_QUADS);
    glVertex3f(40.0f, 40.0f, -0.5f);
    glVertex3f(90.0f, 40.0f, -0.5f);
    glVertex3f(90.0f, 90.0f, -0.5f);
    glVertex3f(40.0f, 90.0f, -0.5f);
    glEnd();
    model_fill(60, 40, 90, 90, 0xfff, mask, GL_COPY);
    model_fill(40, 60, 60, 90, 0xfff, mask, GL_COPY);
    glDisable(GL_DEPTH_TEST);
    check("depth");
}

static void check_arrays(void)
{
    static GLfloat vertices[W][2];
    static GLint indices[W];
    static GLuint order[W];
    static GLfloat junk[W][4];
    const unsigned int mask = 0x83e;
    int k;
    start(0x5c1);
    glIndexMask(mask);
    for (k = 0; k < W; ++k) {
        vertices[k][0] = (GLfloat)k + 0.5f;
        vertices[k][1] = 10.5f;
        indices[k] = (GLint)(((unsigned int)k * 53u + 7u) & BITS);
        order[k] = (GLuint)(W - 1 - k);
        junk[k][0] = junk[k][1] = junk[k][2] = junk[k][3] = 1.0f;
    }
    glVertexPointer(2, GL_FLOAT, 0, vertices);
    glIndexPointer(GL_INT, 0, indices);
    glColorPointer(4, GL_FLOAT, 0, junk);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_INDEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glDrawArrays(GL_POINTS, 0, W);
    for (k = 0; k < W; ++k) model_fill(k, 10, k + 1, 11, (unsigned int)indices[k], mask, GL_COPY);
    glPushMatrix();
    glTranslatef(0.0f, 10.0f, 0.0f);
    glDrawElements(GL_POINTS, W, GL_UNSIGNED_INT, order);
    glPopMatrix();
    for (k = 0; k < W; ++k) model_fill(k, 20, k + 1, 21, (unsigned int)indices[k], mask, GL_COPY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_INDEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    check("arrays");
}

static void check_lists(void)
{
    const unsigned int mask = 0x83e;
    const GLuint inner = glGenLists(1), outer = glGenLists(1), glyphs = glGenLists(3);
    static const GLubyte text[3] = {0, 1, 2};
    int k;
    glNewList(inner, GL_COMPILE);
    glBegin(GL_QUADS);
    glIndexi(0x0a9);
    box(0, 0, 20, 20);
    glIndexi(0xfff);
    box(20, 0, 40, 20);
    glEnd();
    glEndList();
    glNewList(outer, GL_COMPILE);
    glIndexi(0x000);
    glBegin(GL_QUADS);
    box(40, 0, 60, 20);
    glEnd();
    glPushMatrix();
    glTranslatef(0.0f, 20.0f, 0.0f);
    glCallList(inner);
    glPopMatrix();
    glEndList();
    for (k = 0; k < 3; ++k) { /* a glyph each: an index, a cell, an advance */
        glNewList(glyphs + (GLuint)k, GL_COMPILE);
        glIndexi(0x111 * (k + 3));
        glBegin(GL_QUADS);
        box(0, 50, 10, 60);
        glEnd();
        glTranslatef(10.0f, 0.0f, 0.0f);
        glEndList();
    }
    start(0x7c3);
    glIndexMask(mask);
    glIndexi(0x5a5);
    glCallList(outer);
    model_fill(40, 0, 60, 20, 0x000, mask, GL_COPY);
    model_fill(0, 20, 20, 40, 0x0a9, mask, GL_COPY);
    model_fill(20, 20, 40, 40, 0xfff, mask, GL_COPY);
    glPushMatrix();
    glListBase(glyphs);
    glCallLists(3, GL_UNSIGNED_BYTE, text);
    glListBase(0);
    glBegin(GL_QUADS); /* after the glyphs' advances: 30 to the right, once */
    box(0, 80, 10, 90);
    glEnd();
    glPopMatrix();
    model_fill(30, 80, 40, 90, 0x555, mask, GL_COPY);
    for (k = 0; k < 3; ++k) model_fill(10 * k, 50, 10 * k + 10, 60, 0x111u * (unsigned int)(k + 3), mask, GL_COPY);
    /* The index the last glyph left current. */
    glBegin(GL_QUADS);
    box(60, 60, 80, 80);
    glEnd();
    model_fill(60, 60, 80, 80, 0x555, mask, GL_COPY);
    check("lists");
    glDeleteLists(inner, 1);
    glDeleteLists(outer, 1);
    glDeleteLists(glyphs, 3);
}

static void check_logic(void)
{
    const unsigned int mask = 0x3f0;
    const GLuint list = glGenLists(1);
    glNewList(list, GL_COMPILE);
    glBegin(GL_QUADS);
    glIndexi(0x9a6);
    box(0, 0, 50, 50);
    glIndexi(0x0f0);
    box(50, 0, 100, 50);
    glEnd();
    glEndList();
    start(0x6c5);
    glIndexMask(mask);
    glEnable(GL_INDEX_LOGIC_OP);
    glLogicOp(GL_OR_REVERSE);
    glCallList(list);
    model_fill(0, 0, 50, 50, 0x9a6, mask, GL_OR_REVERSE);
    model_fill(50, 0, 100, 50, 0x0f0, mask, GL_OR_REVERSE);
    glIndexi(0x00f); /* and drawn directly */
    glBegin(GL_QUADS);
    box(0, 50, 50, 100);
    glIndexi(0xf3c);
    box(50, 50, 100, 100);
    glEnd();
    model_fill(0, 50, 50, 100, 0x00f, mask, GL_OR_REVERSE);
    model_fill(50, 50, 100, 100, 0xf3c, mask, GL_OR_REVERSE);
    glLogicOp(GL_COPY);
    glDisable(GL_INDEX_LOGIC_OP);
    check("logic");
    glDeleteLists(list, 1);
}

static void check_deleted(void)
{
    const GLuint list = glGenLists(1);
    glNewList(list, GL_COMPILE);
    glIndexi(0xabc);
    glEndList();
    glDeleteLists(list, 1);
    start(0x000);
    glIndexi(0x321);
    glCallList(list); /* no list any more: the index stays */
    glBegin(GL_QUADS);
    box(0, 0, 50, 50);
    glEnd();
    model_fill(0, 0, 50, 50, 0x321, BITS, GL_COPY);
    check("deleted");
}

static void check_feedback(void)
{
    GLfloat buffer[16];
    GLint n;
    start(0x000);
    glIndexMask(0x83e);
    glIndexi(0x0a9);
    glFeedbackBuffer(16, GL_2D, buffer);
    glRenderMode(GL_FEEDBACK);
    glBegin(GL_POINTS);
    glVertex2f(25.5f, 50.5f);
    glEnd();
    glClear(GL_COLOR_BUFFER_BIT); /* a clear gives no tokens */
    n = glRenderMode(GL_RENDER);
    printf("glmasks: %-9s %s: %d values (want 3)\n", "feedback", n == 3 ? "PASS" : "FAIL", (int)n);
    if (n != 3) ++failures;
}

int main(void)
{
    int attributes[] = {GLX_BUFFER_SIZE, 8, GLX_DEPTH_SIZE, 16, None};
    Display *dpy = XOpenDisplay(0);
    XVisualInfo *vi;
    XSetWindowAttributes swa;
    XEvent event;
    Window win;
    GLXContext ctx;
    if (!dpy) {
        fprintf(stderr, "glmasks: cannot open display\n");
        return 1;
    }
    vi = glXChooseVisual(dpy, DefaultScreen(dpy), attributes);
    if (!vi) {
        fprintf(stderr, "glmasks: no colour index visual\n");
        return 1;
    }
    /* A private colormap, as colour index programs make: glpass takes a
       window with the default colormap to be drawn in RGBA. */
    swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocAll);
    swa.border_pixel = 0;
    swa.event_mask = StructureNotifyMask;
    win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 100, 100, W, H, 0, vi->depth, InputOutput, vi->visual,
                        CWColormap | CWBorderPixel | CWEventMask, &swa);
    XMapWindow(dpy, win);
    do XNextEvent(dpy, &event); while (event.type != MapNotify);
    ctx = glXCreateContext(dpy, vi, 0, True);
    if (!ctx || !glXMakeCurrent(dpy, win, ctx)) {
        fprintf(stderr, "glmasks: no context\n");
        return 1;
    }
    glViewport(0, 0, W, H);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DITHER);
    glShadeModel(GL_FLAT);

    check_block();
    check_flushes();
    check_depth();
    check_arrays();
    check_lists();
    check_logic();
    check_deleted();
    check_feedback();

    glXMakeCurrent(dpy, None, 0);
    glXDestroyContext(dpy, ctx);
    XCloseDisplay(dpy);
    printf("glmasks: %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
