/*
 * glstubs: entry points glpass does not provide, and arrays it must not read.
 *   stubs      calls 80 different stubs twice each, by name, from libGL.so;
 *              with GLPASS_TRACE=1 each is logged exactly once ("is not
 *              supported"), without it none is (count the lines on stderr)
 *   null       vector calls with a NULL array (glVertex3fv, glLightfv,
 *              glLoadMatrixf, glRectfv) are ignored, not read
 *   LD_LIBRARYN32_PATH=<glpass dir> ./glstubs
 * Prints PASS when it gets to the end (no crash).
 */
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>

#include <dlfcn.h>
#include <stdio.h>

static const char *const stubs[] = {
    "glAlphaFragmentOp1ATI", "glAlphaFragmentOp2ATI", "glAlphaFragmentOp3ATI", "glArrayObjectATI",
    "glAsyncMarkerSGIX", "glBeginFragmentShaderATI", "glBeginOcclusionQueryNV", "glBeginVertexShaderEXT",
    "glBindFragmentShaderATI", "glBindVertexShaderEXT", "glClientActiveVertexStreamATI", "glColorFragmentOp1ATI",
    "glColorFragmentOp2ATI", "glColorFragmentOp3ATI", "glColorSubTable", "glColorTable",
    "glColorTableParameterfv", "glColorTableParameterfvSGI", "glColorTableParameteriv", "glColorTableParameterivSGI",
    "glColorTableSGI", "glCompressedTexImage1D", "glCompressedTexImage1DARB", "glCompressedTexImage2D",
    "glCompressedTexImage2DARB", "glCompressedTexImage3D", "glCompressedTexImage3DARB", "glCompressedTexSubImage1D",
    "glCompressedTexSubImage1DARB", "glCompressedTexSubImage2D", "glCompressedTexSubImage2DARB", "glCompressedTexSubImage3D",
    "glCompressedTexSubImage3DARB", "glConvolutionFilter1D", "glConvolutionFilter1DEXT", "glConvolutionFilter2D",
    "glConvolutionFilter2DEXT", "glConvolutionParameterfv", "glConvolutionParameterfvEXT", "glConvolutionParameteriv",
    "glConvolutionParameterivEXT", "glDeformSGIX", "glDeformationMap3dSGIX", "glDeformationMap3fSGIX",
    "glDeleteAsyncMarkersSGIX", "glDeleteFragmentShaderATI", "glDeleteObjectBufferATI", "glDeleteOcclusionQueriesNV",
    "glDeleteProgramsARB", "glDeleteVertexShaderEXT", "glDetailTexFuncSGIS", "glDisableVariantClientStateEXT",
    "glDrawBuffersATI", "glDrawElementArrayATI", "glDrawRangeElementArrayATI", "glElementPointerATI",
    "glEnableVariantClientStateEXT", "glEndFragmentShaderATI", "glEndOcclusionQueryNV", "glEndVertexShaderEXT",
    "glExtractComponentEXT", "glFlushRasterSGIX", "glFogCoordPointer", "glFogCoordPointerEXT",
    "glFogFuncSGIS", "glFragmentColorMaterialSGIX", "glFragmentLightModelfSGIX", "glFragmentLightModelfvSGIX",
    "glFragmentLightModeliSGIX", "glFragmentLightModelivSGIX", "glFragmentLightfSGIX", "glFragmentLightfvSGIX",
    "glFragmentLightiSGIX", "glFragmentLightivSGIX", "glFragmentMaterialfSGIX", "glFragmentMaterialfvSGIX",
    "glFragmentMaterialiSGIX", "glFragmentMaterialivSGIX", "glFrameZoomSGIX", "glGenOcclusionQueriesNV"};

int main(void)
{
    int attributes[] = {GLX_RGBA, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None};
    Display *dpy = XOpenDisplay(0);
    XVisualInfo *vi;
    XSetWindowAttributes swa;
    XEvent event;
    Window win;
    GLXContext ctx;
    void *gl = dlopen("libGL.so", RTLD_LAZY);
    unsigned int i, found = 0;
    if (!dpy || !gl || !(vi = glXChooseVisual(dpy, DefaultScreen(dpy), attributes))) {
        fprintf(stderr, "glstubs: no display, libGL.so or visual\n");
        return 1;
    }
    swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
    swa.border_pixel = 0;
    swa.event_mask = StructureNotifyMask;
    win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 100, 100, 64, 64, 0, vi->depth, InputOutput, vi->visual,
                        CWColormap | CWBorderPixel | CWEventMask, &swa);
    XMapWindow(dpy, win);
    do XNextEvent(dpy, &event); while (event.type != MapNotify);
    ctx = glXCreateContext(dpy, vi, 0, True);
    if (!ctx || !glXMakeCurrent(dpy, win, ctx)) {
        fprintf(stderr, "glstubs: no context\n");
        return 1;
    }
    /* The stubs ignore their arguments, so they are called with none. */
    for (i = 0; i < sizeof stubs / sizeof stubs[0]; ++i) {
        void (*fn)(void) = (void (*)(void))dlsym(gl, stubs[i]);
        if (!fn) continue;
        ++found;
        fn();
        fn();
    }
    printf("glstubs: stubs     %u of %u found and called twice\n", found, (unsigned)(sizeof stubs / sizeof stubs[0]));
    glVertex3fv(0);
    glLightfv(GL_LIGHT0, GL_POSITION, 0);
    glLoadMatrixf(0);
    glRectfv(0, 0);
    glFinish();
    printf("glstubs: null      PASS (no crash)\n");
    glXMakeCurrent(dpy, None, 0);
    glXDestroyContext(dpy, ctx);
    XCloseDisplay(dpy);
    printf("glstubs: PASS\n");
    return 0;
}
