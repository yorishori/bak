/*
 * bak - a tiny, careful backup tool.
 *
 * Select folders, pack them with tar, compress with xz (max ratio),
 * optionally encrypt with gpg (AES-256), write to a destination folder,
 * optionally upload with rclone.
 *
 * Safety rules:
 *   - Everything is checked before starting (tools, paths, permissions,
 *     readability of every file, free space, name collisions, ...).
 *   - The archive is written to a hidden ".partial" file, fsync'ed, then
 *     fully decrypted/decompressed/listed to verify it, and only then
 *     renamed to its final name. A final-named file is always verified.
 *   - Any error, warning or interruption at any step discards the archive.
 *
 * Only libc is used. External programs are run directly (no shell).
 */
#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/vfs.h>
#define MSDOS_MAGIC 0x4d44 /* FAT12/16/32: 4 GiB file size limit */
#endif

#define MAX_SRC 64
#define LINE 4096
#define PASS_MAX 1024

static char *src[MAX_SRC];
static int nsrc;
static char dest[PATH_MAX];
static char label[41] = "backup";
static int multithread;           /* 0: xz -9e -T1 (best ratio), 1: -T0 */
static int encrypt;
static char remote[LINE];         /* "" = no upload, else "name:path" */
static int keep_local = 1;
static char msg[1024];

/* ------------------------------------------------------------------ */
/* small helpers                                                      */

static void setmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
}

/* Read a line from stdin. Returns -1 on EOF or overlong line. */
static int read_line(const char *prompt, char *buf, size_t n)
{
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, (int)n, stdin))
        return -1;
    size_t len = strlen(buf);
    if (len && buf[len - 1] == '\n') {
        buf[--len] = '\0';
    } else if (len == n - 1) {        /* line too long: drain and reject */
        int c;
        while ((c = getchar()) != '\n' && c != EOF)
            ;
        printf("Input too long.\n");
        return -1;
    }
    while (len && (buf[len - 1] == ' ' || buf[len - 1] == '\t'))
        buf[--len] = '\0';
    char *s = buf;
    while (*s == ' ' || *s == '\t')
        s++;
    memmove(buf, s, strlen(s) + 1);
    return 0;
}

static int confirm(const char *question)
{
    char b[16];
    printf("%s [y/N] ", question);
    if (read_line("", b, sizeof b) < 0)
        return 0;
    return b[0] == 'y' || b[0] == 'Y';
}

static void wait_enter(void)
{
    char b[16];
    read_line("\nPress Enter to continue...", b, sizeof b);
}

static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n--)
        *v++ = 0;
}

/* Expand leading "~/" and resolve to an absolute, canonical path. */
static int resolve(const char *in, char *out)
{
    char tmp[PATH_MAX];
    if (in[0] == '~' && (in[1] == '/' || in[1] == '\0')) {
        const char *home = getenv("HOME");
        if (!home || snprintf(tmp, sizeof tmp, "%s%s", home, in + 1) >= (int)sizeof tmp)
            return -1;
    } else {
        if (snprintf(tmp, sizeof tmp, "%s", in) >= (int)sizeof tmp)
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

static int fsync_dir(const char *d)
{
    int fd = open(d, O_RDONLY);
    if (fd < 0)
        return -1;
    int r = fsync(fd);
    close(fd);
    return r;
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

/* All our fds are close-on-exec, so the child only gets 0, 1, 2 (+3). */
static pid_t spawn(char *const argv[], int in, int out, int fd3)
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        if (move_fd(in, 0) || move_fd(out, 1) || (fd3 >= 0 && move_fd(fd3, 3)))
            _exit(127);
        execvp(argv[0], argv);
        fprintf(stderr, "cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    return pid;
}

static int wait_ok(pid_t pid, const char *name)
{
    int st;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return 0;
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
        return 1;
    if (WIFEXITED(st))
        fprintf(stderr, "  ! %s failed (exit code %d)\n", name, WEXITSTATUS(st));
    else if (WIFSIGNALED(st))
        fprintf(stderr, "  ! %s was killed (signal %d)\n", name, WTERMSIG(st));
    return 0;
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
 * If pass_idx >= 0, that command gets the passphrase on fd 3.
 * Returns 1 only if every process exited with status 0.
 */
static int run_pipeline(char **cmds[], int n, int in, int out, int pass_idx, const char *pass)
{
    pid_t pids[8];
    int started = 0, ok = 1, prev = in;

    for (int i = 0; i < n; i++) {
        int p[2] = {-1, -1}, pp[2] = {-1, -1};
        int o = out;
        if (i < n - 1) {
            if (cloexec_pipe(p) < 0) {
                perror("pipe");
                ok = 0;
                break;
            }
            o = p[1];
        }
        if (i == pass_idx && cloexec_pipe(pp) < 0) {
            perror("pipe");
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
        if (prev != in)
            close(prev);
        prev = in;
        if (i < n - 1) {
            close(p[1]);
            prev = p[0];
        }
        if (pid < 0) {
            ok = 0;
            break;
        }
        pids[started++] = pid;
    }
    if (prev != in)
        close(prev);
    for (int i = 0; i < started; i++)
        if (!wait_ok(pids[i], cmds[i][0]))
            ok = 0;
    return ok;
}

static int run1(char **argv)
{
    int null = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (null < 0)
        return 0;
    char **cmds[1] = {argv};
    int ok = run_pipeline(cmds, 1, null, STDOUT_FILENO, -1, NULL);
    close(null);
    return ok;
}

/* Run argv, capture stdout into buf. Returns 1 on success. */
static int run_capture(char **argv, char *buf, size_t n)
{
    int p[2], null = open("/dev/null", O_RDONLY | O_CLOEXEC);
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
    while ((r = read(p[0], buf + len, n - 1 - len)) != 0) {
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        len += (size_t)r;
        if (len == n - 1)
            break;
    }
    buf[len] = '\0';
    close(p[0]);
    return pid > 0 && wait_ok(pid, argv[0]);
}

/* ------------------------------------------------------------------ */
/* preflight checks                                                   */

static long long w_bytes, w_entries, w_errors;

static void walk_err(const char *path, const char *why)
{
    if (w_errors < 10)
        printf("  ! %s: %s\n", why, path);
    else if (w_errors == 10)
        printf("  ! (more problems not shown)\n");
    w_errors++;
}

static int walk_cb(const char *p, const struct stat *st, int type, struct FTW *f)
{
    (void)f;
    w_entries++;
    switch (type) {
    case FTW_F:
        w_bytes += st->st_size;
        if (S_ISREG(st->st_mode) && access(p, R_OK) != 0)
            walk_err(p, "file not readable");
        break;
    case FTW_D:
        if (access(p, R_OK | X_OK) != 0)
            walk_err(p, "folder not accessible");
        break;
    case FTW_DNR:
        walk_err(p, "folder not readable");
        break;
    case FTW_NS:
        walk_err(p, "cannot stat");
        break;
    default: /* symlinks are stored as links, never followed */
        break;
    }
    return 0;
}

static int remote_exists(const char *r)
{
    char name[LINE], out[65536];
    const char *colon = strchr(r, ':');
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
    printf("\n== Checking ==\n");

    if (nsrc == 0) {
        printf("  ! No source folders selected.\n");
        ok = 0;
    }
    if (!dest[0]) {
        printf("  ! No destination folder set.\n");
        ok = 0;
    }
    if (!ok)
        return 0;

    /* tools */
    const char *tools[] = {"tar", "xz", encrypt ? "gpg" : NULL, remote[0] ? "rclone" : NULL};
    for (int i = 0; i < 4; i++)
        if (tools[i] && !in_path(tools[i])) {
            printf("  ! Required program not found: %s\n", tools[i]);
            ok = 0;
        }

    /* destination */
    if (!is_dir(dest)) {
        printf("  ! Destination is not a folder anymore: %s\n", dest);
        ok = 0;
    } else if (access(dest, W_OK | X_OK) != 0) {
        printf("  ! Destination is not writable: %s\n", dest);
        ok = 0;
    }

    /* sources */
    for (int i = 0; i < nsrc; i++) {
        if (!is_dir(src[i])) {
            printf("  ! Source is not a folder anymore: %s\n", src[i]);
            ok = 0;
            continue;
        }
        if (is_inside(dest, src[i])) {
            printf("  ! Destination is inside source %s (it would back up itself)\n", src[i]);
            ok = 0;
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(base_of(src[i]), base_of(src[j])) == 0) {
                printf("  ! Two sources have the same name '%s' (they would collide in the archive)\n",
                       base_of(src[i]));
                ok = 0;
            }
            if (is_inside(src[i], src[j]) || is_inside(src[j], src[i])) {
                printf("  ! Sources overlap: %s and %s\n", src[i], src[j]);
                ok = 0;
            }
        }
    }
    if (!ok)
        return 0;

    /* walk every file: readable? how big? */
    w_bytes = w_entries = w_errors = 0;
    for (int i = 0; i < nsrc; i++) {
        printf("  scanning %s ...\n", src[i]);
        fflush(stdout);
        if (nftw(src[i], walk_cb, 32, FTW_PHYS) != 0) {
            printf("  ! Could not scan %s: %s\n", src[i], strerror(errno));
            ok = 0;
        }
    }
    if (w_errors) {
        printf("  ! %lld item(s) cannot be read. Fix permissions or remove those folders.\n", w_errors);
        ok = 0;
    }
    if (!ok)
        return 0;

    char hs[32];
    human((double)w_bytes, hs, sizeof hs);
    printf("  ok: %lld items, %s uncompressed\n", w_entries, hs);

    /* names */
    char stamp[32];
    time_t t = time(NULL);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&t));
    snprintf(fname, n, "%s-%s.tar.xz%s", label, stamp, encrypt ? ".gpg" : "");
    if (snprintf(final, n, "%s/%s", dest, fname) >= (int)n ||
        snprintf(partial, n, "%s/.%s.partial", dest, fname) >= (int)n) {
        printf("  ! Destination path too long.\n");
        return 0;
    }
    if (access(final, F_OK) == 0 || access(partial, F_OK) == 0) {
        printf("  ! Output file already exists: %s (try again in a second)\n", final);
        return 0;
    }

    /* space: compressed size is unknown, so worst case = input + overhead */
    unsigned long long need = (unsigned long long)w_bytes + (unsigned long long)w_entries * 1024 +
                              (unsigned long long)w_bytes / 100 + 16ULL * 1024 * 1024;
    struct statvfs vfs;
    if (statvfs(dest, &vfs) == 0) {
        unsigned long long avail = (unsigned long long)vfs.f_bavail * vfs.f_frsize;
        char ha[32];
        human((double)avail, ha, sizeof ha);
        printf("  free space at destination: %s\n", ha);
        if (avail < need) {
            printf("  ? Free space is less than the uncompressed size. It will probably fit\n"
                   "    after compression, but it is not guaranteed. If it runs out, the\n"
                   "    backup is discarded safely.\n");
            if (!confirm("    Continue anyway?"))
                return 0;
        }
    }
#ifdef __linux__
    struct statfs sfs;
    if (statfs(dest, &sfs) == 0 && sfs.f_type == MSDOS_MAGIC && need >= 4ULL * 1024 * 1024 * 1024) {
        printf("  ? Destination is FAT32, which cannot hold files of 4 GiB or more.\n"
               "    The backup may fail (it would be discarded safely).\n");
        if (!confirm("    Continue anyway?"))
            return 0;
    }
#endif

    if (remote[0]) {
        if (!remote_exists(remote)) {
            printf("  ! rclone remote not configured: %s (see 'rclone config')\n", remote);
            return 0;
        }
        printf("  ok: rclone remote found\n");
    }
    return 1;
}

static int read_secret(const char *prompt, char *buf, size_t n)
{
    struct termios old, quiet;
    int tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old) == 0;
    if (tty) {
        quiet = old;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
    }
    int r = read_line(prompt, buf, n);
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
        printf("\n");
    }
    return r;
}

static int ask_passphrase(char *pass)
{
    char again[PASS_MAX];
    int ok = 0;
    for (int tries = 0; tries < 3 && !ok; tries++) {
        if (read_secret("  Passphrase (min 8 chars): ", pass, PASS_MAX) < 0)
            break;
        if (strlen(pass) < 8) {
            printf("  ! Too short.\n");
            continue;
        }
        if (read_secret("  Repeat passphrase: ", again, PASS_MAX) < 0)
            break;
        if (strcmp(pass, again) != 0)
            printf("  ! Passphrases do not match.\n");
        else
            ok = 1;
    }
    wipe(again, sizeof again);
    if (!ok)
        wipe(pass, PASS_MAX);
    return ok;
}

/* ------------------------------------------------------------------ */
/* the backup itself                                                  */

static int create_archive(const char *partial, const char *pass)
{
    char *tar[3 + 3 * MAX_SRC + 1];
    char parents[MAX_SRC][PATH_MAX], names[MAX_SRC][PATH_MAX];
    int a = 0;
    tar[a++] = "tar";
    tar[a++] = "-cf";
    tar[a++] = "-";
    for (int i = 0; i < nsrc; i++) {
        const char *b = base_of(src[i]);
        size_t plen = (size_t)(b - src[i]) - 1;
        if (plen == 0)
            plen = 1; /* parent is "/" */
        memcpy(parents[i], src[i], plen);
        parents[i][plen] = '\0';
        snprintf(names[i], PATH_MAX, "./%s", b); /* "./" so names never look like options */
        tar[a++] = "-C";
        tar[a++] = parents[i];
        tar[a++] = names[i];
    }
    tar[a] = NULL;

    char *xz[] = {"xz", "-9e", multithread ? "-T0" : "-T1", "--check=sha256", "-c", NULL};
    char *gpg[] = {"gpg", "--batch", "--yes", "--quiet", "--no-symkey-cache",
                   "--pinentry-mode", "loopback", "--passphrase-fd", "3",
                   "--symmetric", "--cipher-algo", "AES256",
                   "--s2k-mode", "3", "--s2k-digest-algo", "SHA512", "--s2k-count", "65011712",
                   "--compress-algo", "none", "-o", "-", NULL};
    char **cmds[3] = {tar, xz, gpg};

    int null = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int out = open(partial, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (null < 0 || out < 0) {
        printf("  ! Cannot create %s: %s\n", partial, strerror(errno));
        if (null >= 0)
            close(null);
        if (out >= 0)
            close(out);
        return 0;
    }
    int ok = run_pipeline(cmds, encrypt ? 3 : 2, null, out, encrypt ? 2 : -1, pass);
    if (fsync(out) != 0) {
        printf("  ! Could not flush archive to disk: %s\n", strerror(errno));
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
        if (in >= 0)
            close(in);
        if (null >= 0)
            close(null);
        return 0;
    }
    int ok = run_pipeline(cmds, n, in, null, encrypt ? 0 : -1, pass);
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

    printf("  uploading to %s ...\n", target);
    char *cp[] = {"rclone", "copyto", (char *)final, target, NULL};
    if (!run1(cp))
        return 0;
    printf("  checking uploaded copy ...\n");
    char *ck[] = {"rclone", "check", "--one-way", "--include", filter, dest, remote, NULL};
    return run1(ck);
}

static void do_backup(void)
{
    char final[PATH_MAX], partial[PATH_MAX], fname[PATH_MAX];
    char pass[PASS_MAX] = "";

    /* Ctrl-C reaches the child programs (they stop), we stay to clean up. */
    signal(SIGINT, SIG_IGN);

    if (!preflight(final, partial, fname, sizeof final)) {
        printf("\nNothing was written.\n");
        goto out;
    }
    printf("\n  Will create: %s\n", final);
    if (remote[0])
        printf("  Then upload to: %s%s\n", remote, keep_local ? "" : " (and delete local copy)");
    if (!confirm("\nStart backup?")) {
        printf("Cancelled. Nothing was written.\n");
        goto out;
    }
    if (encrypt) {
        printf("\n  Remember this passphrase: without it the backup cannot be restored.\n");
        if (!ask_passphrase(pass)) {
            printf("Cancelled. Nothing was written.\n");
            goto out;
        }
    }

    time_t t0 = time(NULL);
    printf("\n== Compressing%s (Ctrl-C to abort) ==\n", encrypt ? " + encrypting" : "");
    if (!create_archive(partial, pass)) {
        unlink(partial);
        printf("\nFAILED while creating the archive. Partial file removed; nothing was saved.\n");
        goto out;
    }
    printf("== Verifying (full decompress%s) ==\n", encrypt ? " + decrypt" : "");
    if (!verify_archive(partial, pass)) {
        unlink(partial);
        printf("\nFAILED verification. Archive removed; nothing was saved.\n");
        goto out;
    }
    if (rename(partial, final) != 0 || fsync_dir(dest) != 0) {
        printf("\nFAILED to finalize %s: %s\n", final, strerror(errno));
        unlink(partial);
        goto out;
    }

    struct stat st;
    char hs[32], ho[32];
    stat(final, &st);
    human((double)st.st_size, hs, sizeof hs);
    human((double)w_bytes, ho, sizeof ho);
    printf("\nOK  %s\n    %s (from %s, %.1f%%) in %ld s, verified.\n", final, hs, ho,
           w_bytes ? 100.0 * (double)st.st_size / (double)w_bytes : 100.0, (long)(time(NULL) - t0));

    if (remote[0]) {
        printf("\n== Uploading ==\n");
        if (!upload(final, fname)) {
            printf("\nUPLOAD FAILED. The local backup is fine and was kept: %s\n", final);
        } else {
            printf("OK  upload verified.\n");
            if (!keep_local) {
                if (unlink(final) == 0 && fsync_dir(dest) == 0)
                    printf("    local copy deleted.\n");
                else
                    printf("    ! could not delete local copy: %s\n", strerror(errno));
            }
        }
    }

    printf("\nTo restore:\n  %s%s\n", encrypt ? "gpg -d FILE | xz -dc | tar -xf -" : "xz -dc FILE | tar -xf -",
           "   (extracts into the current folder)");
out:
    wipe(pass, sizeof pass);
    signal(SIGINT, SIG_DFL);
    wait_enter();
}

/* ------------------------------------------------------------------ */
/* menu                                                               */

static void add_source(const char *in)
{
    char path[PATH_MAX];
    if (!in[0])
        return;
    if (resolve(in, path) < 0) {
        setmsg("Cannot find: %s", in);
        return;
    }
    if (!is_dir(path)) {
        setmsg("Not a folder: %s", path);
        return;
    }
    if (strcmp(path, "/") == 0) {
        setmsg("Refusing to back up the whole filesystem root.");
        return;
    }
    for (int i = 0; i < nsrc; i++)
        if (strcmp(src[i], path) == 0) {
            setmsg("Already selected: %s", path);
            return;
        }
    if (nsrc == MAX_SRC) {
        setmsg("Too many folders (max %d).", MAX_SRC);
        return;
    }
    char *s = strdup(path);
    if (!s) {
        setmsg("Out of memory.");
        return;
    }
    src[nsrc++] = s;
    setmsg("Added: %s", path);
}

static void draw(void)
{
    if (isatty(STDOUT_FILENO))
        printf("\033[H\033[2J");
    printf("bak - simple verified backups\n");
    printf("------------------------------------------------------------\n");
    printf("Folders to back up:\n");
    if (!nsrc)
        printf("   (none)\n");
    for (int i = 0; i < nsrc; i++)
        printf("  %2d) %s\n", i + 1, src[i]);
    printf("\nDestination : %s\n", dest[0] ? dest : "(not set)");
    printf("File name   : %s-YYYYMMDD-HHMMSS.tar.xz%s\n", label, encrypt ? ".gpg" : "");
    printf("Compression : xz -9e, %s\n", multithread ? "all cores (faster, ~1% bigger)" : "1 core (smallest, slow)");
    printf("Encryption  : %s\n", encrypt ? "ON (gpg AES-256, passphrase)" : "off");
    printf("Upload      : %s\n", remote[0] ? remote : "off");
    if (remote[0])
        printf("Keep local  : %s\n", keep_local ? "yes" : "no (deleted after verified upload)");
    printf("------------------------------------------------------------\n");
    printf(" a) add folder      r) remove folder    d) destination\n");
    printf(" n) file name       c) compression      e) encryption\n");
    printf(" u) upload target   k) keep local       s) CHECK & START\n");
    printf(" q) quit\n");
    if (msg[0])
        printf("\n> %s\n", msg);
    msg[0] = '\0';
}

int main(int argc, char **argv)
{
    char line[LINE], path[PATH_MAX];

    signal(SIGPIPE, SIG_IGN);
    umask(077); /* backups are private */
    for (int i = 1; i < argc; i++)
        add_source(argv[i]);

    for (;;) {
        draw();
        if (read_line("\nChoice: ", line, sizeof line) < 0)
            break;
        switch (line[0]) {
        case 'a':
            if (read_line("Folder to add: ", line, sizeof line) == 0)
                add_source(line);
            break;
        case 'r':
            if (!nsrc) {
                setmsg("No folders to remove.");
                break;
            }
            if (read_line("Number to remove: ", line, sizeof line) == 0) {
                int k = atoi(line);
                if (k < 1 || k > nsrc) {
                    setmsg("Invalid number.");
                    break;
                }
                setmsg("Removed: %s", src[k - 1]);
                free(src[k - 1]);
                memmove(&src[k - 1], &src[k], (size_t)(nsrc - k) * sizeof src[0]);
                nsrc--;
            }
            break;
        case 'd':
            if (read_line("Destination folder: ", line, sizeof line) < 0 || !line[0])
                break;
            if (resolve(line, path) < 0 || !is_dir(path))
                setmsg("Not an existing folder: %s", line);
            else if (access(path, W_OK | X_OK) != 0)
                setmsg("Folder is not writable: %s", path);
            else {
                strcpy(dest, path);
                setmsg("Destination: %s", dest);
            }
            break;
        case 'n': {
            if (read_line("Name prefix (letters, digits, . _ -): ", line, sizeof line) < 0 || !line[0])
                break;
            size_t len = strlen(line);
            if (len > 40 || line[0] == '.' || line[0] == '-' ||
                strspn(line, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != len)
                setmsg("Invalid name. Use up to 40 letters, digits, '.', '_' or '-'.");
            else
                strcpy(label, line);
            break;
        }
        case 'c':
            multithread = !multithread;
            break;
        case 'e':
            encrypt = !encrypt;
            break;
        case 'u':
            if (read_line("rclone target as remote:path (empty = no upload): ", line, sizeof line) < 0)
                break;
            if (!line[0]) {
                remote[0] = '\0';
                setmsg("Upload disabled.");
            } else if (!strchr(line, ':') || line[0] == ':' || line[0] == '/' || line[0] == '-') {
                setmsg("Use the form remote:path, e.g. gdrive:backups");
            } else {
                strcpy(remote, line);
                setmsg("Upload target: %s (checked before starting)", remote);
            }
            break;
        case 'k':
            keep_local = !keep_local;
            break;
        case 's':
            do_backup();
            break;
        case 'q':
            return 0;
        case '\0':
            break;
        default:
            setmsg("Unknown option: %s", line);
        }
    }
    printf("\n");
    return 0;
}
