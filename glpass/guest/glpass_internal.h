/*
 * Internals shared by the hand-written (glpass.c) and generated (glpass_gen.c)
 * parts of the guest library.
 */
#ifndef GLPASS_INTERNAL_H
#define GLPASS_INTERNAL_H

#include <GL/gl.h>

#include "glpass_protocol.h"
#include "glpass_fn.h"

/* The calling thread's GL state: its current context, and the command
   stream and glBegin batch being built for it. Found with a pthread key in
   POSIX threads programs, by the thread's own pid in sproc ones (see
   glpass.c); made on the thread's first GL call. */
struct glp_thread;
struct glp_thread *glp_self(void);

/* The id of the calling thread's current context (0: none), which every
   GL hypercall carries (GLP_OP_WORD). */
unsigned int glp_current_id(void);

/* The library lock: its own X connection and the tables shared by all
   threads. The thread holding it may take it again. */
void glp_lock(void);
void glp_unlock(void);

/* Appends a command record with `words` payload words to the calling
   thread's stream; returns the payload. */
unsigned int *glp_put(unsigned int cmd, unsigned int words);
unsigned int glp_fbits(GLfloat f);
void glp_dbits(unsigned int *p, GLdouble d);

/* Where GL and GLX calls go (glpass.c, "Route"), decided at the first call:
   the host GPU, or IRIX's own OpenGL. Every entry point starts with
       if (glp_route != GLP_ROUTE_HOST && glp_forwarding()) { forward; return; }
   so the route costs one comparison on the host's path. */
typedef void (*glp_proc)(void); /* what glXGetProcAddress returns */
#define GLP_ROUTE_HOST 1
#define GLP_ROUTE_NATIVE 2
extern int glp_route;
int glp_forwarding(void);
void *glp_native_symbol(const char *name); /* IRIX's own entry point, or 0 */

/* An entry point the pass-through does not provide was called: logs its
   first call with GLPASS_TRACE (`logged`: the caller's own flag). */
void glp_unimplemented(const char *name, int *logged);

/* GLPASS_TRACE=1: logs the first call of every entry point. */
extern int glp_trace;
void glp_trace_first(unsigned int fn, const char *name);
#define GLP_TRACE(fn, name) do { if (glp_trace) glp_trace_first((fn), (name)); } while (0)

/* Number of values the vector forms of glLight, glMaterial, ... take for pname. */
enum { GLP_GROUP_LIGHT, GLP_GROUP_LIGHTMODEL, GLP_GROUP_MATERIAL, GLP_GROUP_FOG, GLP_GROUP_TEXPARAM,
       GLP_GROUP_TEXENV, GLP_GROUP_TEXGEN, GLP_GROUP_POINTPARAM };
int glp_pname_count(int group, GLenum pname);

#endif
