/*
 * Internals shared by the hand-written (glpass.c) and generated (glpass_gen.c)
 * parts of the guest library.
 */
#ifndef GLPASS_INTERNAL_H
#define GLPASS_INTERNAL_H

#include <GL/gl.h>

#include "glpass_protocol.h"
#include "glpass_fn.h"

/* Appends a command record with `words` payload words; returns the payload. */
unsigned int *glp_put(unsigned int cmd, unsigned int words);
unsigned int glp_fbits(GLfloat f);
void glp_dbits(unsigned int *p, GLdouble d);

/* Logs the first call of an entry point the pass-through does not provide. */
void glp_unimplemented(const char *name);

/* GLPASS_TRACE=1: logs the first call of every entry point. */
extern int glp_trace;
void glp_trace_first(unsigned int fn, const char *name);
#define GLP_TRACE(fn, name) do { if (glp_trace) glp_trace_first((fn), (name)); } while (0)

/* Number of values the vector forms of glLight, glMaterial, ... take for pname. */
enum { GLP_GROUP_LIGHT, GLP_GROUP_LIGHTMODEL, GLP_GROUP_MATERIAL, GLP_GROUP_FOG, GLP_GROUP_TEXPARAM,
       GLP_GROUP_TEXENV, GLP_GROUP_TEXGEN, GLP_GROUP_POINTPARAM };
int glp_pname_count(int group, GLenum pname);

#endif
