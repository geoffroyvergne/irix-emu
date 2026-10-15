/*
 * glmove: checks that a GL window's image follows the window when it moves
 * or is unmapped without being redrawn (glpass window tracking).
 *   LD_LIBRARYN32_PATH=<glpass dir> ./glmove [seconds per step]
 * Draws the window green once, then moves it, then unmaps it; between steps
 * it only makes its context current again (no drawing). Prints each step and
 * the window's centre on screen, then waits, so the screen can be checked:
 *   glmove: shown at <x> <y>     the centre is green
 *   glmove: moved to <x> <y>     green there, not at the first place
 *   glmove: unmapped             green at neither place
 */
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void centre(Display *dpy, Window win, int *x, int *y)
{
    Window child;
    XTranslateCoordinates(dpy, win, DefaultRootWindow(dpy), 100, 75, x, y, &child);
}

int main(int argc, char **argv)
{
    int attributes[] = {GLX_RGBA, GLX_DOUBLEBUFFER, None};
    int pause = argc > 1 ? atoi(argv[1]) : 10;
    Display *dpy = XOpenDisplay(0);
    XVisualInfo *vi;
    XSetWindowAttributes swa;
    XEvent event;
    Window win;
    GLXContext ctx;
    int x, y;
    if (!dpy) {
        fprintf(stderr, "glmove: cannot open display\n");
        return 1;
    }
    vi = glXChooseVisual(dpy, DefaultScreen(dpy), attributes);
    if (!vi) {
        fprintf(stderr, "glmove: no visual\n");
        return 1;
    }
    swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
    swa.border_pixel = 0;
    swa.event_mask = StructureNotifyMask;
    win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 100, 100, 200, 150, 0, vi->depth, InputOutput, vi->visual,
                        CWColormap | CWBorderPixel | CWEventMask, &swa);
    XMapWindow(dpy, win);
    do XNextEvent(dpy, &event); while (event.type != MapNotify);
    ctx = glXCreateContext(dpy, vi, 0, True);
    if (!ctx || !glXMakeCurrent(dpy, win, ctx)) {
        fprintf(stderr, "glmove: no context\n");
        return 1;
    }
    sleep(2); /* let the window manager place the window */
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glXSwapBuffers(dpy, win);
    centre(dpy, win, &x, &y);
    printf("glmove: shown at %d %d\n", x, y);
    fflush(stdout);
    sleep(pause);

    XMoveWindow(dpy, win, 500, 400);
    XSync(dpy, False);
    sleep(2);
    glXMakeCurrent(dpy, win, ctx); /* any GL call that presents or binds reads the events */
    centre(dpy, win, &x, &y);
    printf("glmove: moved to %d %d\n", x, y);
    fflush(stdout);
    sleep(pause);

    XUnmapWindow(dpy, win);
    XSync(dpy, False);
    sleep(2);
    glXMakeCurrent(dpy, win, ctx);
    printf("glmove: unmapped\n");
    fflush(stdout);
    sleep(pause);

    glXMakeCurrent(dpy, None, 0);
    glXDestroyContext(dpy, ctx);
    XCloseDisplay(dpy);
    printf("glmove: done\n");
    return 0;
}
