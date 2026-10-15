/*
 * glthreads: two threads drawing at once, each with its own GLX context and
 * window, to check that the GL pass-through keeps threads apart.
 *   LD_LIBRARYN32_PATH=<glpass dir> ./glthreads [frames]     (sproc threads)
 *   LD_LIBRARYN32_PATH=<glpass dir> ./glthreads_pt [frames]  (POSIX threads)
 * Every frame, each thread clears its window to a colour of its own, reads a
 * pixel back and checks it, and checks its current context. Prints a line
 * per thread and exits 0 when no frame went wrong.
 */
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef GLTHREADS_PTHREAD
#include <pthread.h>
#else
#include <sys/prctl.h>
#endif

struct job {
    int which;
    int frames;
    int errors;
};

static unsigned char channel(int which, int frame, int c)
{
    return (unsigned char)((frame * (3 + which * 4) + c * 85 + which * 128) & 0xFF);
}

static int draw(int which, int frames)
{
    int attributes[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None};
    Display *dpy = XOpenDisplay(0); /* one connection per thread: Xlib connections are not thread-safe */
    XVisualInfo *vi;
    XSetWindowAttributes swa;
    XEvent event;
    Window win;
    GLXContext ctx;
    int i, c, errors = 0;
    if (!dpy) {
        fprintf(stderr, "glthreads: thread %d: cannot open display\n", which);
        return -1;
    }
    vi = glXChooseVisual(dpy, DefaultScreen(dpy), attributes);
    if (!vi) {
        fprintf(stderr, "glthreads: thread %d: no visual\n", which);
        return -1;
    }
    swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
    swa.border_pixel = 0;
    swa.event_mask = StructureNotifyMask;
    win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 60 + which * 260, 100, 240, 180, 0, vi->depth,
                        InputOutput, vi->visual, CWColormap | CWBorderPixel | CWEventMask, &swa);
    XMapWindow(dpy, win);
    do XNextEvent(dpy, &event); while (event.type != MapNotify);
    ctx = glXCreateContext(dpy, vi, 0, True);
    if (!ctx || !glXMakeCurrent(dpy, win, ctx)) {
        fprintf(stderr, "glthreads: thread %d: no context\n", which);
        return -1;
    }
    for (i = 0; i < frames; ++i) {
        unsigned char want[3], got[4];
        for (c = 0; c < 3; ++c) want[c] = channel(which, i, c);
        glClearColor(want[0] / 255.0f, want[1] / 255.0f, want[2] / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glReadPixels(120, 90, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, got);
        if (got[0] != want[0] || got[1] != want[1] || got[2] != want[2]) {
            if (errors < 5) {
                fprintf(stderr, "glthreads: thread %d frame %d: read %02x%02x%02x, cleared to %02x%02x%02x\n",
                        which, i, got[0], got[1], got[2], want[0], want[1], want[2]);
            }
            ++errors;
        }
        if (glXGetCurrentContext() != ctx || glXGetCurrentDrawable() != win) {
            if (errors < 5) fprintf(stderr, "glthreads: thread %d frame %d: another current context\n", which, i);
            ++errors;
        }
        glXSwapBuffers(dpy, win);
    }
    glXMakeCurrent(dpy, None, 0);
    glXDestroyContext(dpy, ctx);
    XCloseDisplay(dpy);
    printf("glthreads: thread %d: %d frames, %d errors\n", which, frames, errors);
    fflush(stdout);
    return errors;
}

#ifdef GLTHREADS_PTHREAD
static void *run(void *arg)
{
    struct job *job = (struct job *)arg;
    job->errors = draw(job->which, job->frames);
    return 0;
}
#else
static void run(void *arg)
{
    struct job *job = (struct job *)arg;
    job->errors = draw(job->which, job->frames);
}
#endif

int main(int argc, char **argv)
{
    struct job jobs[2];
    int frames = argc > 1 ? atoi(argv[1]) : 200;
    int i;
    for (i = 0; i < 2; ++i) {
        jobs[i].which = i;
        jobs[i].frames = frames;
        jobs[i].errors = -1;
    }
#ifdef GLTHREADS_PTHREAD
    {
        pthread_t other;
        if (pthread_create(&other, 0, run, &jobs[1]) != 0) {
            fprintf(stderr, "glthreads: cannot start a thread\n");
            return 1;
        }
        run(&jobs[0]);
        pthread_join(other, 0);
    }
#else
    {
        /* PR_SADDR: shares the address space (jobs[] included), as Maya's sprocs do. */
        pid_t other = sproc(run, PR_SALL, &jobs[1]);
        if (other < 0) {
            perror("glthreads: sproc");
            return 1;
        }
        run(&jobs[0]);
        while (waitpid(other, 0, 0) < 0) {
        }
    }
#endif
    if (jobs[0].errors == 0 && jobs[1].errors == 0) {
        printf("glthreads: PASS\n");
        return 0;
    }
    printf("glthreads: FAIL\n");
    return 1;
}
