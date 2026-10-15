/*
 * Colours noted by libglpass_x.so (xstore.c), read by libGL.so's
 * colour-index emulation.
 */
#ifndef GLPASS_XSTORE_H
#define GLPASS_XSTORE_H

#define GLPASS_PALETTE_SIZE 4096 /* 12-bit colour index */

struct glpass_colormap {
    unsigned long id; /* X Colormap */
    unsigned char rgb[GLPASS_PALETTE_SIZE][3];
    unsigned char set[GLPASS_PALETTE_SIZE];
    struct glpass_colormap *next;
};

/* The colours stored in colormap `id`, or 0 if none were. */
struct glpass_colormap *glpass_find_colormap(unsigned long id);

/* Changes whenever a colour is stored (never 0). */
unsigned long glpass_colormap_generation(void);

#endif
