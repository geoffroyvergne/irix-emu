/*
 * glcheck: checks GL features whose results come back into the program's
 * memory, value by value (byte order included):
 *   selection  glSelectBuffer / glRenderMode(GL_SELECT): hits, names, depths
 *   feedback   glFeedbackBuffer / glRenderMode(GL_FEEDBACK): tokens, window coordinates
 *   texture3d  glTexImage3DEXT with GL_UNPACK_SKIP_IMAGES, sampled and read back
 *   large      a 1 MiB texture up and back (glTexImage2D, glGetTexImage): more
 *              pages than the TLB maps, so the copies resume across faults
 *   texformat  dark texels uploaded as GL_RGB5 and GL_RGBA4 come back unchanged
 *              (the host stores low-precision formats at 8 bits)
 *   multitex   two texture units with coordinate arrays of their own
 *              (glClientActiveTextureARB), drawn with glDrawArrays and with
 *              glBegin/glArrayElement (Quake III's lightmaps)
 *   LD_LIBRARYN32_PATH=<glpass dir> ./glcheck
 * Prints a line per check and exits 0 when all pass.
 */
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GL_TEXTURE_3D_EXT
#define GL_TEXTURE_3D_EXT 0x806F
#endif
#ifndef GL_UNPACK_SKIP_IMAGES_EXT
#define GL_UNPACK_SKIP_IMAGES_EXT 0x806D
#endif
#ifndef GL_TEXTURE_WRAP_R_EXT
#define GL_TEXTURE_WRAP_R_EXT 0x8072
#endif

static int failures;

static void report(const char *what, int ok, const char *detail)
{
    printf("glcheck: %-10s %s%s%s\n", what, ok ? "PASS" : "FAIL", detail[0] ? ": " : "", detail);
    if (!ok) ++failures;
}

static void square(float x0, float y0, float x1, float y1)
{
    glBegin(GL_QUADS);
    glVertex2f(x0, y0);
    glVertex2f(x1, y0);
    glVertex2f(x1, y1);
    glVertex2f(x0, y1);
    glEnd();
}

static void check_selection(void)
{
    GLuint buffer[64];
    GLint hits;
    char detail[200];
    int ok;
    glSelectBuffer(64, buffer);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(0);
    glLoadName(7);
    square(10, 10, 90, 90); /* in view: a hit */
    glLoadName(9);
    square(200, 200, 300, 300); /* outside: none */
    glLoadName(0x12345678);
    square(20, 20, 30, 30); /* in view: a second hit, a name with all bytes distinct */
    hits = glRenderMode(GL_RENDER);
    /* Each hit: name count, min depth, max depth, names. Depth 0 maps to 0x7fffffff. */
    ok = hits == 2 && buffer[0] == 1 && buffer[3] == 7 && buffer[4] == 1 && buffer[7] == 0x12345678 &&
         buffer[1] > 0x7f000000u && buffer[1] < 0x81000000u;
    sprintf(detail, "%d hits; first %u names, depth %#x-%#x, name %u; second name %#x", (int)hits,
            (unsigned)buffer[0], (unsigned)buffer[1], (unsigned)buffer[2], (unsigned)buffer[3],
            (unsigned)buffer[7]);
    report("selection", ok, detail);
}

static void check_feedback(void)
{
    GLfloat buffer[64];
    GLint n;
    char detail[200];
    int ok;
    glFeedbackBuffer(64, GL_3D, buffer);
    glRenderMode(GL_FEEDBACK);
    glBegin(GL_POINTS);
    glVertex2f(25.0f, 50.0f);
    glEnd();
    glPassThrough(3.5f);
    n = glRenderMode(GL_RENDER);
    /* GL_POINT_TOKEN x y z, GL_PASS_THROUGH_TOKEN value; window x, y for a 100x100 viewport. */
    ok = n == 6 && buffer[0] == (GLfloat)GL_POINT_TOKEN && fabs(buffer[1] - 25.0f) < 0.01f &&
         fabs(buffer[2] - 50.0f) < 0.01f && fabs(buffer[3] - 0.5f) < 0.01f &&
         buffer[4] == (GLfloat)GL_PASS_THROUGH_TOKEN && buffer[5] == 3.5f;
    sprintf(detail, "%d values: %g %g %g %g, %g %g", (int)n, buffer[0], buffer[1], buffer[2], buffer[3], buffer[4],
            buffer[5]);
    report("feedback", ok, detail);
}

static void check_texture3d(void)
{
    /* Three 2x2 RGBA slices; the upload skips the first, so the texture's
       second slice (r = 0.75) is the third one here. */
    static const GLubyte slice[3][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}};
    GLubyte data[3][2][2][4], got[4];
    GLuint texture;
    char detail[200];
    int s, y, x, c, ok;
    for (s = 0; s < 3; ++s)
        for (y = 0; y < 2; ++y)
            for (x = 0; x < 2; ++x)
                for (c = 0; c < 4; ++c) data[s][y][x][c] = slice[s][c];
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_3D_EXT, texture);
    glTexParameteri(GL_TEXTURE_3D_EXT, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D_EXT, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D_EXT, GL_TEXTURE_WRAP_R_EXT, GL_CLAMP);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_SKIP_IMAGES_EXT, 1);
    glTexImage3DEXT(GL_TEXTURE_3D_EXT, 0, GL_RGBA, 2, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glPixelStorei(GL_UNPACK_SKIP_IMAGES_EXT, 0);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glEnable(GL_TEXTURE_3D_EXT);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glTexCoord3f(0, 0, 0.75f); glVertex2f(0, 0);
    glTexCoord3f(1, 0, 0.75f); glVertex2f(100, 0);
    glTexCoord3f(1, 1, 0.75f); glVertex2f(100, 100);
    glTexCoord3f(0, 1, 0.75f); glVertex2f(0, 100);
    glEnd();
    glDisable(GL_TEXTURE_3D_EXT);
    glReadPixels(50, 50, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, got);
    ok = got[0] == 0 && got[1] == 0 && got[2] == 255 && glGetError() == GL_NO_ERROR;
    sprintf(detail, "centre %02x%02x%02x (want 0000ff, the third slice)", got[0], got[1], got[2]);
    report("texture3d", ok, detail);
}

static void check_large(void)
{
    enum { kSize = 512 };
    const unsigned bytes = kSize * kSize * 4;
    unsigned char *up = (unsigned char *)malloc(bytes), *down = (unsigned char *)malloc(bytes);
    GLuint texture;
    unsigned i, wrong = 0;
    char detail[200];
    if (!up || !down) {
        report("large", 0, "out of memory");
        return;
    }
    for (i = 0; i < bytes; ++i) up[i] = (unsigned char)(i * 7 + (i >> 12));
    memset(down, 0, bytes);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kSize, kSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, up);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, down);
    for (i = 0; i < bytes; ++i) wrong += up[i] != down[i];
    sprintf(detail, "%u of %u bytes differ", wrong, bytes);
    report("large", wrong == 0 && glGetError() == GL_NO_ERROR, detail);
    free(up);
    free(down);
}

static void check_texformat(void)
{
    static const GLubyte dark[4][4] = {{3, 7, 12, 255}, {20, 40, 60, 128}, {100, 101, 102, 7}, {1, 2, 4, 15}};
    static const GLenum formats[2] = {GL_RGB5_EXT, GL_RGBA4_EXT};
    GLubyte got[4][4];
    GLuint texture;
    char detail[200];
    int f, k, wrong = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    detail[0] = 0;
    for (f = 0; f < 2; ++f) {
        glTexImage2D(GL_TEXTURE_2D, 0, (GLint)formats[f], 4, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, dark);
        memset(got, 0, sizeof got);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, got);
        for (k = 0; k < 4; ++k) {
            /* GL_RGB5 has no alpha: it reads back as 255. */
            const int ok = got[k][0] == dark[k][0] && got[k][1] == dark[k][1] && got[k][2] == dark[k][2] &&
                           got[k][3] == (f == 0 ? 255 : dark[k][3]);
            if (!ok && !wrong++) {
                sprintf(detail, "%s texel %d: %d %d %d %d, want %d %d %d %d", f ? "GL_RGBA4" : "GL_RGB5", k, got[k][0],
                        got[k][1], got[k][2], got[k][3], dark[k][0], dark[k][1], dark[k][2], f ? dark[k][3] : 255);
            }
        }
    }
    glDeleteTextures(1, &texture);
    report("texformat", !wrong && glGetError() == GL_NO_ERROR, detail);
}

/* Texture unit 0 holds red | green, unit 1 white | black (2x1 each), modulated.
   Each unit's array picks its left texel: red x white = red. The current
   coordinates pick the right ones, so a unit that lost its array shows. */
static void check_multitex(void)
{
    static const GLubyte left_right[2][2][4] = {{{255, 0, 0, 255}, {0, 255, 0, 255}},
                                                {{255, 255, 255, 255}, {0, 0, 0, 255}}};
    static const GLfloat vertices[4][2] = {{0, 0}, {100, 0}, {100, 100}, {0, 100}};
    static const GLfloat coords0[4][2] = {{0.25f, 0.5f}, {0.25f, 0.5f}, {0.25f, 0.5f}, {0.25f, 0.5f}};
    static const GLfloat coords1[4][2] = {{0.20f, 0.5f}, {0.30f, 0.5f}, {0.30f, 0.5f}, {0.20f, 0.5f}};
    GLuint textures[2];
    GLubyte got[2][4];
    char detail[200];
    int unit, pass, ok;
    glGenTextures(2, textures);
    for (unit = 0; unit < 2; ++unit) {
        glActiveTextureARB(GL_TEXTURE0_ARB + unit);
        glBindTexture(GL_TEXTURE_2D, textures[unit]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, left_right[unit]);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glEnable(GL_TEXTURE_2D);
        glMultiTexCoord2fARB(GL_TEXTURE0_ARB + unit, 0.75f, 0.5f); /* the wrong texel */
    }
    glColor4f(1, 1, 1, 1);
    glVertexPointer(2, GL_FLOAT, 0, vertices);
    glEnableClientState(GL_VERTEX_ARRAY);
    glClientActiveTextureARB(GL_TEXTURE0_ARB);
    glTexCoordPointer(2, GL_FLOAT, 0, coords0);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glClientActiveTextureARB(GL_TEXTURE1_ARB);
    glTexCoordPointer(2, GL_FLOAT, 0, coords1);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    for (pass = 0; pass < 2; ++pass) {
        glClearColor(0, 0, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        if (pass == 0) {
            glDrawArrays(GL_QUADS, 0, 4);
        } else {
            glBegin(GL_QUADS);
            glArrayElement(0); glArrayElement(1); glArrayElement(2); glArrayElement(3);
            glEnd();
        }
        glReadPixels(50, 50, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, got[pass]);
    }
    glDisableClientState(GL_TEXTURE_COORD_ARRAY); /* unit 1's */
    glClientActiveTextureARB(GL_TEXTURE0_ARB);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    for (unit = 1; unit >= 0; --unit) {
        glActiveTextureARB(GL_TEXTURE0_ARB + unit);
        glDisable(GL_TEXTURE_2D);
    }
    glDeleteTextures(2, textures);
    ok = 1;
    for (pass = 0; pass < 2; ++pass) ok = ok && got[pass][0] == 255 && got[pass][1] == 0 && got[pass][2] == 0;
    sprintf(detail, "glDrawArrays %02x%02x%02x, glArrayElement %02x%02x%02x (want ff0000)", got[0][0], got[0][1],
            got[0][2], got[1][0], got[1][1], got[1][2]);
    report("multitex", ok && glGetError() == GL_NO_ERROR, detail);
}

int main(void)
{
    int attributes[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
                        GLX_DEPTH_SIZE, 16, None};
    Display *dpy = XOpenDisplay(0);
    XVisualInfo *vi;
    XSetWindowAttributes swa;
    XEvent event;
    Window win;
    GLXContext ctx;
    if (!dpy) {
        fprintf(stderr, "glcheck: cannot open display\n");
        return 1;
    }
    vi = glXChooseVisual(dpy, DefaultScreen(dpy), attributes);
    if (!vi) {
        fprintf(stderr, "glcheck: no visual\n");
        return 1;
    }
    swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
    swa.border_pixel = 0;
    swa.event_mask = StructureNotifyMask;
    win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 100, 100, 100, 100, 0, vi->depth, InputOutput,
                        vi->visual, CWColormap | CWBorderPixel | CWEventMask, &swa);
    XMapWindow(dpy, win);
    do XNextEvent(dpy, &event); while (event.type != MapNotify);
    ctx = glXCreateContext(dpy, vi, 0, True);
    if (!ctx || !glXMakeCurrent(dpy, win, ctx)) {
        fprintf(stderr, "glcheck: no context\n");
        return 1;
    }
    glViewport(0, 0, 100, 100);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 100, 0, 100, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    check_selection();
    check_feedback();
    check_texture3d();
    check_large();
    check_texformat();
    check_multitex();

    glXMakeCurrent(dpy, None, 0);
    glXDestroyContext(dpy, ctx);
    XCloseDisplay(dpy);
    printf("glcheck: %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
