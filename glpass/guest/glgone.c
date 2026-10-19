/*
 * glgone: a context left current on a window the program destroys, then
 * flushed again (Houdini's License Administrator does so with its splash
 * window). Checks that glpass neither keeps showing the dead window nor
 * hands the program X errors it did not cause.
 *   LD_LIBRARYN32_PATH=<glpass dir> ./glgone
 * Both in RGBA and colour index (whose present reads the window's palette):
 * after the destroy, the program flushes, swaps and finishes, then asks to
 * bind the context to the window again, which GLX refuses. Prints the X
 * errors the program's handler received, and PASS when there are none and
 * the bind was refused. With GLPASS_TRACE=1, the log after "destroying the
 * window" shows any request glpass still sends for the window.
 */
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>

#include <stdio.h>
#include <unistd.h>

static int errors;

static int count_error(Display *dpy, XErrorEvent *event)
{
    char text[128];
    XGetErrorText(dpy, event->error_code, text, sizeof text);
    printf("glgone: X error on %s connection: %s (request %d, resource %#lx)\n",
           dpy ? "a" : "no", text, (int)event->request_code, (unsigned long)event->resourceid);
    ++errors;
    return 0;
}

/* One pass: draw, destroy the window, keep using the context on it. */
static int pass(int index_mode)
{
    int rgba[] = {GLX_RGBA, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None};
    int ci[] = {GLX_BUFFER_SIZE, 8, None};
    int *attributes = index_mode ? ci : rgba;
    Display *dpy = XOpenDisplay(0);
    Bool rebound;
    XVisualInfo *vi;
    XSetWindowAttributes swa;
    XEvent event;
    Window win;
    GLXContext ctx;
    int i;
    if (!dpy) {
        fprintf(stderr, "glgone: cannot open display\n");
        return 0;
    }
    XSetErrorHandler(count_error);
    vi = glXChooseVisual(dpy, DefaultScreen(dpy), attributes);
    if (!vi) {
        fprintf(stderr, "glgone: no visual\n");
        return 0;
    }
    swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
    swa.border_pixel = 0;
    swa.override_redirect = True; /* a splash: no window manager frame */
    swa.event_mask = StructureNotifyMask;
    win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 340, 478, 363, 82, 0, vi->depth, InputOutput, vi->visual,
                        CWColormap | CWBorderPixel | CWOverrideRedirect | CWEventMask, &swa);
    XMapWindow(dpy, win);
    do XNextEvent(dpy, &event); while (event.type != MapNotify);
    ctx = glXCreateContext(dpy, vi, 0, True);
    if (!ctx || !glXMakeCurrent(dpy, win, ctx)) {
        fprintf(stderr, "glgone: no context\n");
        return 0;
    }
    glDrawBuffer(GL_FRONT_AND_BACK);
    for (i = 0; i < 5; ++i) {
        glClearColor(0.0f, 0.2f * (float)i, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFlush();
        usleep(200000);
    }
    fprintf(stderr, "glgone: destroying the window\n"); /* marks the log (GLPASS_TRACE) */
    XDestroyWindow(dpy, win); /* the context stays current on it */
    XSync(dpy, False);
    for (i = 0; i < 20; ++i) {
        glClear(GL_COLOR_BUFFER_BIT);
        glFlush();
        glXSwapBuffers(dpy, win);
        glFinish();
        usleep(50000);
    }
    rebound = glXMakeCurrent(dpy, win, ctx); /* GLX refuses a destroyed window */
    XSync(dpy, False);
    glXMakeCurrent(dpy, None, 0);
    glXDestroyContext(dpy, ctx);
    XSync(dpy, False);
    XCloseDisplay(dpy);
    printf("glgone: %s: %d X errors, glXMakeCurrent on the destroyed window %s\n",
           index_mode ? "colour index" : "RGBA", errors, rebound ? "succeeded" : "refused");
    return errors == 0 && !rebound;
}

int main(void)
{
    int ok = pass(0);
    ok = pass(1) && ok;
    printf("glgone: %s\n", ok ? "PASS" : "FAIL");
    return !ok;
}
