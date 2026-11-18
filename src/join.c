/* See LICENSE for licence details. */
/*
 * One Imperaterm has all the windows of a display. An Imperaterm started while
 * another runs hands it what it was to run, with the directory and the
 * environment it was started in, and waits until the tab that came of it
 * has ended, as if the terminal were its own. They meet at a socket in
 * XDG_RUNTIME_DIR that is named for the display. The one that listens
 * holds a lock beside it, so that of several started at once one listens
 * and the rest find it.
 *
 * What is asked is "imperaterm 1", the length of the rest in four bytes with
 * the high one first, and the rest, which is strings that each end in a
 * zero: the directory, the number of words in the command, which is 0
 * for the shell, the words, and the environment to the end. The answer
 * is one byte: 'k' when it is taken, before the window is made, and
 * anything else when it is not. Then nothing comes until the tab ends
 * and the socket is closed, but for 'x' if no window could be made.
 *
 * Nothing here is ever a reason to stop: an Imperaterm that cannot ask runs
 * on its own, and one that cannot listen is not found.
 */
#define _XOPEN_SOURCE 700
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "imperaterm.h"

#define MAGIC "imperaterm 1"

enum {
    HEAD = 12,  /* the name and the length */
    MOST = 8 << 20,  /* the longest request that is heard out */
    ANSWER = 5000,  /* how long, in ms, an Imperaterm waits for its answer before it goes its own way */
    PATIENCE = 2000,  /* how long one that asks has to say it all, which is less: the next may be waiting for its place */
    ATONCE = 32,  /* how many may be asking at a time; the rest wait their turn */
    TRIES = 60, PAUSE = 50  /* how often one that is not heard asks again, and how long between, in ms */
};

/* One that is asking. */
typedef struct {
    int fd;
    guint tag, timer;
    unsigned char head[HEAD];
    size_t got, len;
    char *buf;
    int no;  /* it was not taken, and what it still says is not listened to */
} Ask;

extern char **environ;

static struct sockaddr_un addr;
static int lockfd = -1, lfd = -1;
static guint ltag, lwait;
static int asking;
static JoinOpen *opened;

/* The socket's name: one for each display of each user. Not named, there is no joining. */
static int
named(void)
{
    const char *dir = getenv("XDG_RUNTIME_DIR"), *be = getenv("GDK_BACKEND"), *d;
    struct stat st;
    int wl = 0, n;
    char *p, *colon, *dot;

    /* The directory is to be this user's alone, as it is laid down: who can reach the socket can run what he likes. */
    if (!dir || *dir != '/' || stat(dir, &st) || st.st_uid != getuid() || (st.st_mode & 077))
        return 0;
    /* The display is the one GDK will choose. */
#if defined GDK_WINDOWING_WAYLAND && defined GDK_WINDOWING_X11
    wl = be && !strcmp(be, "wayland") ? 1 : be && !strcmp(be, "x11") ? 0 : getenv("WAYLAND_DISPLAY") != NULL;
#elif defined GDK_WINDOWING_WAYLAND
    wl = 1;
#endif
    (void)be;
    d = getenv(wl ? "WAYLAND_DISPLAY" : "DISPLAY");
    if (!d || !*d) {
        if (!wl)
            return 0;
        d = "wayland-0";
    }
    n = snprintf(addr.sun_path, sizeof addr.sun_path, "%s/imperaterm-%s-%s", dir, wl ? "wayland" : "x11", d);
    if (n < 0 || (size_t)n >= sizeof addr.sun_path)
        return 0;
    p = addr.sun_path + n - strlen(d);
    if (!wl && (colon = strrchr(p, ':')) && (dot = strchr(colon, '.')))
        *dot = 0;  /* a screen of an X display is the same display */
    for (; *p; p++)
        if (*p == '/')
            *p = '_';
    addr.sun_family = AF_UNIX;
    return 1;
}

/* The lock is this Imperaterm's, which is then the one to listen, or it is another's. */
static int
lock(void)
{
    char file[sizeof addr.sun_path + 8];
    struct flock fl;
    int fd;

    snprintf(file, sizeof file, "%s.lock", addr.sun_path);
    if ((fd = open(file, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600)) < 0)
        return 0;
    memset(&fl, 0, sizeof fl);
    fl.l_type = F_WRLCK, fl.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &fl)) {
        close(fd);
        return 0;
    }
    lockfd = fd;
    return 1;
}

static void
gone(void)
{
    if (lfd >= 0)
        unlink(addr.sun_path);
}

/* The socket is made, and those that come wait at it until join_listen(). */
static void
hear(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd >= 0) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        fcntl(fd, F_SETFL, O_NONBLOCK);
        unlink(addr.sun_path);  /* one that was left behind */
        if (!bind(fd, (struct sockaddr *)&addr, sizeof addr) && !chmod(addr.sun_path, 0600) && !listen(fd, 64)) {
            lfd = fd;
            atexit(gone);
            return;
        }
        close(fd);
    }
    close(lockfd);  /* another may do better */
    lockfd = -1;
}

static int
put(int fd, const char *s, size_t n)
{
    while (n) {
        ssize_t r = send(fd, s, n, MSG_NOSIGNAL);

        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return 0;
        s += r, n -= r;
    }
    return 1;
}

static char *
add(char *p, const char *s)
{
    size_t n = strlen(s) + 1;

    memcpy(p, s, n);
    return p + n;
}

/* What this Imperaterm was to run, as it is asked for; NULL if it is too much to ask. */
static char *
request(char **argv, size_t *len)
{
    char dir[4096], count[24], *buf, *p;
    size_t n = 0, argc = 0, i;

    if (!getcwd(dir, sizeof dir))
        *dir = 0;
    for (; argv && argv[argc]; argc++)
        n += strlen(argv[argc]) + 1;
    for (i = 0; environ && environ[i]; i++)
        n += strlen(environ[i]) + 1;
    snprintf(count, sizeof count, "%lu", (unsigned long)argc);
    n += strlen(dir) + 1 + strlen(count) + 1;
    if (n > MOST || !(buf = malloc(HEAD + n)))
        return NULL;
    memcpy(buf, MAGIC, 8);
    buf[8] = (char)(n >> 24), buf[9] = (char)(n >> 16 & 0xff), buf[10] = (char)(n >> 8 & 0xff), buf[11] = (char)(n & 0xff);
    p = add(add(buf + HEAD, dir), count);
    for (i = 0; i < argc; i++)
        p = add(p, argv[i]);
    for (i = 0; environ && environ[i]; i++)
        p = add(p, environ[i]);
    *len = HEAD + n;
    return buf;
}

/*
 * Asked of the one that listens. 1: it was taken, and the tab has ended.
 * 0: nothing was heard, as when the other is just leaving. -1: it is
 * there and does not take it.
 */
static int
hand(int fd, const char *req, size_t len, int *status)
{
    struct timeval tv = { ANSWER / 1000, ANSWER % 1000 * 1000 };
    struct pollfd pfd = { fd, POLLIN, 0 };
    char c[64];
    ssize_t r, i;

    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (!put(fd, req, len))
        return errno == EAGAIN || errno == EWOULDBLOCK ? -1 : 0;
    while ((r = poll(&pfd, 1, ANSWER)) < 0 && errno == EINTR)
        ;
    if (r <= 0)
        return -1;
    while ((r = recv(fd, c, 1, 0)) < 0 && errno == EINTR)
        ;
    if (r <= 0)
        return 0;
    if (*c != 'k')
        return -1;
    *status = 0;
    for (;;) {  /* until the tab ends, which is when the other end closes */
        r = recv(fd, c, sizeof c, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return 1;
        for (i = 0; i < r; i++)
            if (c[i] == 'x')
                *status = 1;
    }
}

/*
 * Before anything else is done: is there an Imperaterm to join? 1 if there
 * was and it has run the command, with what to leave with in *status. 0
 * if this one is to run it; it may then be the one that listens.
 */
int
join_ask(char **argv, int *status)
{
    char *req;
    size_t len = 0;
    int i, fd, r;

    if (!named())
        return 0;
    req = request(argv, &len);
    for (i = 0; i < TRIES; i++) {
        if (!req) {  /* nothing to ask with: it can still be the one that listens */
            if (lock())
                hear();
            break;
        }
        if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) >= 0) {
            fcntl(fd, F_SETFD, FD_CLOEXEC);
            r = connect(fd, (struct sockaddr *)&addr, sizeof addr) ? 0 : hand(fd, req, len, status);
            close(fd);
            if (r) {
                free(req);
                return r > 0;
            }
        }
        if (lock()) {
            hear();
            break;
        }
        poll(NULL, 0, PAUSE);  /* one that has the lock is starting or leaving */
    }
    free(req);
    return 0;
}

static void came(gpointer data, gint fd, GdkInputCondition cond);

static gint
again(gpointer data)
{
    (void)data;
    lwait = 0;
    if (!ltag)
        ltag = gdk_input_add(lfd, GDK_INPUT_READ, came, NULL);
    return FALSE;
}

/* One that asked is done with; its socket is closed unless it is kept. */
static void
end(Ask *a, int keep)
{
    gdk_input_remove(a->tag);
    if (a->timer)
        gtk_timeout_remove(a->timer);
    if (!keep)
        close(a->fd);
    free(a->buf);
    g_free(a);
    asking--;
    if (!ltag && !lwait)  /* there is room for those that waited */
        lwait = gtk_idle_add(again, NULL);
}

/*
 * It is not taken. The socket stays until the other closes it or is too
 * slow about it: closed here with words unread, the answer would be lost
 * on the way with them.
 */
static void
refuse(Ask *a)
{
    send(a->fd, "n", 1, MSG_NOSIGNAL | MSG_DONTWAIT);
    free(a->buf);
    a->buf = NULL;
    a->no = 1;
}

static gint
late(gpointer data)
{
    Ask *a = data;

    a->timer = 0;
    end(a, 0);
    return FALSE;
}

/*
 * All of it is here: it is taken apart, and if it is sound the window
 * is opened. 0 if it was not sound, and the one that asked is still to
 * be heard out.
 */
static int
run(Ask *a)
{
    char *buf = a->buf, *p = buf, *last = buf + a->len, *dir, *q, **vec, **env;
    size_t n = 0, i;
    unsigned long argc;
    int fd = a->fd;

    if (last[-1]) {
        refuse(a);
        return 0;
    }
    for (q = p; q < last; q += strlen(q) + 1)
        n++;
    dir = p, p += strlen(p) + 1;
    errno = 0;
    argc = n < 2 ? 0 : strtoul(p, &q, 10);
    if (n < 2 || *p < '0' || *p > '9' || *q || errno || argc > n - 2 || !(vec = malloc(n * sizeof *vec))) {
        refuse(a);
        return 0;
    }
    p += strlen(p) + 1;
    for (i = 0; i < argc; i++, p += strlen(p) + 1)
        vec[i] = p;
    vec[argc] = NULL;
    env = vec + argc + 1;
    for (i = 0; p < last; p += strlen(p) + 1)
        if (*p != '=' && strchr(p, '='))
            env[i++] = p;
    env[i] = NULL;
    /* The answer first: one that has stopped waiting is found out by it, and gets no window. */
    if (send(fd, "k", 1, MSG_NOSIGNAL | MSG_DONTWAIT) != 1) {
        free(vec);
        end(a, 0);
        return 1;
    }
    a->buf = NULL;  /* the strings are in it, and are needed until the window is there */
    end(a, 1);
    if (!opened(argc ? vec : NULL, *dir == '/' ? dir : NULL, env, fd)) {
        send(fd, "x", 1, MSG_NOSIGNAL | MSG_DONTWAIT);
        close(fd);
    }
    free(vec);
    free(buf);
    return 1;
}

static void
said(gpointer data, gint fd, GdkInputCondition cond)
{
    Ask *a = data;
    char rest[4096];
    ssize_t r;

    (void)cond;
    for (;;) {
        if (a->no)
            r = recv(fd, rest, sizeof rest, 0);
        else if (a->got < HEAD)
            r = recv(fd, a->head + a->got, HEAD - a->got, 0);
        else
            r = recv(fd, a->buf + (a->got - HEAD), a->len - (a->got - HEAD), 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (r <= 0) {
            end(a, 0);
            return;
        }
        if (a->no)
            continue;
        a->got += r;
        if (a->got == HEAD) {
            a->len = (size_t)a->head[8] << 24 | (size_t)a->head[9] << 16 | (size_t)a->head[10] << 8 | a->head[11];
            if (memcmp(a->head, MAGIC, 8) || a->len < 3 || a->len > MOST || !(a->buf = malloc(a->len)))
                refuse(a);
        } else if (a->got == HEAD + a->len && run(a))
            return;
    }
}

static void
came(gpointer data, gint fd, GdkInputCondition cond)
{
    Ask *a;
    int c = 0;

    (void)data, (void)fd, (void)cond;
    while (asking < ATONCE && (c = accept(lfd, NULL, NULL)) >= 0) {
        fcntl(c, F_SETFD, FD_CLOEXEC);
        fcntl(c, F_SETFL, O_NONBLOCK);
        a = g_new0(Ask, 1);
        a->fd = c;
        a->tag = gdk_input_add(c, GDK_INPUT_READ, said, a);
        a->timer = gtk_timeout_add(PATIENCE, late, a);
        asking++;
    }
    if (asking >= ATONCE || (c < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && errno != ECONNABORTED)) {
        /* No room, or no more files: the rest wait, and are not asked after until there may be. */
        gdk_input_remove(ltag);
        ltag = 0;
        if (asking < ATONCE && !lwait)
            lwait = gtk_timeout_add(1000, again, NULL);
    }
}

/* From now on those that come are heard, and fn makes their windows. */
void
join_listen(JoinOpen *fn)
{
    if (lfd < 0)
        return;
    opened = fn;
    ltag = gdk_input_add(lfd, GDK_INPUT_READ, came, NULL);
}
