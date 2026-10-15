/*
 * gltri: a spinning triangle through GLX, to check the GL pass-through.
 *   LD_LIBRARYN32_PATH=<glpass dir> ./gltri [frames]
 * Prints the frame rate every 100 frames.
 */
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

static double now(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

int main(int argc, char **argv)
{
    int attributes[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 16, None};
    int frames = argc > 1 ? atoi(argv[1]) : 1000000;
    Display *dpy = XOpenDisplay(0);
    XVisualInfo *vi;
    XSetWindowAttributes swa;
    Window win;
    GLXContext ctx;
    double t0;
    int i;

    if (!dpy) { fprintf(stderr, "gltri: cannot open display\n"); return 1; }
    if (!glXQueryExtension(dpy, 0, 0)) { fprintf(stderr, "gltri: no GLX\n"); return 1; }
    vi = glXChooseVisual(dpy, DefaultScreen(dpy), attributes);
    if (!vi) { fprintf(stderr, "gltri: no visual\n"); return 1; }
    swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
    swa.border_pixel = 0;
    swa.event_mask = ExposureMask | StructureNotifyMask;
    win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 100, 100, 400, 300, 0, vi->depth, InputOutput,
                        vi->visual, CWColormap | CWBorderPixel | CWEventMask, &swa);
    XStoreName(dpy, win, "gltri (host GPU)");
    XMapWindow(dpy, win);
    XSync(dpy, False);
    ctx = glXCreateContext(dpy, vi, 0, True);
    if (!ctx) { fprintf(stderr, "gltri: cannot create a context\n"); return 1; }
    glXMakeCurrent(dpy, win, ctx);
    printf("gltri: %s / %s\n", (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER));
    fflush(stdout);

    t0 = now();
    for (i = 1; i <= frames; ++i) {
        glClearColor(0.1f, 0.1f, 0.25f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(-1.3, 1.3, -1.0, 1.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glRotatef((float)(i * 2), 0.0f, 0.0f, 1.0f);
        glBegin(GL_TRIANGLES);
        glColor3f(1.0f, 0.2f, 0.2f); glVertex2f(0.0f, 0.8f);
        glColor3f(0.2f, 1.0f, 0.2f); glVertex2f(-0.7f, -0.5f);
        glColor3f(0.2f, 0.4f, 1.0f); glVertex2f(0.7f, -0.5f);
        glEnd();
        glXSwapBuffers(dpy, win);
        if (i % 100 == 0) {
            double t = now();
            printf("gltri: %d frames, %.1f fps\n", i, 100.0 / (t - t0));
            fflush(stdout);
            t0 = t;
        }
    }
    return 0;
}
