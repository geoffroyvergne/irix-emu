/*
 * glcolors: checks libglpass_x (xstore.c), the colormap notes for colour-index
 * programs, against what programs do to colormaps and displays:
 *   freed      a high pixel used after its colormap was freed
 *   closed     a high pixel used after the display of its colormap was closed
 *   threads    sprocs storing colours and closing their displays at once
 * Run through glrun (which preloads libglpass_x):
 *   /usr/local/irix-emu/glpass/glrun ./glcolors
 * The program must receive no X error caused by libglpass_x (its own stores of
 * 12-bit pixels, which the server refuses, do not count), and not crash.
 * Prints PASS or FAIL.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static int errors;

static int count_error(Display *dpy, XErrorEvent *event)
{
    char text[128];
    /* This program's own stores of 12-bit pixels: the server refuses them
       (BadValue, the pixel), as it does Maya's. Not libglpass_x's doing. */
    if (event->request_code == 89 && event->error_code == BadValue && event->resourceid >= 256) return 0;
    XGetErrorText(dpy, event->error_code, text, sizeof text);
    printf("glcolors: X error: %s (request %d, resource %#lx)\n", text, (int)event->request_code,
           (unsigned long)event->resourceid);
    ++errors;
    return 0;
}

/* A 16-entry PseudoColor visual (the 4-bit one), or 0. */
static Visual *small_visual(Display *dpy, int *depth)
{
    XVisualInfo templ, *vi;
    int n = 0, i;
    Visual *v = 0;
    templ.screen = DefaultScreen(dpy);
    templ.class = PseudoColor;
    vi = XGetVisualInfo(dpy, VisualScreenMask | VisualClassMask, &templ, &n);
    for (i = 0; i < n && !v; ++i) {
        if (vi[i].colormap_size == 16) {
            v = vi[i].visual;
            *depth = vi[i].depth;
        }
    }
    if (vi) XFree(vi);
    return v;
}

static void store(Display *dpy, Colormap colormap, unsigned long pixel, unsigned short red)
{
    XColor c;
    c.pixel = pixel;
    c.red = red;
    c.green = 0;
    c.blue = 0;
    c.flags = DoRed | DoGreen | DoBlue;
    XStoreColor(dpy, colormap, &c);
}

static void thread_body(void *arg)
{
    Display *dpy = XOpenDisplay(0);
    int depth = 0, i;
    Visual *v;
    (void)arg;
    if (!dpy) return;
    v = small_visual(dpy, &depth);
    for (i = 0; v && i < 300; ++i) {
        Colormap c = XCreateColormap(dpy, DefaultRootWindow(dpy), v, AllocAll);
        store(dpy, c, 3000 + (unsigned long)(i % 64), (unsigned short)(i * 200));
        if (i % 3 == 0) XFreeColormap(dpy, c);
    }
    XSync(dpy, False);
    XCloseDisplay(dpy);
}

int main(void)
{
    Display *dpy = XOpenDisplay(0), *other;
    int depth = 0, i, k;
    Visual *v;
    Pixmap pixmap;
    GC gc;
    Colormap freed, closed;
    pid_t kids[3];
    if (!dpy) {
        fprintf(stderr, "glcolors: cannot open display\n");
        return 1;
    }
    XSetErrorHandler(count_error);
    v = small_visual(dpy, &depth);
    if (!v) {
        printf("glcolors: no 16-entry PseudoColor visual: SKIP\n");
        return 0;
    }
    pixmap = XCreatePixmap(dpy, DefaultRootWindow(dpy), 8, 8, (unsigned)depth);
    gc = XCreateGC(dpy, pixmap, 0, 0);

    /* freed: pixel 2001 noted in a colormap, which is freed before the pixel is used. */
    freed = XCreateColormap(dpy, DefaultRootWindow(dpy), v, AllocAll);
    store(dpy, freed, 2001, 0xFFFF);
    XFreeColormap(dpy, freed);
    XSetForeground(dpy, gc, 2001);
    XSync(dpy, False);
    printf("glcolors: freed      %s\n", errors ? "FAIL" : "PASS");

    /* closed: pixel 2002 noted on a second display, which is closed. */
    i = errors;
    other = XOpenDisplay(0);
    if (other) {
        XSetErrorHandler(count_error);
        closed = XCreateColormap(other, DefaultRootWindow(other), v, AllocAll);
        store(other, closed, 2002, 0x8000);
        XSync(other, False);
        XCloseDisplay(other);
        XSetForeground(dpy, gc, 2002);
        XSync(dpy, False);
    }
    printf("glcolors: closed     %s\n", errors != i ? "FAIL" : "PASS");

    /* threads: three sprocs at once, each on its own display. */
    i = errors;
    for (k = 0; k < 3; ++k) kids[k] = sproc(thread_body, PR_SALL, 0);
    for (k = 0; k < 3; ++k) {
        if (kids[k] > 0) while (waitpid(kids[k], 0, 0) < 0) {}
    }
    for (k = 0; k < 200; ++k) XSetForeground(dpy, gc, 3000 + (unsigned long)(k % 64));
    XSync(dpy, False);
    printf("glcolors: threads    %s\n", errors != i ? "FAIL" : "PASS");

    XFreeGC(dpy, gc);
    XFreePixmap(dpy, pixmap);
    XCloseDisplay(dpy);
    printf("glcolors: %s\n", errors ? "FAIL" : "PASS");
    return errors != 0;
}
