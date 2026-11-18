/* See LICENSE for licence details. */
/*
 * The terminal: what a child writes becomes a grid of cells. The escape
 * sequence handling and the selection logic come from st; the storage does
 * not. Rows live in a ring, so that a line scrolling off the top is one
 * index moving, and the ring is what the scrollback is. The ring is of
 * rows, not of cells: a region that scrolls has its rows change places,
 * and none of its cells move.
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "vt.h"

enum { E_START = 1, E_CSI = 2, E_STR = 4, E_CHARSET = 8, E_TEST = 16, E_UTF8 = 32 };

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(x, a, b) ((x) < (a) ? (a) : (x) > (b) ? (b) : (x))
#define DEF(a, b) ((a) ? (a) : (b))
#define SEL(t) ((t)->sel.ob.x != -1)
#define ALT(t) ((t)->mode & M_ALT)
/* A reply to the program: what would not fit is cut, not read past the end. */
#define REPLY(t, ...) do { char r_[96]; int n_ = snprintf(r_, sizeof r_, __VA_ARGS__); \
    (t)->write((t), r_, CLAMP(n_, 0, (int)sizeof r_ - 1)); } while (0)

/* The DEC special graphics set, from ` to ~. */
static const uint16_t gfx[] = {
    0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0, 0x00b1, 0x2424, 0x240b,
    0x2518, 0x2510, 0x250c, 0x2514, 0x253c, 0x23ba, 0x23bb, 0x2500, 0x23bc, 0x23bd,
    0x251c, 0x2524, 0x2534, 0x252c, 0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7
};

/* Where a screen row is in the ring; negative rows are the scrollback. */
static Cell **
slot(Term *t, int y)
{
    int i = t->base + y;

    if (ALT(t))
        return t->altline + y;
    if (i < 0)
        i += t->cap;
    else if (i >= t->cap)
        i -= t->cap;
    return t->line + i;
}

static Cell *
row(Term *t, int y)
{
    return *slot(t, y);
}

/* Rows top to bot go round by n, up where n is more than 0. */
static void
turn(Term *t, int top, int bot, int n)
{
    int k = bot - top + 1, i;

    n = (n % k + k) % k;
    for (i = 0; i < k; i++)
        t->spare[i] = row(t, top + i);
    for (i = 0; i < k; i++)
        *slot(t, top + i) = t->spare[(i + n) % k];
}

const Cell *
vt_row(Term *t, int y)
{
    return row(t, ALT(t) ? y : y - t->scr);
}

int
vt_utf8(uint32_t u, char *s)
{
    if (u < 0x80)
        return s[0] = u, 1;
    if (u < 0x800)
        return s[0] = 0xc0 | u >> 6, s[1] = 0x80 | (u & 0x3f), 2;
    if (u < 0x10000)
        return s[0] = 0xe0 | u >> 12, s[1] = 0x80 | (u >> 6 & 0x3f), s[2] = 0x80 | (u & 0x3f), 3;
    return s[0] = 0xf0 | u >> 18, s[1] = 0x80 | (u >> 12 & 0x3f), s[2] = 0x80 | (u >> 6 & 0x3f),
           s[3] = 0x80 | (u & 0x3f), 4;
}

/* Selection. */

static int
linelen(Term *t, const Cell *l)
{
    int i = t->cols;

    if (l[i - 1].u & A_WRAP)
        return i;
    while (i > 0 && (RUNE(l[i - 1].u) == 0 || RUNE(l[i - 1].u) == ' '))
        i--;
    return i;
}

static int
delim(uint32_t u)
{
    u = RUNE(u);
    return !u || (u < 0x80 && strchr(" \t\"'`()[]{}<>|;", u));
}

static void
selsnap(Term *t, Pos *p, int dir)
{
    int lo = ALT(t) ? 0 : -t->hist, hi = t->rows - 1, last = t->cols - 1, x, y, d, pd;
    const Cell *g, *pg;

    if (t->sel.snap == SNAP_WORD) {
        pg = &row(t, p->y)[p->x];
        pd = delim(pg->u);
        for (;;) {
            x = p->x + dir, y = p->y;
            if (x < 0 || x > last) {
                y += dir, x = (x + t->cols) % t->cols;
                if (y < lo || y > hi || !(row(t, dir > 0 ? p->y : y)[last].u & A_WRAP))
                    break;
            }
            if (x >= linelen(t, row(t, y)))
                break;
            g = &row(t, y)[x];
            d = delim(g->u);
            if (!(g->u & A_WDUMMY) && (d != pd || (d && RUNE(g->u) != RUNE(pg->u))))
                break;
            p->x = x, p->y = y, pg = g, pd = d;
        }
    } else if (t->sel.snap == SNAP_LINE) {
        p->x = dir < 0 ? 0 : last;
        if (dir < 0)
            for (; p->y > lo && (row(t, p->y - 1)[last].u & A_WRAP); p->y--);
        else
            for (; p->y < hi && (row(t, p->y)[last].u & A_WRAP); p->y++);
    }
}

static void
selnorm(Term *t)
{
    Pos *b = &t->sel.nb, *e = &t->sel.ne, ob = t->sel.ob, oe = t->sel.oe;

    if (!t->sel.rect && ob.y != oe.y) {
        b->x = ob.y < oe.y ? ob.x : oe.x;
        e->x = ob.y < oe.y ? oe.x : ob.x;
    } else {
        b->x = MIN(ob.x, oe.x);
        e->x = MAX(ob.x, oe.x);
    }
    b->y = MIN(ob.y, oe.y);
    e->y = MAX(ob.y, oe.y);
    selsnap(t, b, -1);
    selsnap(t, e, +1);
    if (t->sel.rect)
        return;
    /* Past the end of a line is the whole of its end. */
    if (linelen(t, row(t, b->y)) < b->x)
        b->x = linelen(t, row(t, b->y));
    if (linelen(t, row(t, e->y)) <= e->x)
        e->x = t->cols - 1;
}

void
vt_selclear(Term *t)
{
    if (SEL(t))
        t->alldirty = 1;
    t->sel.ob.x = -1;
}

void
vt_selstart(Term *t, int x, int y, int snap)
{
    vt_selclear(t);
    t->sel.rect = 0, t->sel.snap = snap, t->sel.alt = ALT(t), t->sel.empty = !snap;
    t->sel.ob.x = t->sel.oe.x = CLAMP(x, 0, t->cols - 1);
    t->sel.ob.y = t->sel.oe.y = CLAMP(y, 0, t->rows - 1) - (ALT(t) ? 0 : t->scr);
    selnorm(t);
    t->alldirty = 1;
}

void
vt_selextend(Term *t, int x, int y, int rect)
{
    if (!SEL(t))
        return;
    t->sel.oe.x = CLAMP(x, 0, t->cols - 1);
    t->sel.oe.y = CLAMP(y, 0, t->rows - 1) - (ALT(t) ? 0 : t->scr);
    t->sel.rect = rect, t->sel.empty = 0;
    selnorm(t);
    t->alldirty = 1;
}

int
vt_selrange(Term *t, int y, int *x1, int *x2)
{
    if (!SEL(t) || t->sel.empty || t->sel.alt != (int)ALT(t))
        return 0;
    if (!ALT(t))
        y -= t->scr;
    if (y < t->sel.nb.y || y > t->sel.ne.y)
        return 0;
    *x1 = t->sel.rect || y == t->sel.nb.y ? t->sel.nb.x : 0;
    *x2 = t->sel.rect || y == t->sel.ne.y ? t->sel.ne.x : t->cols - 1;
    return 1;
}

char *
vt_seltext(Term *t)
{
    int y, x, x1, x2, len, lo = ALT(t) ? 0 : -t->hist;
    char *s, *p;

    if (!SEL(t) || t->sel.empty || t->sel.alt != (int)ALT(t))
        return NULL;
    p = s = malloc((size_t)(t->cols * 4 + 1) * (t->sel.ne.y - t->sel.nb.y + 1) + 1);
    for (y = MAX(t->sel.nb.y, lo); s && y <= t->sel.ne.y; y++) {
        const Cell *l = row(t, y);

        len = linelen(t, l);
        x1 = t->sel.rect || y == t->sel.nb.y ? t->sel.nb.x : 0;
        x2 = t->sel.rect || y == t->sel.ne.y ? t->sel.ne.x : t->cols - 1;
        for (x = x1; x <= MIN(x2, len - 1); x++)
            if (!(l[x].u & A_WDUMMY))
                p += vt_utf8(DEF(RUNE(l[x].u), ' '), p);
        if ((y < t->sel.ne.y || x2 >= len) && (!(l[t->cols - 1].u & A_WRAP) || t->sel.rect))
            *p++ = '\n';
    }
    if (s)
        *p = 0;
    return s;
}

/* The screen. */

static void
clear(Term *t, int x1, int y1, int x2, int y2)
{
    Cell b = { 0, t->c.fg, t->c.bg }, *l;
    int x, y;

    if (SEL(t) && t->sel.alt == (int)ALT(t) && t->sel.nb.y <= y2 && t->sel.ne.y >= y1)
        vt_selclear(t);
    for (y = y1; y <= y2; y++) {
        l = row(t, y);
        t->dirty[y] = 1;
        if (!(b.fg | b.bg))
            memset(l + x1, 0, (x2 - x1 + 1) * sizeof *l);
        else
            for (x = x1; x <= x2; x++)
                l[x] = b;
    }
}

/* Rows top to the bottom margin go up by n; with keep, into the scrollback. */
static void
scrollup(Term *t, int top, int n, int keep)
{
    int bot = t->bot;

    if ((n = CLAMP(n, 0, bot - top + 1)) == 0)
        return;
    if (keep && !top && !ALT(t) && t->histmax) {
        t->base = (t->base + n) % t->cap;
        t->hist = MIN(t->hist + n, t->histmax);
        if (t->scr)
            t->scr = MIN(t->scr + n, t->hist);
        if (bot < t->rows - 1)  /* what is below the margin stays */
            turn(t, bot - n + 1, t->rows - 1, -n);
        if (SEL(t) && !t->sel.alt) {
            t->sel.ob.y -= n, t->sel.oe.y -= n, t->sel.nb.y -= n, t->sel.ne.y -= n;
            /* Its first row is no longer kept: nor is it, as none of it may point at a row that is not there. */
            if (MIN(t->sel.nb.y, MIN(t->sel.ob.y, t->sel.oe.y)) < -t->hist)
                vt_selclear(t);
        }
    } else {
        if (SEL(t))
            vt_selclear(t);
        turn(t, top, bot, n);
    }
    t->alldirty = 1;
    clear(t, 0, bot - n + 1, t->cols - 1, bot);
}

static void
scrolldown(Term *t, int top, int n)
{
    int bot = t->bot;

    if ((n = CLAMP(n, 0, bot - top + 1)) == 0)
        return;
    if (SEL(t))
        vt_selclear(t);
    turn(t, top, bot, -n);
    t->alldirty = 1;
    clear(t, 0, top, t->cols - 1, top + n - 1);
}

static void
moveto(Term *t, int x, int y)
{
    int lo = t->c.origin ? t->top : 0, hi = t->c.origin ? t->bot : t->rows - 1;

    t->c.wrapnext = 0;
    t->c.x = CLAMP(x, 0, t->cols - 1);
    t->c.y = CLAMP(y, lo, hi);
}

static void
moveato(Term *t, int x, int y)
{
    moveto(t, x, y + (t->c.origin ? t->top : 0));
}

static void
newline(Term *t, int cr)
{
    int y = t->c.y;

    if (y == t->bot)
        scrollup(t, t->top, 1, 1);
    else
        y++;
    moveto(t, cr ? 0 : t->c.x, y);
}

static void
puttab(Term *t, int n)
{
    int x = t->c.x;

    for (; n > 0 && x < t->cols - 1; n--)
        for (++x; x < t->cols - 1 && !t->tabs[x]; ++x);
    for (; n < 0 && x > 0; n++)
        for (--x; x > 0 && !t->tabs[x]; --x);
    t->c.x = x;
}

/* A character that is drawn: the path the plain ASCII in vt_write does not take. */
static void
put(Term *t, uint32_t u)
{
    int w = 1, cols = t->cols, x;
    Cell *l, *g;

    if (u >= 0x300) {
        if ((w = wcwidth(u)) == 0)
            return;
        w = w < 0 ? 1 : MIN(w, 2);
    } else if (u >= 0x60 && u < 0x7f && t->c.tran[t->c.charset])
        u = gfx[u - 0x60];
    if (SEL(t) && t->sel.alt == (int)ALT(t) && t->c.y >= t->sel.nb.y && t->c.y <= t->sel.ne.y)
        vt_selclear(t);
    l = row(t, t->c.y);
    if (t->c.wrapnext && (t->mode & M_WRAP)) {
        l[cols - 1].u |= A_WRAP;
        newline(t, 1);
        l = row(t, t->c.y);
    }
    if (t->c.x + w > cols) {
        if (t->mode & M_WRAP)
            newline(t, 1);
        else
            t->c.x = cols - w;
        l = row(t, t->c.y);
    }
    x = t->c.x, g = l + x;
    if ((t->mode & M_INSERT) && x + w < cols)
        memmove(g + w, g, (cols - x - w) * sizeof *g);
    if ((g->u & A_WIDE) && x + 1 < cols && (g[1].u & A_WDUMMY))  /* half a wide character is none */
        g[1].u = 0;
    else if ((g->u & A_WDUMMY) && x > 0 && (g[-1].u & A_WIDE))
        g[-1].u = 0;
    g->u = u | t->c.u, g->fg = t->c.fg, g->bg = t->c.bg;
    if (w == 2 && x + 1 < cols) {
        if ((g[1].u & A_WIDE) && x + 2 < cols && (g[2].u & A_WDUMMY))
            g[2].u = 0;
        g->u |= A_WIDE;
        g[1] = *g;
        g[1].u = A_WDUMMY;
    }
    t->dirty[t->c.y] = 1;
    t->lastc = u;
    if (x + w < cols)
        t->c.x += w;
    else
        t->c.wrapnext = 1;
}

static void
swapscreen(Term *t, int alt)
{
    if (!alt == !ALT(t))
        return;
    vt_selclear(t);
    t->mode ^= M_ALT;
    t->scr = 0;
    t->alldirty = 1;
}

static void
cursor(Term *t, int save)
{
    Cursor *s = &t->saved[!!ALT(t)];

    if (save)
        *s = t->c;
    else {
        t->c = *s;
        moveto(t, s->x, s->y);
    }
}

/* DECSTR, and with hard RIS: the screen and the palette go too. */
static void
reset(Term *t, int hard)
{
    int i;

    if (hard)
        t->mode &= ~M_ALT;
    memset(&t->c, 0, sizeof t->c);
    t->saved[0] = t->saved[1] = t->c;
    t->top = 0, t->bot = t->rows - 1;
    t->mode = (t->mode & M_ALT) | M_WRAP | M_ALTSCROLL;
    t->esc = t->uneed = t->cstyle = 0;
    for (i = 0; i < t->cols; i++)
        t->tabs[i] = i && i % 8 == 0;
    t->alldirty = 1;
    if (!hard)
        return;
    memcpy(t->pal, t->defpal, sizeof t->pal);
    vt_selclear(t);
    t->scr = 0;
    clear(t, 0, 0, t->cols - 1, t->rows - 1);
}

void
vt_reset(Term *t)
{
    reset(t, 1);
}

/* Escape sequences. */

static uint32_t *
modebit(Term *t, int n, uint32_t *bit)
{
    static const struct { int n; uint32_t bit; } m[] = {
        { 1, M_APPCURSOR }, { 5, M_REVERSE }, { 7, M_WRAP }, { 9, M_MOUSEX10 },
        { 25, M_HIDE }, { 1000, M_MOUSEBTN }, { 1002, M_MOUSEMOTION }, { 1003, M_MOUSEMANY },
        { 1004, M_FOCUS }, { 1006, M_MOUSESGR }, { 1007, M_ALTSCROLL }, { 47, M_ALT },
        { 1047, M_ALT }, { 1049, M_ALT }, { 2004, M_BRACKET }, { 2026, M_SYNC }
    };
    size_t i;

    for (i = 0; i < sizeof m / sizeof *m; i++)
        if (m[i].n == n)
            return *bit = m[i].bit, &t->mode;
    return NULL;
}

static void
setmode(Term *t, int set)
{
    uint32_t bit;
    int i, n, on;

    for (i = 0; i < t->narg; i++) {
        n = t->arg[i];
        if (!t->priv) {
            if (n == 4 || n == 20)
                t->mode = set ? t->mode | (n == 4 ? M_INSERT : M_CRLF)
                          : t->mode & ~(n == 4 ? M_INSERT : M_CRLF);
        } else if (n == 6) {
            t->c.origin = set;
            moveato(t, 0, 0);
        } else if (n == 1048) {
            cursor(t, set);
        } else if (n == 47 || n == 1047 || n == 1049) {
            if (n == 1049 && set)
                cursor(t, 1);
            swapscreen(t, set);
            if (set)
                clear(t, 0, 0, t->cols - 1, t->rows - 1);
            if (n == 1049 && !set)
                cursor(t, 0);
        } else if (modebit(t, n, &bit)) {
            if (bit & M_MOUSE)
                t->mode &= ~M_MOUSE;
            on = n == 25 ? !set : set;
            t->mode = on ? t->mode | bit : t->mode & ~bit;
            if (n == 5)
                t->alldirty = 1;
            if (n == 2026 && !set)
                t->whole = 1;
        }
    }
}

/* The colour that follows a 38, 48 or 58 at arg i; where the list goes on. */
static int
sgrcolour(Term *t, int i, uint32_t *col)
{
    int *a = t->arg + i, n = t->narg - i - 1, sub = 0;

    while (i + 1 + sub < t->narg && (t->sub >> (i + 1 + sub) & 1))
        sub++;
    if (n >= 2 && a[1] == 5) {
        *col = (a[2] & 255) + 1;
        return i + MAX(sub, 2);
    }
    if (n >= 4 && a[1] == 2) {
        a += sub >= 5;  /* 38:2:space:r:g:b */
        *col = TRUECOL | (a[2] & 255) << 16 | (a[3] & 255) << 8 | (a[4] & 255);
        return i + MAX(sub, 4);
    }
    return i + MAX(sub, n > 0);
}

static void
sgr(Term *t)
{
    static const uint32_t on[] = { 0, A_BOLD, A_FAINT, A_ITALIC, A_UNDER, A_BLINK, A_BLINK, A_REV, A_INVIS, A_STRUCK };
    static const uint32_t off[] = { 0, 0, A_BOLD | A_FAINT, A_ITALIC, A_UNDER, A_BLINK, 0, A_REV, A_INVIS, A_STRUCK };
    uint32_t col;
    int i, v;

    for (i = 0; i < t->narg; i++) {
        v = t->arg[i];
        col = 0;
        if (v == 0)
            t->c.u = t->c.fg = t->c.bg = 0;
        else if (v == 4 && i + 1 < t->narg && (t->sub >> (i + 1) & 1))
            t->c.u = t->arg[++i] ? t->c.u | A_UNDER : t->c.u & ~A_UNDER;
        else if (v < 10)
            t->c.u |= on[v];
        else if (v == 21)
            t->c.u |= A_UNDER;
        else if (v >= 20 && v < 30)
            t->c.u &= ~off[v - 20];
        else if (v == 38 || v == 48 || v == 58) {
            i = sgrcolour(t, i, &col);
            if (col && v != 58)
                *(v == 38 ? &t->c.fg : &t->c.bg) = col;
        } else if (v >= 30 && v < 38)
            t->c.fg = v - 30 + 1;
        else if (v == 39)
            t->c.fg = 0;
        else if (v >= 40 && v < 48)
            t->c.bg = v - 40 + 1;
        else if (v == 49)
            t->c.bg = 0;
        else if (v >= 90 && v < 98)
            t->c.fg = v - 90 + 9;
        else if (v >= 100 && v < 108)
            t->c.bg = v - 100 + 9;
    }
}

static void
csiparse(Term *t)
{
    char *p = t->csi;
    int n = 0, v;

    t->priv = t->inter = 0, t->sub = 0;
    if (*p >= '<' && *p <= '?')
        t->priv = *p++;
    for (;;) {
        for (v = 0; *p >= '0' && *p <= '9'; p++)
            v = MIN(v * 10 + *p - '0', 65535);
        if (n < 32)
            t->arg[n++] = v;
        if (*p == ':' && n < 32)
            t->sub |= 1u << n;
        if (*p != ';' && *p != ':')
            break;
        p++;
    }
    while (*p >= 0x20 && *p <= 0x2f)
        t->inter = *p++;
    t->fin = *p;
    t->narg = n;
    t->arg[n] = t->arg[n + 1] = 0;  /* what a sequence leaves out is 0 */
}

static void
csihandle(Term *t)
{
    int *a = t->arg, n = DEF(a[0], 1), x = t->c.x, y = t->c.y, last = t->cols - 1, i;
    uint32_t bit, *m;
    Cell *l;

    if (t->priv && !strchr("hlcqpnJK", t->fin))
        return;
    switch (t->fin) {
    case '@':  /* ICH */
        n = MIN(n, t->cols - x);
        l = row(t, y);
        memmove(l + x + n, l + x, (t->cols - x - n) * sizeof *l);
        clear(t, x, y, x + n - 1, y);
        break;
    case 'A': moveto(t, x, y - n); break;
    case 'B': case 'e': moveto(t, x, y + n); break;
    case 'C': case 'a': moveto(t, x + n, y); break;
    case 'D': moveto(t, x - n, y); break;
    case 'E': moveto(t, 0, y + n); break;
    case 'F': moveto(t, 0, y - n); break;
    case 'G': case '`': moveto(t, n - 1, y); break;
    case 'H': case 'f': moveato(t, DEF(a[1], 1) - 1, n - 1); break;
    case 'I': puttab(t, n); break;
    case 'Z': puttab(t, -n); break;
    case 'd': moveato(t, x, n - 1); break;
    case 'J':  /* ED */
        if (a[0] == 0) {
            clear(t, x, y, last, y);
            if (y < t->rows - 1)
                clear(t, 0, y + 1, last, t->rows - 1);
        } else if (a[0] == 1) {
            if (y > 0)
                clear(t, 0, 0, last, y - 1);
            clear(t, 0, y, x, y);
        } else if (a[0] == 2) {
            clear(t, 0, 0, last, t->rows - 1);
        } else if (a[0] == 3 && !ALT(t)) {
            vt_selclear(t);
            t->hist = t->scr = 0;
            t->alldirty = 1;
        }
        break;
    case 'K':  /* EL */
        clear(t, a[0] == 0 ? x : 0, y, a[0] == 1 ? x : last, y);
        break;
    case 'L':  /* IL */
        if (y >= t->top && y <= t->bot)
            scrolldown(t, y, n);
        break;
    case 'M':  /* DL */
        if (y >= t->top && y <= t->bot)
            scrollup(t, y, n, 0);
        break;
    case 'P':  /* DCH */
        n = MIN(n, t->cols - x);
        l = row(t, y);
        memmove(l + x, l + x + n, (t->cols - x - n) * sizeof *l);
        clear(t, t->cols - n, y, last, y);
        break;
    case 'S': scrollup(t, t->top, n, 1); break;
    case 'T': scrolldown(t, t->top, n); break;
    case 'X': clear(t, x, y, MIN(x + n - 1, last), y); break;
    case 'b':  /* REP */
        for (i = MIN(n, 65535); t->lastc && i > 0; i--)
            put(t, t->lastc);
        break;
    case 'c':  /* DA */
        if (t->priv == '>')
            REPLY(t, "\033[>1;10400;0c");
        else if (!t->priv && !a[0])
            REPLY(t, "\033[?62;22c");
        break;
    case 'g':  /* TBC */
        if (a[0] == 0)
            t->tabs[x] = 0;
        else if (a[0] == 3)
            memset(t->tabs, 0, t->cols);
        break;
    case 'h': case 'l':
        setmode(t, t->fin == 'h');
        break;
    case 'm':
        sgr(t);
        break;
    case 'n':  /* DSR */
        if (a[0] == 5)
            REPLY(t, "\033[0n");
        else if (a[0] == 6)
            REPLY(t, "\033[%d;%dR", y - (t->c.origin ? t->top : 0) + 1, x + 1);
        break;
    case 'p':
        if (t->inter == '!') {  /* DECSTR */
            reset(t, 0);
        } else if (t->inter == '$' && t->priv == '?') {  /* DECRQM */
            m = modebit(t, a[0], &bit);
            i = !m ? 0 : (*m & bit) ? 1 : 2;
            if (m && a[0] == 25)
                i = 3 - i;
            REPLY(t, "\033[?%d;%d$y", a[0], i);
        }
        break;
    case 'q':
        if (t->inter == ' ')  /* DECSCUSR */
            t->cstyle = MIN(a[0], 6);
        else if (t->priv == '>')  /* XTVERSION */
            REPLY(t, "\033P>|imperaterm " VERSION "\033\\");
        break;
    case 'r':  /* DECSTBM */
        i = DEF(a[1], t->rows);
        if (n < i && i <= t->rows) {
            t->top = n - 1, t->bot = i - 1;
            moveato(t, 0, 0);
        }
        break;
    case 's': cursor(t, 1); break;
    case 'u': cursor(t, 0); break;
    case 't':
        if (a[0] == 18)
            REPLY(t, "\033[8;%d;%dt", t->rows, t->cols);
        break;
    }
}

static uint32_t
colourparse(const char *s)
{
    unsigned r, g, b;

    if (sscanf(s, "rgb:%2x%*[^/]/%2x%*[^/]/%2x", &r, &g, &b) == 3 ||
        sscanf(s, "rgb:%2x/%2x/%2x", &r, &g, &b) == 3 || sscanf(s, "#%2x%2x%2x", &r, &g, &b) == 3)
        return TRUECOL | r << 16 | g << 8 | b;
    return 0;
}

/* Colour i of the palette is set, reset or asked for. */
static void
colour(Term *t, int osc, int i, const char *spec)
{
    uint32_t c = t->pal[i], v;
    char idx[16] = "";

    if (osc == 4)
        snprintf(idx, sizeof idx, ";%d", i);
    if (!spec)
        t->pal[i] = t->defpal[i];
    else if (*spec == '?')
        REPLY(t, "\033]%d%s;rgb:%02x%02x/%02x%02x/%02x%02x%s", osc, idx, c >> 16 & 255, c >> 16 & 255,
              c >> 8 & 255, c >> 8 & 255, c & 255, c & 255, t->sbel ? "\a" : "\033\\");
    else if ((v = colourparse(spec)))
        t->pal[i] = v & 0xffffff;
    t->alldirty = 1;
}

static void
strhandle(Term *t)
{
    char *p = t->str, *q;
    int n, i;

    t->str[t->slen] = 0;
    if (t->stype != ']')
        return;
    n = strtol(p, &p, 10);
    p += *p == ';';
    if (n == 0 || n == 2) {
        if (t->title)
            t->title(t, p);
    } else if (n == 7 || n == 1337) {
        /*
         * OSC 7 has a file: URI, the host and the directory the shell is
         * in; OSC 1337, which is iTerm2's, has RemoteHost=user@host among
         * much that is not for here. One that did not fit is not half
         * believed.
         */
        if (!t->told || t->slen >= (int)sizeof t->str - 1)
            return;
        if (n == 7)
            t->told(t, TOLD_WHERE, p);
        else if (!strncmp(p, "RemoteHost=", 11))
            t->told(t, TOLD_WHO, p + 11);
    } else if (n == 4 || n == 104) {
        if (n == 104 && !*p)
            for (i = 0; i < 256; i++)
                t->pal[i] = t->defpal[i];
        while (*p) {
            i = strtol(p, &q, 10) & 255;
            if (q == p)
                break;
            p = q + (*q == ';');
            if (n == 4) {
                if ((q = strchr(p, ';')))
                    *q++ = 0;
                colour(t, 4, i, p);
                p = q ? q : "";
            } else
                colour(t, 104, i, NULL);
        }
        t->alldirty = 1;
    } else if (n >= 10 && n <= 12) {
        colour(t, n, C_FG + n - 10, p);
    } else if (n >= 110 && n <= 112) {
        colour(t, n, C_FG + n - 110, NULL);
    }
}

/* What follows an ESC; 0 while the sequence goes on. */
static int
eschandle(Term *t, int c)
{
    switch (c) {
    case '[':
        t->esc |= E_CSI;
        return 0;
    case '#':
        t->esc |= E_TEST;
        return 0;
    case '%':
        t->esc |= E_UTF8;
        return 0;
    case 'P': case '_': case '^': case ']': case 'k': case 'X':
        t->stype = c, t->slen = 0;
        t->esc = E_STR;
        return 0;
    case '(': case ')': case '*': case '+':
        t->priv = c - '(';
        t->esc |= E_CHARSET;
        return 0;
    case 'n': case 'o':
        t->c.charset = 2 + (c - 'n');
        break;
    case 'D':  /* IND */
        if (t->c.y == t->bot)
            scrollup(t, t->top, 1, 1);
        else
            moveto(t, t->c.x, t->c.y + 1);
        break;
    case 'E':  /* NEL */
        newline(t, 1);
        break;
    case 'H':  /* HTS */
        t->tabs[t->c.x] = 1;
        break;
    case 'M':  /* RI */
        if (t->c.y == t->top)
            scrolldown(t, t->top, 1);
        else
            moveto(t, t->c.x, t->c.y - 1);
        break;
    case 'Z':  /* DECID */
        REPLY(t, "\033[?62;22c");
        break;
    case 'c':  /* RIS */
        vt_reset(t);
        if (t->title)
            t->title(t, "");
        break;
    case '=': t->mode |= M_APPKEYPAD; break;
    case '>': t->mode &= ~M_APPKEYPAD; break;
    case '7': cursor(t, 1); break;
    case '8': cursor(t, 0); break;
    }
    return 1;
}

static void
control(Term *t, int c)
{
    switch (c) {
    case '\t': puttab(t, 1); break;
    case '\b': moveto(t, t->c.x - 1, t->c.y); break;
    case '\r': moveto(t, 0, t->c.y); break;
    case '\n': case '\v': case '\f': newline(t, t->mode & M_CRLF); break;
    case '\a':
        if (t->bell)
            t->bell(t);
        break;
    case 033: t->csilen = 0, t->esc = E_START; break;
    case 016: case 017: t->c.charset = c == 016; break;
    case 030: case 032: t->esc = 0; break;
    }
}

/*
 * How many of the n bytes at p are printable ASCII. Eight at a time: a
 * byte below 0x20 or above 0x7e sets its top bit in one of the two words,
 * and the bytes before it are counted one by one.
 */
static size_t
printable(const uint8_t *p, size_t n)
{
    const uint64_t ones = 0x0101010101010101u, tops = 0x8080808080808080u;
    uint64_t v;
    size_t i = 0;

    for (; i + 8 <= n; i += 8) {
        memcpy(&v, p + i, 8);
        if (((v - ones * 0x20) & ~v & tops) | (((v + ones) | v) & tops))
            break;
    }
    while (i < n && p[i] - 0x20u < 0x5f)
        i++;
    return i;
}

/* The cells a run of bytes goes into, which are not the bytes: the compiler need not read them again. */
static void
fill(Cell *restrict g, const uint8_t *restrict p, size_t k, uint32_t a, uint32_t fg, uint32_t bg)
{
    size_t i;

    for (i = 0; i < k; i++)
        g[i].u = p[i] | a, g[i].fg = fg, g[i].bg = bg;
}

/*
 * A run of plain ASCII goes straight into its row, up to the last column,
 * which put() has for what wrapping needs. Only a wide character at either
 * end of the run is left with half of itself to clear.
 */
static const uint8_t *
ascii(Term *t, const uint8_t *p, const uint8_t *e)
{
    int x = t->c.x, room = t->cols - 1 - x;
    uint32_t a = t->c.u, fg = t->c.fg, bg = t->c.bg;
    size_t k;
    Cell *g;

    if (t->c.wrapnext || room <= 0 || (t->mode & M_INSERT) || t->c.tran[t->c.charset]) {
        put(t, *p);
        return p + 1;
    }
    k = printable(p, MIN((size_t)(e - p), (size_t)room));
    if (SEL(t) && t->sel.alt == (int)ALT(t) && t->c.y >= t->sel.nb.y && t->c.y <= t->sel.ne.y)
        vt_selclear(t);
    g = row(t, t->c.y) + x;
    if ((g->u & A_WDUMMY) && x > 0 && (g[-1].u & A_WIDE))
        g[-1].u = 0;
    if ((g[k - 1].u & A_WIDE) && (g[k].u & A_WDUMMY))
        g[k].u = 0;
    fill(g, p, k, a, fg, bg);
    t->c.x = x + k;
    t->lastc = p[k - 1];
    t->dirty[t->c.y] = 1;
    return p + k;
}

/*
 * A character in UTF-8 that is all there, decoded as the byte by byte way
 * would: 0 where that way is to have it, as it is for what is not a drawn
 * character.
 */
static int
utf8char(const uint8_t *p, const uint8_t *e, uint32_t *u)
{
    int n = p[0] >= 0xf0 ? 4 : p[0] >= 0xe0 ? 3 : 2, i;
    uint32_t c = p[0] & (0x3f >> (n - 1));

    if (p[0] < 0xc2 || p[0] >= 0xf5 || e - p < n)
        return 0;
    for (i = 1; i < n; i++) {
        if ((p[i] & 0xc0) != 0x80)
            return 0;
        c = c << 6 | (p[i] & 0x3f);
    }
    if (c < 0xa0)
        return 0;
    *u = c;
    return n;
}

/* An escape sequence that is all there and only what one can be: taken whole. */
static int
csiwhole(Term *t, const uint8_t *p, const uint8_t *e)
{
    const uint8_t *q = p + 2, *end = MIN(e, p + 2 + sizeof t->csi - 1);
    size_t n;

    while (q < end && *q - 0x20u < 0x20)
        q++;
    if (q == end || *q - 0x40u > 0x3e)
        return 0;
    n = q + 1 - (p + 2);
    memcpy(t->csi, p + 2, n);
    t->csi[n] = 0;
    t->csilen = n;
    csiparse(t);
    csihandle(t);
    return n + 2;
}

void
vt_write(Term *t, const char *s, size_t n)
{
    const uint8_t *p = (const uint8_t *)s, *e = p + n, *q;
    uint32_t c;
    size_t k;
    int x, y;

    while (p < e) {
        c = *p;
        if (!t->esc && !t->uneed) {
            if (c - 0x20 < 0x5f) {
                p = ascii(t, p, e);
                continue;
            }
            if (c >= 0xc2 && (k = utf8char(p, e, &c))) {
                put(t, c);
                p += k;
                continue;
            }
            if (c == 033 && e - p > 2 && p[1] == '[' && (k = csiwhole(t, p, e))) {
                p += k;
                continue;
            }
        }
        if (t->esc & E_STR) {  /* a string runs to BEL or ST, and what is too long for it is lost */
            for (q = p; q < e && *q != '\a' && *q != 033 && *q != 030 && *q != 032; q++);
            k = MIN((size_t)(q - p), sizeof t->str - 1 - t->slen);
            memcpy(t->str + t->slen, p, k);
            t->slen += k;
            if ((p = q) == e)
                break;
            c = *p;
        }
        p++;
        if (t->esc & E_STR) {
            if (c == '\a' || c == 033) {
                t->sbel = c == '\a';
                t->esc = c == 033 ? E_START : 0;
                strhandle(t);
            } else if (c == 030 || c == 032)
                t->esc = 0;
            else if (t->slen < (int)sizeof t->str - 1)
                t->str[t->slen++] = c;
            continue;
        }
        if (t->uneed) {  /* UTF-8, which may come in pieces */
            if ((c & 0xc0) != 0x80) {
                t->uneed = 0;
                p--;
                c = 0xfffd;
            } else {
                t->ucp = t->ucp << 6 | (c & 0x3f);
                if (--t->uneed)
                    continue;
                c = t->ucp;
            }
        } else if (c >= 0x80) {
            t->uneed = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
            t->ucp = c & (0x3f >> t->uneed);
            if (c >= 0xc2 && c < 0xf5)
                continue;
            t->uneed = 0;
            c = 0xfffd;
        }
        if (c < 0x20 || c == 0x7f) {
            control(t, c);
        } else if (c >= 0x80 && c < 0xa0) {
            /* the C1 controls are not acted on */
        } else if (!(t->esc & E_START)) {
            put(t, c);
        } else if (t->esc & E_CSI) {
            t->csi[t->csilen++] = c < 0x80 ? c : '?';
            if ((c >= 0x40 && c <= 0x7e) || t->csilen >= (int)sizeof t->csi - 1) {
                t->csi[t->csilen] = 0;
                t->esc = 0;
                csiparse(t);
                csihandle(t);
            }
        } else if (t->esc & E_CHARSET) {
            t->c.tran[(int)t->priv] = c == '0';
            t->esc = 0;
        } else if (t->esc & E_TEST) {
            if (c == '8')  /* DECALN */
                for (y = 0; y < t->rows; y++)
                    for (x = 0, t->dirty[y] = 1; x < t->cols; x++)
                        row(t, y)[x] = (Cell){ 'E', 0, 0 };
            t->esc = 0;
        } else if (t->esc & E_UTF8) {
            t->esc = 0;
        } else if (eschandle(t, c)) {
            t->esc = 0;
        }
    }
}

/* Input. */

void
vt_paste(Term *t, const char *s, size_t n)
{
    char *b = malloc(n + 1), *p = b;
    size_t i;

    if (!b)
        return;
    for (i = 0; i < n; i++)  /* no escape sequence gets in this way */
        if (s[i] == '\n')
            *p++ = '\r';
        else if ((uint8_t)s[i] >= 0x20 || s[i] == '\t' || s[i] == '\r')
            *p++ = s[i];
    if (t->mode & M_BRACKET)
        t->write(t, "\033[200~", 6);
    t->write(t, b, p - b);
    if (t->mode & M_BRACKET)
        t->write(t, "\033[201~", 6);
    free(b);
}

/*
 * A button (1 to 3, 4 to 7 for the wheel, 0 for none) pressed (type 0),
 * released (1) or moved with (2) over a cell; mods has 4 for shift, 8 for
 * alt and 16 for control. 0 if the child does not want to know.
 */
int
vt_mouse(Term *t, int x, int y, int btn, int type, int mods)
{
    int code;

    if (!(t->mode & M_MOUSE))
        return 0;
    x = CLAMP(x, 0, t->cols - 1), y = CLAMP(y, 0, t->rows - 1);
    if (type == 2) {
        if (!(t->mode & (M_MOUSEMOTION | M_MOUSEMANY)) || (!btn && !(t->mode & M_MOUSEMANY)))
            return 1;
        code = 32 + (btn ? btn - 1 : 3);
    } else {
        if (type == 1 && ((t->mode & M_MOUSEX10) || btn > 3))
            return 1;
        code = btn > 3 ? 64 + btn - 4 : btn - 1;
        if (type == 1 && !(t->mode & M_MOUSESGR))
            code = 3;
    }
    if (!(t->mode & M_MOUSEX10))
        code += mods;
    if (t->mode & M_MOUSESGR)
        REPLY(t, "\033[<%d;%d;%d%c", code, x + 1, y + 1, type == 1 ? 'm' : 'M');
    else if (x < 223 && y < 223)
        REPLY(t, "\033[M%c%c%c", 32 + code, 32 + x + 1, 32 + y + 1);
    return 1;
}

/* Sizes. */

/* What of a row is worth keeping when the lines are wrapped anew. */
static int
keeplen(Term *t, const Cell *l)
{
    int i = t->cols;

    if (l[i - 1].u & A_WRAP)
        return i;
    while (i > 0 && !l[i - 1].u && !l[i - 1].bg)
        i--;
    return i;
}

void
vt_resize(Term *t, int cols, int rows)
{
    int ocols = t->cols, orows = t->rows, alt = ALT(t), cap, x, y, len, wrapped, last, live;
    int ny = 0, nx = 0, cy = -1, cx = 0;
    Cursor *c = alt ? &t->saved[0] : &t->c;
    Cell *nb, *na, *l, *d, g, **nl, **nal, **ns;
    uint8_t *nd, *nt;

    cols = MAX(cols, 2), rows = MAX(rows, 1);
    if (cols == ocols && rows == orows)
        return;
    cap = rows + t->histmax;
    nb = calloc((size_t)cap * cols, sizeof *nb);
    na = calloc((size_t)rows * cols, sizeof *na);
    nd = malloc(rows), nt = malloc(cols);
    nl = malloc(cap * sizeof *nl), nal = malloc(rows * sizeof *nal), ns = malloc(rows * sizeof *ns);
    if (!nb || !na || !nd || !nt || !nl || !nal || !ns) {  /* and the terminal is as it was */
        free(nb), free(na), free(nd), free(nt), free(nl), free(nal), free(ns);
        return;
    }
    memset(nd, 1, rows);
    for (y = 0; y < cap; y++)
        nl[y] = nb + (size_t)y * cols;
    for (y = 0; y < rows; y++)
        nal[y] = na + (size_t)y * cols;
    for (y = 0; y < MIN(rows, orows); y++)  /* the alternate screen is cut to fit */
        memcpy(nal[y], t->altline[y], MIN(cols, ocols) * sizeof *na);

    /*
     * The scrollback and the screen are one text, wrapped at the new width,
     * but for the line the cursor is on. A shell redraws that line itself
     * when the size changes, and it counts its rows as they were: wrapped
     * anew under it, the start of the prompt would be left behind above
     * the redrawn one. So its rows stay rows, cut or filled out to the new
     * width.
     */
    t->mode &= ~M_ALT;
    for (last = orows - 1; last > c->y && !keeplen(t, row(t, last)); last--);
    for (live = c->y; live > -t->hist && (row(t, live - 1)[ocols - 1].u & A_WRAP); live--);
    d = nb;
    for (y = -t->hist; y <= last; y++) {
        l = row(t, y);
        if (y >= live) {
            len = MIN(ocols, cols);
            memcpy(d, l, len * sizeof *d);
            for (x = 0; x < len; x++)
                d[x].u &= ~A_WRAP;
            if (d[len - 1].u & A_WIDE)  /* half a wide character is none */
                memset(&d[len - 1], 0, sizeof *d);
            if (y == c->y)
                cy = ny, cx = MIN(c->x, cols - 1);
            if (y < last) {
                d = nb + (size_t)(++ny % cap) * cols;
                memset(d, 0, cols * sizeof *d);
            }
            continue;
        }
        wrapped = l[ocols - 1].u & A_WRAP;
        len = keeplen(t, l);
        if (y == c->y)
            len = MAX(len, MIN(c->x, ocols - 1));
        for (x = 0; x < len; x++) {
            g = l[x];
            g.u &= ~A_WRAP;
            if (nx >= cols || ((g.u & A_WIDE) && nx + 2 > cols)) {
                d[cols - 1].u |= A_WRAP;
                d = nb + (size_t)(++ny % cap) * cols;
                memset(d, 0, cols * sizeof *d);
                nx = 0;
            }
            if (y == c->y && x == c->x)
                cy = ny, cx = nx;
            d[nx++] = g;
        }
        if (y == c->y && cy < 0)
            cy = ny, cx = MIN(nx, cols - 1);
        if (!wrapped && y < last) {
            d = nb + (size_t)(++ny % cap) * cols;
            memset(d, 0, cols * sizeof *d);
            nx = 0;
        }
    }
    free(t->buf), free(t->alt), free(t->dirty), free(t->tabs), free(t->line), free(t->altline), free(t->spare);
    t->buf = nb, t->alt = na, t->dirty = nd, t->tabs = nt, t->line = nl, t->altline = nal, t->spare = ns;
    t->cols = cols, t->rows = rows, t->cap = cap;
    t->hist = CLAMP(ny + 1 - rows, 0, t->histmax);
    t->base = ny + 1 > rows ? (ny + 1 - rows) % cap : 0;
    c->x = cx, c->y = CLAMP(cy - MAX(ny + 1 - rows, 0), 0, rows - 1);
    c->wrapnext = 0;

    t->mode |= alt;
    for (x = 0; x < cols; x++)
        t->tabs[x] = x && x % 8 == 0;
    t->top = 0, t->bot = rows - 1, t->scr = 0, t->alldirty = 1;
    t->sel.ob.x = -1;
    t->c.x = MIN(t->c.x, cols - 1), t->c.y = MIN(t->c.y, rows - 1);
    t->saved[1].x = MIN(t->saved[1].x, cols - 1), t->saved[1].y = MIN(t->saved[1].y, rows - 1);
}

Term *
vt_new(int cols, int rows, int hist, const uint32_t *pal)
{
    Term *t = calloc(1, sizeof *t);
    int i;

    if (!t)
        return NULL;
    t->cols = MAX(cols, 2), t->rows = MAX(rows, 1);
    t->histmax = MAX(hist, 0), t->cap = t->rows + t->histmax;
    t->buf = calloc((size_t)t->cap * t->cols, sizeof(Cell));
    t->alt = calloc((size_t)t->rows * t->cols, sizeof(Cell));
    t->dirty = malloc(t->rows);
    t->tabs = malloc(t->cols);
    t->line = malloc(t->cap * sizeof *t->line);
    t->altline = malloc(t->rows * sizeof *t->altline);
    t->spare = malloc(t->rows * sizeof *t->spare);
    t->defpal = pal;
    t->sel.ob.x = -1;
    if (!t->buf || !t->alt || !t->dirty || !t->tabs || !t->line || !t->altline || !t->spare)
        return vt_free(t), NULL;
    for (i = 0; i < t->cap; i++)
        t->line[i] = t->buf + (size_t)i * t->cols;
    for (i = 0; i < t->rows; i++)
        t->altline[i] = t->alt + (size_t)i * t->cols;
    vt_reset(t);
    return t;
}

void
vt_free(Term *t)
{
    free(t->buf), free(t->alt), free(t->dirty), free(t->tabs);
    free(t->line), free(t->altline), free(t->spare), free(t);
}
