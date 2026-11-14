/*
 * bak - a tiny, careful backup tool with a terminal UI.
 *
 * Pick folders, pack them with tar, compress with xz, optionally encrypt
 * with gpg (AES-256), save into a destination folder, optionally upload
 * the result with rclone.
 *
 * Safety rules:
 *   - Everything is checked before starting (tools, paths, permissions,
 *     readability of every file, free space, name collisions, ...).
 *   - The archive is written to a hidden ".partial" file, fsync'ed, fully
 *     decrypted/decompressed/listed to verify it, made read-only, and only
 *     then renamed to its final name. A final-named file is always verified.
 *   - Any error, warning or abort at any step discards the archive.
 *   - The folders being backed up are only ever read: nothing in them is
 *     written, moved or deleted (tar only reads; no --remove-files etc.).
 *   - bak never deletes a finished backup, even after uploading it.
 *
 * Uses libc and ncurses. External programs are run directly (no shell).
 */
#define _XOPEN_SOURCE 700

#include <ctype.h>
#include <curses.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#ifdef __linux__
#include <sys/vfs.h>
#define MSDOS_MAGIC 0x4d44 /* FAT12/16/32: 4 GiB file size limit */
#endif

#define MAX_SRC 64
#define LINE 4096
#define PASS_MAX 1024
#define LOG_MAX 500
#define NSET 5
#define EV_CLICK (KEY_MAX + 1)

/* hit-region ids (values below 1000 are key codes, used by buttons) */
#define ID_ROW 1000    /* + folder index */
#define ID_DEL 2000    /* + folder index */
#define ID_SET 3000    /* + setting index */
#define ID_ENT 4000    /* + picker entry */
#define ID_OPT 5000    /* + choice option */

/* ------------------------------------------------------------------ */
/* state                                                              */

struct source {
    char *path;
    long long bytes, files, errors;
};

static struct source src[MAX_SRC];
static int nsrc;
static char dest[PATH_MAX];
static long long dest_free = -1;
static char label[41] = "backup";
static int comp_sel;
static int encrypt;
static char remote[LINE]; /* "" = no upload, else "name:path" */

static const struct {
    const char *desc, *level, *threads;
} comps[] = {
    {"Smallest - xz -9e, 1 thread (best ratio, slowest)", "-9e", "-T1"},
    {"Small    - xz -9e, all cores (~1% bigger, faster)", "-9e", "-T0"},
    {"Fast     - xz -6, all cores (a few % bigger, fastest)", "-6", "-T0"},
};
#define NCOMP (int)(sizeof comps / sizeof comps[0])

static char msg[512];
static int msg_err;

static int log_fd = -1; /* children's stderr goes here, then into the log */

/* ------------------------------------------------------------------ */
/* small helpers                                                      */

static void setmsg(int err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    msg_err = err;
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n--)
        *v++ = 0;
}

static void human(double b, char *out, size_t n)
{
    const char *u[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    int i = 0;
    while (b >= 1024 && i < 5) {
        b /= 1024;
        i++;
    }
    snprintf(out, n, i ? "%.1f %s" : "%.0f %s", b, u[i]);
}

static void fmt_count(long long v, char *out, size_t n)
{
    char t[32];
    int len = snprintf(t, sizeof t, "%lld", v);
    size_t j = 0;
    for (int i = 0; i < len && j + 2 < n; i++) {
        if (i && (len - i) % 3 == 0)
            out[j++] = ',';
        out[j++] = t[i];
    }
    out[j] = '\0';
}

static void fmt_time(double s, char *out, size_t n)
{
    long t = (long)s;
    snprintf(out, n, "%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60);
}

/* Expand leading "~/" and resolve to an absolute, canonical path. */
static int resolve(const char *in, char *out)
{
    char tmp[PATH_MAX];
    if (in[0] == '~' && (in[1] == '/' || in[1] == '\0')) {
        const char *home = getenv("HOME");
        if (!home || snprintf(tmp, sizeof tmp, "%s%s", home, in + 1) >= (int)sizeof tmp)
            return -1;
    } else if (snprintf(tmp, sizeof tmp, "%s", in) >= (int)sizeof tmp) {
        return -1;
    }
    return realpath(tmp, out) ? 0 : -1;
}

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Is path a equal to, or inside, directory b? (both canonical) */
static int is_inside(const char *a, const char *b)
{
    size_t n = strlen(b);
    if (strcmp(b, "/") == 0)
        return 1;
    return strncmp(a, b, n) == 0 && (a[n] == '/' || a[n] == '\0');
}

static const char *base_of(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static int join(char *out, const char *dir, const char *name)
{
    int r = snprintf(out, PATH_MAX, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
    return r < 0 || r >= PATH_MAX ? -1 : 0;
}

static int in_path(const char *name)
{
    const char *path = getenv("PATH");
    char dir[PATH_MAX], full[PATH_MAX];
    if (!path)
        return 0;
    while (*path) {
        size_t n = strcspn(path, ":");
        if (n > 0 && n < sizeof dir) {
            memcpy(dir, path, n);
            dir[n] = '\0';
            if (snprintf(full, sizeof full, "%s/%s", dir, name) < (int)sizeof full &&
                access(full, X_OK) == 0)
                return 1;
        }
        path += n;
        if (*path == ':')
            path++;
    }
    return 0;
}

static int fsync_dir(const char *d)
{
    int fd = open(d, O_RDONLY);
    if (fd < 0)
        return -1;
    int r = fsync(fd);
    close(fd);
    return r;
}

static void update_dest_free(void)
{
    struct statvfs v;
    dest_free = (dest[0] && statvfs(dest, &v) == 0) ? (long long)v.f_bavail * (long long)v.f_frsize : -1;
}

static const char *media_dir(char *buf)
{
    const char *u = getenv("USER");
    const char *c[] = {"/run/media/%s", "/media/%s", "/run/media", "/media", "/mnt"};
    for (int i = 0; i < 5; i++) {
        snprintf(buf, PATH_MAX, c[i], u ? u : "");
        if ((i >= 2 || u) && is_dir(buf))
            return buf;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* drawing primitives                                                 */

enum { C_TITLE = 1, C_ERR, C_WARN, C_OK, C_BAR };
static int colors;
static void (*bg_draw)(void); /* redraws the screen under a dialog */

static struct {
    int y, x0, x1, id;
} hits[256];
static int nhits;
static int ev_id;

static int cp(int p) { return colors ? COLOR_PAIR(p) : 0; }

static void hit_add(int y, int x, int w, int id)
{
    if (nhits < 256 && w > 0) {
        hits[nhits].y = y;
        hits[nhits].x0 = x;
        hits[nhits].x1 = x + w - 1;
        hits[nhits].id = id;
        nhits++;
    }
}

/* Read a key. Mouse clicks become EV_CLICK (with ev_id), wheel = up/down. */
static int get_input(void)
{
    int ch = getch();
    if (ch != KEY_MOUSE)
        return ch;
    MEVENT m;
    if (getmouse(&m) != OK)
        return 0;
    if (m.bstate & BUTTON4_PRESSED)
        return KEY_UP;
#ifdef BUTTON5_PRESSED
    if (m.bstate & BUTTON5_PRESSED)
        return KEY_DOWN;
#endif
    if (m.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED)) {
        for (int i = nhits - 1; i >= 0; i--)
            if (hits[i].y == m.y && m.x >= hits[i].x0 && m.x <= hits[i].x1) {
                ev_id = hits[i].id;
                return EV_CLICK;
            }
    }
    return 0;
}

static int is_enter(int ch) { return ch == '\n' || ch == '\r' || ch == KEY_ENTER; }
static int is_back(int ch) { return ch == KEY_BACKSPACE || ch == 127 || ch == 8; }

static size_t mb_step(const char *p, int *cw)
{
    mbstate_t st;
    wchar_t wc;
    memset(&st, 0, sizeof st);
    size_t k = mbrtowc(&wc, p, MB_CUR_MAX, &st);
    if (k == (size_t)-1 || k == (size_t)-2 || k == 0) {
        *cw = 1;
        return 1;
    }
    int w = wcwidth(wc);
    *cw = w < 0 ? 1 : w;
    return k;
}

static int str_cols(const char *s)
{
    int total = 0, cw;
    while (*s) {
        s += mb_step(s, &cw);
        total += cw;
    }
    return total;
}

/* Print s in at most w columns; if too long, cut it and add "...". */
static void put(int y, int x, int w, const char *s, int keep_end)
{
    int total = str_cols(s), cw;
    const char *p = s;
    if (w <= 0)
        return;
    if (total <= w) {
        mvaddstr(y, x, s);
        return;
    }
    if (w <= 3) {
        mvaddnstr(y, x, "...", w);
        return;
    }
    if (keep_end) {
        while (*p && total > w - 3) {
            p += mb_step(p, &cw);
            total -= cw;
        }
        mvaddstr(y, x, "...");
        addstr(p);
    } else {
        int used = 0;
        while (*p) {
            size_t k = mb_step(p, &cw);
            if (used + cw > w - 3)
                break;
            used += cw;
            p += k;
        }
        mvaddnstr(y, x, s, (int)(p - s));
        addstr("...");
    }
}

static void draw_box(int y, int x, int h, int w, const char *title)
{
    mvhline(y, x + 1, ACS_HLINE, w - 2);
    mvhline(y + h - 1, x + 1, ACS_HLINE, w - 2);
    mvvline(y + 1, x, ACS_VLINE, h - 2);
    mvvline(y + 1, x + w - 1, ACS_VLINE, h - 2);
    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + w - 1, ACS_URCORNER);
    mvaddch(y + h - 1, x, ACS_LLCORNER);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
    if (title && title[0]) {
        char t[256];
        snprintf(t, sizeof t, " %s ", title);
        attron(A_BOLD);
        put(y, x + 2, w - 4, t, 0);
        attroff(A_BOLD);
    }
}

static void title_bar(const char *left, const char *right)
{
    int a = colors ? (cp(C_TITLE) | A_BOLD) : (A_REVERSE | A_BOLD);
    attron(a);
    mvhline(0, 0, ' ', COLS);
    put(0, 1, COLS - 2, left, 0);
    if (right && (int)strlen(right) + (int)strlen(left) + 4 < COLS)
        mvaddstr(0, COLS - 1 - (int)strlen(right), right);
    attroff(a);
}

struct btn {
    int id;
    const char *label;
};

/* Buttons are clickable; their id is the key they stand for. */
static void draw_buttons(int y, int ymax, int x0, const struct btn *b, int n)
{
    int x = x0;
    for (int i = 0; i < n; i++) {
        int w = (int)strlen(b[i].label) + 2;
        if (x + w > COLS - 1) {
            if (y >= ymax)
                break;
            y++;
            x = x0;
        }
        attron(A_REVERSE);
        mvprintw(y, x, " %s ", b[i].label);
        attroff(A_REVERSE);
        hit_add(y, x, w, b[i].id);
        x += w + 1;
    }
}

static int too_small(void)
{
    if (LINES >= 18 && COLS >= 60)
        return 0;
    erase();
    nhits = 0;
    mvaddstr(0, 0, "Terminal too small: need at least 60x18.");
    return 1;
}

/* Centered dialog frame; adjusts h/w to fit, returns top-left in y/x. */
static void dialog_frame(int *h, int *w, const char *title, int *y, int *x)
{
    if (*w > COLS - 2)
        *w = COLS - 2;
    if (*h > LINES - 2)
        *h = LINES - 2;
    *y = (LINES - *h) / 2;
    *x = (COLS - *w) / 2;
    for (int r = 0; r < *h; r++)
        mvhline(*y + r, *x, ' ', *w);
    draw_box(*y, *x, *h, *w, title);
}

/* ------------------------------------------------------------------ */
/* dialogs                                                            */

/* Pick one option. Returns its index, or -1 if cancelled. */
static int choose(const char *title, const char *const *opts, int n, int sel)
{
    int top = 0;
    for (;;) {
        bg_draw();
        int w = str_cols(title) + 8, h = n + 4, y, x;
        for (int i = 0; i < n; i++)
            if (str_cols(opts[i]) + 10 > w)
                w = str_cols(opts[i]) + 10;
        if (w < 34)
            w = 34;
        dialog_frame(&h, &w, title, &y, &x);
        int vis = h - 4 > 0 ? h - 4 : 1;
        if (sel < top)
            top = sel;
        if (sel >= top + vis)
            top = sel - vis + 1;
        for (int i = top; i < n && i < top + vis; i++) {
            int yy = y + 2 + i - top;
            char num[16] = "  ";
            if (i < 9)
                snprintf(num, sizeof num, "%d ", i + 1);
            if (i == sel)
                attron(A_REVERSE);
            mvhline(yy, x + 2, ' ', w - 4);
            mvaddstr(yy, x + 3, num);
            put(yy, x + 5, w - 8, opts[i], 0);
            if (i == sel)
                attroff(A_REVERSE);
            hit_add(yy, x + 2, w - 4, ID_OPT + i);
        }
        put(y + h - 1, x + 2, w - 4, " Enter/click: choose   Esc: cancel ", 0);
        refresh();

        int ch = get_input();
        if (ch == EV_CLICK) {
            if (ev_id >= ID_OPT && ev_id < ID_OPT + n)
                return ev_id - ID_OPT;
            continue;
        }
        if (ch == KEY_UP || ch == 'k')
            sel = sel > 0 ? sel - 1 : n - 1;
        else if (ch == KEY_DOWN || ch == 'j' || ch == '\t')
            sel = sel < n - 1 ? sel + 1 : 0;
        else if (ch == KEY_HOME)
            sel = 0;
        else if (ch == KEY_END)
            sel = n - 1;
        else if (is_enter(ch))
            return sel;
        else if (ch == 27 || ch == 3 || ch == 'q')
            return -1;
        else if (ch >= '1' && ch <= '9' && ch - '1' < n)
            return ch - '1';
        else if (ch > 0 && ch < 128 && isalpha(ch)) {
            int found = -1, count = 0; /* unique first letter chooses */
            for (int i = 0; i < n; i++)
                if (tolower((unsigned char)opts[i][0]) == tolower(ch)) {
                    found = i;
                    count++;
                }
            if (count == 1)
                return found;
        }
    }
}

static int confirm(const char *question, int def_yes)
{
    const char *opts[] = {"Yes", "No"};
    return choose(question, opts, 2, def_yes ? 0 : 1) == 0;
}

/* Text entry. Returns 1 if confirmed (buf updated), 0 if cancelled. */
static int input_box(const char *title, const char *hint, char *buf, size_t n, int masked)
{
    char t[LINE];
    size_t len;
    if (n > sizeof t)
        n = sizeof t;
    snprintf(t, n, "%s", buf);
    len = strlen(t);
    for (;;) {
        bg_draw();
        int h = 8, w = 72, y, x;
        dialog_frame(&h, &w, title, &y, &x);
        put(y + 2, x + 3, w - 6, hint, 0);
        attron(A_UNDERLINE);
        mvhline(y + 4, x + 3, ' ', w - 6);
        if (masked) {
            char stars[64];
            int k = 0;
            for (size_t i = 0; i < len && k < 63; i++)
                if (((unsigned char)t[i] & 0xC0) != 0x80)
                    stars[k++] = '*';
            stars[k] = '\0';
            mvaddstr(y + 4, x + 3, stars);
        } else {
            put(y + 4, x + 3, w - 7, t, 1);
        }
        attroff(A_UNDERLINE);
        struct btn b[] = {{'\n', "OK (Enter)"}, {27, "Cancel (Esc)"}};
        int save_y, save_x;
        getyx(stdscr, save_y, save_x);
        draw_buttons(y + h - 2, y + h - 2, x + 2, b, 2);
        move(save_y, save_x);
        curs_set(1);
        refresh();

        int ch = get_input();
        if (ch == EV_CLICK) {
            if (ev_id == '\n' || ev_id == 27)
                ch = ev_id;
            else
                continue;
        }
        if (is_enter(ch)) {
            curs_set(0);
            snprintf(buf, n, "%s", t);
            wipe(t, sizeof t);
            return 1;
        }
        if (ch == 27 || ch == 3) {
            curs_set(0);
            wipe(t, sizeof t);
            return 0;
        }
        if (is_back(ch)) {
            while (len > 0 && ((unsigned char)t[len - 1] & 0xC0) == 0x80)
                len--; /* UTF-8 continuation bytes */
            if (len > 0)
                len--;
            t[len] = '\0';
        } else if (ch == 21) { /* Ctrl-U */
            len = 0;
            t[0] = '\0';
        } else if (ch >= 32 && ch < 256 && ch != 127 && len + 1 < n) {
            t[len++] = (char)ch;
            t[len] = '\0';
        }
    }
}

static void draw_note(const char *title, const char *l1, const char *l2, const char *l3)
{
    int h = 7, w = 64, y, x;
    dialog_frame(&h, &w, title, &y, &x);
    put(y + 2, x + 3, w - 6, l1, 1);
    put(y + 3, x + 3, w - 6, l2, 0);
    put(y + 5, x + 3, w - 6, l3, 0);
}

/* ------------------------------------------------------------------ */
/* run screen: log + progress                                         */

static char *logv[LOG_MAX];
static int nlog, log_scroll;
static char phase[128];
static int phase_attr;
static long long pg_done, pg_total;
static double pg_t0;
static int run_done, aborted;
static const char *abort_q = "Abort?";

static void draw_run(void)
{
    if (too_small())
        return;
    erase();
    nhits = 0;
    title_bar(run_done ? "bak - finished" : "bak - working", NULL);
    int bh = LINES - 5, vis = bh - 2;
    draw_box(1, 0, bh, COLS, "Log");
    if (log_scroll > nlog - vis)
        log_scroll = nlog - vis;
    if (log_scroll < 0)
        log_scroll = 0;
    int start = nlog - vis - log_scroll;
    if (start < 0)
        start = 0;
    for (int i = start; i < nlog && i < start + vis; i++) {
        const char *l = logv[i];
        int a = 0;
        if (l[0] == '!')
            a = cp(C_ERR) | A_BOLD;
        else if (l[0] == '?')
            a = cp(C_WARN) | A_BOLD;
        else if (strncmp(l, "OK", 2) == 0 || strncmp(l, "ok", 2) == 0)
            a = cp(C_OK);
        else if (strncmp(l, "  |", 3) == 0)
            a = A_DIM;
        attron(a);
        put(2 + i - start, 2, COLS - 4, l, 1); /* keep the end: file names */
        attroff(a);
    }
    if (log_scroll > 0)
        mvaddstr(bh, COLS - 22, " (scrolled: Up/Down) ");

    attron(A_BOLD | phase_attr);
    put(LINES - 4, 1, COLS - 2, phase, 0);
    attroff(A_BOLD | phase_attr);

    double el = now() - pg_t0;
    char t1[32], t2[32], t3[32], t4[32], line[256];
    fmt_time(el, t3, sizeof t3);
    if (!run_done && pg_total > 0) {
        double f = (double)pg_done / (double)pg_total;
        if (f > 0.999)
            f = 0.999; /* the total is an estimate */
        int bw = COLS - 9, fill = (int)(f * bw);
        attron(colors ? cp(C_BAR) : (int)A_REVERSE);
        mvhline(LINES - 3, 1, ' ', fill);
        attroff(colors ? cp(C_BAR) : (int)A_REVERSE);
        mvhline(LINES - 3, 1 + fill, ACS_CKBOARD, bw - fill);
        mvprintw(LINES - 3, COLS - 7, "%3d%%", (int)(f * 100));
        human((double)pg_done, t1, sizeof t1);
        human((double)pg_total, t2, sizeof t2);
        double rate = el > 0 ? (double)pg_done / el : 0;
        human(rate, t4, sizeof t4);
        int len = snprintf(line, sizeof line, "%s of ~%s   %s/s   elapsed %s", t1, t2, t4, t3);
        if (rate > 0 && pg_done > 0) {
            char r[32];
            fmt_time((double)(pg_total - pg_done) / rate, r, sizeof r);
            snprintf(line + len, sizeof line - (size_t)len, "   left ~%s", r);
        }
        put(LINES - 2, 1, COLS - 2, line, 0);
    } else if (!run_done && phase[0]) {
        const char spin[] = "|/-\\";
        snprintf(line, sizeof line, "%c  elapsed %s", spin[(int)(el * 4) % 4], t3);
        put(LINES - 2, 1, COLS - 2, line, 0);
    }
    if (run_done) {
        struct btn b[] = {{'\n', "Back to menu (Enter)"}};
        draw_buttons(LINES - 1, LINES - 1, 1, b, 1);
        attron(A_DIM);
        put(LINES - 1, 26, COLS - 27, "Up/Down/wheel: scroll log", 0);
        attroff(A_DIM);
    } else {
        struct btn b[] = {{27, "Abort (Esc)"}};
        draw_buttons(LINES - 1, LINES - 1, 1, b, 1);
    }
}

static void log_line(const char *s)
{
    if (nlog == LOG_MAX) {
        free(logv[0]);
        memmove(logv, logv + 1, (LOG_MAX - 1) * sizeof logv[0]);
        nlog--;
    }
    char *d = strdup(s);
    if (d)
        logv[nlog++] = d;
}

static void logmsg(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    for (char *s = buf, *e; s; s = e) {
        e = strchr(s, '\n');
        if (e)
            *e++ = '\0';
        log_line(s);
    }
    log_scroll = 0;
    if (bg_draw == draw_run) {
        draw_run();
        refresh();
    }
}

static void log_clear(void)
{
    for (int i = 0; i < nlog; i++)
        free(logv[i]);
    nlog = 0;
    log_scroll = 0;
}

/* Copy what the child programs printed into the log. */
static void log_children(void)
{
    off_t end = lseek(log_fd, 0, SEEK_END), off = 0;
    char buf[16384 + 1];
    if (end <= 0)
        return;
    if (end > 16384) {
        off = end - 16384;
        logmsg("  | (earlier output trimmed)");
    }
    ssize_t r = pread(log_fd, buf, (size_t)(end - off), off);
    if (r > 0) {
        buf[r] = '\0';
        for (char *s = buf, *e; s && *s; s = e) {
            e = strchr(s, '\n');
            if (e)
                *e++ = '\0';
            if (*s)
                logmsg("  | %s", s);
        }
    }
    if (ftruncate(log_fd, 0) != 0)
        logmsg("  | (could not reset program output log)");
}

static void set_phase(const char *name, long long total)
{
    snprintf(phase, sizeof phase, "%s", name);
    phase_attr = 0;
    pg_done = 0;
    pg_total = total;
    pg_t0 = now();
}

/* Redraw (rate limited) and handle keys while working. 1 = abort asked. */
static int ui_poll(int wait_ms)
{
    static double last;
    if (now() - last >= 0.1) {
        draw_run();
        refresh();
        last = now();
    }
    timeout(wait_ms);
    int ch = get_input();
    timeout(-1);
    if (ch == EV_CLICK)
        ch = ev_id < 1000 ? ev_id : 0;
    if (ch == 27 || ch == 'q' || ch == 3) {
        int r = confirm(abort_q, 0);
        draw_run();
        refresh();
        return r;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* process handling                                                   */

static int cloexec_pipe(int p[2])
{
    if (pipe(p) < 0)
        return -1;
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    return 0;
}

/* Place fd `from` at `to` in a child, without close-on-exec. */
static int move_fd(int from, int to)
{
    if (from == to)
        return fcntl(to, F_SETFD, 0);
    return dup2(from, to) < 0 ? -1 : 0;
}

/* All our fds are close-on-exec, so the child only gets 0-2 (+3).
 * stderr always goes to the log file, never to the screen. */
static pid_t spawn(char *const argv[], int in, int out, int fd3)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        signal(SIGPIPE, SIG_DFL);
        if (move_fd(in, 0) || move_fd(out, 1) || move_fd(log_fd, 2) ||
            (fd3 >= 0 && move_fd(fd3, 3)))
            _exit(127);
        execvp(argv[0], argv);
        fprintf(stderr, "cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    return pid;
}

static int write_all(int fd, const char *s, size_t n)
{
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        s += w;
        n -= (size_t)w;
    }
    return 0;
}

/*
 * Run cmds[0] | cmds[1] | ... reading `in`, writing `out`.
 * pass_idx >= 0: that command gets the passphrase on fd 3.
 * relay_at >= 0: bak itself copies the data going into that command,
 *                counting bytes for the progress bar.
 * The UI stays responsive; the user can abort (children get SIGTERM).
 * Returns 1 only if every process exited with status 0.
 */
static int run_pipeline(char **cmds[], int n, int in, int out, int pass_idx, const char *pass,
                        int relay_at)
{
    pid_t pids[8];
    int started = 0, ok = 1, prev = in, rsrc = -1, rdst = -1;
    aborted = 0;

    for (int i = 0; i < n; i++) {
        if (i == relay_at) {
            int r[2];
            if (cloexec_pipe(r) < 0) {
                ok = 0;
                break;
            }
            rsrc = prev;
            rdst = r[1];
            prev = r[0];
        }
        int p[2] = {-1, -1}, pp[2] = {-1, -1}, o = out;
        if (i < n - 1) {
            if (cloexec_pipe(p) < 0) {
                ok = 0;
                break;
            }
            o = p[1];
        }
        if (i == pass_idx && cloexec_pipe(pp) < 0) {
            if (p[0] >= 0) {
                close(p[0]);
                close(p[1]);
            }
            ok = 0;
            break;
        }
        pid_t pid = spawn(cmds[i], prev, o, pp[0]);
        if (i == pass_idx) {
            close(pp[0]);
            if (pid > 0 && write_all(pp[1], pass, strlen(pass)) < 0)
                ok = 0;
            close(pp[1]);
        }
        if (prev != in && prev != rsrc)
            close(prev);
        prev = in;
        if (i < n - 1) {
            close(p[1]);
            prev = p[0];
        }
        if (pid < 0) {
            logmsg("! cannot start %s: %s", cmds[i][0], strerror(errno));
            ok = 0;
            break;
        }
        pids[started++] = pid;
    }
    if (prev != in)
        close(prev);

    if (ok && rsrc >= 0) {
        static char buf[65536];
        while (!aborted) {
            struct pollfd pf = {rsrc, POLLIN, 0};
            if (poll(&pf, 1, 100) > 0) {
                ssize_t r = read(rsrc, buf, sizeof buf);
                if (r == 0)
                    break;
                if (r < 0) {
                    if (errno == EINTR || errno == EAGAIN)
                        continue;
                    ok = 0;
                    break;
                }
                if (write_all(rdst, buf, (size_t)r) < 0) {
                    ok = 0; /* the next program died */
                    break;
                }
                pg_done += r;
            }
            if (ui_poll(0))
                aborted = 1;
        }
    }
    if (rdst >= 0)
        close(rdst);
    if (rsrc >= 0 && rsrc != in)
        close(rsrc);

    int st[8], done[8] = {0}, left = started, killed = 0;
    double tkill = 0;
    while (left > 0) {
        for (int i = 0; i < started; i++) {
            if (done[i])
                continue;
            pid_t r = waitpid(pids[i], &st[i], WNOHANG);
            if (r == pids[i] || (r < 0 && errno == ECHILD)) {
                if (r < 0)
                    st[i] = -1;
                done[i] = 1;
                left--;
            }
        }
        if (!left)
            break;
        if (aborted && !killed) {
            for (int i = 0; i < started; i++)
                if (!done[i])
                    kill(pids[i], SIGTERM);
            killed = 1;
            tkill = now();
        } else if (killed == 1 && now() - tkill > 3) {
            for (int i = 0; i < started; i++)
                if (!done[i])
                    kill(pids[i], SIGKILL);
            killed = 2;
        }
        if (!aborted && ui_poll(100))
            aborted = 1;
        else if (aborted)
            ui_poll(100);
    }
    for (int i = 0; i < started; i++) {
        if (st[i] != -1 && WIFEXITED(st[i]) && WEXITSTATUS(st[i]) == 0)
            continue;
        ok = 0;
        if (aborted)
            continue;
        if (st[i] != -1 && WIFEXITED(st[i]))
            logmsg("! %s failed (exit code %d)", cmds[i][0], WEXITSTATUS(st[i]));
        else if (st[i] != -1 && WIFSIGNALED(st[i]))
            logmsg("! %s was killed (signal %d)", cmds[i][0], WTERMSIG(st[i]));
        else
            logmsg("! %s: lost track of process", cmds[i][0]);
    }
    return ok && !aborted;
}

static int run1(char **argv)
{
    int null = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (null < 0)
        return 0;
    char **cmds[1] = {argv};
    int ok = run_pipeline(cmds, 1, null, log_fd, -1, NULL, -1);
    close(null);
    return ok;
}

/* Quick blocking run, stdout captured into buf. Returns 1 on success. */
static int run_capture(char **argv, char *buf, size_t n)
{
    int p[2], st, null = open("/dev/null", O_RDONLY | O_CLOEXEC);
    buf[0] = '\0';
    if (null < 0 || cloexec_pipe(p) < 0) {
        if (null >= 0)
            close(null);
        return 0;
    }
    pid_t pid = spawn(argv, null, p[1], -1);
    close(null);
    close(p[1]);
    size_t len = 0;
    ssize_t r;
    while (len < n - 1 && (r = read(p[0], buf + len, n - 1 - len)) != 0) {
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        len += (size_t)r;
    }
    buf[len] = '\0';
    close(p[0]);
    if (pid < 0)
        return 0;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return 0;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* ------------------------------------------------------------------ */
/* scanning folders                                                   */

static long long w_bytes, w_files, w_entries, w_errors, w_tar;
static int w_verbose, w_abort;
static double w_last;
static const char *w_root;

static void scan_tick(void)
{
    char a[32], b[32], l2[96];
    w_last = now();
    bg_draw();
    fmt_count(w_entries, a, sizeof a);
    human((double)w_bytes, b, sizeof b);
    snprintf(l2, sizeof l2, "%s items, %s so far", a, b);
    draw_note("Scanning", w_root, l2, "Esc: cancel");
    refresh();
    timeout(0);
    int ch = get_input();
    timeout(-1);
    if (ch == 27 || ch == 3 || ch == 'q')
        w_abort = 1;
}

static void walk_err(const char *path, const char *why)
{
    if (w_verbose && w_errors < 10)
        logmsg("! %s: %s", why, path);
    else if (w_verbose && w_errors == 10)
        logmsg("! (more problems not shown)");
    w_errors++;
}

static int walk_cb(const char *p, const struct stat *st, int type, struct FTW *f)
{
    (void)f;
    w_entries++;
    w_tar += 512;
    switch (type) {
    case FTW_F:
        w_files++;
        if (S_ISREG(st->st_mode)) {
            w_bytes += st->st_size;
            w_tar += (st->st_size + 511) / 512 * 512;
            if (access(p, R_OK) != 0)
                walk_err(p, "file not readable");
        }
        break;
    case FTW_D:
        if (access(p, R_OK | X_OK) != 0)
            walk_err(p, "folder not accessible");
        break;
    case FTW_DNR:
        walk_err(p, "folder not readable");
        break;
    case FTW_NS:
        walk_err(p, "cannot read file info");
        break;
    default: /* symlinks are stored as links, never followed */
        w_files++;
        break;
    }
    if ((w_entries & 1023) == 0 && now() - w_last > 0.15) {
        scan_tick();
        if (w_abort)
            return 1;
    }
    return 0;
}

static int scan(const char *path, int verbose)
{
    w_bytes = w_files = w_entries = w_errors = w_tar = 0;
    w_verbose = verbose;
    w_abort = 0;
    w_root = path;
    w_last = now();
    int r = nftw(path, walk_cb, 32, FTW_PHYS);
    return r == 0 && !w_abort ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* folder list                                                        */

static void add_source(const char *path)
{
    if (strcmp(path, "/") == 0) {
        setmsg(1, "Refusing to back up the whole filesystem root.");
        return;
    }
    for (int i = 0; i < nsrc; i++) {
        if (strcmp(src[i].path, path) == 0) {
            setmsg(1, "Already in the list: %s", path);
            return;
        }
        if (is_inside(path, src[i].path) || is_inside(src[i].path, path)) {
            setmsg(1, "Overlaps with %s (one is inside the other).", src[i].path);
            return;
        }
        if (strcmp(base_of(path), base_of(src[i].path)) == 0) {
            setmsg(1, "A folder named '%s' is already in the list (names must be unique).",
                   base_of(path));
            return;
        }
    }
    if (nsrc == MAX_SRC) {
        setmsg(1, "Too many folders (max %d).", MAX_SRC);
        return;
    }
    if (scan(path, 0) < 0) {
        setmsg(1, w_abort ? "Cancelled." : "Could not read %s", path);
        return;
    }
    char *s = strdup(path);
    if (!s) {
        setmsg(1, "Out of memory.");
        return;
    }
    src[nsrc].path = s;
    src[nsrc].bytes = w_bytes;
    src[nsrc].files = w_files;
    src[nsrc].errors = w_errors;
    nsrc++;
    if (w_errors)
        setmsg(1, "Added, but %lld item(s) cannot be read: fix permissions before backing up.", w_errors);
    else
        setmsg(0, "Added %s", path);
}

static void remove_source(int k)
{
    if (k < 0 || k >= nsrc)
        return;
    setmsg(0, "Removed %s", src[k].path);
    free(src[k].path);
    memmove(&src[k], &src[k + 1], (size_t)(nsrc - k - 1) * sizeof src[0]);
    nsrc--;
}

/* ------------------------------------------------------------------ */
/* folder picker                                                      */

enum { PICK_SRC, PICK_DEST };
static char pk_cur[PATH_MAX], pk_last_src[PATH_MAX], pk_msg[PATH_MAX + 128];
static char **ents;
static int nents, nfixed, pk_sel, pk_top, pk_mode, pk_hidden;

static int cmp_name(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

static void free_ents(void)
{
    for (int i = 0; i < nents; i++)
        free(ents[i]);
    free(ents);
    ents = NULL;
    nents = 0;
}

static int push_ent(char ***v, int *n, int *cap, const char *s)
{
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 64;
        char **nv = realloc(*v, (size_t)nc * sizeof *nv);
        if (!nv)
            return -1;
        *v = nv;
        *cap = nc;
    }
    if (!((*v)[*n] = strdup(s)))
        return -1;
    (*n)++;
    return 0;
}

/* Load the subfolders of path. Entry 0 is "." (use this folder). */
static int pk_load(const char *path, const char *select_name)
{
    char dir[PATH_MAX], full[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", path);
    DIR *d = opendir(dir);
    if (!d) {
        snprintf(pk_msg, sizeof pk_msg, "Cannot open %s: %s", dir, strerror(errno));
        return -1;
    }
    char **v = NULL;
    int n = 0, cap = 0, fixed;
    push_ent(&v, &n, &cap, ".");
    if (strcmp(dir, "/") != 0)
        push_ent(&v, &n, &cap, "..");
    fixed = n;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (!pk_hidden && e->d_name[0] == '.')
            continue;
        if (join(full, dir, e->d_name) == 0 && is_dir(full))
            if (push_ent(&v, &n, &cap, e->d_name) < 0)
                break;
    }
    closedir(d);
    qsort(v + fixed, (size_t)(n - fixed), sizeof *v, cmp_name);
    free_ents();
    ents = v;
    nents = n;
    nfixed = fixed;
    snprintf(pk_cur, sizeof pk_cur, "%s", dir);
    pk_top = 0;
    pk_sel = nents > nfixed ? nfixed : 0; /* first subfolder, not "use this" */
    for (int i = 0; select_name && i < nents; i++)
        if (strcmp(ents[i], select_name) == 0)
            pk_sel = i;
    return 0;
}

static int already_added(const char *path)
{
    for (int i = 0; i < nsrc; i++)
        if (strcmp(src[i].path, path) == 0)
            return 1;
    return 0;
}

static void draw_picker(void)
{
    if (too_small())
        return;
    erase();
    nhits = 0;
    title_bar(pk_mode == PICK_SRC ? "bak - add a folder to back up" : "bak - choose where to save backups",
              NULL);
    mvaddstr(1, 1, "In: ");
    attron(A_BOLD);
    put(1, 5, COLS - 6, pk_cur, 1);
    attroff(A_BOLD);

    int bh = LINES - 5, h = bh - 2;
    draw_box(2, 0, bh, COLS, NULL);
    if (pk_sel < pk_top)
        pk_top = pk_sel;
    if (pk_sel >= pk_top + h)
        pk_top = pk_sel - h + 1;
    for (int i = pk_top; i < nents && i < pk_top + h; i++) {
        int y = 3 + i - pk_top;
        char text[PATH_MAX + 8], full[PATH_MAX];
        int added = 0;
        if (i == 0)
            snprintf(text, sizeof text, "%s",
                     pk_mode == PICK_SRC ? "[ Add this folder ]" : "[ Save backups in this folder ]");
        else if (!strcmp(ents[i], ".."))
            snprintf(text, sizeof text, "../   (parent folder)");
        else {
            snprintf(text, sizeof text, "%s/", ents[i]);
            added = pk_mode == PICK_SRC && join(full, pk_cur, ents[i]) == 0 && already_added(full);
        }
        if (i == 0 && pk_mode == PICK_SRC && already_added(pk_cur))
            added = 1;
        if (i == pk_sel)
            attron(A_REVERSE);
        mvhline(y, 1, ' ', COLS - 2);
        if (i == 0)
            attron(A_BOLD);
        put(y, 2, COLS - 14, text, 0);
        if (i == 0)
            attroff(A_BOLD);
        if (added) {
            attron(cp(C_OK));
            mvaddstr(y, COLS - 10, "(added)");
            attroff(cp(C_OK));
        }
        if (i == pk_sel)
            attroff(A_REVERSE);
        hit_add(y, 1, COLS - 2, ID_ENT + i);
    }
    if (nents <= nfixed) {
        attron(A_DIM);
        mvaddstr(3 + nfixed, 2, pk_hidden ? "(no subfolders)" : "(no subfolders; '.' shows hidden ones)");
        attroff(A_DIM);
    }
    if (nents > h)
        mvprintw(bh + 1, COLS - 16, " %d/%d ", pk_sel + 1, nents);

    attron(cp(C_ERR) | A_BOLD);
    put(LINES - 3, 1, COLS - 2, pk_msg, 0);
    attroff(cp(C_ERR) | A_BOLD);

    struct btn b[] = {{'\n', "Open (Enter)"},  {' ', "Select (Space)"}, {KEY_LEFT, "Up (Bksp)"},
                      {'~', "Home (~)"},       {'m', "Drives (m)"},     {'/', "Type path (/)"},
                      {'.', "Hidden (.)"},     {27, "Cancel (Esc)"}};
    draw_buttons(LINES - 2, LINES - 1, 1, b, 8);
}

static void pk_up(void)
{
    char parent[PATH_MAX], prev[PATH_MAX];
    if (!strcmp(pk_cur, "/"))
        return;
    snprintf(prev, sizeof prev, "%s", base_of(pk_cur));
    snprintf(parent, sizeof parent, "%s", pk_cur);
    char *s = strrchr(parent, '/');
    if (s == parent)
        s[1] = '\0';
    else if (s)
        *s = '\0';
    pk_load(parent, prev);
}

/* Validate a choice; on success copy it to out and return 1. */
static int pk_accept(const char *path, char *out)
{
    if (pk_mode == PICK_SRC) {
        if (!strcmp(path, "/")) {
            snprintf(pk_msg, sizeof pk_msg, "Refusing to back up the whole filesystem root.");
            return 0;
        }
        if (access(path, R_OK | X_OK) != 0) {
            snprintf(pk_msg, sizeof pk_msg, "Cannot read that folder: %s", strerror(errno));
            return 0;
        }
    } else if (access(path, W_OK | X_OK) != 0) {
        snprintf(pk_msg, sizeof pk_msg, "Cannot write there: %s", strerror(errno));
        return 0;
    }
    snprintf(out, PATH_MAX, "%s", path);
    return 1;
}

static int pk_entry_path(int i, char *out)
{
    if (i < nfixed)
        return snprintf(out, PATH_MAX, "%s", pk_cur) >= PATH_MAX ? -1 : 0;
    char full[PATH_MAX];
    if (join(full, pk_cur, ents[i]) < 0 || !realpath(full, out)) {
        snprintf(pk_msg, sizeof pk_msg, "Cannot open %s: %s", ents[i], strerror(errno));
        return -1;
    }
    return 0;
}

/* Full-screen folder browser. Returns 1 with the chosen folder in out. */
static int pick_dir(int mode, char *out)
{
    char start[PATH_MAX], m[PATH_MAX], p[PATH_MAX];
    const char *home = getenv("HOME");
    void (*saved_bg)(void) = bg_draw;
    int result = 0;

    pk_mode = mode;
    pk_msg[0] = '\0';
    if (mode == PICK_SRC && pk_last_src[0])
        snprintf(start, sizeof start, "%s", pk_last_src);
    else if (mode == PICK_DEST && dest[0])
        snprintf(start, sizeof start, "%s", dest);
    else if (mode == PICK_DEST && media_dir(m))
        snprintf(start, sizeof start, "%s", m);
    else
        snprintf(start, sizeof start, "%s", home ? home : "/");
    if (pk_load(start, NULL) < 0 && pk_load("/", NULL) < 0)
        return 0;
    pk_msg[0] = '\0';
    bg_draw = draw_picker;

    for (;;) {
        draw_picker();
        refresh();
        int ch = get_input();
        if (ch == EV_CLICK) {
            if (ev_id >= ID_ENT && ev_id < ID_ENT + nents) {
                int i = ev_id - ID_ENT;
                if (i != pk_sel) { /* first click selects, second opens */
                    pk_sel = i;
                    continue;
                }
                ch = '\n';
            } else if (ev_id < 1000) {
                ch = ev_id;
            } else {
                continue;
            }
        }
        if (ch != 0 && ch != ERR && ch != KEY_RESIZE)
            pk_msg[0] = '\0';
        int page = LINES - 7 > 1 ? LINES - 7 : 1;
        if (ch == KEY_UP || ch == 'k')
            pk_sel = pk_sel > 0 ? pk_sel - 1 : 0;
        else if (ch == KEY_DOWN || ch == 'j')
            pk_sel = pk_sel < nents - 1 ? pk_sel + 1 : pk_sel;
        else if (ch == KEY_PPAGE)
            pk_sel = pk_sel > page ? pk_sel - page : 0;
        else if (ch == KEY_NPAGE)
            pk_sel = pk_sel + page < nents ? pk_sel + page : nents - 1;
        else if (ch == KEY_HOME || ch == 'g')
            pk_sel = 0;
        else if (ch == KEY_END || ch == 'G')
            pk_sel = nents - 1;
        else if (is_enter(ch) || ch == KEY_RIGHT || ch == 'l') {
            if (pk_sel == 0) {
                if (is_enter(ch) && pk_accept(pk_cur, out)) {
                    result = 1;
                    break;
                }
            } else if (!strcmp(ents[pk_sel], "..")) {
                pk_up();
            } else if (pk_entry_path(pk_sel, p) == 0) {
                pk_load(p, NULL);
            }
        } else if (ch == ' ') {
            if (pk_entry_path(pk_sel, p) == 0 && pk_accept(p, out)) {
                result = 1;
                break;
            }
        } else if (is_back(ch) || ch == KEY_LEFT || ch == 'h') {
            pk_up();
        } else if (ch == '~') {
            if (home)
                pk_load(home, NULL);
        } else if (ch == 'm') {
            if (media_dir(m))
                pk_load(m, NULL);
            else
                snprintf(pk_msg, sizeof pk_msg, "No drives folder found (/run/media, /media, /mnt).");
        } else if (ch == '.') {
            char keep[PATH_MAX];
            snprintf(keep, sizeof keep, "%s", ents[pk_sel]);
            pk_hidden = !pk_hidden;
            pk_load(pk_cur, keep);
        } else if (ch == '/') {
            char t[PATH_MAX] = "/";
            if (input_box("Go to folder", "Type a path (~ works), e.g. /run/media/me/USB", t, sizeof t, 0) &&
                t[0]) {
                if (resolve(t, p) < 0 || !is_dir(p))
                    snprintf(pk_msg, sizeof pk_msg, "Not a folder: %s", t);
                else
                    pk_load(p, NULL);
            }
        } else if (ch == 27 || ch == 'q' || ch == 3) {
            break;
        }
    }
    if (result && mode == PICK_SRC) {
        /* reopen next time in the parent of what was just added */
        char *s;
        snprintf(pk_last_src, sizeof pk_last_src, "%s", out);
        s = strrchr(pk_last_src, '/');
        if (s == pk_last_src)
            s[1] = '\0';
        else if (s)
            *s = '\0';
    }
    free_ents();
    bg_draw = saved_bg;
    return result;
}

/* ------------------------------------------------------------------ */
/* main screen                                                        */

static int focus, src_top;
static const char *set_names[NSET] = {"Destination", "Compression", "Encryption", "Upload", "File name"};
static const char set_keys[NSET] = {'d', 'c', 'e', 'u', 'n'};

static void draw_main(void)
{
    char a[32], b[32], line[PATH_MAX + 64];
    if (too_small())
        return;
    erase();
    nhits = 0;
    title_bar("bak - simple verified backups", "keyboard or mouse");

    /* folders */
    int bh = LINES - 10, vis = bh - 2;
    long long tb = 0, tf = 0;
    draw_box(1, 0, bh, COLS, "Folders to back up");
    if (focus < nsrc) {
        if (focus < src_top)
            src_top = focus;
        if (focus >= src_top + vis)
            src_top = focus - vis + 1;
    }
    if (src_top > nsrc - vis)
        src_top = nsrc - vis > 0 ? nsrc - vis : 0;
    if (!nsrc) {
        attron(A_DIM);
        put(2, 2, COLS - 4, "No folders yet. Press 'a' or click \"Add folder\" below.", 0);
        attroff(A_DIM);
    }
    for (int i = 0; i < nsrc; i++) {
        tb += src[i].bytes;
        tf += src[i].files;
        if (i < src_top || i >= src_top + vis)
            continue;
        int y = 2 + i - src_top, f = focus == i;
        human((double)src[i].bytes, a, sizeof a);
        fmt_count(src[i].files, b, sizeof b);
        snprintf(line, sizeof line, "%10s %12s file%s", a, b, src[i].files == 1 ? " " : "s");
        int infox = COLS - 6 - (int)strlen(line), pw = infox - 3;
        if (f)
            attron(A_REVERSE);
        mvhline(y, 1, ' ', COLS - 2);
        if (src[i].errors) {
            char e[48];
            snprintf(e, sizeof e, "%lld unreadable", src[i].errors);
            pw -= (int)strlen(e) + 2;
            attron(cp(C_ERR) | A_BOLD);
            mvaddstr(y, infox - (int)strlen(e) - 1, e);
            attroff(cp(C_ERR) | A_BOLD);
        }
        put(y, 2, pw, src[i].path, 1);
        mvaddstr(y, infox, line);
        if (f)
            attroff(A_REVERSE);
        attron(cp(C_ERR));
        mvaddstr(y, COLS - 5, "[x]");
        attroff(cp(C_ERR));
        hit_add(y, 1, COLS - 7, ID_ROW + i);
        hit_add(y, COLS - 5, 3, ID_DEL + i);
    }
    if (nsrc) {
        human((double)tb, a, sizeof a);
        fmt_count(tf, b, sizeof b);
        snprintf(line, sizeof line, " %d folder%s, %s, %s file%s ", nsrc, nsrc == 1 ? "" : "s", a, b,
                 tf == 1 ? "" : "s");
        put(bh, 2, COLS - 4, line, 0);
    }
    if (src_top > 0 || src_top + vis < nsrc)
        mvaddstr(1, COLS - 18, " (scroll: wheel) ");

    /* settings */
    int y0 = bh + 1;
    draw_box(y0, 0, 7, COLS, "Settings");
    for (int k = 0; k < NSET; k++) {
        int y = y0 + 1 + k, f = focus == nsrc + k;
        switch (k) {
        case 0:
            if (!dest[0])
                snprintf(line, sizeof line, "(not set - choose where to save)");
            else if (dest_free >= 0) {
                human((double)dest_free, a, sizeof a);
                snprintf(line, sizeof line, "%s   (%s free)", dest, a);
            } else
                snprintf(line, sizeof line, "%s", dest);
            break;
        case 1:
            snprintf(line, sizeof line, "%s", comps[comp_sel].desc);
            break;
        case 2:
            snprintf(line, sizeof line, "%s", encrypt ? "On - gpg AES-256, passphrase asked at start" : "Off");
            break;
        case 3:
            snprintf(line, sizeof line, "%s", remote[0] ? remote : "Off (only saved locally)");
            break;
        case 4:
            snprintf(line, sizeof line, "%s-YYYYMMDD-HHMMSS.tar.xz%s", label, encrypt ? ".gpg" : "");
            break;
        }
        if (f)
            attron(A_REVERSE);
        mvhline(y, 1, ' ', COLS - 2);
        attron(A_BOLD);
        mvaddstr(y, 2, set_names[k]);
        attroff(A_BOLD);
        put(y, 16, COLS - 23, line, k == 0);
        mvprintw(y, COLS - 5, "(%c)", set_keys[k]);
        if (f)
            attroff(A_REVERSE);
        hit_add(y, 1, COLS - 2, ID_SET + k);
    }

    /* message + buttons */
    if (msg[0]) {
        attron(msg_err ? (int)(cp(C_ERR) | A_BOLD) : cp(C_OK));
        put(LINES - 2, 1, COLS - 2, msg, 0);
        attroff(msg_err ? (int)(cp(C_ERR) | A_BOLD) : cp(C_OK));
    } else {
        attron(A_DIM);
        put(LINES - 2, 1, COLS - 2, "Up/Down: move   Enter/click: change setting   r/Del: remove folder", 0);
        attroff(A_DIM);
    }
    struct btn bt[] = {{'a', "Add folder (a)"}, {'r', "Remove (r)"}, {'s', "Start backup (s)"},
                       {'q', "Quit (q)"}};
    draw_buttons(LINES - 1, LINES - 1, 1, bt, 4);
}

static void edit_setting(int k)
{
    char path[PATH_MAX], out[65536];
    switch (k) {
    case 0:
        if (pick_dir(PICK_DEST, path)) {
            snprintf(dest, sizeof dest, "%s", path);
            update_dest_free();
            setmsg(0, "Backups will be saved in %s", dest);
        }
        break;
    case 1: {
        const char *o[NCOMP];
        for (int i = 0; i < NCOMP; i++)
            o[i] = comps[i].desc;
        int r = choose("Compression", o, NCOMP, comp_sel);
        if (r >= 0)
            comp_sel = r;
        break;
    }
    case 2: {
        const char *o[] = {"Off", "On - gpg AES-256 with a passphrase"};
        if (encrypt == 0 && !in_path("gpg")) {
            setmsg(1, "gpg is not installed (pacman -S gnupg).");
            break;
        }
        int r = choose("Encryption", o, 2, encrypt);
        if (r >= 0)
            encrypt = r;
        break;
    }
    case 3: {
        if (!in_path("rclone")) {
            setmsg(1, "rclone is not installed (pacman -S rclone), so uploading is not available.");
            break;
        }
        char *argv[] = {"rclone", "listremotes", NULL};
        if (!run_capture(argv, out, sizeof out)) {
            setmsg(1, "Could not list rclone remotes.");
            break;
        }
        const char *o[33];
        int n = 0;
        o[n++] = "Off (only save locally)";
        for (char *l = strtok(out, "\n"); l && n < 33; l = strtok(NULL, "\n"))
            o[n++] = l;
        if (n == 1) {
            setmsg(1, "No rclone remotes configured. Run 'rclone config' first.");
            break;
        }
        int r = choose("Upload to (rclone remote)", o, n, 0);
        if (r == 0) {
            remote[0] = '\0';
            setmsg(0, "Upload off.");
        } else if (r > 0) {
            char p[LINE] = "backups";
            const char *c = strchr(remote, ':');
            if (c && !strncmp(remote, o[r], (size_t)(c - remote + 1)))
                snprintf(p, sizeof p, "%s", c + 1);
            if (input_box("Folder on the remote", "Where to put backups on the remote (empty = top level)", p,
                          sizeof p, 0)) {
                snprintf(remote, sizeof remote, "%s%s", o[r], p);
                setmsg(0, "Backups will also be uploaded to %s", remote);
            }
        }
        break;
    }
    case 4: {
        char t[64];
        snprintf(t, sizeof t, "%s", label);
        if (!input_box("File name prefix", "Letters, digits, '.', '_' and '-' (up to 40)", t, sizeof t, 0))
            break;
        size_t len = strlen(t);
        if (len == 0 || len > 40 || t[0] == '.' || t[0] == '-' ||
            strspn(t, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != len)
            setmsg(1, "Invalid name. Use up to 40 letters, digits, '.', '_' or '-'.");
        else
            snprintf(label, sizeof label, "%.40s", t);
        break;
    }
    }
}

/* ------------------------------------------------------------------ */
/* the backup                                                         */

static long long tot_bytes, tot_tar, tot_entries;

static int remote_exists(const char *r)
{
    char name[LINE], out[65536];
    const char *colon = strchr(r, ':');
    if (!colon || (size_t)(colon - r) + 2 > sizeof name)
        return 0;
    size_t n = (size_t)(colon - r);
    memcpy(name, r, n);
    name[n] = ':';
    name[n + 1] = '\0';
    char *argv[] = {"rclone", "listremotes", NULL};
    if (!run_capture(argv, out, sizeof out))
        return 0;
    for (char *line = strtok(out, "\n"); line; line = strtok(NULL, "\n"))
        if (strcmp(line, name) == 0)
            return 1;
    return 0;
}

/* Fills final/partial names. Returns 1 if it is safe to start. */
static int preflight(char *final, char *partial, char *fname, size_t n)
{
    int ok = 1;
    char a[32], b[32];
    logmsg("Checking everything before writing anything...");

    if (nsrc == 0) {
        logmsg("! No folders selected.");
        ok = 0;
    }
    if (!dest[0]) {
        logmsg("! No destination chosen.");
        ok = 0;
    }
    if (!ok)
        return 0;

    const char *tools[] = {"tar", "xz", encrypt ? "gpg" : NULL, remote[0] ? "rclone" : NULL};
    for (int i = 0; i < 4; i++)
        if (tools[i] && !in_path(tools[i])) {
            logmsg("! Required program not found: %s", tools[i]);
            ok = 0;
        }

    if (!is_dir(dest)) {
        logmsg("! Destination is not a folder anymore: %s", dest);
        ok = 0;
    } else if (access(dest, W_OK | X_OK) != 0) {
        logmsg("! Destination is not writable: %s", dest);
        ok = 0;
    }
    for (int i = 0; i < nsrc; i++) {
        if (!is_dir(src[i].path)) {
            logmsg("! Folder is gone: %s", src[i].path);
            ok = 0;
            continue;
        }
        if (is_inside(dest, src[i].path)) {
            logmsg("! Destination is inside %s (it would back up itself)", src[i].path);
            ok = 0;
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(base_of(src[i].path), base_of(src[j].path)) == 0) {
                logmsg("! Two folders are named '%s' (they would collide in the archive)",
                       base_of(src[i].path));
                ok = 0;
            }
            if (is_inside(src[i].path, src[j].path) || is_inside(src[j].path, src[i].path)) {
                logmsg("! Folders overlap: %s and %s", src[i].path, src[j].path);
                ok = 0;
            }
        }
    }
    if (!ok)
        return 0;

    long long errs = 0;
    tot_bytes = tot_tar = tot_entries = 0;
    for (int i = 0; i < nsrc; i++) {
        logmsg("   scanning %s", src[i].path);
        if (scan(src[i].path, 1) < 0) {
            logmsg(w_abort ? "! Cancelled." : "! Could not scan %s", src[i].path);
            return 0;
        }
        src[i].bytes = w_bytes;
        src[i].files = w_files;
        src[i].errors = w_errors;
        tot_bytes += w_bytes;
        tot_tar += w_tar;
        tot_entries += w_entries;
        errs += w_errors;
    }
    tot_tar += 10240; /* tar end-of-archive padding */
    draw_run();
    if (errs) {
        logmsg("! %lld item(s) cannot be read. Fix permissions or remove those folders.", errs);
        return 0;
    }
    fmt_count(tot_entries, a, sizeof a);
    human((double)tot_bytes, b, sizeof b);
    logmsg("ok  %s items, %s, all readable", a, b);

    char stamp[32];
    time_t t = time(NULL);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&t));
    snprintf(fname, n, "%s-%s.tar.xz%s", label, stamp, encrypt ? ".gpg" : "");
    if (snprintf(final, n, "%s/%s", dest, fname) >= (int)n ||
        snprintf(partial, n, "%s/.%s.partial", dest, fname) >= (int)n) {
        logmsg("! Destination path too long.");
        return 0;
    }
    if (access(final, F_OK) == 0 || access(partial, F_OK) == 0) {
        logmsg("! Output file already exists: %s (try again in a second)", final);
        return 0;
    }

    /* compressed size is unknown, so worst case = input + overhead */
    unsigned long long need = (unsigned long long)tot_tar + (unsigned long long)tot_tar / 100 + 16ULL * 1024 * 1024;
    update_dest_free();
    if (dest_free >= 0) {
        human((double)dest_free, a, sizeof a);
        logmsg("ok  %s free at destination", a);
        if ((unsigned long long)dest_free < need) {
            logmsg("? Free space is less than the uncompressed size. It will probably fit after");
            logmsg("? compression, but that is not guaranteed. If it runs out, nothing is kept.");
            if (!confirm("Low free space. Continue anyway?", 0))
                return 0;
        }
    }
#ifdef __linux__
    struct statfs sfs;
    if (statfs(dest, &sfs) == 0 && sfs.f_type == MSDOS_MAGIC && need >= 4ULL * 1024 * 1024 * 1024) {
        logmsg("? The destination is FAT32, which cannot hold files of 4 GiB or more.");
        logmsg("? The backup may fail (it would be discarded safely).");
        if (!confirm("FAT32 size limit. Continue anyway?", 0))
            return 0;
    }
#endif
    if (remote[0]) {
        if (!remote_exists(remote)) {
            logmsg("! rclone remote not configured: %s (see 'rclone config')", remote);
            return 0;
        }
        logmsg("ok  rclone remote found");
    }
    return 1;
}

static int ask_passphrase(char *pass)
{
    char again[PASS_MAX];
    int ok = 0;
    for (int tries = 0; tries < 3 && !ok; tries++) {
        pass[0] = '\0';
        if (!input_box("Passphrase", "At least 8 characters. Typing is hidden.", pass, PASS_MAX, 1))
            break;
        if (strlen(pass) < 8) {
            logmsg("! Passphrase too short.");
            continue;
        }
        again[0] = '\0';
        if (!input_box("Repeat passphrase", "Type it again to make sure.", again, PASS_MAX, 1))
            break;
        if (strcmp(pass, again) != 0)
            logmsg("! Passphrases do not match.");
        else
            ok = 1;
    }
    wipe(again, sizeof again);
    if (!ok)
        wipe(pass, PASS_MAX);
    return ok;
}

static int create_archive(const char *partial, const char *pass)
{
    char *tar[3 + 3 * MAX_SRC + 1];
    static char parents[MAX_SRC][PATH_MAX], names[MAX_SRC][PATH_MAX];
    int a = 0;
    tar[a++] = "tar";
    tar[a++] = "-cf";
    tar[a++] = "-";
    for (int i = 0; i < nsrc; i++) {
        const char *b = base_of(src[i].path);
        size_t plen = (size_t)(b - src[i].path) - 1;
        if (plen == 0)
            plen = 1; /* parent is "/" */
        memcpy(parents[i], src[i].path, plen);
        parents[i][plen] = '\0';
        snprintf(names[i], PATH_MAX, "./%s", b); /* "./" so names never look like options */
        tar[a++] = "-C";
        tar[a++] = parents[i];
        tar[a++] = names[i];
    }
    tar[a] = NULL;

    char *xz[] = {"xz", (char *)comps[comp_sel].level, (char *)comps[comp_sel].threads, "--check=sha256",
                  "-c", NULL};
    char *gpg[] = {"gpg", "--batch", "--yes", "--quiet", "--no-symkey-cache",
                   "--pinentry-mode", "loopback", "--passphrase-fd", "3",
                   "--symmetric", "--cipher-algo", "AES256",
                   "--s2k-mode", "3", "--s2k-digest-algo", "SHA512", "--s2k-count", "65011712",
                   "--compress-algo", "none", "-o", "-", NULL};
    char **cmds[3] = {tar, xz, gpg};

    int null = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int out = open(partial, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (null < 0 || out < 0) {
        logmsg("! Cannot create %s: %s", partial, strerror(errno));
        if (null >= 0)
            close(null);
        if (out >= 0)
            close(out);
        return 0;
    }
    /* bak sits between tar and xz to count progress */
    int ok = run_pipeline(cmds, encrypt ? 3 : 2, null, out, encrypt ? 2 : -1, pass, 1);
    if (ok && fsync(out) != 0) {
        logmsg("! Could not flush the archive to disk: %s", strerror(errno));
        ok = 0;
    }
    if (close(out) != 0)
        ok = 0;
    close(null);
    return ok;
}

static int verify_archive(const char *partial, const char *pass)
{
    char *gpg[] = {"gpg", "--batch", "--quiet", "--no-symkey-cache",
                   "--pinentry-mode", "loopback", "--passphrase-fd", "3",
                   "--decrypt", "-o", "-", NULL};
    char *xz[] = {"xz", "-dc", NULL};
    char *tar[] = {"tar", "-tf", "-", NULL};
    char **cmds[3];
    int n = 0;
    if (encrypt)
        cmds[n++] = gpg;
    cmds[n++] = xz;
    cmds[n++] = tar;

    int in = open(partial, O_RDONLY | O_CLOEXEC);
    int null = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (in < 0 || null < 0) {
        logmsg("! Cannot read back %s: %s", partial, strerror(errno));
        if (in >= 0)
            close(in);
        if (null >= 0)
            close(null);
        return 0;
    }
    /* bak feeds the file into the first program to count progress */
    int ok = run_pipeline(cmds, n, in, null, encrypt ? 0 : -1, pass, 0);
    close(in);
    close(null);
    return ok;
}

static int upload(const char *final, const char *fname)
{
    char target[LINE + PATH_MAX], filter[PATH_MAX + 2];
    size_t rl = strlen(remote);
    const char *sep = (remote[rl - 1] == ':' || remote[rl - 1] == '/') ? "" : "/";
    snprintf(target, sizeof target, "%s%s%s", remote, sep, fname);
    snprintf(filter, sizeof filter, "/%s", fname);

    abort_q = "Abort the upload? The local backup is kept.";
    set_phase("Uploading", 0);
    logmsg("   uploading to %s", target);
    char *cp_[] = {"rclone", "copyto", (char *)final, target, NULL};
    int ok = run1(cp_);
    log_children();
    if (!ok)
        return 0;
    set_phase("Checking the uploaded copy", 0);
    logmsg("   checking the uploaded copy");
    char *ck[] = {"rclone", "check", "--one-way", "--include", filter, dest, remote, NULL};
    ok = run1(ck);
    log_children();
    return ok;
}

static void do_backup(void)
{
    char final[PATH_MAX], partial[PATH_MAX], fname[PATH_MAX], a[32], b[32];
    static char pass[PASS_MAX];
    int result = 0; /* 0 nothing saved, 1 saved, 2 saved but upload failed */
    struct stat st;

    log_clear();
    run_done = 0;
    bg_draw = draw_run;
    if (ftruncate(log_fd, 0) != 0)
        logmsg("  | (could not reset program output log)");
    set_phase("Checking", 0);
    abort_q = "Abort?";

    if (!preflight(final, partial, fname, sizeof final)) {
        logmsg("Nothing was written.");
        goto out;
    }
    logmsg("");
    logmsg("Will create:\n   %s", final);
    if (remote[0])
        logmsg("Then upload to:\n   %s", remote);
    if (!confirm("Start the backup?", 1)) {
        logmsg("Cancelled. Nothing was written.");
        goto out;
    }
    if (encrypt) {
        logmsg("Remember the passphrase: without it the backup cannot be restored.");
        if (!ask_passphrase(pass)) {
            logmsg("Cancelled. Nothing was written.");
            goto out;
        }
    }

    abort_q = "Abort the backup? Nothing will be saved.";
    set_phase(encrypt ? "Compressing + encrypting" : "Compressing", tot_tar);
    logmsg("   compressing (%s)", comps[comp_sel].desc);
    int ok = create_archive(partial, pass);
    log_children();
    if (!ok) {
        unlink(partial);
        logmsg(aborted ? "! Aborted. Partial file removed; nothing was saved."
                       : "! FAILED while creating the archive. Partial file removed; nothing was saved.");
        goto out;
    }
    if (stat(partial, &st) != 0) {
        logmsg("! The archive disappeared: %s", strerror(errno));
        goto out;
    }
    set_phase("Verifying (full read-back)", st.st_size);
    logmsg("   verifying: full %sdecompress and read-back", encrypt ? "decrypt, " : "");
    ok = verify_archive(partial, pass);
    log_children();
    if (!ok) {
        unlink(partial);
        logmsg(aborted ? "! Aborted. Archive removed; nothing was saved."
                       : "! FAILED verification. Archive removed; nothing was saved.");
        goto out;
    }
    if (chmod(partial, 0400) != 0 || access(final, F_OK) == 0 || rename(partial, final) != 0 ||
        fsync_dir(dest) != 0) {
        logmsg("! FAILED to finalize %s: %s", final, strerror(errno));
        unlink(partial);
        goto out;
    }
    result = 1;
    human((double)st.st_size, a, sizeof a);
    human((double)tot_bytes, b, sizeof b);
    logmsg("OK  saved:\nOK     %s", final);
    logmsg("OK  %s (from %s, %.1f%%), verified, read-only", a, b,
           tot_bytes ? 100.0 * (double)st.st_size / (double)tot_bytes : 100.0);

    if (remote[0]) {
        if (upload(final, fname))
            logmsg("OK  upload verified");
        else {
            result = 2;
            logmsg(aborted ? "? Upload aborted. The local backup is fine." : "! UPLOAD FAILED. The local backup is fine.");
        }
    }
    logmsg("");
    logmsg("To restore (extracts into the current folder):");
    logmsg(encrypt ? "   gpg -d FILE | xz -dc | tar -xf -" : "   xz -dc FILE | tar -xf -");

out:
    wipe(pass, sizeof pass);
    run_done = 1;
    pg_total = 0;
    if (result == 1) {
        snprintf(phase, sizeof phase, "Done: backup saved and verified.");
        phase_attr = cp(C_OK);
    } else if (result == 2) {
        snprintf(phase, sizeof phase, "Backup saved and verified, but the upload did not complete.");
        phase_attr = cp(C_WARN);
    } else {
        snprintf(phase, sizeof phase, "Stopped: nothing was saved.");
        phase_attr = cp(C_ERR);
    }
    for (;;) {
        draw_run();
        refresh();
        int ch = get_input();
        if (ch == EV_CLICK) {
            if (ev_id == '\n')
                break;
            continue;
        }
        int page = LINES - 8 > 1 ? LINES - 8 : 1;
        if (ch == KEY_UP || ch == 'k')
            log_scroll++;
        else if (ch == KEY_DOWN || ch == 'j')
            log_scroll--;
        else if (ch == KEY_PPAGE)
            log_scroll += page;
        else if (ch == KEY_NPAGE)
            log_scroll -= page;
        else if (is_enter(ch) || ch == 27 || ch == 'q' || ch == ' ' || ch == 3)
            break;
    }
    update_dest_free();
    if (result)
        setmsg(result == 2, result == 1 ? "Last backup: %s" : "Last backup saved (upload failed): %s", final);
    bg_draw = draw_main;
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "bak needs an interactive terminal.\n");
        return 1;
    }
    FILE *lf = tmpfile();
    if (!lf) {
        perror("bak: cannot create temporary log");
        return 1;
    }
    log_fd = fileno(lf);
    fcntl(log_fd, F_SETFD, FD_CLOEXEC);
    fcntl(log_fd, F_SETFL, fcntl(log_fd, F_GETFL) | O_APPEND);

    signal(SIGPIPE, SIG_IGN);
    umask(077); /* backups are private */

    initscr();
    raw(); /* Ctrl-C is a key: we decide what it does */
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    set_escdelay(25);
    mouseinterval(0);
#ifdef BUTTON5_PRESSED
    mousemask(BUTTON1_PRESSED | BUTTON4_PRESSED | BUTTON5_PRESSED, NULL);
#else
    mousemask(BUTTON1_PRESSED | BUTTON4_PRESSED, NULL);
#endif
    if (has_colors()) {
        colors = 1;
        start_color();
        use_default_colors();
        init_pair(C_TITLE, COLOR_BLACK, COLOR_CYAN);
        init_pair(C_ERR, COLOR_RED, -1);
        init_pair(C_WARN, COLOR_YELLOW, -1);
        init_pair(C_OK, COLOR_GREEN, -1);
        init_pair(C_BAR, COLOR_BLACK, COLOR_GREEN);
    }
    bg_draw = draw_main;

    for (int i = 1; i < argc; i++) {
        char p[PATH_MAX];
        if (resolve(argv[i], p) < 0 || !is_dir(p))
            setmsg(1, "Not a folder: %s", argv[i]);
        else
            add_source(p);
    }

    for (;;) {
        int nitems = nsrc + NSET;
        if (focus >= nitems)
            focus = nitems - 1;
        draw_main();
        refresh();
        int ch = get_input();
        if (ch == EV_CLICK) {
            if (ev_id < 1000)
                ch = ev_id;
            else if (ev_id >= ID_DEL && ev_id < ID_DEL + MAX_SRC) {
                remove_source(ev_id - ID_DEL);
                continue;
            } else if (ev_id >= ID_ROW && ev_id < ID_ROW + MAX_SRC) {
                focus = ev_id - ID_ROW;
                continue;
            } else if (ev_id >= ID_SET && ev_id < ID_SET + NSET) {
                focus = nsrc + ev_id - ID_SET;
                edit_setting(ev_id - ID_SET);
                continue;
            } else
                continue;
        }
        if (ch != ERR && ch != 0 && ch != KEY_RESIZE)
            msg[0] = '\0';
        switch (ch) {
        case KEY_UP:
        case 'k':
            if (focus > 0)
                focus--;
            break;
        case KEY_DOWN:
        case 'j':
        case '\t':
            if (focus < nitems - 1)
                focus++;
            break;
        case KEY_HOME:
            focus = 0;
            break;
        case KEY_END:
            focus = nitems - 1;
            break;
        case '\n':
        case '\r':
        case KEY_ENTER:
        case ' ':
        case KEY_RIGHT:
            if (focus >= nsrc)
                edit_setting(focus - nsrc);
            break;
        case 'a':
        case '+':
        case KEY_IC: {
            char p[PATH_MAX];
            if (pick_dir(PICK_SRC, p))
                add_source(p);
            break;
        }
        case 'r':
        case '-':
        case KEY_DC:
        case KEY_BACKSPACE:
        case 127:
        case 8:
            if (focus < nsrc)
                remove_source(focus);
            else
                setmsg(1, "Select a folder in the list first.");
            break;
        case 'd':
        case 'c':
        case 'e':
        case 'u':
        case 'n':
            for (int k = 0; k < NSET; k++)
                if (set_keys[k] == ch) {
                    focus = nsrc + k;
                    edit_setting(k);
                }
            break;
        case 's':
            do_backup();
            break;
        case 'q':
        case 3:
            endwin();
            return 0;
        }
    }
}
