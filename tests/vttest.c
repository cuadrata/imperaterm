/* See LICENSE for licence details. */
/* The terminal without a window: sequences in, cells and replies out. */
#define _XOPEN_SOURCE 700
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../src/vt.h"

static uint32_t pal[C_N];
static char reply[4096], title[256];
static int fails, checks, bells;

static void wr(Term *t, const char *s, size_t n) { (void)t; strncat(reply, s, n); }
static void ti(Term *t, const char *s) { (void)t; snprintf(title, sizeof title, "%s", s); }
static void be(Term *t) { (void)t; bells++; }
static char said[2][256];
static void to(Term *t, int what, const char *s) { (void)t; snprintf(said[what], sizeof said[what], "%s", s); }

static Term *
mk(int cols, int rows, int hist)
{
    Term *t = vt_new(cols, rows, hist, pal);

    t->write = wr, t->title = ti, t->bell = be, t->told = to;
    reply[0] = title[0] = said[0][0] = said[1][0] = 0;
    return t;
}

static void feed(Term *t, const char *s) { vt_write(t, s, strlen(s)); }

/* A row of the view as text, without what trails it. */
static const char *
text(Term *t, int y)
{
    static char b[4096];
    const Cell *l = vt_row(t, y);
    char *p = b;
    int x, n = t->cols;

    while (n > 0 && !RUNE(l[n - 1].u))
        n--;
    for (x = 0; x < n; x++)
        if (!(l[x].u & A_WDUMMY))
            p += vt_utf8(RUNE(l[x].u) ? RUNE(l[x].u) : ' ', p);
    *p = 0;
    return b;
}

#define IS(what, got, want) do { const char *g_ = (got), *w_ = (want); checks++; \
    if (strcmp(g_, w_)) fails++, printf("FAIL %s:%d %s: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, what, g_, w_); } while (0)
#define EQ(what, got, want) do { long g_ = (got), w_ = (want); checks++; \
    if (g_ != w_) fails++, printf("FAIL %s:%d %s: got %ld, want %ld\n", __FILE__, __LINE__, what, g_, w_); } while (0)

int
main(void)
{
    Term *t;
    char *s, buf[64];
    int i, x1, x2;

    if (!setlocale(LC_CTYPE, "C.UTF-8") && !setlocale(LC_CTYPE, "en_US.UTF-8"))
        setlocale(LC_CTYPE, "");
    for (i = 0; i < C_N; i++)
        pal[i] = i * 0x010203;
    pal[C_BG] = 0x112233;

    /* Text, wrapping and the cursor. */
    t = mk(10, 4, 100);
    feed(t, "hello\r\nworld");
    IS("line 0", text(t, 0), "hello");
    IS("line 1", text(t, 1), "world");
    EQ("cursor x", t->c.x, 5); EQ("cursor y", t->c.y, 1);
    feed(t, "\033[2J\033[H0123456789ab");
    IS("wrapped 0", text(t, 0), "0123456789");
    IS("wrapped 1", text(t, 1), "ab");
    EQ("wrap flag", !!(vt_row(t, 0)[9].u & A_WRAP), 1);
    feed(t, "\033[2;1Hxy\033[1D\033[K");
    IS("EL", text(t, 1), "x");
    feed(t, "\033[H\033[2Kabcdef\033[3G\033[2P");
    IS("DCH", text(t, 0), "abef");
    feed(t, "\033[3G\033[2@");
    IS("ICH", text(t, 0), "ab  ef");
    feed(t, "\033[1;1HZ\033[5b");
    IS("REP", text(t, 0), "ZZZZZZ");
    feed(t, "\033[2J\033[H\tx\033[2Zy");
    IS("tabs", text(t, 0), "y       x");
    vt_free(t);

    /* Scrolling into the scrollback, and looking back. */
    t = mk(20, 3, 100);
    for (i = 1; i <= 10; i++) {
        snprintf(buf, sizeof buf, "%sline %d", i > 1 ? "\r\n" : "", i);
        feed(t, buf);
    }
    IS("bottom", text(t, 2), "line 10");
    IS("top", text(t, 0), "line 8");
    EQ("history", t->hist, 7);
    t->scr = 7;
    IS("oldest", text(t, 0), "line 1");
    feed(t, "\r\nline 11");
    EQ("view stays", t->scr, 8);
    IS("view text", text(t, 0), "line 1");
    t->scr = 0;
    feed(t, "\033[3J");
    EQ("ED 3", t->hist, 0);
    vt_free(t);

    /* A scroll region with a line held below it, as tmux does. */
    t = mk(20, 4, 100);
    feed(t, "\033[4;1Hstatus\033[1;3r\033[1;1Ha\r\nb\r\nc\r\nd\r\ne");
    IS("region top", text(t, 0), "c");
    IS("region bottom", text(t, 2), "e");
    IS("held line", text(t, 3), "status");
    EQ("region history", t->hist, 2);
    feed(t, "\033[1;1H\033M");
    IS("RI", text(t, 1), "c");
    IS("RI blank", text(t, 0), "");
    feed(t, "\033[r\033[2;1H\033[1L");
    IS("IL", text(t, 2), "c");
    feed(t, "\033[1M");
    IS("DL", text(t, 1), "c");
    vt_free(t);

    /* Colours and attributes. */
    t = mk(20, 2, 0);
    feed(t, "\033[1;4;31;42mA\033[0mB\033[38;5;200mC\033[38;2;1;2;3mD\033[48:2::4:5:6mE\033[38:2:7:8:9mF\033[4:3mG\033[4:0mH");
    EQ("bold", !!(vt_row(t, 0)[0].u & A_BOLD), 1);
    EQ("underline", !!(vt_row(t, 0)[0].u & A_UNDER), 1);
    EQ("fg 31", vt_row(t, 0)[0].fg, 2); EQ("bg 42", vt_row(t, 0)[0].bg, 3);
    EQ("reset", vt_row(t, 0)[1].u, 'B'); EQ("reset fg", vt_row(t, 0)[1].fg, 0);
    EQ("256", vt_row(t, 0)[2].fg, 201);
    EQ("rgb", vt_row(t, 0)[3].fg, TRUECOL | 0x010203);
    EQ("rgb colons", vt_row(t, 0)[4].bg, TRUECOL | 0x040506);
    EQ("rgb short colons", vt_row(t, 0)[5].fg, TRUECOL | 0x070809);
    EQ("curly", !!(vt_row(t, 0)[6].u & A_UNDER), 1);
    EQ("4:0", !!(vt_row(t, 0)[7].u & A_UNDER), 0);
    feed(t, "\033[0m\033[97;100mI");
    EQ("bright fg", vt_row(t, 0)[8].fg, 16); EQ("bright bg", vt_row(t, 0)[8].bg, 9);
    vt_free(t);

    /* UTF-8 in pieces, line drawing, and wide characters where the C library knows them. */
    t = mk(6, 3, 10);
    vt_write(t, "\xc3", 1); vt_write(t, "\xa9\xe4\xb8", 3); vt_write(t, "\xad!", 2);
    IS("utf-8", text(t, 0), "\xc3\xa9\xe4\xb8\xad!");
    feed(t, "\r\n\033(0lqk\033(Bq\xff.");
    IS("graphics", text(t, 1), "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90q\xef\xbf\xbd.");
    if (wcwidth(0x4e2d) == 2) {
        EQ("wide", !!(vt_row(t, 0)[1].u & A_WIDE), 1);
        EQ("dummy", !!(vt_row(t, 0)[2].u & A_WDUMMY), 1);
        feed(t, "\033[1;3Hx");
        EQ("half a wide one", RUNE(vt_row(t, 0)[1].u), 0);
        feed(t, "\033[3;1H12345\xe4\xb8\xad");
        IS("wide at the edge", text(t, 2), "\xe4\xb8\xad");
    } else
        printf("no UTF-8 locale here: wide characters not tested\n");
    vt_free(t);

    /* The alternate screen keeps the other one, and the cursor where it was. */
    t = mk(10, 3, 10);
    feed(t, "main\033[?1049halt");
    IS("alt", text(t, 0), "    alt");
    feed(t, "\033[?1049l");
    IS("back", text(t, 0), "main");
    EQ("cursor back", t->c.x, 4);
    vt_free(t);

    /* What is asked is answered. */
    t = mk(10, 3, 0);
    feed(t, "\033[2;3H\033[6n");
    IS("CPR", reply, "\033[2;3R"); reply[0] = 0;
    feed(t, "\033[c");
    IS("DA", reply, "\033[?62;22c"); reply[0] = 0;
    feed(t, "\033[?2004h\033[?2004$p\033[?25$p\033[?9999$p");
    IS("DECRQM", reply, "\033[?2004;1$y\033[?25;1$y\033[?9999;0$y"); reply[0] = 0;
    feed(t, "\033[?2026h\033[?2026$p");
    IS("synchronized output, begun", reply, "\033[?2026;1$y"); reply[0] = 0;
    EQ("and not yet whole", t->whole, 0);
    feed(t, "\033[?2026l\033[?2026$p");
    IS("and ended", reply, "\033[?2026;2$y"); reply[0] = 0;
    EQ("when what changed is whole", t->whole, 1);
    t->whole = 0;
    feed(t, "\033]11;?\033\\");
    IS("OSC 11", reply, "\033]11;rgb:1111/2222/3333\033\\"); reply[0] = 0;
    feed(t, "\033]4;1;#ff8000\a\033]4;1;?\a");
    IS("OSC 4", reply, "\033]4;1;rgb:ffff/8080/0000\a"); reply[0] = 0;
    feed(t, "\033]104\a\033]0;a title\a\033]2;caf\xc3\xa9\033\\\033[?u\033[>4;2m\033P$q q\033\\x");
    EQ("OSC 104", t->pal[1], pal[1]);
    IS("title", title, "caf\xc3\xa9");
    IS("unknown ones are quiet", reply, "");
    IS("and eaten", text(t, 1), "  x");
    vt_paste(t, "a\nb\033[201~", 10);
    IS("paste", reply, "\033[200~a\rb[201~\033[201~"); reply[0] = 0;
    feed(t, "\033[?1000;1006h");
    vt_mouse(t, 2, 1, 1, 0, 0); vt_mouse(t, 2, 1, 1, 1, 0); vt_mouse(t, 0, 0, 4, 0, 16);
    IS("mouse", reply, "\033[<0;3;2M\033[<0;3;2m\033[<80;1;1M");
    feed(t, "\a");
    EQ("bell", bells, 1);
    vt_free(t);

    /* Lines are wrapped anew at another width, and the cursor follows. */
    t = mk(10, 4, 100);
    feed(t, "0123456789abcdefghij\r\nxy\r\n$ ");
    vt_resize(t, 5, 4);
    IS("narrow 0", text(t, 0), "abcde");
    IS("narrow 3", text(t, 3), "$ ");
    EQ("narrow history", t->hist, 2);
    EQ("narrow cursor x", t->c.x, 2); EQ("narrow cursor y", t->c.y, 3);
    t->scr = 2;
    IS("narrow oldest", text(t, 0), "01234");
    vt_resize(t, 20, 4);
    IS("wide 0", text(t, 0), "0123456789abcdefghij");
    IS("wide 1", text(t, 1), "xy");
    EQ("wide history", t->hist, 0);
    EQ("wide cursor", t->c.y * 100 + t->c.x, 202);
    vt_resize(t, 20, 2);
    IS("short 0", text(t, 0), "xy");
    EQ("short history", t->hist, 1);
    vt_resize(t, 20, 6);
    IS("tall 0", text(t, 0), "0123456789abcdefghij");
    EQ("tall cursor", t->c.y, 2);
    vt_free(t);

    /*
     * The line the cursor is on keeps its rows, cut to the width, for the
     * shell redraws it: as bash does, back to its first row and over it.
     */
    t = mk(12, 4, 100);
    feed(t, "out\r\nhost:/a/b/c$ ");
    EQ("prompt wraps", t->c.y * 100 + t->c.x, 201);
    vt_resize(t, 6, 4);
    IS("history is wrapped", text(t, 0), "out");
    IS("the cursor's line is cut", text(t, 1), "host:/");
    IS("and not wrapped", text(t, 2), " ");
    EQ("cut cursor", t->c.y * 100 + t->c.x, 201);
    feed(t, "\r\033[K\033[1Ahost:/a/b/c$ ");
    IS("redrawn 1", text(t, 0), "out");
    IS("redrawn 2", text(t, 1), "host:/");
    IS("redrawn 3", text(t, 2), "a/b/c$");
    vt_resize(t, 20, 4);
    IS("filled out 1", text(t, 1), "host:/");
    IS("filled out 2", text(t, 2), "a/b/c$");
    feed(t, "\r\033[K\033[2Ahost:/a/b/c$ \033[J");
    IS("redrawn wide 1", text(t, 1), "host:/a/b/c$ ");
    IS("nothing left behind", text(t, 2), "");
    vt_free(t);

    /* Selecting: by character, by word across a wrap, by line, and a block. */
    t = mk(10, 4, 100);
    feed(t, "one two\r\nfoo-bar baz_quux\r\nlast");
    vt_selstart(t, 4, 0, SNAP_NONE); vt_selextend(t, 2, 1, 0);
    s = vt_seltext(t); IS("characters", s, "two\nfoo"); free(s);
    EQ("range", vt_selrange(t, 1, &x1, &x2) * 100 + x1 * 10 + x2, 102);
    vt_selstart(t, 9, 1, SNAP_WORD);
    s = vt_seltext(t); IS("word across a wrap", s, "baz_quux"); free(s);
    vt_selstart(t, 3, 2, SNAP_LINE);
    s = vt_seltext(t); IS("line", s, "foo-bar baz_quux\n"); free(s);
    vt_selstart(t, 1, 0, SNAP_NONE); vt_selextend(t, 2, 1, 1);
    s = vt_seltext(t); IS("block", s, "ne\noo"); free(s);
    feed(t, "\r\nmore\r\nmore");
    s = vt_seltext(t); IS("scrolled with the text", s, "ne\noo"); free(s);
    vt_selstart(t, 0, 3, SNAP_NONE);
    EQ("a click selects nothing", vt_seltext(t) == NULL, 1);
    vt_free(t);

    /* What is selected goes when its first row leaves the scrollback: there is no such row to read. */
    t = mk(10, 4, 3);
    feed(t, "a\r\nb\r\nc\r\nd\r\ne\r\nf\r\ng");
    t->scr = 3;
    vt_selstart(t, 0, 0, SNAP_NONE);
    t->scr = 0;
    vt_selextend(t, 0, 3, 0);
    s = vt_seltext(t); IS("from the first row kept to the last", s, "a\nb\nc\nd\ne\nf\ng"); free(s);
    feed(t, "\r\nh");
    EQ("gone with its first row", vt_seltext(t) == NULL, 1);
    for (i = 0; i < 20; i++)
        feed(t, "\r\nmore");
    vt_selextend(t, 5, 2, 0);
    EQ("and not to be taken up again", vt_seltext(t) == NULL, 1);
    vt_free(t);

    /* What a shell says of who is at the terminal and where is passed on, whole or not at all. */
    t = mk(20, 4, 0);
    feed(t, "\033]7;file://box/tmp/a%20b\033\\");
    IS("where, ended by ST", said[TOLD_WHERE], "file://box/tmp/a%20b");
    feed(t, "\033]1337;RemoteHost=root@box\007");
    IS("who, ended by BEL", said[TOLD_WHO], "root@box");
    feed(t, "\033]1337;CurrentDir=/x\007\033]1337;SetMark\007");
    IS("the rest of 1337 is not for here", said[TOLD_WHO], "root@box");
    said[TOLD_WHERE][0] = 0;
    feed(t, "\033]7;file://box/");
    for (i = 0; i < 300; i++)
        feed(t, "0123456789");
    feed(t, "\007");
    IS("one too long to keep is not half believed", said[TOLD_WHERE], "");
    feed(t, "\033]7;file:///home\007ok");
    IS("and the next is heard", said[TOLD_WHERE], "file:///home");
    IS("with nothing of them shown", text(t, 0), "ok");
    vt_free(t);

    /* Rows that scroll change places; their cells are moved by none of it. */
    t = mk(10, 5, 3);
    feed(t, "a\r\nb\r\nc\r\nd\r\ne\033[2;4r\033[4;1H\n\n");
    IS("a region scrolled up: what is above it stays", text(t, 0), "a");
    IS("and its rows go up", text(t, 1), "d");
    IS("the rows it lost are clear", text(t, 2), "");
    IS("and what is below it stays", text(t, 4), "e");
    EQ("none of it went into the scrollback", t->hist, 0);
    feed(t, "\033[2;1H\033M\033M");
    IS("scrolled down", text(t, 3), "d");
    IS("from its top", text(t, 1), "");
    feed(t, "\033[r\033[?1049h\033[H1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7");
    IS("the alternate screen scrolls too", text(t, 0), "3");
    IS("to its last row", text(t, 4), "7");
    feed(t, "\033[?1049l");
    IS("and the main one is as it was", text(t, 4), "e");
    vt_free(t);

    t = mk(10, 4, 0);
    feed(t, "1\r\n2\r\n3\r\n4\r\n5\033[1;3r\033[3;1H\n");
    IS("with no scrollback, a full screen scroll", text(t, 0), "3");
    IS("leaves the row below the region alone", text(t, 3), "5");
    vt_free(t);

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
