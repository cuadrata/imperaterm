/* See LICENSE for licence details. */
#ifndef VT_H
#define VT_H

#include <stddef.h>
#include <stdint.h>

/*
 * A cell: the character in the low 21 bits of u and its attributes above
 * them. A colour is 0 for the default, 1 to 256 for the palette, or
 * TRUECOL | rgb. An erased cell is all zeroes.
 */
typedef struct { uint32_t u, fg, bg; } Cell;

#define RUNE(u) ((u) & 0x1fffff)
#define TRUECOL 0x1000000u

#define A_BOLD   (1u << 21)
#define A_FAINT  (1u << 22)
#define A_ITALIC (1u << 23)
#define A_UNDER  (1u << 24)
#define A_BLINK  (1u << 25)
#define A_REV    (1u << 26)
#define A_INVIS  (1u << 27)
#define A_STRUCK (1u << 28)
#define A_WRAP   (1u << 29)  /* on a row's last cell: the line goes on below */
#define A_WIDE   (1u << 30)
#define A_WDUMMY (1u << 31)  /* the second cell of a wide character */

enum {
    M_WRAP = 1 << 0, M_INSERT = 1 << 1, M_ALT = 1 << 2, M_CRLF = 1 << 3,
    M_APPCURSOR = 1 << 4, M_APPKEYPAD = 1 << 5, M_HIDE = 1 << 6, M_REVERSE = 1 << 7,
    M_MOUSEX10 = 1 << 8, M_MOUSEBTN = 1 << 9, M_MOUSEMOTION = 1 << 10,
    M_MOUSEMANY = 1 << 11, M_MOUSESGR = 1 << 12, M_FOCUS = 1 << 13,
    M_BRACKET = 1 << 14, M_ALTSCROLL = 1 << 15, M_SYNC = 1 << 16,
    M_MOUSE = M_MOUSEX10 | M_MOUSEBTN | M_MOUSEMOTION | M_MOUSEMANY
};

/* The palette: 256 colours, then the default foreground, background and cursor. */
enum { C_FG = 256, C_BG, C_CURSOR, C_N };

/* What a shell tells the terminal: where it is, as a file: URI, and who it is, as user@host. */
enum { TOLD_WHERE, TOLD_WHO };

typedef struct { int x, y; } Pos;
typedef struct {
    int x, y;
    uint32_t u, fg, bg;  /* what is written takes these */
    uint8_t wrapnext, origin, charset, tran[4];
} Cursor;

typedef struct Term Term;
struct Term {
    int cols, rows, top, bot;
    Cell *buf, *alt;                /* the cells of cap rows, and of the alternate screen's */
    Cell **line, **altline, **spare; /* a ring of cap rows and the alternate screen's, in order */
    int cap, base, hist, histmax;   /* row 0 is ring row base, with hist rows before it */
    int scr;                        /* rows the view is scrolled back by */
    uint8_t *dirty, *tabs;
    int alldirty;
    int whole;  /* the child has said that what it changed is whole: it ended a synchronized update */
    Cursor c, saved[2];
    uint32_t mode, lastc, pal[C_N];
    const uint32_t *defpal;
    int cstyle;  /* DECSCUSR: 0 is the user's own */
    int esc, uneed, csilen, slen, narg, arg[34];  /* 32, and two that are 0 after the last */
    uint32_t ucp, sub;
    char csi[128], str[2048], priv, inter, fin, stype, sbel;
    struct { int rect, snap, alt, empty; Pos ob, oe, nb, ne; } sel;
    void (*write)(Term *, const char *, size_t);  /* to the child */
    void (*title)(Term *, const char *);
    void (*told)(Term *, int what, const char *);  /* who is at the terminal or where, as the shell says it */
    void (*bell)(Term *);
    void *data;
};

Term *vt_new(int cols, int rows, int hist, const uint32_t *pal);
void vt_free(Term *t);
void vt_reset(Term *t);
void vt_resize(Term *t, int cols, int rows);
void vt_write(Term *t, const char *s, size_t n);        /* from the child */
const Cell *vt_row(Term *t, int y);                     /* a row of the view */

void vt_paste(Term *t, const char *s, size_t n);
int vt_mouse(Term *t, int x, int y, int btn, int type, int mods);
int vt_utf8(uint32_t u, char *s);

/* Selections are in view coordinates going in, and screen rows inside. */
enum { SNAP_NONE, SNAP_WORD, SNAP_LINE };
void vt_selstart(Term *t, int x, int y, int snap);
void vt_selextend(Term *t, int x, int y, int rect);
void vt_selclear(Term *t);
int vt_selrange(Term *t, int y, int *x1, int *x2);  /* of a view row */
char *vt_seltext(Term *t);

#endif
