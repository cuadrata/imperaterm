/* See LICENSE for licence details. */
#ifndef IMPERATERM_H
#define IMPERATERM_H

#include <sys/types.h>

#include <gtk/gtk.h>
#include <gtkgl/gtkglarea.h>

#include "render.h"
#include "vt.h"

/* What is shown is in the user's language where there is a catalogue for it. */
#ifdef ENABLE_NLS
#include <libintl.h>
#define _(s) gettext(s)
#else
#define _(s) (s)
#endif
#define N_(s) (s)  /* said in a table, and translated where it is shown */

#define PAD 2  /* around the text, in the window's pixels */
#define LEN(a) (sizeof (a) / sizeof *(a))

typedef struct Win Win;
typedef struct Tab Tab;

/* A window, and the one area in which whichever of its tabs is shown is drawn. */
struct Win {
    GtkWidget *window, *menubar, *notebook, *popup, *tabmenu, *gl, *sbar;
    GtkWidget *ticks[4];  /* Menubar and Scrollbar in the View menu, then in the right button's */
    GtkAdjustment *adj;
    GtkIMContext *im;
    Tab *cur, *sel;  /* the tab that is shown, and the one the primary selection is in */
    Grid grid;
    int w, h, frame, focus, blinkoff, motion, hist, scr, rows;
    int writing;  /* the child has written what is not yet drawn */
    guint drawtag, blinktag, refittag;
    int64_t refit;  /* until then, in microseconds, the window is sized to its terminal, which keeps its size meanwhile */
    int64_t lastdraw, began, held;  /* in microseconds: when a frame was last drawn, when the child began to write, and to hold its frame */
    char *primary, *clip;
};

/* A terminal: a child on a pty and what it has written. */
struct Tab {
    Win *win;
    GtkWidget *page, *label, *namewin;
    Term *vt;
    pid_t pid;
    int fd, rtag, wtag;
    guint telltag;  /* the window has another size, which the terminal takes once the window keeps it */
    int wantc, wantr;  /* that size, in columns and rows */
    char *name, *title;  /* what the user called it, and the title its child set */
    /*
     * Who is at the terminal and where, as the shell has said: the user
     * if it has said who, the host if that is not this one, and the
     * directory. here is the last directory that was on this host.
     */
    char *user, *host, *dir, *here;
    char *out;
    size_t outlen;
    int waiter;  /* the socket of the Imperaterm that was started for it and waits for it to end, or -1 */
    int selecting, btn, mx, my;
};

typedef struct {
    char *font;
    int size, scrollback, scheme, cursor, blink, bell, menubar, scrollbar, tabsbelow, cols, rows;
} Config;

extern Config cfg;
extern uint32_t palette[C_N];
extern GList *tabs;
extern int scale;

/* join.c */
typedef int JoinOpen(char **argv, const char *dir, char **env, int waiter);
int join_ask(char **argv, int *status);
void join_listen(JoinOpen *fn);

/* term.c */
GtkWidget *term_new(Win *w);
void term_refit(Win *w);
Tab *tab_new(Win *w, const char *cwd, char **argv, char **env);
void tab_show(Tab *tb);
void tab_close(Tab *tb);
void tab_leave(Tab *tb);
void tab_damage(Tab *tb);
void tab_copy(Tab *tb);
void tab_paste(Tab *tb, GdkAtom selection, guint32 time);
char *tab_cwd(Tab *tb);
char *host_name(void);
char *tab_clean(const char *s);
int tab_current(void);

/* imperaterm.c */
int win_key(Win *w, GdkEventKey *e);
void win_popup(Win *w, GdkEventButton *e);
void win_title(Tab *tb);
void win_check(Win *w);

#endif
