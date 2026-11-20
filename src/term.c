/* See LICENSE for licence details. */
/*
 * A tab is a child and its pty. A window has one area that every tab of
 * it is drawn in, the one that is shown at the time, so that going from
 * tab to tab is one frame after another and no window comes or goes.
 * Here are the keys and the mouse, the selections, and when to draw.
 * Nothing is drawn while nothing changes. What the child writes is drawn
 * when it pauses, so that a change it makes to the screen is seen whole;
 * while it writes without pause it is read as fast as it writes and
 * drawn once a frame.
 */
#define _XOPEN_SOURCE 700
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <gdk/gdkkeysyms.h>

#include "imperaterm.h"

extern char **environ;

GList *tabs;

static GdkAtom utf8, clipboard;
static int *glattr;

/*
 * A program that has the whole screen sends one change to it as many
 * small writes, and a frame drawn between two of them shows the change
 * half made. So what the child wrote is drawn once it has written nothing
 * more for LULL, or LATEST after it began if it does not pause. A child
 * that says where a change begins and ends, with synchronized output, is
 * drawn as soon as it ends one, and is not drawn in the middle of one for
 * as long as HELD. All three are in microseconds.
 */
enum { LULL = 500, LATEST = 16000, HELD = 150000 };

/*
 * A shell redraws its prompt when the terminal has a new size, counting
 * on the rows it had. Changed again before what it drew for the last
 * size is there, as when a window is dragged to another size, the
 * terminal and the shell no longer agree on those rows, and pieces of
 * the prompt are left behind. So the terminal takes a new size, and the
 * child is told of it, once the window has kept it for SETTLE, in
 * microseconds; meanwhile it is drawn as it was.
 */
enum { SETTLE = 100000 };

/* How long a window has to become the size it is asked to be, when its text changes size. */
enum { REFIT = 300000 };

static void output(Term *t, const char *s, size_t len);
static void input(gpointer data, gint fd, GdkInputCondition cond);

/* In microseconds, on a clock that is not set back or forward. */
static int64_t
now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * (int64_t)1000000 + ts.tv_nsec / 1000;
}

/* A timeout of a wait in microseconds, as the main loop counts them. */
static guint
later(int64_t us, GSourceFunc fn, gpointer data)
{
    return g_timeout_add(us > 0 ? (guint)((us + 999) / 1000) : 0, fn, data);
}

/* Drawing. */

/* The window asks to have room for so many columns and rows, as it is when it is new. */
static void
usize(Win *w, int cols, int rows)
{
    gtk_widget_set_usize(w->gl, (cols * cellw + scale - 1) / scale + 2 * PAD, (rows * cellh + scale - 1) / scale + 2 * PAD);
}

/* The columns and rows the window has room for. */
static void
room(Win *w, int *cols, int *rows)
{
    int pad = PAD * scale;

    *cols = MAX((w->w - 2 * pad) / cellw, 2);
    *rows = MAX((w->h - 2 * pad) / cellh, 1);
}

/* The window has kept its size: the terminal takes it, and the child is told. */
static gboolean
settled(gpointer data)
{
    Tab *tb = data;
    int cols, rows;

    tb->telltag = 0;
    room(tb->win, &cols, &rows);
    if (cols != tb->vt->cols || rows != tb->vt->rows) {
        struct winsize ws = { rows, cols, cols * cellw, rows * cellh };

        vt_resize(tb->vt, cols, rows);
        ioctl(tb->fd, TIOCSWINSZ, &ws);
        tab_damage(tb);
    }
    return FALSE;
}

/* The terminal takes the size its window has, once the window keeps it. */
static void
fit(Tab *tb)
{
    Win *w = tb->win;
    int cols, rows;

    room(w, &cols, &rows);

    if (w->refit) {  /* the window is not yet the size asked for: the terminal keeps the size it has */
        if (cols == tb->vt->cols && rows == tb->vt->rows)
            w->refit = 0;
        else if (now() < w->refit)
            return;
        else {  /* nor will it be: what it asks for is again what a new window would */
            w->refit = 0;
            usize(w, cfg.cols, cfg.rows);
        }
    }
    if (w->w && (cols != tb->vt->cols || rows != tb->vt->rows) && (cols != tb->wantc || rows != tb->wantr || !tb->telltag)) {
        if (tb->telltag)
            g_source_remove(tb->telltag);
        tb->wantc = cols, tb->wantr = rows;
        tb->telltag = later(SETTLE, settled, tb);
    }
}

static void draw(Win *w);

/* The window was not made the size asked for, maximized or tiled: the terminal takes what it has. */
static gboolean
refitted(gpointer data)
{
    Win *w = data;

    w->refittag = 0;
    draw(w);
    return FALSE;
}

/* The text has changed size, and the window is asked to change with it, to keep the terminal's. */
void
term_refit(Win *w)
{
    Tab *tb = w->cur;

    if (!tb)
        return;
    usize(w, tb->vt->cols, tb->vt->rows);
    w->refit = now() + REFIT;
    if (w->refittag)
        g_source_remove(w->refittag);
    w->refittag = later(REFIT, refitted, w);
}

static void
draw(Win *w)
{
    static const int shapes[] = { -1, CUR_BLOCK, CUR_BLOCK, CUR_UNDER, CUR_UNDER, CUR_BAR, CUR_BAR };
    Tab *tb = w->cur;
    GtkAdjustment *a = w->adj;
    Term *t;
    int shape, motion;

    if (w->drawtag)
        g_source_remove(w->drawtag), w->drawtag = 0;
    w->writing = 0, w->held = 0;
    if (!tb || !w->gl || !GTK_WIDGET_MAPPED(w->gl) || !gtk_gl_area_make_current(GTK_GL_AREA(w->gl)))
        return;
    t = tb->vt;
    fit(tb);
    shape = t->cstyle ? shapes[t->cstyle] : cfg.cursor, motion = !!(t->mode & M_MOUSEMANY);
    if (w->hist != t->hist || w->scr != t->scr || w->rows != t->rows) {  /* the scrollbar follows */
        w->hist = t->hist, w->scr = t->scr, w->rows = t->rows;
        a->upper = t->hist + t->rows, a->page_size = a->page_increment = t->rows;
        a->value = t->hist - t->scr;
        gtk_signal_handler_block_by_data(GTK_OBJECT(a), w);
        gtk_adjustment_changed(a);
        gtk_adjustment_value_changed(a);
        gtk_signal_handler_unblock_by_data(GTK_OBJECT(a), w);
    }
    if (motion != w->motion) {  /* the pointer is followed only for a child that asks */
        w->motion = motion;
        gdk_window_set_events(w->gl->window, gtk_widget_get_events(w->gl) |
                              (motion ? GDK_POINTER_MOTION_MASK : 0));
    }
    if (!w->focus)
        shape = CUR_HOLLOW;
    else if (w->blinkoff)
        shape = CUR_NONE;
    r_draw(&w->grid, t, w->w, w->h, PAD * scale, shape);
    gtk_gl_area_swap_buffers(GTK_GL_AREA(w->gl));
    w->lastdraw = now();
}

/* More may have come from the child while this waited its turn: that is read first, and the wait begins again. */
static gint
drawtimer(gpointer data)
{
    Win *w = data;
    struct pollfd p = { w->cur ? w->cur->fd : -1, POLLIN, 0 };

    w->drawtag = 0;
    if (w->writing && now() - w->began < LATEST && poll(&p, 1, 0) > 0 && (p.revents & POLLIN))
        input(w->cur, p.fd, GDK_INPUT_READ);
    else
        draw(w);
    return FALSE;
}

/*
 * Something changed: drawn at once if the last frame is old enough and
 * the child is not in the middle of writing, else when that is so. A tab
 * that is not the one shown is drawn when it is.
 */
static void
damage(Tab *tb, int now_if_due)
{
    Win *w = tb->win;
    int64_t t = now(), wait = w->lastdraw + w->frame - t;

    if (w->cur != tb || w->drawtag || !w->gl || !GTK_WIDGET_MAPPED(w->gl))
        return;
    if (w->held)
        wait = MAX(wait, w->held + HELD - t);
    else if (w->writing)
        wait = MAX(wait, w->began + LATEST - t);
    if (now_if_due && wait <= 0)
        draw(w);
    else
        w->drawtag = later(wait, drawtimer, w);
}

void
tab_damage(Tab *tb)
{
    damage(tb, 0);
}

/*
 * The child wrote, and reading it began at a time. If what it changed is
 * whole, the tab that is shown is drawn as soon as a frame may be, which
 * may be now; else
 * it waits for the child to finish, or to pause for long enough.
 */
static void
wrote(Tab *tb, int64_t began, int whole)
{
    Win *w = tb->win;

    if (w->cur == tb) {
        if (whole)
            w->writing = 0;
        else if (!w->writing)
            w->writing = 1, w->began = began;
        w->held = !(tb->vt->mode & M_SYNC) ? 0 : w->held ? w->held : now();
        if (w->drawtag)
            g_source_remove(w->drawtag), w->drawtag = 0;
    }
    damage(tb, whole);  /* and not a turn of the main loop later */
}

/* Nothing more has come from the child for this many microseconds. */
static int
quiet(int fd, long us)
{
    struct timespec ts = { 0, us * 1000 };
    fd_set r;

    if (fd >= FD_SETSIZE)  /* which select cannot wait on: as good as quiet */
        return 1;
    FD_ZERO(&r);
    FD_SET(fd, &r);
    return pselect(fd + 1, &r, NULL, NULL, &ts, NULL) == 0;
}

/*
 * The tab its window shows. The area stays as it is and its next frame is
 * the new tab, so there is nothing between the two to be seen.
 */
void
tab_show(Tab *tb)
{
    Win *w = tb->win;
    Tab *old = w->cur;

    if (old == tb)
        return;
    if (old) {
        old->selecting = 0;
        if (w->focus && (old->vt->mode & M_FOCUS))
            output(old->vt, "\033[O", 3);
    }
    if (w->focus && (tb->vt->mode & M_FOCUS))
        output(tb->vt, "\033[I", 3);
    w->cur = tb, w->blinkoff = 0;
    tb->vt->alldirty = 1;  /* the window's cells have another tab's text in them */
    tab_damage(tb);
}

static void
scroll(Tab *tb, int n)
{
    Term *t = tb->vt;

    n = t->mode & M_ALT ? 0 : CLAMP(t->scr - n, 0, t->hist);
    if (n != t->scr) {
        t->scr = n, t->alldirty = 1;
        tab_damage(tb);
    }
}

static void
scrolled(GtkAdjustment *a, Win *w)
{
    if (w->cur)
        scroll(w->cur, (int)a->value - (w->cur->vt->hist - w->cur->vt->scr));
}

static gint
blink(gpointer data)
{
    Win *w = data;

    w->blinkoff = !w->blinkoff;
    if (w->cur)
        tab_damage(w->cur);
    return TRUE;
}

/* The pty. */

static void
flush(gpointer data, gint fd, GdkInputCondition cond)
{
    Tab *tb = data;
    ssize_t n = write(fd, tb->out, tb->outlen);

    (void)cond;
    if (n > 0)
        memmove(tb->out, tb->out + n, tb->outlen -= n);
    if (!tb->outlen || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        gdk_input_remove(tb->wtag);
        tb->wtag = 0, tb->outlen = 0;
    }
}

/* To the child; what it does not take now waits for it. */
static void
output(Term *t, const char *s, size_t len)
{
    Tab *tb = t->data;
    ssize_t n = tb->outlen ? 0 : write(tb->fd, s, len);

    if (n < 0)
        n = 0;
    if ((size_t)n == len)
        return;
    tb->out = g_realloc(tb->out, tb->outlen + len - n);
    memcpy(tb->out + tb->outlen, s + n, len - n);
    tb->outlen += len - n;
    if (!tb->wtag)
        tb->wtag = gdk_input_add(tb->fd, GDK_INPUT_WRITE, flush, tb);
}

/* What the user types or pastes: the view goes back to the end. */
static void
typed(Tab *tb, const char *s, size_t n)
{
    tb->win->blinkoff = 0;
    scroll(tb, tb->vt->scr);
    output(tb->vt, s, n);
}

/*
 * What the child wrote. When it pauses, what it changed is whole if it
 * said so; if not, and it is the tab shown, a moment more may bring the
 * rest of the change, and it is waited for here rather than by a timer,
 * which counts only in milliseconds.
 */
static void
input(gpointer data, gint fd, GdkInputCondition cond)
{
    static char buf[65536];
    Tab *tb = data;
    Win *w = tb->win;
    Term *t = tb->vt;
    int64_t began = now();
    ssize_t n;
    size_t total = 0;
    int whole = 0;

    (void)cond;
    for (;;) {
        while ((n = read(fd, buf, sizeof buf)) > 0) {
            vt_write(t, buf, n);
            if ((total += n) >= 1 << 20)  /* the rest next time round: the keys get a turn */
                break;
        }
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            tab_close(tb);
            return;
        }
        if (n > 0)
            break;
        if (t->whole && !(t->mode & M_SYNC)) {
            whole = 1;
            break;
        }
        if (w->cur != tb || (t->mode & M_SYNC) || now() - (w->writing ? w->began : began) >= LATEST)
            break;
        if (quiet(fd, LULL)) {
            whole = 1;
            break;
        }
    }
    t->whole = 0;
    wrote(tb, began, whole);
}

/*
 * imperaterm.sh, which has a shell say who is at the terminal and where, as
 * a shell is to read it: bash at each prompt, for PROMPT_COMMAND, and
 * the other shells of its kind when they start, by ENV. NULL if it is
 * not where it was installed.
 */
static const char *
hook(int bash)
{
    static const char file[] = PKGDATADIR "/imperaterm.sh";
    static char *command;
    static int looked;

    if (!looked++ && !strchr(file, '\'') && !access(file, R_OK))
        command = g_strconcat("[ -r '", file, "' ] && . '", file, "'", NULL);
    return !command ? NULL : bash ? command : file;
}

static int
spawn(Tab *tb, const char *cwd, char **argv, char **env)
{
    struct winsize ws = { tb->vt->rows, tb->vt->cols, 0, 0 };
    struct passwd *pw;
    const char *name, *sh = NULL, *atprompt = hook(1), *atstart = hook(0);
    int m = posix_openpt(O_RDWR | O_NOCTTY), s, i;

    if (m < 0)
        return -1;
    if (grantpt(m) || unlockpt(m) || !(name = ptsname(m)))
        return close(m), -1;
    ioctl(m, TIOCSWINSZ, &ws);
    if ((tb->pid = fork()) < 0)
        return close(m), -1;
    if (!tb->pid) {
        setsid();
        if ((s = open(name, O_RDWR)) < 0)
            _exit(1);
        ioctl(s, TIOCSCTTY, 0);
        dup2(s, 0), dup2(s, 1), dup2(s, 2);
        for (i = 3; i < 256; i++)
            close(i);
        /* A directory that cannot be gone to, as root's own when a tab was told of it: home will do. */
        if (cwd && chdir(cwd)) {
            const char *home = getenv("HOME");

            if (!home || chdir(home))
                perror(cwd);
        }
        /* As a login would leave them: Imperaterm may have been started with some ignored. */
        signal(SIGCHLD, SIG_DFL), signal(SIGPIPE, SIG_DFL), signal(SIGHUP, SIG_DFL);
        signal(SIGINT, SIG_DFL), signal(SIGQUIT, SIG_DFL), signal(SIGTERM, SIG_DFL);
        if (env)
            environ = env;  /* that of the Imperaterm that was started for it */
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        if (atprompt) {  /* and what the user has set for these is left alone */
            setenv("PROMPT_COMMAND", atprompt, 0);
            setenv("ENV", atstart, 0);
        }
        unsetenv("COLUMNS"), unsetenv("LINES"), unsetenv("TERMCAP");
        if (argv)
            execvp(argv[0], argv);
        else {
            if (!(sh = getenv("SHELL")) || !*sh)
                sh = (pw = getpwuid(getuid())) && *pw->pw_shell ? pw->pw_shell : "/bin/sh";
            execlp(sh, sh, (char *)NULL);
        }
        perror(argv ? argv[0] : sh);
        _exit(127);
    }
    fcntl(m, F_SETFL, O_NONBLOCK);
    fcntl(m, F_SETFD, FD_CLOEXEC);
    tb->fd = m;
    return 0;
}

/* Where the tab last was on this host, for a new tab to start there. */
char *
tab_cwd(Tab *tb)
{
    return g_strdup(tb->here);
}

/* This host, by its name up to the first dot. */
char *
host_name(void)
{
    static char host[256];

    if (!*host) {
        if (gethostname(host, sizeof host - 1) || !*host)
            strcpy(host, "localhost");
        host[strcspn(host, ".")] = 0;
    }
    return host;
}

/* A host as a tab keeps it: NULL for this one, by any of its names. */
static char *
host_kept(const char *s, size_t n)
{
    size_t brief = strcspn(s, ".") < n ? strcspn(s, ".") : n;

    if (!n || (n == 9 && !g_ascii_strncasecmp(s, "localhost", 9)) ||
        (brief == strlen(host_name()) && !g_ascii_strncasecmp(s, host_name(), brief)))
        return NULL;
    return g_strndup(s, n);
}

static int
hex(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/*
 * A tab knows who is at it and where only as it is told. There is no
 * asking: what a system has for looking at another process is its own
 * and no other's, shows the shell that was started and not the one
 * that is used after su or ssh, and may not be read at all when that
 * one is root's. So the shell says it at each prompt, in two strings
 * that other terminals read too: OSC 7 with file://host/directory, and
 * OSC 1337 with RemoteHost=user@host. imperaterm.sh has shells do so.
 */
static void
told(Term *t, int what, const char *s)
{
    Tab *tb = t->data;
    const char *p;
    char *host, *dir, *d;

    if (what == TOLD_WHO) {
        if (!(p = strrchr(s, '@')) || p == s || !p[1])
            return;
        g_free(tb->user), g_free(tb->host);
        tb->user = g_strndup(s, p - s);
        tb->host = host_kept(p + 1, strlen(p + 1));
    } else {
        if (g_ascii_strncasecmp(s, "file://", 7) || !(p = strchr(s + 7, '/')))
            return;
        d = dir = g_malloc(strlen(p) + 1);
        for (; *p; p++)
            if (*p != '%')
                *d++ = *p;
            else if (hex(p[1]) < 0 || hex(p[2]) < 0 || !(hex(p[1]) || hex(p[2]))) {
                g_free(dir);
                return;
            } else
                *d++ = hex(p[1]) << 4 | hex(p[2]), p += 2;
        *d = 0;
        host = host_kept(s + 7, strchr(s + 7, '/') - (s + 7));
        if (!host != !tb->host || (host && g_ascii_strcasecmp(host, tb->host))) {
            g_free(tb->user);  /* another host: who is there is not known until it says */
            tb->user = NULL;
        }
        g_free(tb->host), g_free(tb->dir);
        tb->host = host, tb->dir = dir;
        if (!host) {
            g_free(tb->here);
            tb->here = g_strdup(dir);
        }
    }
    win_title(tb);
}

/* Keys. */

static const struct { guint key; char fin; int num; } keys[] = {
    { GDK_Up, 'A', 0 }, { GDK_Down, 'B', 0 }, { GDK_Right, 'C', 0 }, { GDK_Left, 'D', 0 },
    { GDK_Home, 'H', 0 }, { GDK_End, 'F', 0 }, { GDK_KP_Up, 'A', 0 }, { GDK_KP_Down, 'B', 0 },
    { GDK_KP_Right, 'C', 0 }, { GDK_KP_Left, 'D', 0 }, { GDK_KP_Home, 'H', 0 }, { GDK_KP_End, 'F', 0 },
    { GDK_F1, 'P', 1 }, { GDK_F2, 'Q', 1 }, { GDK_F3, 'R', 1 }, { GDK_F4, 'S', 1 },
    { GDK_Insert, '~', 2 }, { GDK_Delete, '~', 3 }, { GDK_Page_Up, '~', 5 }, { GDK_Page_Down, '~', 6 },
    { GDK_KP_Insert, '~', 2 }, { GDK_KP_Delete, '~', 3 }, { GDK_KP_Page_Up, '~', 5 }, { GDK_KP_Page_Down, '~', 6 },
    { GDK_F5, '~', 15 }, { GDK_F6, '~', 17 }, { GDK_F7, '~', 18 }, { GDK_F8, '~', 19 },
    { GDK_F9, '~', 20 }, { GDK_F10, '~', 21 }, { GDK_F11, '~', 23 }, { GDK_F12, '~', 24 }
};

static gint
keypress(GdkEventKey *e, Tab *tb)
{
    Term *t = tb->vt;
    guint k = e->keyval, i;
    int sh = !!(e->state & GDK_SHIFT_MASK), ct = !!(e->state & GDK_CONTROL_MASK), al = !!(e->state & GDK_MOD1_MASK);
    int mod = 1 + sh + 2 * al + 4 * ct, n = 0;
    GdkRectangle r = { (PAD + t->c.x * cellw) / scale, (PAD + t->c.y * cellh) / scale, cellw / scale, cellh / scale };
    char b[32];

    if (win_key(tb->win, e))
        return TRUE;
    if (sh && !ct && !al) {  /* the scrollback, and the old way to paste */
        if (k == GDK_Page_Up || k == GDK_Page_Down || k == GDK_KP_Page_Up || k == GDK_KP_Page_Down)
            return scroll(tb, (k == GDK_Page_Up || k == GDK_KP_Page_Up ? -1 : 1) * MAX(t->rows - 1, 1)), TRUE;
        if (k == GDK_Home || k == GDK_End)
            return scroll(tb, k == GDK_Home ? -t->hist : t->hist), TRUE;
        if (k == GDK_Insert || k == GDK_KP_Insert)
            return tab_paste(tb, GDK_SELECTION_PRIMARY, e->time), TRUE;
    }
    for (i = 0; i < LEN(keys) && keys[i].key != k; i++);
    if (i < LEN(keys)) {
        if (keys[i].fin == '~')
            n = mod > 1 ? sprintf(b, "\033[%d;%d~", keys[i].num, mod) : sprintf(b, "\033[%d~", keys[i].num);
        else if (mod > 1)
            n = sprintf(b, "\033[1;%d%c", mod, keys[i].fin);
        else
            n = sprintf(b, "\033%c%c", keys[i].num || (t->mode & M_APPCURSOR) ? 'O' : '[', keys[i].fin);
    } else if (k == GDK_BackSpace) {
        n = sprintf(b, "%s%c", al ? "\033" : "", ct ? '\b' : 0x7f);
    } else if (k == GDK_Tab || k == GDK_ISO_Left_Tab || k == GDK_KP_Tab) {
        n = sprintf(b, "%s", sh || k == GDK_ISO_Left_Tab ? "\033[Z" : "\t");
    } else if (k == GDK_Return || k == GDK_KP_Enter) {
        n = sprintf(b, "%s\r", al ? "\033" : "");
    } else if (k == GDK_Escape) {
        n = sprintf(b, "%s\033", al ? "\033" : "");
    } else if ((ct || al) && k >= 0x20 && k < 0x7f) {
        if (ct)
            k = k >= 'a' && k <= 'z' ? k - 96 : k >= '@' && k <= '_' ? k - 64 : k == ' ' || k == '2' ? 0 :
                k >= '3' && k <= '7' ? k - '3' + 27 : k == '8' || k == '?' ? 127 : k == '/' ? 31 : k;
        if (al)
            b[n++] = 033;
        b[n++] = k;
    } else {
        gtk_im_context_set_cursor_location(tb->win->im, &r);
        if (gtk_im_context_filter_keypress(tb->win->im, e))
            return TRUE;
        if (e->length <= 0)
            return FALSE;
        typed(tb, e->string, e->length);
        return TRUE;
    }
    typed(tb, b, n);
    return TRUE;
}

/* A key that is taken stops here: the widget's own handler would say it was not. */
static gint
key(GtkWidget *wd, GdkEventKey *e, Win *w)
{
    if (!w->cur || !keypress(e, w->cur))
        return FALSE;
    gtk_signal_emit_stop_by_name(GTK_OBJECT(wd), "key_press_event");
    return TRUE;
}

static void
commit(GtkIMContext *im, const gchar *s, Win *w)
{
    (void)im;
    if (w->cur)
        typed(w->cur, s, strlen(s));
}

static gint
focus(GtkWidget *wd, GdkEventFocus *e, Win *w)
{
    Tab *tb = w->cur;

    (void)wd;
    w->focus = e->in, w->blinkoff = 0;
    if (e->in)
        gtk_im_context_focus_in(w->im);
    else
        gtk_im_context_focus_out(w->im);
    if (w->blinktag)
        g_source_remove(w->blinktag), w->blinktag = 0;
    if (e->in && cfg.blink)
        w->blinktag = g_timeout_add(600, blink, w);
    if (tb && (tb->vt->mode & M_FOCUS))
        output(tb->vt, e->in ? "\033[I" : "\033[O", 3);
    if (tb)
        tab_damage(tb);
    return FALSE;
}

/* The mouse. */

static int
mods(guint state)
{
    return (state & GDK_SHIFT_MASK ? 4 : 0) | (state & GDK_MOD1_MASK ? 8 : 0) | (state & GDK_CONTROL_MASK ? 16 : 0);
}

static void
cell(Tab *tb, gdouble ex, gdouble ey, int *x, int *y)
{
    *x = CLAMP(((int)(ex * scale) - PAD * scale) / cellw, 0, tb->vt->cols - 1);
    *y = CLAMP(((int)(ey * scale) - PAD * scale) / cellh, 0, tb->vt->rows - 1);
}

static gint
press(GtkWidget *wd, GdkEventButton *e, Win *w)
{
    Tab *tb = w->cur;
    Term *t;
    int x, y, i, b = e->button, report;

    if (!tb)
        return TRUE;
    t = tb->vt, report = (t->mode & M_MOUSE) && !(e->state & GDK_SHIFT_MASK);
    cell(tb, e->x, e->y, &x, &y);
    if (!GTK_WIDGET_HAS_FOCUS(wd))
        gtk_widget_grab_focus(wd);
    if (report) {
        if (e->type == GDK_BUTTON_PRESS && vt_mouse(t, x, y, b, 0, mods(e->state)) && b < 4)
            tb->btn = b;
    } else if (b == 4 || b == 5) {  /* the wheel */
        if ((t->mode & M_ALT) && (t->mode & M_ALTSCROLL))
            for (i = 0; i < 3; i++)
                output(t, t->mode & M_APPCURSOR ? (b == 4 ? "\033OA" : "\033OB") : (b == 4 ? "\033[A" : "\033[B"), 3);
        else
            scroll(tb, b == 4 ? -3 : 3);
    } else if (b == 1) {
        if (e->type == GDK_BUTTON_PRESS && (e->state & GDK_SHIFT_MASK) && t->sel.ob.x != -1 && !(t->mode & M_MOUSE))
            vt_selextend(t, x, y, 0);
        else
            vt_selstart(t, x, y, e->type == GDK_3BUTTON_PRESS ? SNAP_LINE :
                        e->type == GDK_2BUTTON_PRESS ? SNAP_WORD : SNAP_NONE);
        tb->selecting = 1, tb->mx = x, tb->my = y;
        tab_damage(tb);
    } else if (b == 2 && e->type == GDK_BUTTON_PRESS) {
        tab_paste(tb, GDK_SELECTION_PRIMARY, e->time);
    } else if (b == 3 && e->type == GDK_BUTTON_PRESS) {
        win_popup(tb->win, e);
    }
    return TRUE;
}

static gint
release(GtkWidget *wd, GdkEventButton *e, Win *w)
{
    Tab *tb = w->cur;
    char *s;
    int x, y;

    if (!tb)
        return TRUE;
    cell(tb, e->x, e->y, &x, &y);
    if (tb->btn) {
        vt_mouse(tb->vt, x, y, e->button, 1, mods(e->state));
        tb->btn = 0;
    } else if (e->button == 1 && tb->selecting) {
        tb->selecting = 0;
        if ((s = vt_seltext(tb->vt)) && *s) {  /* what is selected is the primary selection */
            free(w->primary);
            w->primary = s;
            if (w->sel && w->sel != tb)  /* and what was, in another tab, is not */
                vt_selclear(w->sel->vt);
            w->sel = tb;
            gtk_selection_owner_set(wd, GDK_SELECTION_PRIMARY, e->time);
        } else
            free(s);
    }
    return TRUE;
}

static gint
motion(GtkWidget *wd, GdkEventMotion *e, Win *w)
{
    Tab *tb = w->cur;
    int x, y;

    (void)wd;
    if (!tb)
        return TRUE;
    cell(tb, e->x, e->y, &x, &y);
    if (tb->selecting && (e->y < 0 || e->y * scale >= w->h))  /* dragged past the edge */
        scroll(tb, e->y < 0 ? -1 : 1);
    else if (x == tb->mx && y == tb->my)
        return TRUE;
    tb->mx = x, tb->my = y;
    if (tb->selecting) {
        vt_selextend(tb->vt, x, y, !!(e->state & GDK_CONTROL_MASK));
        tab_damage(tb);
    } else if (!(e->state & GDK_SHIFT_MASK))
        vt_mouse(tb->vt, x, y, tb->btn, 2, mods(e->state));
    return TRUE;
}

/* Selections. */

void
tab_copy(Tab *tb)
{
    char *s = vt_seltext(tb->vt);

    if (!s || !*s) {
        free(s);
        return;
    }
    free(tb->win->clip);
    tb->win->clip = s;
    gtk_selection_owner_set(tb->win->gl, clipboard, GDK_CURRENT_TIME);
}

void
tab_paste(Tab *tb, GdkAtom selection, guint32 time)
{
    gtk_selection_convert(tb->win->gl, selection ? selection : clipboard, utf8, time);
}

static void
selget(GtkWidget *wd, GtkSelectionData *sd, guint info, guint time, Win *w)
{
    const char *s = sd->selection == GDK_SELECTION_PRIMARY ? w->primary : w->clip;

    (void)wd, (void)time;
    if (s)
        gtk_selection_data_set(sd, info ? GDK_SELECTION_TYPE_STRING : utf8, 8, (guchar *)s, strlen(s));
}

static void
selreceived(GtkWidget *wd, GtkSelectionData *sd, guint time, Win *w)
{
    Tab *tb = w->cur;

    if (!tb)
        return;
    if (sd->length > 0) {
        scroll(tb, tb->vt->scr);
        vt_paste(tb->vt, (char *)sd->data, sd->length);
    } else if (sd->target == utf8)
        gtk_selection_convert(wd, sd->selection, GDK_TARGET_STRING, time);
}

static gint
selclear(GtkWidget *wd, GdkEventSelection *e, Win *w)
{
    if (!gtk_selection_clear(wd, e))
        return FALSE;
    if (e->selection == GDK_SELECTION_PRIMARY && w->sel) {
        vt_selclear(w->sel->vt);
        tab_damage(w->sel);
        w->sel = NULL;
    }
    return TRUE;
}

/* The widgets. */

static void
realize(GtkWidget *wd, Win *w)
{
    GdkCursor *ibeam = gdk_cursor_new(GDK_XTERM);
    int rate;

    gdk_window_set_cursor(wd->window, ibeam);  /* text is what is under the pointer */
    gdk_cursor_destroy(ibeam);
    if (!gtk_gl_area_make_current(GTK_GL_AREA(wd)) || !r_init()) {
        g_printerr(_("imperaterm: OpenGL 3.1 or OpenGL ES 3.0 is needed, and neither is here\n"));
        exit(1);
    }
    gdk_gl_set_swap_interval(wd->window, 0);  /* frames are paced here, not by waiting in a swap */
    rate = gdk_window_get_refresh_rate(wd->window);
    w->frame = rate > 0 ? MAX((int)(1000000000LL / rate), 1000) : 16667;
    gtk_im_context_set_client_window(w->im, wd->window);
}

/*
 * The area is going, and is no longer realized, so its own context cannot
 * be made current. Its texture is every window's, the contexts sharing
 * what they have, and is deleted in another window's; its vertex array
 * goes with its context. With the last window everything goes.
 */
static void
gone(GtkWidget *wd, Win *w)
{
    (void)wd;
    w->gl = NULL;
    if (!tab_current())
        w->grid.tex = 0;
    w->grid.vao = 0;
    r_free(&w->grid);
    if (w->drawtag)
        g_source_remove(w->drawtag);
    if (w->blinktag)
        g_source_remove(w->blinktag);
    if (w->refittag)
        g_source_remove(w->refittag);
    gtk_im_context_set_client_window(w->im, NULL);
    gtk_object_unref(GTK_OBJECT(w->im));
    free(w->primary), free(w->clip);
    w->drawtag = w->blinktag = w->refittag = 0, w->primary = w->clip = NULL;
}

static gint
configure(GtkWidget *wd, GdkEventConfigure *e, Win *w)
{
    int ww, hh;

    (void)e;
    if (!GTK_WIDGET_REALIZED(wd))
        return TRUE;
    gtk_gl_area_get_drawable_size(GTK_GL_AREA(wd), &ww, &hh);
    if (ww != w->w || hh != w->h) {  /* told again of the size it has, it has nothing to draw */
        w->w = ww, w->h = hh;
        draw(w);
    }
    return TRUE;
}

static gint
expose(GtkWidget *wd, GdkEventExpose *e, Win *w)
{
    (void)wd;
    if (!e->count)
        draw(w);
    return TRUE;
}

/*
 * A title is the child's to choose and a directory has any name, and the
 * window system is owed UTF-8 with no control characters in it: one of
 * those becomes a space, and what is not UTF-8 a question mark.
 */
char *
tab_clean(const char *s)
{
    static const uint32_t least[] = { 0, 0, 0x80, 0x800, 0x10000 };
    unsigned char *b = (unsigned char *)g_strdup(s), *p = b, *q = b;
    uint32_t u;
    int n, i;

    for (; *p; p += n) {
        n = *p < 0x80 ? 1 : *p < 0xc2 ? 0 : *p < 0xe0 ? 2 : *p < 0xf0 ? 3 : *p < 0xf5 ? 4 : 0;
        u = n > 1 ? *p & (0x7f >> n) : *p;
        for (i = 1; i < n && (p[i] & 0xc0) == 0x80; i++)
            u = u << 6 | (p[i] & 0x3f);
        if (i == n && u >= least[n] && u >= 0x20 && (u < 0x7f || u >= 0xa0) && (u < 0xd800 || u >= 0xe000) &&
            u <= 0x10ffff) {
            memmove(q, p, n);
            q += n;
        } else {
            *q++ = u < 0x20 && n == 1 ? ' ' : '?';
            n = n ? i : 1;
        }
    }
    *q = 0;
    return (char *)b;
}

static void
settitle(Term *t, const char *s)
{
    Tab *tb = t->data;

    g_free(tb->title);
    tb->title = *s ? tab_clean(s) : NULL;
    win_title(tb);
}

static void
bell(Term *t)
{
    (void)t;
    if (cfg.bell)
        gdk_beep();
}

static void
tabfree(gpointer data)
{
    Tab *tb = data;
    Win *w = tb->win;

    tabs = g_list_remove(tabs, tb);
    if (w->cur == tb)
        w->cur = NULL;
    if (w->sel == tb)
        w->sel = NULL;
    gdk_input_remove(tb->rtag);
    if (tb->wtag)
        gdk_input_remove(tb->wtag);
    if (tb->telltag)
        g_source_remove(tb->telltag);
    if (tb->namewin)
        gtk_widget_destroy(tb->namewin);
    close(tb->fd);  /* which hangs the child up */
    if (tb->waiter >= 0)
        close(tb->waiter);  /* which is what it waited for */
    vt_free(tb->vt);
    g_free(tb->out), g_free(tb->name), g_free(tb->title);
    g_free(tb->user), g_free(tb->host), g_free(tb->dir), g_free(tb->here);
    g_free(tb);
}

/* A tab is to leave its window for another: the window forgets it. */
void
tab_leave(Tab *tb)
{
    Win *w = tb->win;

    if (w->cur == tb)
        w->cur = NULL;
    if (w->sel == tb) {  /* what was selected in it is the window's still, and no longer shown as selected */
        vt_selclear(tb->vt);
        w->sel = NULL;
    }
    tb->selecting = 0;
}

void
tab_close(Tab *tb)
{
    Win *w = tb->win;

    if (w->cur == tb)
        w->cur = NULL;
    gtk_widget_destroy(tb->page);
    win_check(w);
}

/* A context is current for what the renderer is about to do; 0 if there is none yet. */
int
tab_current(void)
{
    GList *l;

    for (l = tabs; l; l = l->next) {
        Win *w = ((Tab *)l->data)->win;

        if (w->gl && GTK_WIDGET_REALIZED(w->gl) && gtk_gl_area_make_current(GTK_GL_AREA(w->gl)))
            return 1;
    }
    return 0;
}

/* An area for OpenGL 3.1, or for OpenGL ES 3.0 where that is all there is or what is asked for. */
static GtkWidget *
glarea(void)
{
    static int gl[] = { GDK_GL_CONTEXT_MAJOR_VERSION, 3, GDK_GL_CONTEXT_MINOR_VERSION, 1, GDK_GL_DOUBLEBUFFER, GDK_GL_NONE };
    static int es[] = { GDK_GL_CONTEXT_MAJOR_VERSION, 3, GDK_GL_CONTEXT_MINOR_VERSION, 0,
                        GDK_GL_CONTEXT_PROFILE_MASK, GDK_GL_CONTEXT_ES_PROFILE_BIT, GDK_GL_DOUBLEBUFFER, GDK_GL_NONE };
    GtkWidget *wd;

    if (glattr)  /* every window shares the first one's glyphs and shader */
        return gtk_gl_area_share_new(glattr, tabs ? GTK_GL_AREA(((Tab *)tabs->data)->win->gl) : NULL);
    if ((!getenv("IMPERATERM_GLES") && (wd = gtk_gl_area_new(glattr = gl))) || (wd = gtk_gl_area_new(glattr = es)))
        return wd;
    g_printerr(_("imperaterm: OpenGL 3.1 or OpenGL ES 3.0 is needed, and neither is here\n"));
    exit(1);
}

/* A window's terminal: the area its tabs are drawn in, and the scrollbar beside it. */
GtkWidget *
term_new(Win *w)
{
    static const GtkTargetEntry targets[] = { { "UTF8_STRING", 0, 0 }, { "STRING", 0, 1 }, { "TEXT", 0, 0 } };
    GtkWidget *box = gtk_hbox_new(FALSE, 0);
    GtkObject *gl;

    if (!utf8) {
        utf8 = gdk_atom_intern("UTF8_STRING", FALSE);
        clipboard = gdk_atom_intern("CLIPBOARD", FALSE);
    }
    w->frame = 16667;
    w->gl = glarea();
    gl = GTK_OBJECT(w->gl);
    GTK_WIDGET_SET_FLAGS(w->gl, GTK_CAN_FOCUS);
    gtk_widget_set_events(w->gl, GDK_EXPOSURE_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                          GDK_BUTTON_MOTION_MASK | GDK_KEY_PRESS_MASK | GDK_FOCUS_CHANGE_MASK);
    usize(w, cfg.cols, cfg.rows);
    gtk_signal_connect(gl, "realize", GTK_SIGNAL_FUNC(realize), w);
    gtk_signal_connect(gl, "destroy", GTK_SIGNAL_FUNC(gone), w);
    gtk_signal_connect(gl, "configure_event", GTK_SIGNAL_FUNC(configure), w);
    gtk_signal_connect(gl, "expose_event", GTK_SIGNAL_FUNC(expose), w);
    gtk_signal_connect(gl, "key_press_event", GTK_SIGNAL_FUNC(key), w);
    gtk_signal_connect(gl, "focus_in_event", GTK_SIGNAL_FUNC(focus), w);
    gtk_signal_connect(gl, "focus_out_event", GTK_SIGNAL_FUNC(focus), w);
    gtk_signal_connect(gl, "button_press_event", GTK_SIGNAL_FUNC(press), w);
    gtk_signal_connect(gl, "button_release_event", GTK_SIGNAL_FUNC(release), w);
    gtk_signal_connect(gl, "motion_notify_event", GTK_SIGNAL_FUNC(motion), w);
    gtk_signal_connect(gl, "selection_get", GTK_SIGNAL_FUNC(selget), w);
    gtk_signal_connect(gl, "selection_received", GTK_SIGNAL_FUNC(selreceived), w);
    gtk_signal_connect(gl, "selection_clear_event", GTK_SIGNAL_FUNC(selclear), w);
    gtk_selection_add_targets(w->gl, GDK_SELECTION_PRIMARY, targets, LEN(targets));
    gtk_selection_add_targets(w->gl, clipboard, targets, LEN(targets));

    w->im = gtk_im_multicontext_new();
    gtk_object_ref(GTK_OBJECT(w->im));
    gtk_object_sink(GTK_OBJECT(w->im));
    gtk_signal_connect(GTK_OBJECT(w->im), "commit", GTK_SIGNAL_FUNC(commit), w);

    w->adj = GTK_ADJUSTMENT(gtk_adjustment_new(0, 0, cfg.rows, 1, cfg.rows, cfg.rows));
    gtk_signal_connect(GTK_OBJECT(w->adj), "value_changed", GTK_SIGNAL_FUNC(scrolled), w);
    w->sbar = gtk_vscrollbar_new(w->adj);
    GTK_WIDGET_UNSET_FLAGS(w->sbar, GTK_CAN_FOCUS);
    gtk_box_pack_start(GTK_BOX(box), w->gl, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), w->sbar, FALSE, FALSE, 0);
    return box;
}

/* A tab starts at the size the one shown has, so that its child is not told of another at once. */
Tab *
tab_new(Win *w, const char *cwd, char **argv, char **env)
{
    Tab *tb = g_new0(Tab, 1);

    tb->win = w, tb->mx = tb->my = -1, tb->waiter = -1;
    tb->vt = vt_new(w->cur ? w->cur->vt->cols : cfg.cols, w->cur ? w->cur->vt->rows : cfg.rows, cfg.scrollback, palette);
    tb->vt->data = tb, tb->vt->write = output, tb->vt->title = settitle, tb->vt->bell = bell, tb->vt->told = told;
    if (spawn(tb, cwd, argv, env) < 0) {
        g_printerr(_("imperaterm: no pty: %s\n"), g_strerror(errno));
        vt_free(tb->vt);
        g_free(tb);
        return NULL;
    }
    tb->dir = cwd ? g_strdup(cwd) : g_get_current_dir();  /* where it starts, until the shell says where it is */
    tb->here = g_strdup(tb->dir);
    tb->page = gtk_hbox_new(FALSE, 0);  /* nothing is in it: the notebook is there for its tabs */
    gtk_object_set_data_full(GTK_OBJECT(tb->page), "tab", tb, tabfree);
    tb->rtag = gdk_input_add(tb->fd, GDK_INPUT_READ, input, tb);
    tabs = g_list_append(tabs, tb);
    return tb;
}
