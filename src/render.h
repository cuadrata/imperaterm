/* See LICENSE for licence details. */
#ifndef RENDER_H
#define RENDER_H

#include <glib.h>

#include "vt.h"

/* What one terminal keeps on the GPU: its cells, as a texture. */
typedef struct {
    unsigned tex, vao;
    int cols, rows, gen;
    uint8_t *cells;
} Grid;

enum { CUR_BLOCK, CUR_UNDER, CUR_BAR, CUR_HOLLOW, CUR_NONE };

extern int cellw, cellh;  /* a cell, in the device's pixels */

int r_font(const char *family, int px);
GList *r_families(void);
int r_init(void);               /* with a context current */
const char *r_version(void);    /* what GL says it is */
void r_draw(Grid *g, Term *t, int w, int h, int pad, int shape);
void r_free(Grid *g);

#endif
