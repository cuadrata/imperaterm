/* See LICENSE for licence details. */
/* How fast the terminal takes text: bench [megabytes]. */
#define _POSIX_C_SOURCE 200809L
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/vt.h"

static uint32_t pal[C_N];
static void wr(Term *t, const char *s, size_t n) { (void)t, (void)s, (void)n; }

static void
run(const char *name, const char *unit, size_t mb)
{
    const char *setup = strstr(name, "tmux") ? "\033[?1049h\033[1;39r\033[39;1H" : strstr(name, "less") ? "\033[?1049h" : "";
    size_t ul = strlen(unit), n = 0, sz = 1 << 16, done = 0, total = mb << 20;
    char *buf = malloc(sz);
    Term *t = vt_new(120, 40, 10000, pal);
    struct timespec a, b;
    double s;

    t->write = wr;
    vt_write(t, setup, strlen(setup));
    while (n + ul <= sz)
        memcpy(buf + n, unit, ul), n += ul;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (; done < total; done += n)
        vt_write(t, buf, n);
    clock_gettime(CLOCK_MONOTONIC, &b);
    s = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
    printf("%-28s %7.0f MB/s\n", name, done / 1048576.0 / s);
    vt_free(t);
    free(buf);
}

int
main(int argc, char **argv)
{
    size_t mb = argc > 1 ? strtoul(argv[1], NULL, 10) : 256;

    if (!setlocale(LC_CTYPE, "C.UTF-8") && !setlocale(LC_CTYPE, "en_US.UTF-8"))
        setlocale(LC_CTYPE, "");
    run("plain lines", "The quick brown fox jumps over the lazy dog, 0123456789 times.\r\n", mb);
    run("long lines, wrapped", "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", mb);
    run("coloured, like ls", "\033[0m\033[01;34mdir\033[0m  \033[01;32mprog\033[0m  file.c  \033[38;5;208mnotes\033[0m\r\n", mb);
    run("true colour runs", "\033[38;2;10;200;30m\033[48;2;1;2;3mab\033[0m", mb);
    run("UTF-8, accents and CJK", "caf\xc3\xa9 se\xc3\xb1or \xe4\xb8\xad\xe6\x96\x87 \xe2\x94\x80\xe2\x94\x82 ok\r\n", mb);
    run("cursor moves, as a TUI", "\033[10;20Hx\033[K\033[11;1H\033[2Ky\033[5A", mb);
    run("SGR on every cell", "\033[1;31mA\033[0;32;44mB\033[0m", mb);
    run("lines inside tmux", "The quick brown fox jumps over the lazy dog, 0123456789 times.\r\n", mb);
    run("paging in less", "\033[40;1H\nline of a file being paged through in less, scrolled one at a time\r", mb);
    return 0;
}
