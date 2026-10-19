/*
 * glshare: GLX's rule that a context is current in one thread at most,
 * checked with two sprocs (as in Maya):
 *   refused    the main thread binding a context current in thread A: False
 *   deferred   the main thread destroying it while A uses it: A's drawing
 *              stays correct until A releases the context
 *   LD_LIBRARYN32_PATH=<glpass dir> ./glshare
 * Prints a line per check and PASS or FAIL.
 */
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>

#include <stdio.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile struct {
    GLXContext ctx;
    int ready, step, drawn1, drawn2, done;
    int ok1, ok2;
} shared;

static void wait_for(volatile int *flag, int value)
{
    int i;
    for (i = 0; i < 3000 && *flag != value; ++i) usleep(10000);
}

static Window make_window(Display *dpy, XVisualInfo *vi, int x)
{
    XSetWindowAttributes swa;
    XEvent event;
    Window win;
    swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
    swa.border_pixel = 0;
    swa.event_mask = StructureNotifyMask;
    win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), x, 100, 160, 120, 0, vi->depth, InputOutput, vi->visual,
                        CWColormap | CWBorderPixel | CWEventMask, &swa);
    XMapWindow(dpy, win);
    do XNextEvent(dpy, &event); while (event.type != MapNotify);
    return win;
}

static int clear_and_check(float r, float g, float b)
{
    GLubyte px[4];
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glReadPixels(80, 60, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return px[0] == (GLubyte)(r * 255) && px[1] == (GLubyte)(g * 255) && px[2] == (GLubyte)(b * 255);
}

static int attributes[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None};

static void thread_a(void *arg)
{
    Display *dpy = XOpenDisplay(0);
    XVisualInfo *vi;
    Window win;
    GLXContext ctx;
    (void)arg;
    if (!dpy || !(vi = glXChooseVisual(dpy, DefaultScreen(dpy), attributes))) return;
    win = make_window(dpy, vi, 60);
    ctx = glXCreateContext(dpy, vi, 0, True);
    if (!ctx || !glXMakeCurrent(dpy, win, ctx)) return;
    shared.ctx = ctx;
    shared.ready = 1;
    wait_for(&shared.step, 1);
    shared.ok1 = clear_and_check(1.0f, 0.0f, 0.0f);
    shared.drawn1 = 1;
    wait_for(&shared.step, 2); /* the main thread has asked to destroy the context */
    shared.ok2 = clear_and_check(0.0f, 1.0f, 0.0f);
    shared.drawn2 = 1;
    glXMakeCurrent(dpy, None, 0); /* the destroy happens now */
    XCloseDisplay(dpy);
    shared.done = 1;
}

int main(void)
{
    Display *dpy = XOpenDisplay(0);
    XVisualInfo *vi;
    Window win;
    pid_t a;
    Bool bound;
    int ok;
    if (!dpy || !(vi = glXChooseVisual(dpy, DefaultScreen(dpy), attributes))) {
        fprintf(stderr, "glshare: no display or visual\n");
        return 1;
    }
    win = make_window(dpy, vi, 260);
    a = sproc(thread_a, PR_SALL, 0);
    if (a < 0) {
        perror("glshare: sproc");
        return 1;
    }
    wait_for(&shared.ready, 1);
    bound = glXMakeCurrent(dpy, win, shared.ctx); /* current in thread A: GLX refuses */
    printf("glshare: refused     %s\n", bound ? "FAIL (bound)" : "PASS");
    shared.step = 1;
    wait_for(&shared.drawn1, 1);
    glXDestroyContext(dpy, shared.ctx); /* still current in A: destroyed when A releases it */
    shared.step = 2;
    wait_for(&shared.done, 1);
    while (waitpid(a, 0, 0) < 0) {
    }
    printf("glshare: deferred    %s (thread A drew %s before, %s after the destroy request)\n",
           shared.ok1 && shared.ok2 ? "PASS" : "FAIL", shared.ok1 ? "right" : "wrong", shared.ok2 ? "right" : "wrong");
    ok = !bound && shared.ok1 && shared.ok2;
    printf("glshare: %s\n", ok ? "PASS" : "FAIL");
    XCloseDisplay(dpy);
    return !ok;
}
