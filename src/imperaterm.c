/* See LICENSE for licence details. */
/* The windows, their tabs and menus, and the settings. */
#define _XOPEN_SOURCE 700
#include <langinfo.h>
#include <locale.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gdk/gdkkeysyms.h>

#include "imperaterm.h"
#include "imperaterm.xpm"

enum {
    NEWTAB, NEWWIN, CLOSETAB, CLOSEWIN, COPY, PASTE, RESET, PREFS, MENUBAR, SCROLLBAR, ZOOMIN, ZOOMOUT, ZOOMOFF,
    PREVTAB, NEXTTAB, TABLEFT, TABRIGHT, NAMETAB, ABOUT
};

Config cfg = { NULL, 11, 10000, 0, CUR_BLOCK, 0, 0, 1, 1, 0, 80, 24 };
uint32_t palette[C_N];
int scale = 1;

static GList *wins;
static int zoom;
static GdkPixmap *icon;
static GdkBitmap *iconmask;
static char **command;

/*
 * Sixteen colours, then the foreground, the background and the cursor.
 * The first is the one a new user gets. They are known by their names,
 * in the settings file too. The last is the user's own, kept there colour
 * by colour, and starts as a copy of the first.
 *
 * The Default sets are Imperaterm's own: the usual hues, their lightness
 * evened out so that each is read on its ground, greys with a trace of
 * violet, and the text's colour for the cursor. In the light one black is
 * a pale grey and white a dark one, so that what a program draws in black
 * on a colour, as htop does its headings, is read there too.
 */
/* The Shades of Purple sets are one set of colours on two backgrounds. */
#define SP(name, bg) { name, { 0x000000, 0xec3a37, 0x3ad900, 0xfad000, 0x7857fe, 0xff2c70, 0x80fcff, 0xffffff, \
    0x5c5c61, 0xec3a37, 0x3ad900, 0xfad000, 0x6943ff, 0xfb94ff, 0x80fcff, 0xffffff, 0xffffff, bg, 0xfad000 } }

enum { NCOL = 19 };  /* the sixteen, then the text, the ground and the cursor */
static struct { const char *name; uint32_t c[NCOL]; } schemes[] = {
    { N_("Default (Dark)"), { 0x212127, 0xfd6761, 0x2cd952, 0xe2c233, 0x6399ff, 0xd879e7, 0x3dd2d9, 0xc3c3cb,
        0x787985, 0xff8780, 0x4ff86c, 0xfcdd5c, 0x89b3ff, 0xed9ffa, 0x69ebf2, 0xf8f8fb,
        0xe0e1e7, 0x000000, 0xe0e1e7 } },
    { N_("Default (Light)"), { 0xe7e7ec, 0xad4542, 0x3a7941, 0x886e34, 0x3760ae, 0x8e4899, 0x34787b, 0x565760,
        0x767681, 0xc84a47, 0x418e4a, 0x9c7f3f, 0x4574cd, 0xa554b2, 0x3e8b8f, 0x1e1e26,
        0x1e1e26, 0xfafafc, 0x1e1e26 } },
    { "Black Metal", { 0x000000, 0x486e6f, 0xdd9999, 0xa06666, 0x888888, 0x999999, 0xaaaaaa, 0xc1c1c1,
        0x404040, 0x486e6f, 0xdd9999, 0xa06666, 0x888888, 0x999999, 0xaaaaaa, 0xc1c1c1,
        0xc1c1c1, 0x000000, 0xc1c1c1 } },
    { "Iceberg (dark)", { 0x1e2132, 0xe27878, 0xb4be82, 0xe2a478, 0x84a0c6, 0xa093c7, 0x89b8c2, 0xc6c8d1,
        0x6b7089, 0xe98989, 0xc0ca8e, 0xe9b189, 0x91acd1, 0xada0d3, 0x95c4ce, 0xd2d4de,
        0xc6c8d1, 0x161821, 0xc6c8d1 } },
    { "Iceberg (light)", { 0xdcdfe7, 0xcc517a, 0x668e3d, 0xc57339, 0x2d539e, 0x7759b4, 0x3f83a6, 0x33374c,
        0x8389a3, 0xcc3768, 0x598030, 0xb6662d, 0x22478e, 0x6845ad, 0x327698, 0x262a3f,
        0x33374c, 0xe8e9ec, 0x33374c } },
    { "Rose Pine", { 0x26233a, 0xeb6f92, 0x31748f, 0xf6c177, 0x9ccfd8, 0xc4a7e7, 0xebbcba, 0xe0def4,
        0x6e6a86, 0xeb6f92, 0x31748f, 0xf6c177, 0x9ccfd8, 0xc4a7e7, 0xebbcba, 0xe0def4,
        0xe0def4, 0x191724, 0xe0def4 } },
    { "Rose Pine Dawn", { 0xf2e9e1, 0xb4637a, 0x286983, 0xea9d34, 0x56949f, 0x907aa9, 0xd7827e, 0x575279,
        0x9893a5, 0xb4637a, 0x286983, 0xea9d34, 0x56949f, 0x907aa9, 0xd7827e, 0x575279,
        0x575279, 0xfaf4ed, 0x575279 } },
    SP("Shades of Purple", 0x1e1e3f),
    SP("Shades of Purple (Super Dark)", 0x15152b),
    { "Amber CRT Retro", { 0x2a1e00, 0xff6a00, 0xffb000, 0xffc742, 0xff8c00, 0xffa033, 0xffd27f, 0xffd9a0,
        0x6a4e00, 0xff8a1e, 0xffc742, 0xffe08a, 0xffab3d, 0xffc266, 0xffe3b0, 0xfff0d6,
        0xffb000, 0x1a1200, 0xffb000 } },
    { "Green Phosphor CRT", { 0x002200, 0x00aa00, 0x33ff33, 0x66ff66, 0x00cc44, 0x00ff88, 0x66ffaa, 0xb6ffb6,
        0x0a5a0a, 0x19cc19, 0x66ff66, 0x99ff99, 0x33ff77, 0x66ffaa, 0x99ffcc, 0xe6ffe6,
        0x33ff33, 0x0b0f0b, 0x33ff33 } },
    { N_("Custom"), { 0 } }
};
#define CUSTOM (LEN(schemes) - 1)

/* Settings. */

static char *
rcfile(int mkdir_too)
{
    const char *x = g_getenv("XDG_CONFIG_HOME");  /* which counts only as a full path */
    char *dir = x && *x == '/' ? g_strconcat(x, "/imperaterm", NULL) : g_strconcat(g_get_home_dir(), "/.config/imperaterm", NULL);
    char *file = g_strconcat(dir, "/imperatermrc", NULL);

    if (mkdir_too) {
        *strrchr(dir, '/') = 0;
        mkdir(dir, 0700);
        dir[strlen(dir)] = '/';
        mkdir(dir, 0700);
    }
    g_free(dir);
    return file;
}

static const struct { const char *key; int *val, min, max; } ints[] = {
    { "size", &cfg.size, 4, 96 }, { "scrollback", &cfg.scrollback, 0, 1000000 }, { "cursor", &cfg.cursor, 0, 2 },
    { "blink", &cfg.blink, 0, 1 }, { "bell", &cfg.bell, 0, 1 }, { "menubar", &cfg.menubar, 0, 1 },
    { "scrollbar", &cfg.scrollbar, 0, 1 }, { "tabsbelow", &cfg.tabsbelow, 0, 1 },
    { "columns", &cfg.cols, 2, 1000 }, { "rows", &cfg.rows, 1, 1000 }
};

/* What the settings file calls the Custom set's colour i. */
static const char *
ckey(int i)
{
    static const char *named[] = { "foreground", "background", "cursorcolour" };
    static char k[16];

    if (i >= 16)
        return named[i - 16];
    g_snprintf(k, sizeof k, "colour%d", i);
    return k;
}

/* A colour as rrggbb, with or without a # before it. */
static int
hex(const char *s, uint32_t *c)
{
    s += *s == '#';
    if (strlen(s) != 6 || strspn(s, "0123456789abcdefABCDEF") != 6)
        return 0;
    *c = strtoul(s, NULL, 16);
    return 1;
}

static void
load(void)
{
    char *file = rcfile(0), line[512], *v;
    FILE *f = fopen(file, "r");
    guint i;

    g_free(file);
    cfg.font = g_strdup("Monospace");
    memcpy(schemes[CUSTOM].c, schemes[0].c, sizeof schemes[0].c);
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!(v = strchr(line, '=')))
            continue;
        *v++ = 0;
        if (!strcmp(line, "font") && *v) {
            g_free(cfg.font);
            cfg.font = g_strdup(v);
        }
        for (i = 0; i < LEN(schemes) && !strcmp(line, "scheme"); i++)
            if (!g_strcasecmp(v, schemes[i].name))
                cfg.scheme = i;
        for (i = 0; i < LEN(ints); i++)
            if (!strcmp(line, ints[i].key))
                *ints[i].val = CLAMP(strtol(v, NULL, 10), ints[i].min, ints[i].max);
        for (i = 0; i < NCOL; i++)
            if (!strcmp(line, ckey(i)))
                hex(v, &schemes[CUSTOM].c[i]);
    }
    if (f)
        fclose(f);
}

static void
save(void)
{
    char *file = rcfile(1);
    FILE *f = fopen(file, "w");
    guint i;

    g_free(file);
    if (!f)
        return;
    fprintf(f, "font=%s\nscheme=%s\n", cfg.font, schemes[cfg.scheme].name);
    for (i = 0; i < LEN(ints); i++)
        fprintf(f, "%s=%d\n", ints[i].key, *ints[i].val);
    for (i = 0; i < NCOL; i++)
        fprintf(f, "%s=#%06x\n", ckey(i), (unsigned)schemes[CUSTOM].c[i]);
    fclose(f);
}

/* The settings take effect in every terminal there is. */
/* A window's tabs are over its terminal or under it, and its menubar and
 * scrollbar are there or not, as the settings have it. */
static void
place(Win *w)
{
    (cfg.menubar ? gtk_widget_show : gtk_widget_hide)(w->menubar);
    (cfg.scrollbar ? gtk_widget_show : gtk_widget_hide)(w->sbar);
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(w->notebook), cfg.tabsbelow ? GTK_POS_BOTTOM : GTK_POS_TOP);
    gtk_box_reorder_child(GTK_BOX(w->notebook->parent), w->notebook, cfg.tabsbelow ? 2 : 1);  /* the menubar is first */
}

/* A window that was made to fit its terminal again keeps its size, as it does otherwise. */
static gboolean
keepsize(gpointer data)
{
    GList *l;

    (void)data;
    for (l = wins; l; l = l->next)
        gtk_window_set_policy(GTK_WINDOW(((Win *)l->data)->window), TRUE, TRUE, FALSE);
    return FALSE;
}

/*
 * The text is larger or smaller, and the windows with it: a terminal
 * keeps its columns and rows, and the programs in it are not told of
 * a new size. Where a window cannot change its size, being maximized
 * or tiled, its terminal has more or fewer of them instead.
 */
static void
refit(void)
{
    GList *l;

    for (l = wins; l; l = l->next) {
        gtk_window_set_policy(GTK_WINDOW(((Win *)l->data)->window), TRUE, TRUE, TRUE);  /* to shrink, too */
        term_refit(l->data);
    }
    g_idle_add_full(GTK_PRIORITY_RESIZE + 1, keepsize, NULL, NULL);  /* once the windows have been sized */
}

static void
apply(void)
{
    static const uint8_t ramp[] = { 0, 95, 135, 175, 215, 255 };
    int i, px = MAX((int)((cfg.size + zoom) * scale * 96 / 72.0 + 0.5), 4), ow = cellw, oh = cellh;
    GList *l;

    for (i = 0; i < 16; i++)
        palette[i] = schemes[cfg.scheme].c[i];
    for (i = 0; i < 216; i++)
        palette[16 + i] = ramp[i / 36] << 16 | ramp[i / 6 % 6] << 8 | ramp[i % 6];
    for (i = 0; i < 24; i++)
        palette[232 + i] = (8 + i * 10) * 0x010101;
    for (i = 0; i < 3; i++)
        palette[C_FG + i] = schemes[cfg.scheme].c[16 + i];

    tab_current();
    if (!r_font(cfg.font, px) && !r_font("monospace", px)) {
        g_printerr(_("imperaterm: no font\n"));
        exit(1);
    }
    if (cellw != ow || cellh != oh)
        refit();
    for (l = tabs; l; l = l->next) {
        Tab *tb = l->data;

        memcpy(tb->vt->pal, palette, sizeof palette);
        tb->vt->alldirty = 1;
        tab_damage(tb);
    }
    for (l = wins; l; l = l->next)
        place(l->data);
}

/* Windows and tabs. */

static Tab *
current(Win *w)
{
    GtkNotebook *nb = GTK_NOTEBOOK(w->notebook);
    GtkWidget *page = gtk_notebook_get_nth_page(nb, gtk_notebook_get_current_page(nb));

    return page ? gtk_object_get_data(GTK_OBJECT(page), "tab") : NULL;
}

/*
 * Who is at the terminal and where, as a shell's prompt has it and as
 * some shells put in the title themselves: user@host: directory, with ~
 * for home. A tab has room for DIRLEN characters of the directory, and of
 * a longer one shows the end, from a slash on, after three dots.
 */
enum { DIRLEN = 24 };

static int
chars(const char *s)
{
    int n = 0;

    for (; *s; s++)
        n += (*s & 0xc0) != 0x80;
    return n;
}

/*
 * Who is at a tab's terminal and where, as a prompt has it. The shell has
 * said, or it is taken to be the user Imperaterm runs as, on this host, where
 * the tab started. On another host the user is not known unless the shell
 * says who, and nor is the home directory.
 */
static char *
whereabouts(Tab *tb, int brief)
{
    const char *dir = tb->dir, *user = tb->user ? tb->user : tb->host ? NULL : g_get_user_name();
    const char *home = NULL, *p, *q;
    struct passwd *pw;
    size_t n;
    char *d, *u, *s, *host = g_strdup(tb->host ? tb->host : host_name());

    host[strcspn(host, ".")] = 0;
    if (!tb->host)
        home = !tb->user || !strcmp(tb->user, g_get_user_name()) ? g_get_home_dir() :
               (pw = getpwnam(tb->user)) ? pw->pw_dir : NULL;
    n = home ? strlen(home) : 0;
    d = n > 1 && !strncmp(dir, home, n) && (!dir[n] || dir[n] == '/') ? g_strconcat("~", dir + n, NULL) : g_strdup(dir);
    p = d;
    if (brief && chars(d) > DIRLEN) {
        while ((q = strchr(p + 1, '/')) && (p = q))
            if (chars(p) + 3 <= DIRLEN)
                break;
        if (p - d <= 3)  /* dots there would save nothing */
            p = d;
    }
    s = g_strdup_printf("%s%s%s: %s%s", user ? user : "", user ? "@" : "", host, p != d ? "..." : "", p);
    u = tab_clean(s);
    g_free(d), g_free(s), g_free(host);
    return u;
}

/*
 * What a tab is called: what the user called it, else the title its child
 * set, else who and where its child is. The window is called what the tab
 * it shows is, and has room for all of it.
 */
void
win_title(Tab *tb)
{
    const char *set = tb->name ? tb->name : tb->title;
    char *u, *s;
    int brief;

    for (brief = 1; brief >= 0; brief--) {
        u = set ? g_strdup(set) : tb->dir ? whereabouts(tb, brief) : NULL;
        s = u ? gtk_im_utf8_to_locale(u) : g_strdup(_("Terminal"));
        if (brief && strcmp(GTK_LABEL(tb->label)->label, s)) {
            gtk_label_set_text(GTK_LABEL(tb->label), s);
            /* A label that wants less room is left with what it had: the notebook is told to lay its tabs out again. */
            gtk_widget_queue_resize(tb->win->notebook);
        }
        else if (current(tb->win) == tb)
            gtk_window_set_title(GTK_WINDOW(tb->win->window), s);
        g_free(u), g_free(s);
    }
}

/* A tab closed: the bar is there for two or more, and the window for one. */
void
win_check(Win *w)
{
    int n = g_list_length(GTK_NOTEBOOK(w->notebook)->children);

    if (!n)
        gtk_widget_destroy(w->window);
    else
        gtk_notebook_set_show_tabs(GTK_NOTEBOOK(w->notebook), n > 1);
}

static void
closed(GtkWidget *button, Tab *tb)
{
    (void)button;
    tab_close(tb);
}

/*
 * A tab's x: a button with no window of its own, as a check button has
 * none. A button's window is filled with the notebook's plain ground, and
 * what a theme draws on a tab does not show through it: a tab of another
 * shade, or a line under the current one. This one is drawn on the
 * notebook's window, over the tab, and only when the pointer is on it or
 * it is held down. A window that is not drawn at all takes the pointer.
 */
typedef struct {
    GtkButton button;
    GdkWindow *in;
} XButton;

static GtkWidgetClass *xparent;

static void
xrealize(GtkWidget *wd)
{
    GtkAllocation *al = &wd->allocation;
    GdkWindowAttr a = { 0 };

    GTK_WIDGET_SET_FLAGS(wd, GTK_REALIZED);
    a.window_type = GDK_WINDOW_CHILD, a.wclass = GDK_INPUT_ONLY;
    a.x = al->x, a.y = al->y, a.width = al->width, a.height = al->height;
    a.event_mask = GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK;
    wd->window = gtk_widget_get_parent_window(wd);
    gdk_window_ref(wd->window);
    ((XButton *)wd)->in = gdk_window_new(wd->window, &a, GDK_WA_X | GDK_WA_Y);
    gdk_window_set_user_data(((XButton *)wd)->in, wd);
    wd->style = gtk_style_attach(wd->style, wd->window);
}

static void
xunrealize(GtkWidget *wd)
{
    gdk_window_set_user_data(((XButton *)wd)->in, NULL);
    gdk_window_destroy(((XButton *)wd)->in);
    ((XButton *)wd)->in = NULL;
    xparent->unrealize(wd);
}

static void
xmap(GtkWidget *wd)
{
    gdk_window_show(((XButton *)wd)->in);
    xparent->map(wd);
}

static void
xunmap(GtkWidget *wd)
{
    gdk_window_hide(((XButton *)wd)->in);
    xparent->unmap(wd);
}

static void
xallocate(GtkWidget *wd, GtkAllocation *al)
{
    wd->allocation = *al;
    if (GTK_WIDGET_REALIZED(wd))
        gdk_window_move_resize(((XButton *)wd)->in, al->x, al->y, al->width, al->height);
    gtk_widget_size_allocate(GTK_BIN(wd)->child, al);
}

/*
 * As a button with no relief is: nothing but its x until the pointer is on
 * it. The box is a pixel short above and below, which keeps it off a line
 * a theme has drawn along the tab's edge.
 */
static void
xpaint(GtkWidget *wd, GdkRectangle *area)
{
    GtkAllocation *al = &wd->allocation;
    GtkStateType st = GTK_WIDGET_STATE(wd);

    if (GTK_WIDGET_DRAWABLE(wd) && (st == GTK_STATE_PRELIGHT || st == GTK_STATE_ACTIVE))
        gtk_paint_box(wd->style, wd->window, st, st == GTK_STATE_ACTIVE ? GTK_SHADOW_IN : GTK_SHADOW_OUT,
                      area, wd, "button", al->x, al->y + 1, al->width, al->height - 2);
}

static void
xdraw(GtkWidget *wd, GdkRectangle *area)
{
    GdkRectangle r;

    xpaint(wd, area);
    if (gtk_widget_intersect(GTK_BIN(wd)->child, area, &r))
        gtk_widget_draw(GTK_BIN(wd)->child, &r);
}

static gint
xexpose(GtkWidget *wd, GdkEventExpose *e)
{
    GdkEventExpose ce = *e;

    xpaint(wd, &e->area);
    if (gtk_widget_intersect(GTK_BIN(wd)->child, &e->area, &ce.area))
        gtk_widget_event(GTK_BIN(wd)->child, (GdkEvent *)&ce);
    return FALSE;
}

static void
xclass(gpointer klass)
{
    GtkWidgetClass *k = klass;

    xparent = gtk_type_class(GTK_TYPE_BUTTON);
    k->realize = xrealize, k->unrealize = xunrealize, k->map = xmap, k->unmap = xunmap;
    k->size_allocate = xallocate, k->draw = xdraw, k->expose_event = xexpose;
}

static void
xinit(gpointer wd, gpointer klass)
{
    (void)klass;
    GTK_WIDGET_SET_FLAGS(wd, GTK_NO_WINDOW);
    GTK_WIDGET_UNSET_FLAGS(wd, GTK_CAN_FOCUS);
}

static GtkWidget *
xbutton(void)
{
    static const GtkTypeInfo info = { "ImperatermTabButton", sizeof(XButton), sizeof(GtkButtonClass),
                                      xclass, xinit, NULL, NULL, NULL };
    static GtkType type;
    GtkWidget *wd;

    if (!type)
        type = gtk_type_unique(GTK_TYPE_BUTTON, &info);
    wd = gtk_type_new(type);
    gtk_button_set_relief(GTK_BUTTON(wd), GTK_RELIEF_NONE);
    gtk_container_add(GTK_CONTAINER(wd), gtk_label_new("x"));
    return wd;
}

static void
named(GtkWidget *d)
{
    Tab *tb = gtk_object_get_data(GTK_OBJECT(d), "tab");
    char *s = gtk_im_locale_to_utf8(gtk_entry_get_text(GTK_ENTRY(gtk_object_get_data(GTK_OBJECT(d), "entry"))), -1);

    g_free(tb->name);
    tb->name = s && *g_strstrip(s) ? tab_clean(s) : NULL;
    g_free(s);
    win_title(tb);
    gtk_widget_destroy(d);
}

static gint
namekey(GtkWidget *d, GdkEventKey *e)
{
    if (e->keyval != GDK_Escape)
        return FALSE;
    gtk_widget_destroy(d);
    return TRUE;
}

/*
 * The user's own name for a tab. It is the tab's while the tab lasts and
 * is kept nowhere; an empty one gives the tab back the name it had.
 */
static void
nametab(Tab *tb)
{
    GtkWidget *d, *box, *entry, *label, *button;
    char *s;

    if (tb->namewin) {
        gdk_window_raise(tb->namewin->window);
        return;
    }
    d = tb->namewin = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(d), _("Rename Tab"));
    gtk_window_set_transient_for(GTK_WINDOW(d), GTK_WINDOW(tb->win->window));
    gtk_signal_connect(GTK_OBJECT(d), "destroy", GTK_SIGNAL_FUNC(gtk_widget_destroyed), &tb->namewin);
    gtk_signal_connect(GTK_OBJECT(d), "key_press_event", GTK_SIGNAL_FUNC(namekey), NULL);
    box = gtk_hbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    entry = gtk_entry_new_with_max_length(80);
    if (tb->name) {
        gtk_entry_set_text(GTK_ENTRY(entry), s = gtk_im_utf8_to_locale(tb->name));
        gtk_entry_select_region(GTK_ENTRY(entry), 0, -1);
        g_free(s);
    }
    gtk_object_set_data(GTK_OBJECT(d), "tab", tb);
    gtk_object_set_data(GTK_OBJECT(d), "entry", entry);
    gtk_signal_connect_object(GTK_OBJECT(entry), "activate", GTK_SIGNAL_FUNC(named), GTK_OBJECT(d));
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new(_("Name:")), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), entry, TRUE, TRUE, 0);
    label = gtk_label_new(_("With no name, the tab names itself."));
    gtk_misc_set_padding(GTK_MISC(label), 8, 0);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(d)->vbox), box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(d)->vbox), label, FALSE, FALSE, 4);
    button = gtk_button_new_with_label(_("OK"));
    gtk_signal_connect_object(GTK_OBJECT(button), "clicked", GTK_SIGNAL_FUNC(named), GTK_OBJECT(d));
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(d)->action_area), button, TRUE, TRUE, 0);
    button = gtk_button_new_with_label(_("Cancel"));
    gtk_signal_connect_object(GTK_OBJECT(button), "clicked", GTK_SIGNAL_FUNC(gtk_widget_destroy), GTK_OBJECT(d));
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(d)->action_area), button, TRUE, TRUE, 0);
    gtk_widget_show_all(d);
    gtk_widget_grab_focus(entry);
}

/*
 * A notebook takes the keyboard when a tab is clicked, and the terminal
 * has it back. The notebook has by now shown the tab that was clicked:
 * the right button has a menu for it, and clicked twice it is renamed.
 */
static Tab *held;  /* the tab the left button went down on, while it is down */

/*
 * A tab taken off the bar is carried, as anything dragged is, and goes
 * to the window it is dropped on: at the place in the bar it is dropped
 * at, or at the end. Dropped on no window of this program, it has a new
 * one of its own, unless its own is all it has. Escape puts it back.
 */
enum { OUT = 12 };  /* how far from the bar the pointer goes before the tab leaves it */

static const GtkTargetEntry tabtarget[] = { { "application/x-imperaterm-tab", GTK_TARGET_SAME_APP, 0 } };
static struct {
    Tab *tb;
    Win *to;  /* the window it was dropped on */
    int n, escaped;
    guint snoop;
    GtkWidget *icon;
} carried;

static gint
escape(GtkWidget *wd, GdkEventKey *e, gpointer data)
{
    (void)wd, (void)data;
    if (e->type == GDK_KEY_PRESS && e->keyval == GDK_Escape)
        carried.escaped = 1;
    return FALSE;
}

static void
carry(GtkWidget *nb, GdkEventMotion *e)
{
    GtkTargetList *list = gtk_target_list_new(tabtarget, 1);
    GdkDragContext *c = gtk_drag_begin(nb, list, GDK_ACTION_MOVE, 1, (GdkEvent *)e);
    GtkWidget *frame = gtk_frame_new(NULL), *label = gtk_label_new(GTK_LABEL(held->label)->label);

    gtk_target_list_unref(list);
    memset(&carried, 0, sizeof carried);
    carried.tb = held, held = NULL;
    carried.snoop = gtk_key_snooper_install(escape, NULL);
    carried.icon = gtk_window_new(GTK_WINDOW_POPUP);  /* what is seen of it on the way: its name */
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_OUT);
    gtk_misc_set_padding(GTK_MISC(label), 8, 3);
    gtk_container_add(GTK_CONTAINER(frame), label);
    gtk_container_add(GTK_CONTAINER(carried.icon), frame);
    gtk_widget_show_all(frame);
    gtk_widget_realize(carried.icon);
    gtk_drag_set_icon_widget(c, carried.icon, -10, -10);
}

/* Dropped on a window: where is noted, and the tab moves when the drag has ended. */
static gint
tabdropped(GtkWidget *wd, GdkDragContext *c, gint x, gint y, guint time, Win *w)
{
    GtkNotebook *nb = GTK_NOTEBOOK(w->notebook);
    GtkAllocation *al = &w->notebook->allocation;
    GList *l;
    int n = 0;

    (void)wd;
    if (!carried.tb)
        return FALSE;
    carried.to = w, carried.n = -1;
    if (nb->show_tabs && y >= al->y && y < al->y + al->height)  /* on the bar: before the tab whose middle it is left of */
        for (l = nb->children; l; l = l->next, n++) {
            GtkNotebookPage *page = l->data;

            if (x < al->x + page->allocation.x + page->allocation.width / 2) {
                carried.n = n;
                break;
            }
        }
    gtk_drag_finish(c, TRUE, FALSE, time);
    return TRUE;
}

static Win *win_new(char **argv, const char *dir, char **env, Tab *tb);
static void movetab(Tab *tb, Win *to, int n);

static void
tabcarried(GtkWidget *nb, GdkDragContext *c, Win *w)
{
    Tab *tb = carried.tb;

    (void)c;
    gtk_key_snooper_remove(carried.snoop);
    gtk_widget_destroy(carried.icon);
    carried.tb = NULL;
    if (!tb || !g_list_find(tabs, tb))  /* its child may have gone on the way */
        return;
    if (carried.to && g_list_find(wins, carried.to)) {
        if (carried.to == w && carried.n > gtk_notebook_page_num(GTK_NOTEBOOK(nb), tb->page))
            carried.n--;  /* the places are counted with it still in its own */
        movetab(tb, carried.to, carried.n);
    } else if (!carried.to && !carried.escaped && g_list_length(GTK_NOTEBOOK(nb)->children) > 1)
        win_new(NULL, NULL, NULL, tb);
}

static gint
tabclicked(GtkWidget *nb, GdkEventButton *e, Win *w)
{
    GtkNotebookPage *page = GTK_NOTEBOOK(nb)->cur_page;
    Tab *tb = current(w);

    gtk_widget_grab_focus(w->gl);
    held = NULL;
    if (e->window != nb->window || !tb || !page ||
        e->x < page->allocation.x || e->x >= page->allocation.x + page->allocation.width ||
        e->y < page->allocation.y || e->y >= page->allocation.y + page->allocation.height)
        return FALSE;
    if (e->type == GDK_BUTTON_PRESS && e->button == 3)
        gtk_menu_popup(GTK_MENU(w->tabmenu), NULL, NULL, NULL, NULL, e->button, e->time);
    else if (e->type == GDK_2BUTTON_PRESS && e->button == 1)
        nametab(tb);
    else if (e->type == GDK_BUTTON_PRESS && e->button == 1)
        held = tb;
    return FALSE;
}

/*
 * A tab dragged along the bar changes places with the one the pointer
 * comes to, once the pointer is where the dragged tab will then be: tabs
 * are of different widths, and one that moved sooner could find the
 * pointer over its neighbour again and move back.
 */
static gint
tabdragged(GtkWidget *nb, GdkEventMotion *e, Win *w)
{
    GtkNotebook *n = GTK_NOTEBOOK(nb);
    GtkNotebookPage *page = n->cur_page, *by;
    GList *l;
    int i;

    if (!held || held != current(w) || !page || e->window != nb->window || !(e->state & GDK_BUTTON1_MASK))
        return FALSE;
    if (e->y < page->allocation.y - OUT || e->y >= page->allocation.y + page->allocation.height + OUT) {
        carry(nb, e);
        return TRUE;
    }
    i = gtk_notebook_page_num(n, held->page);
    l = g_list_nth(n->children, i);
    if (l->next && (by = l->next->data) && e->x >= by->allocation.x && e->x >= page->allocation.x + by->allocation.width)
        gtk_notebook_reorder_child(n, held->page, i + 1);
    else if (l->prev && (by = l->prev->data) && e->x < page->allocation.x && e->x < by->allocation.x + page->allocation.width)
        gtk_notebook_reorder_child(n, held->page, i - 1);
    return FALSE;
}

static void
switched(GtkNotebook *nb, GtkNotebookPage *page, guint n, Win *w)
{
    Tab *tb = current(w);

    (void)nb, (void)page, (void)n;
    if (!tb || GTK_OBJECT_DESTROYED(w->window))
        return;
    win_title(tb);
    tab_show(tb);
}

/* What the bar shows of a tab: its name and its x. */
static GtkWidget *
tablabel(Tab *tb)
{
    GtkWidget *hbox = gtk_hbox_new(FALSE, 4), *button = xbutton();

    tb->label = gtk_label_new(NULL);
    win_title(tb);
    gtk_signal_connect(GTK_OBJECT(button), "clicked", GTK_SIGNAL_FUNC(closed), tb);
    gtk_box_pack_start(GTK_BOX(hbox), tb->label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 0);
    gtk_widget_show_all(hbox);
    return hbox;
}

/* A tab is put in its window's bar, at place n or at the end if that is -1, and shown. */
static void
puttab(Tab *tb, int n)
{
    Win *w = tb->win;
    GtkNotebook *nb = GTK_NOTEBOOK(w->notebook);

    gtk_notebook_insert_page(nb, tb->page, tablabel(tb), n);
    gtk_notebook_set_show_tabs(nb, g_list_length(nb->children) > 1);
    gtk_notebook_set_page(nb, gtk_notebook_page_num(nb, tb->page));
    tab_show(tb);
    gtk_widget_grab_focus(w->gl);
}

/*
 * A new tab, which runs argv or the shell. It starts where the tab that
 * is shown is, and with the environment Imperaterm has, unless it is told
 * otherwise.
 */
static void
addtab(Win *w, char **argv, const char *dir, char **env)
{
    Tab *cur = current(w), *tb;
    char *cwd = dir ? g_strdup(dir) : cur ? tab_cwd(cur) : NULL;

    tb = tab_new(w, cwd, argv, env);
    g_free(cwd);
    if (!tb)
        return;
    gtk_widget_show_all(tb->page);
    puttab(tb, -1);
}

/*
 * A tab goes to another window, or to another place in its own. Its
 * child knows nothing of it, but for the size of its terminal if the
 * windows differ.
 */
static void
movetab(Tab *tb, Win *to, int n)
{
    Win *from = tb->win;

    if (from == to) {
        if (n >= 0)
            gtk_notebook_reorder_child(GTK_NOTEBOOK(to->notebook), tb->page, n);
        return;
    }
    if (tb->namewin)
        gtk_widget_destroy(tb->namewin);
    tab_leave(tb);
    gtk_widget_ref(tb->page);
    gtk_container_remove(GTK_CONTAINER(from->notebook), tb->page);  /* the label goes with it */
    tb->win = to;
    puttab(tb, n);
    gtk_widget_unref(tb->page);
    win_check(from);
}

static void about(Win *w);
static void prefs(Win *w);
static GtkWidget *pfmenubar, *pfscrollbar;  /* Preferences' check boxes, while it is open */
static void prefs_says(GtkWidget *button, int *val);

static void
act(gpointer data, guint action, GtkWidget *item)
{
    Win *w = data;
    Tab *tb = current(w);
    GtkNotebook *nb = GTK_NOTEBOOK(w->notebook);
    int n = gtk_notebook_get_current_page(nb), last = g_list_length(nb->children) - 1;
    GList *l;

    (void)item;
    switch (action) {
    case NEWTAB: addtab(w, NULL, NULL, NULL); break;
    case NEWWIN: win_new(NULL, NULL, NULL, NULL); break;
    case CLOSETAB:
        if (tb)
            tab_close(tb);
        break;
    case CLOSEWIN: gtk_widget_destroy(w->window); break;
    case COPY:
        if (tb)
            tab_copy(tb);
        break;
    case PASTE:
        if (tb)
            tab_paste(tb, GDK_NONE, GDK_CURRENT_TIME);
        break;
    case RESET:
        if (tb) {
            vt_reset(tb->vt);
            tab_damage(tb);
        }
        break;
    case PREFS: prefs(w); break;
    case MENUBAR: case SCROLLBAR:  /* in every window, and kept */
        if (action == MENUBAR)
            cfg.menubar = !cfg.menubar;
        else
            cfg.scrollbar = !cfg.scrollbar;
        for (l = wins; l; l = l->next) {
            (cfg.menubar ? gtk_widget_show : gtk_widget_hide)(((Win *)l->data)->menubar);
            (cfg.scrollbar ? gtk_widget_show : gtk_widget_hide)(((Win *)l->data)->sbar);
        }
        prefs_says(pfmenubar, &cfg.menubar);  /* Preferences, if it is open, says so too */
        prefs_says(pfscrollbar, &cfg.scrollbar);
        save();
        break;
    case ZOOMIN: case ZOOMOUT: case ZOOMOFF:
        zoom = action == ZOOMOFF ? 0 : CLAMP(zoom + (action == ZOOMIN ? 1 : -1), 4 - cfg.size, 96 - cfg.size);
        apply();
        break;
    case PREVTAB: gtk_notebook_set_page(nb, n > 0 ? n - 1 : last); break;
    case NEXTTAB: gtk_notebook_set_page(nb, n < last ? n + 1 : 0); break;
    case TABLEFT: case TABRIGHT:
        if (tb)
            gtk_notebook_reorder_child(nb, tb->page, CLAMP(n + (action == TABLEFT ? -1 : 1), 0, last));
        break;
    case NAMETAB:
        if (tb)
            nametab(tb);
        break;
    case ABOUT: about(w); break;
    }
}

#define ACT(a) (GtkItemFactoryCallback)act, a
static GtkItemFactoryEntry menu[] = {
    { N_("/_File"), NULL, NULL, 0, "<Branch>" },
    { N_("/File/New _Tab"), "<control><shift>T", ACT(NEWTAB), NULL },
    { N_("/File/New _Window"), "<control><shift>N", ACT(NEWWIN), NULL },
    { "/File/sep", NULL, NULL, 0, "<Separator>" },
    { N_("/File/_Close Tab"), "<control><shift>W", ACT(CLOSETAB), NULL },
    { N_("/File/Close W_indow"), "<control><shift>Q", ACT(CLOSEWIN), NULL },
    { N_("/_Edit"), NULL, NULL, 0, "<Branch>" },
    { N_("/Edit/_Copy"), "<control><shift>C", ACT(COPY), NULL },
    { N_("/Edit/_Paste"), "<control><shift>V", ACT(PASTE), NULL },
    { "/Edit/sep", NULL, NULL, 0, "<Separator>" },
    { N_("/Edit/_Reset"), NULL, ACT(RESET), NULL },
    { "/Edit/sep2", NULL, NULL, 0, "<Separator>" },
    { N_("/Edit/Pr_eferences..."), NULL, ACT(PREFS), NULL },
    { N_("/_View"), NULL, NULL, 0, "<Branch>" },
    { N_("/View/_Menubar"), NULL, ACT(MENUBAR), NULL },
    { N_("/View/_Scrollbar"), NULL, ACT(SCROLLBAR), NULL },
    { "/View/sep", NULL, NULL, 0, "<Separator>" },
    { N_("/View/Zoom _In"), "<control>plus", ACT(ZOOMIN), NULL },
    { N_("/View/Zoom _Out"), "<control>minus", ACT(ZOOMOUT), NULL },
    { N_("/View/_Normal Size"), "<control>0", ACT(ZOOMOFF), NULL },
    { N_("/_Tabs"), NULL, NULL, 0, "<Branch>" },
    { N_("/Tabs/_Previous Tab"), "<control>Page_Up", ACT(PREVTAB), NULL },
    { N_("/Tabs/_Next Tab"), "<control>Page_Down", ACT(NEXTTAB), NULL },
    { "/Tabs/sep", NULL, NULL, 0, "<Separator>" },
    { N_("/Tabs/Move Tab _Left"), "<control><shift>Page_Up", ACT(TABLEFT), NULL },
    { N_("/Tabs/Move Tab _Right"), "<control><shift>Page_Down", ACT(TABRIGHT), NULL },
    { "/Tabs/sep2", NULL, NULL, 0, "<Separator>" },
    { N_("/Tabs/Rename _Tab..."), NULL, ACT(NAMETAB), NULL },
    { N_("/_Help"), NULL, NULL, 0, "<Branch>" },
    { N_("/Help/_About"), NULL, ACT(ABOUT), NULL }
};

static GtkItemFactoryEntry popup[] = {
    { N_("/Copy"), NULL, ACT(COPY), NULL },
    { N_("/Paste"), NULL, ACT(PASTE), NULL },
    { "/sep", NULL, NULL, 0, "<Separator>" },
    { N_("/New Tab"), NULL, ACT(NEWTAB), NULL },
    { N_("/New Window"), NULL, ACT(NEWWIN), NULL },
    { N_("/Close Tab"), NULL, ACT(CLOSETAB), NULL },
    { N_("/Rename Tab..."), NULL, ACT(NAMETAB), NULL },
    { "/sep2", NULL, NULL, 0, "<Separator>" },
    { N_("/Menubar"), NULL, ACT(MENUBAR), NULL },
    { N_("/Scrollbar"), NULL, ACT(SCROLLBAR), NULL },
    { N_("/Preferences..."), NULL, ACT(PREFS), NULL }
};

/* The right button's on a tab, which is the one shown by then. */
static GtkItemFactoryEntry tabmenu[] = {
    { N_("/Rename Tab..."), NULL, ACT(NAMETAB), NULL },
    { "/sep", NULL, NULL, 0, "<Separator>" },
    { N_("/Close Tab"), NULL, ACT(CLOSETAB), NULL }
};

/*
 * The keys that are the window's and not the child's. The terminal has
 * the focus, so it asks here first; the menu shows the same keys.
 */
int
win_key(Win *w, GdkEventKey *e)
{
    static const struct { guint key, mods; int action; } keys[] = {
        { 't', GDK_CONTROL_MASK | GDK_SHIFT_MASK, NEWTAB }, { 'n', GDK_CONTROL_MASK | GDK_SHIFT_MASK, NEWWIN },
        { 'w', GDK_CONTROL_MASK | GDK_SHIFT_MASK, CLOSETAB }, { 'q', GDK_CONTROL_MASK | GDK_SHIFT_MASK, CLOSEWIN },
        { 'c', GDK_CONTROL_MASK | GDK_SHIFT_MASK, COPY }, { 'v', GDK_CONTROL_MASK | GDK_SHIFT_MASK, PASTE },
        { GDK_Page_Up, GDK_CONTROL_MASK, PREVTAB }, { GDK_Page_Down, GDK_CONTROL_MASK, NEXTTAB },
        { GDK_Page_Up, GDK_CONTROL_MASK | GDK_SHIFT_MASK, TABLEFT },
        { GDK_Page_Down, GDK_CONTROL_MASK | GDK_SHIFT_MASK, TABRIGHT },
        { '0', GDK_CONTROL_MASK, ZOOMOFF }, { '-', GDK_CONTROL_MASK, ZOOMOUT }, { GDK_KP_Subtract, GDK_CONTROL_MASK, ZOOMOUT },
        { '+', GDK_CONTROL_MASK, ZOOMIN }, { '=', GDK_CONTROL_MASK, ZOOMIN }, { GDK_KP_Add, GDK_CONTROL_MASK, ZOOMIN },
        { '+', GDK_CONTROL_MASK | GDK_SHIFT_MASK, ZOOMIN }
    };
    guint k = gdk_keyval_to_lower(e->keyval), m = e->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_MOD1_MASK), i;

    for (i = 0; i < LEN(keys); i++)
        if (keys[i].key == k && keys[i].mods == m) {
            act(w, keys[i].action, NULL);
            return 1;
        }
    return 0;
}

void
win_popup(Win *w, GdkEventButton *e)
{
    gtk_menu_popup(GTK_MENU(w->popup), NULL, NULL, NULL, NULL, e->button, e->time);
}

static void
gone(GtkWidget *window, Win *w)
{
    (void)window;
    wins = g_list_remove(wins, w);
    if (!wins)
        gtk_main_quit();
}

/* A menu item's path in the user's language: only its last part is shown. */
static gchar *
translate(const gchar *path, gpointer data)
{
    (void)data;
    return (gchar *)_(path);
}

/* A window, with one tab: a new one, as addtab() makes it, or tb, from another window. */
static Win *
win_new(char **argv, const char *dir, char **env, Tab *tb)
{
    Win *w = g_new0(Win, 1);
    GtkAccelGroup *accel = gtk_accel_group_new();
    GtkItemFactory *bar = gtk_item_factory_new(GTK_TYPE_MENU_BAR, "<main>", accel);
    GtkItemFactory *pop = gtk_item_factory_new(GTK_TYPE_MENU, "<popup>", NULL);
    GtkItemFactory *tab = gtk_item_factory_new(GTK_TYPE_MENU, "<tab>", NULL);
    GtkWidget *vbox = gtk_vbox_new(FALSE, 0);

    gtk_item_factory_set_translate_func(bar, translate, NULL, NULL);
    gtk_item_factory_set_translate_func(pop, translate, NULL, NULL);
    gtk_item_factory_set_translate_func(tab, translate, NULL, NULL);
    w->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(w->window), _("Terminal"));
    gtk_window_set_wmclass(GTK_WINDOW(w->window), "imperaterm", "Imperaterm");
    gtk_window_set_policy(GTK_WINDOW(w->window), TRUE, TRUE, FALSE);
    gtk_signal_connect(GTK_OBJECT(w->window), "destroy", GTK_SIGNAL_FUNC(gone), w);
    gtk_item_factory_create_items(bar, LEN(menu), menu, w);
    gtk_item_factory_create_items(pop, LEN(popup), popup, w);
    gtk_item_factory_create_items(tab, LEN(tabmenu), tabmenu, w);
    gtk_window_add_accel_group(GTK_WINDOW(w->window), accel);
    gtk_accel_group_unref(accel);
    /* These go when the window has gone, its tabs before it. */
    gtk_object_set_data_full(GTK_OBJECT(w->window), "bar", bar, (GtkDestroyNotify)gtk_object_unref);
    gtk_object_set_data_full(GTK_OBJECT(w->window), "popup", pop, (GtkDestroyNotify)gtk_object_unref);
    gtk_object_set_data_full(GTK_OBJECT(w->window), "tabmenu", tab, (GtkDestroyNotify)gtk_object_unref);
    gtk_object_set_data_full(GTK_OBJECT(w->window), "win", w, g_free);
    w->menubar = gtk_item_factory_get_widget(bar, "<main>");
    w->popup = gtk_item_factory_get_widget(pop, "<popup>");
    w->tabmenu = gtk_item_factory_get_widget(tab, "<tab>");

    w->notebook = gtk_notebook_new();
    GTK_WIDGET_UNSET_FLAGS(w->notebook, GTK_CAN_FOCUS);
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(w->notebook), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(w->notebook), FALSE);
    gtk_signal_connect_after(GTK_OBJECT(w->notebook), "switch_page", GTK_SIGNAL_FUNC(switched), w);
    gtk_signal_connect_after(GTK_OBJECT(w->notebook), "button_press_event", GTK_SIGNAL_FUNC(tabclicked), w);
    gtk_widget_add_events(w->notebook, GDK_BUTTON1_MOTION_MASK);
    gtk_signal_connect(GTK_OBJECT(w->notebook), "motion_notify_event", GTK_SIGNAL_FUNC(tabdragged), w);
    gtk_signal_connect(GTK_OBJECT(w->notebook), "drag_end", GTK_SIGNAL_FUNC(tabcarried), w);
    gtk_drag_dest_set(w->window, GTK_DEST_DEFAULT_MOTION, tabtarget, 1, GDK_ACTION_MOVE);
    gtk_signal_connect(GTK_OBJECT(w->window), "drag_drop", GTK_SIGNAL_FUNC(tabdropped), w);
    gtk_container_add(GTK_CONTAINER(w->window), vbox);
    gtk_box_pack_start(GTK_BOX(vbox), w->menubar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), w->notebook, FALSE, FALSE, 0);  /* the tabs alone: its pages are empty */
    gtk_box_pack_start(GTK_BOX(vbox), term_new(w), TRUE, TRUE, 0);
    place(w);
    gtk_widget_show_all(vbox);
    if (!cfg.menubar)
        gtk_widget_hide(w->menubar);
    if (!cfg.scrollbar)
        gtk_widget_hide(w->sbar);
    wins = g_list_append(wins, w);
    if (tb) {
        int x, y;

        gdk_window_get_pointer(NULL, &x, &y, NULL);  /* where it was dropped, where the window system lets a window say */
        gtk_widget_set_uposition(w->window, MAX(x - 40, 0), MAX(y - 12, 0));
        movetab(tb, w, -1);
    } else
        addtab(w, argv, dir, env);
    if (!current(w)) {
        gtk_widget_destroy(w->window);
        return NULL;
    }
    /*
     * The icon as a pixmap, for a window manager that knows no other, and for
     * when Imperaterm is not installed where icons are looked for. A Wayland
     * compositor has none from the program: it takes the one the desktop file
     * names.
     */
    gtk_widget_realize(w->window);
    if (!icon)
        icon = gdk_pixmap_create_from_xpm_d(w->window->window, &iconmask, NULL, imperaterm_xpm);
    gdk_window_set_icon(w->window->window, NULL, icon, iconmask);
    gtk_widget_show(w->window);
    return w;
}

/*
 * Preferences. Each control is its setting: a change is in effect, and in
 * the file, as it is made.
 */

static GtkWidget *pfwin;
static GList *families;

static void
changed(void)
{
    save();
    apply();
}

static void
picked(GtkWidget *item, gpointer val)
{
    *(int *)val = GPOINTER_TO_INT(gtk_object_get_data(GTK_OBJECT(item), "n"));
    changed();
}

static void
toggled(GtkWidget *button, gpointer val)
{
    *(int *)val = GTK_TOGGLE_BUTTON(button)->active;
    changed();
}

static void
spun(GtkAdjustment *adj, gpointer val)
{
    *(int *)val = (int)(adj->value + 0.5);
    if (val == &cfg.size)
        zoom = 0;
    changed();
}

/* A font is taken once its name is one there is, and Enter takes whatever is typed. */
static void
fontset(GtkWidget *entry, gpointer typed)
{
    char *font = gtk_im_locale_to_utf8(gtk_entry_get_text(GTK_ENTRY(entry)), -1);
    GList *l = families;

    while (!typed && l && g_strcasecmp(l->data, font ? font : ""))
        l = l->next;
    if (font && *font && strcmp(font, cfg.font) && (typed || l)) {
        g_free(cfg.font);
        cfg.font = font;
        changed();
    } else
        g_free(font);
}

static GtkWidget *
option(const char **names, int n, int active, GtkSignalFunc func, gpointer data)
{
    GtkWidget *o = gtk_option_menu_new(), *m = gtk_menu_new(), *item;
    int i;

    for (i = 0; i < n; i++) {
        gtk_menu_append(GTK_MENU(m), item = gtk_menu_item_new_with_label(_(names[i])));
        gtk_object_set_data(GTK_OBJECT(item), "n", GINT_TO_POINTER(i));
        gtk_signal_connect(GTK_OBJECT(item), "activate", func, data);
    }
    gtk_widget_show_all(m);
    gtk_option_menu_set_menu(GTK_OPTION_MENU(o), m);
    gtk_option_menu_set_history(GTK_OPTION_MENU(o), active);
    return o;
}

static GtkWidget *
spin(int *val, int min, int max, int step, int page)
{
    GtkObject *adj = gtk_adjustment_new(*val, min, max, step, page, 0);

    gtk_signal_connect(adj, "value_changed", GTK_SIGNAL_FUNC(spun), val);
    return gtk_spin_button_new(GTK_ADJUSTMENT(adj), 1, 0);
}

/* A check box of Preferences shows a setting changed elsewhere. */
static void
prefs_says(GtkWidget *button, int *val)
{
    if (!button)
        return;
    gtk_signal_handler_block_by_data(GTK_OBJECT(button), val);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), *val);
    gtk_signal_handler_unblock_by_data(GTK_OBJECT(button), val);
}

static GtkWidget *
check(const char *name, int *val)
{
    GtkWidget *button = gtk_check_button_new_with_label(name);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), *val);
    gtk_signal_connect(GTK_OBJECT(button), "toggled", GTK_SIGNAL_FUNC(toggled), val);
    return button;
}

/*
 * The Custom set's editor. Each colour is a swatch, which opens the colour
 * selector, and its rrggbb, which can be typed.
 */

static GtkWidget *edwin, *edarea[NCOL], *edentry[NCOL];
static int edbusy;  /* an entry is being filled in, not typed in */

static void
edshow(int i)
{
    char s[8];

    g_snprintf(s, sizeof s, "%06x", (unsigned)schemes[CUSTOM].c[i]);
    edbusy = 1;
    gtk_entry_set_text(GTK_ENTRY(edentry[i]), s);
    edbusy = 0;
    gtk_widget_queue_draw(edarea[i]);
}

static gint
edexpose(GtkWidget *area, GdkEventExpose *e, gpointer i)
{
    uint32_t v = schemes[CUSTOM].c[GPOINTER_TO_INT(i)];
    GdkGC *gc = gdk_gc_new(area->window);
    GdkColor c;

    (void)e;
    c.red = (v >> 16) * 0x101, c.green = (v >> 8 & 255) * 0x101, c.blue = (v & 255) * 0x101;
    gdk_color_alloc(gtk_widget_get_colormap(area), &c);
    gdk_gc_set_foreground(gc, &c);
    gdk_draw_rectangle(area->window, gc, TRUE, 0, 0, area->allocation.width, area->allocation.height);
    gdk_gc_unref(gc);
    return TRUE;
}

static void
edtyped(GtkWidget *entry, gpointer i)
{
    char *s = gtk_entry_get_text(GTK_ENTRY(entry));

    if (!edbusy && hex(s, &schemes[CUSTOM].c[GPOINTER_TO_INT(i)])) {
        gtk_widget_queue_draw(edarea[GPOINTER_TO_INT(i)]);
        changed();
    }
}

/* The selector's colour shows in the terminals as it moves, and is kept at OK. */
static void
edmoved(GtkColorSelection *sel, gpointer i)
{
    gdouble c[4];

    gtk_color_selection_get_color(sel, c);
    schemes[CUSTOM].c[GPOINTER_TO_INT(i)] = (int)(c[0] * 255 + 0.5) << 16 | (int)(c[1] * 255 + 0.5) << 8 | (int)(c[2] * 255 + 0.5);
    edshow(GPOINTER_TO_INT(i));
    apply();
}

static void
edcancel(GtkWidget *sel)
{
    int i = GPOINTER_TO_INT(gtk_object_get_data(GTK_OBJECT(sel), "i"));

    schemes[CUSTOM].c[i] = GPOINTER_TO_UINT(gtk_object_get_data(GTK_OBJECT(sel), "was"));
    edshow(i);
    gtk_widget_destroy(sel);
}

static void
edpick(GtkWidget *button, gpointer i)
{
    GtkWidget *d = gtk_color_selection_dialog_new(_("Colour"));
    GtkColorSelectionDialog *cs = GTK_COLOR_SELECTION_DIALOG(d);
    uint32_t v = schemes[CUSTOM].c[GPOINTER_TO_INT(i)];
    gdouble c[4] = { 0, 0, 0, 1 };

    (void)button;
    c[0] = (v >> 16) / 255.0, c[1] = (v >> 8 & 255) / 255.0, c[2] = (v & 255) / 255.0;
    gtk_color_selection_set_color(GTK_COLOR_SELECTION(cs->colorsel), c);
    gtk_object_set_data(GTK_OBJECT(d), "i", i);
    gtk_object_set_data(GTK_OBJECT(d), "was", GUINT_TO_POINTER(v));
    gtk_signal_connect(GTK_OBJECT(cs->colorsel), "color_changed", GTK_SIGNAL_FUNC(edmoved), i);
    gtk_signal_connect_object(GTK_OBJECT(cs->ok_button), "clicked", GTK_SIGNAL_FUNC(gtk_widget_destroy), GTK_OBJECT(d));
    gtk_signal_connect_object(GTK_OBJECT(cs->cancel_button), "clicked", GTK_SIGNAL_FUNC(edcancel), GTK_OBJECT(d));
    gtk_signal_connect(GTK_OBJECT(d), "destroy", GTK_SIGNAL_FUNC(changed), NULL);
    gtk_widget_hide(cs->help_button);
    gtk_window_set_transient_for(GTK_WINDOW(d), GTK_WINDOW(edwin));
    gtk_window_set_modal(GTK_WINDOW(d), TRUE);
    gtk_widget_show(d);
}

/* Custom becomes a copy of the set picked. The first item of that menu is its name. */
static void
edcopy(GtkWidget *item, gpointer data)
{
    int i, n = GPOINTER_TO_INT(gtk_object_get_data(GTK_OBJECT(item), "n")) - 1;

    (void)data;
    if (n < 0)
        return;
    memcpy(schemes[CUSTOM].c, schemes[n].c, sizeof schemes[n].c);
    for (i = 0; i < NCOL; i++)
        edshow(i);
    changed();
}

static GtkWidget *
edcell(int i)
{
    GtkWidget *box = gtk_hbox_new(FALSE, 4), *button = gtk_button_new();

    edarea[i] = gtk_drawing_area_new();
    gtk_drawing_area_size(GTK_DRAWING_AREA(edarea[i]), 28, 14);
    gtk_signal_connect(GTK_OBJECT(edarea[i]), "expose_event", GTK_SIGNAL_FUNC(edexpose), GINT_TO_POINTER(i));
    gtk_container_add(GTK_CONTAINER(button), edarea[i]);
    gtk_signal_connect(GTK_OBJECT(button), "clicked", GTK_SIGNAL_FUNC(edpick), GINT_TO_POINTER(i));
    edentry[i] = gtk_entry_new_with_max_length(7);
    gtk_widget_set_usize(edentry[i], 64, -1);
    gtk_signal_connect(GTK_OBJECT(edentry[i]), "changed", GTK_SIGNAL_FUNC(edtyped), GINT_TO_POINTER(i));
    gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), edentry[i], TRUE, TRUE, 0);
    edshow(i);
    return box;
}

static void
editor(void)
{
    static const char *rows[] = { N_("Black"), N_("Red"), N_("Green"), N_("Yellow"), N_("Blue"), N_("Magenta"),
                                  N_("Cyan"), N_("White"), N_("Text"), N_("Background"), N_("Cursor") };
    const char *names[LEN(schemes)];
    GtkWidget *table, *label, *button;
    guint i;

    if (edwin) {
        gdk_window_raise(edwin->window);
        return;
    }
    edwin = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(edwin), _("Custom Colours"));
    gtk_window_set_transient_for(GTK_WINDOW(edwin), GTK_WINDOW(pfwin));
    gtk_signal_connect(GTK_OBJECT(edwin), "destroy", GTK_SIGNAL_FUNC(gtk_widget_destroyed), &edwin);
    table = gtk_table_new(13, 3, FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(table), 8);
    gtk_table_set_row_spacings(GTK_TABLE(table), 4);
    gtk_table_set_col_spacings(GTK_TABLE(table), 8);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(edwin)->vbox), table, TRUE, TRUE, 0);

    gtk_table_attach(GTK_TABLE(table), gtk_label_new(_("Normal")), 1, 2, 0, 1, GTK_FILL, 0, 0, 0);
    gtk_table_attach(GTK_TABLE(table), gtk_label_new(_("Bright")), 2, 3, 0, 1, GTK_FILL, 0, 0, 0);
    for (i = 0; i < LEN(rows); i++) {
        label = gtk_label_new(_(rows[i]));
        gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
        gtk_table_attach(GTK_TABLE(table), label, 0, 1, i + 1, i + 2, GTK_FILL, 0, 0, 0);
        gtk_table_attach(GTK_TABLE(table), edcell(i < 8 ? i : i + 8), 1, 2, i + 1, i + 2, GTK_FILL, 0, 0, 0);
        if (i < 8)
            gtk_table_attach(GTK_TABLE(table), edcell(i + 8), 2, 3, i + 1, i + 2, GTK_FILL, 0, 0, 0);
    }
    names[0] = N_("Copy from...");
    for (i = 0; i < CUSTOM; i++)
        names[i + 1] = schemes[i].name;
    gtk_table_attach(GTK_TABLE(table), option(names, LEN(schemes), 0, GTK_SIGNAL_FUNC(edcopy), NULL),
                     1, 3, 12, 13, GTK_FILL, 0, 0, 0);

    button = gtk_button_new_with_label(_("Close"));
    gtk_signal_connect_object(GTK_OBJECT(button), "clicked", GTK_SIGNAL_FUNC(gtk_widget_destroy), GTK_OBJECT(edwin));
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(edwin)->action_area), button, TRUE, TRUE, 0);
    gtk_widget_show_all(edwin);
}

/* Custom is chosen, since what is being edited ought to be what is seen. */
static void
custom(GtkWidget *button, gpointer sets)
{
    (void)button;
    cfg.scheme = CUSTOM;
    gtk_option_menu_set_history(GTK_OPTION_MENU(sets), CUSTOM);
    changed();
    editor();
}

static void
edgone(void)
{
    if (edwin)
        gtk_widget_destroy(edwin);
}

static void
row(GtkWidget *table, int y, const char *name, GtkWidget *wd)
{
    GtkWidget *label = gtk_label_new(name ? name : "");

    gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
    gtk_table_attach(GTK_TABLE(table), label, 0, 1, y, y + 1, GTK_FILL, 0, 0, 0);
    gtk_table_attach(GTK_TABLE(table), wd, 1, 2, y, y + 1, GTK_EXPAND | GTK_FILL, 0, 0, 0);
}

static void
prefs(Win *w)
{
    static const char *cursors[] = { N_("Block"), N_("Underline"), N_("Bar") };
    const char *names[LEN(schemes)];
    GtkWidget *table, *font, *entry, *button, *box, *sets;
    GList *l;
    guint i;

    if (pfwin) {
        gdk_window_raise(pfwin->window);
        return;
    }
    pfwin = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(pfwin), _("Terminal Preferences"));
    gtk_window_set_transient_for(GTK_WINDOW(pfwin), GTK_WINDOW(w->window));
    gtk_signal_connect(GTK_OBJECT(pfwin), "destroy", GTK_SIGNAL_FUNC(gtk_widget_destroyed), &pfwin);
    gtk_signal_connect(GTK_OBJECT(pfwin), "destroy", GTK_SIGNAL_FUNC(edgone), NULL);
    table = gtk_table_new(10, 2, FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(table), 8);
    gtk_table_set_row_spacings(GTK_TABLE(table), 6);
    gtk_table_set_col_spacings(GTK_TABLE(table), 8);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(pfwin)->vbox), table, TRUE, TRUE, 0);

    for (l = families; l; l = l->next)
        g_free(l->data);
    g_list_free(families);
    font = gtk_combo_new();
    entry = GTK_COMBO(font)->entry;
    if ((families = r_families()))
        gtk_combo_set_popdown_strings(GTK_COMBO(font), families);
    gtk_combo_disable_activate(GTK_COMBO(font));
    gtk_entry_set_text(GTK_ENTRY(entry), cfg.font);
    gtk_signal_connect(GTK_OBJECT(entry), "changed", GTK_SIGNAL_FUNC(fontset), NULL);
    gtk_signal_connect(GTK_OBJECT(entry), "activate", GTK_SIGNAL_FUNC(fontset), entry);
    row(table, 0, _("Font:"), font);
    row(table, 1, _("Size:"), spin(&cfg.size, 4, 96, 1, 4));
    for (i = 0; i < LEN(schemes); i++)
        names[i] = schemes[i].name;
    box = gtk_hbox_new(FALSE, 4);
    sets = option(names, LEN(schemes), cfg.scheme, GTK_SIGNAL_FUNC(picked), &cfg.scheme);
    gtk_box_pack_start(GTK_BOX(box), sets, TRUE, TRUE, 0);
    button = gtk_button_new_with_label(_("Custom..."));
    gtk_signal_connect(GTK_OBJECT(button), "clicked", GTK_SIGNAL_FUNC(custom), sets);
    gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
    row(table, 2, _("Colours:"), box);
    row(table, 3, _("Cursor:"), option(cursors, 3, cfg.cursor, GTK_SIGNAL_FUNC(picked), &cfg.cursor));
    row(table, 4, NULL, check(_("Blinking cursor"), &cfg.blink));
    row(table, 5, _("Scrollback lines:"), spin(&cfg.scrollback, 0, 1000000, 100, 1000));
    row(table, 6, NULL, check(_("Audible bell"), &cfg.bell));
    row(table, 7, NULL, check(_("Tabs at the bottom"), &cfg.tabsbelow));
    pfmenubar = check(_("Menubar"), &cfg.menubar);
    gtk_signal_connect(GTK_OBJECT(pfmenubar), "destroy", GTK_SIGNAL_FUNC(gtk_widget_destroyed), &pfmenubar);
    row(table, 8, NULL, pfmenubar);
    pfscrollbar = check(_("Scrollbar"), &cfg.scrollbar);
    gtk_signal_connect(GTK_OBJECT(pfscrollbar), "destroy", GTK_SIGNAL_FUNC(gtk_widget_destroyed), &pfscrollbar);
    row(table, 9, NULL, pfscrollbar);

    button = gtk_button_new_with_label(_("Close"));
    gtk_signal_connect_object(GTK_OBJECT(button), "clicked", GTK_SIGNAL_FUNC(gtk_widget_destroy), GTK_OBJECT(pfwin));
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(pfwin)->action_area), button, TRUE, TRUE, 0);
    gtk_widget_show_all(pfwin);
}

static void
about(Win *w)
{
    GtkWidget *d = gtk_dialog_new(), *label, *button, *pix = gtk_pixmap_new(icon, iconmask);
    char *s = g_strdup_printf("%s " VERSION "\n\n%s\n%s %s\n\nCopyright (C) 2026 The CGTK authors\n%s",
                              "Imperaterm", _("A terminal emulator for CGTK, drawn with OpenGL."),
                              _("GL Version String:"), r_version(),
                              _("The terminal's logic derives from st, (C) the st engineers."));

    gtk_window_set_title(GTK_WINDOW(d), _("About Imperaterm"));
    gtk_window_set_transient_for(GTK_WINDOW(d), GTK_WINDOW(w->window));
    label = gtk_label_new(s);
    g_free(s);
    gtk_misc_set_padding(GTK_MISC(label), 16, 8);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(d)->vbox), pix, FALSE, FALSE, 12);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(d)->vbox), label, TRUE, TRUE, 0);
    button = gtk_button_new_with_label(_("Close"));
    gtk_signal_connect_object(GTK_OBJECT(button), "clicked", GTK_SIGNAL_FUNC(gtk_widget_destroy), GTK_OBJECT(d));
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(d)->action_area), button, TRUE, TRUE, 0);
    gtk_widget_show_all(d);
}

/*
 * An Imperaterm was started while this one ran, and has handed over what it
 * was to run: that has a window here, and the other waits for its tab
 * to end.
 */
static int
joined(char **argv, const char *dir, char **env, int waiter)
{
    Win *w = win_new(argv, dir, env, NULL);

    if (!w)
        return 0;
    current(w)->waiter = waiter;
    return 1;
}

int
main(int argc, char **argv)
{
    int i, own = 0, status = 0;

    setlocale(LC_ALL, "");
#ifdef ENABLE_NLS
    bindtextdomain(PACKAGE, LOCALEDIR);
    textdomain(PACKAGE);
#endif
    for (i = 1; i < argc && strcmp(argv[i], "-e"); i++)  /* these need no display */
        if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-v")) {
            puts("imperaterm " VERSION);
            return 0;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            puts(_("Usage: imperaterm [--separate] [-e command [arguments]]\n"
                   "  -e command   run the command, not the shell, in the first tab\n"
                   "  --separate   be an Imperaterm of its own, not a window of the one that runs\n"
                   "  --version    print the version"));
            return 0;
        } else
            own = 1;  /* told to be, or given what is GTK's to read, or mistaken */
    /* One Imperaterm has all the windows: if it is running, this one asks it for a window and waits. */
    if (!own && i + 1 != argc && join_ask(i < argc ? argv + i + 1 : NULL, &status))
        return status;
    gtk_set_locale();
    /* A terminal's text is UTF-8, whatever the locale says. */
    if (strcmp(nl_langinfo(CODESET), "UTF-8") && !setlocale(LC_CTYPE, "C.UTF-8"))
        setlocale(LC_CTYPE, "en_US.UTF-8");
    gtk_init(&argc, &argv);
    /* Every window's icon, the one Imperaterm is installed with, where the window manager takes one in colour. */
    gtk_window_set_default_icon_name("imperaterm");
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-e") && i + 1 < argc) {
            command = argv + i + 1;
            break;
        }
        if (!strcmp(argv[i], "--separate"))
            continue;
        g_printerr(_("imperaterm: %s is not an option; see imperaterm --help\n"), argv[i]);
        return 1;
    }
    if (!gdk_gl_query()) {
        g_printerr(_("imperaterm: OpenGL is not available on this display\n"));
        return 1;
    }
    signal(SIGCHLD, SIG_IGN);  /* the children are not waited for */
    load();
    scale = MAX(gdk_screen_scale_factor(), 1);
    apply();
    if (!win_new(command, NULL, NULL, NULL))
        return 1;
    join_listen(joined);
    gtk_main();
    return 0;
}
