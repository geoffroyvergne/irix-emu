/*
 * glcheck: checks GL features whose results come back into the program's
 * memory, value by value (byte order included):
 *   selection  glSelectBuffer / glRenderMode(GL_SELECT): hits, names, depths
 *   feedback   glFeedbackBuffer / glRenderMode(GL_FEEDBACK): tokens, window coordinates
 *   texture3d  glTexImage3DEXT with GL_UNPACK_SKIP_IMAGES, sampled and read back
 *   LD_LIBRARYN32_PATH=<glpass dir> ./glcheck
 * Prints a line per check and exits 0 when all pass.
 */
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

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

    glXMakeCurrent(dpy, None, 0);
    glXDestroyContext(dpy, ctx);
    XCloseDisplay(dpy);
    printf("glcheck: %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
