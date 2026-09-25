#define _POSIX_C_SOURCE 200809L
/*
 * git-file-sha256: list every commit touching a single file and print
 * sha256sum of the file content at that commit.
 *
 * Usage: git-file-sha256 <path/to/file>
 *   Must be run inside a git repository. <path/to/file> is the
 *   current path (as accepted by `git log -- <path>`).
 *
 * Efficient strategy (no checkouts):
 *   1. One `git log --follow --raw --abbrev=40 --format=%H -- <file>`
 *      to get (commit, blob) pairs. Using --raw gives the blob SHA
 *      directly, so renames handled by --follow need no per-commit
 *      path resolution.
 *   2. One persistent `git cat-file --batch` to stream all blob
 *      contents (avoids one git spawn per commit).
 *   3. One `sha256sum` (looked up via PATH) per blob, fed by streaming
 *      the batch output in 64 KiB chunks (no temp files, no full-file
 *      buffering, binary-safe).
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

typedef struct {
    char commit[41];
    char blob[41];
} Entry;

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static int is_hex40(const char *s) {
    if (strlen(s) != 40) return 0;
    for (int i = 0; i < 40; i++)
        if (!isxdigit((unsigned char)s[i])) return 0;
    return 1;
}

static int is_all_zero(const char *s) {
    for (; *s; s++)
        if (*s != '0') return 0;
    return 1;
}

/* Write exactly n bytes, handling EINTR / partial writes. */
static int write_all(int fd, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void strip_nl(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) s[--n] = '\0';
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <path/to/file>\n", argv[0]);
        return 2;
    }
    const char *filepath = argv[1];

    /* ---- 1. git log -> (commit, blob) pairs ---- */
    Entry *entries = NULL;
    size_t n = 0, cap = 0;

    int log_pipe[2];
    if (pipe(log_pipe) < 0) die("pipe");
    pid_t log_pid = fork();
    if (log_pid < 0) die("fork");
    if (log_pid == 0) {
        dup2(log_pipe[1], STDOUT_FILENO);
        close(log_pipe[0]);
        close(log_pipe[1]);
        execlp("git", "git", "log", "--follow", "--raw",
               "--abbrev=40", "--format=%H", "--", filepath, (char *)NULL);
        perror("execlp git log");
        _exit(127);
    }
    close(log_pipe[1]);
    FILE *log_fp = fdopen(log_pipe[0], "r");
    if (!log_fp) die("fdopen");

    char *line = NULL;
    size_t linecap = 0;
    char cur[41] = {0};
    int have_cur = 0;
    while (getline(&line, &linecap, log_fp) >= 0) {
        strip_nl(line);
        if (line[0] == '\0') continue;
        if (is_hex40(line)) {
            memcpy(cur, line, 41);
            have_cur = 1;
            continue;
        }
        if (line[0] == ':' && have_cur) {
            char oldblob[64] = {0}, newblob[64] = {0};
            char status = 0;
            /* :<oldmode> <newmode> <oldblob> <newblob> <STATUS>\t... */
            if (sscanf(line, ":%*s %*s %63s %63s %c",
                       oldblob, newblob, &status) == 3) {
                if (status != 'D' && !is_all_zero(newblob)) {
                    if (n == cap) {
                        cap = cap ? cap * 2 : 64;
                        entries = realloc(entries, cap * sizeof *entries);
                        if (!entries) die("realloc");
                    }
                    memcpy(entries[n].commit, cur, 41);
                    /* newblob is 40 hex with --abbrev=40; truncate defensively */
                    strncpy(entries[n].blob, newblob, 40);
                    entries[n].blob[40] = '\0';
                    n++;
                }
            }
        }
    }
    free(line);
    fclose(log_fp);
    int st = 0;
    waitpid(log_pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fprintf(stderr, "git log failed (are you in a git repo?)\n");
        free(entries);
        return 1;
    }
    if (n == 0) {
        fprintf(stderr, "no history found for '%s'\n", filepath);
        free(entries);
        return 1;
    }

    /* ---- 2. persistent git cat-file --batch ---- */
    int to_cat[2], from_cat[2];
    if (pipe(to_cat) < 0) die("pipe");
    if (pipe(from_cat) < 0) die("pipe");
    pid_t cat_pid = fork();
    if (cat_pid < 0) die("fork");
    if (cat_pid == 0) {
        dup2(to_cat[0], STDIN_FILENO);
        dup2(from_cat[1], STDOUT_FILENO);
        close(to_cat[0]); close(to_cat[1]);
        close(from_cat[0]); close(from_cat[1]);
        execlp("git", "git", "cat-file", "--batch", (char *)NULL);
        perror("execlp git cat-file");
        _exit(127);
    }
    close(to_cat[0]);
    close(from_cat[1]);
    FILE *cat_w = fdopen(to_cat[1], "w");
    FILE *cat_r = fdopen(from_cat[0], "r");
    if (!cat_w || !cat_r) die("fdopen cat-file");

    char buf[65536];

    /* ---- 3. one sha256sum (via PATH) per blob ---- */
    for (size_t i = 0; i < n; i++) {
        fprintf(cat_w, "%s\n", entries[i].blob);
        fflush(cat_w);
        if (ferror(cat_w)) {
            fprintf(stderr, "write to git cat-file failed\n");
            break;
        }

        char hdr[256];
        if (!fgets(hdr, sizeof hdr, cat_r)) {
            fprintf(stderr, "%s: failed to read batch header\n", entries[i].commit);
            continue;
        }
        char sha[128] = {0}, type[32] = {0};
        long long size = -1;
        /* header is "<sha> <type> <size>" or "<sha> missing" */
        if (sscanf(hdr, "%127s %31s %lld", sha, type, &size) < 2) {
            fprintf(stderr, "%s: bad batch header: %s\n", entries[i].commit, hdr);
            continue;
        }
        if (strcmp(type, "missing") == 0) {
            fprintf(stderr, "%s: blob %s missing, skipping\n",
                    entries[i].commit, entries[i].blob);
            continue;
        }
        if (strcmp(type, "blob") != 0 || size < 0) {
            fprintf(stderr, "%s: unexpected header: %s\n", entries[i].commit, hdr);
            continue;
        }

        int sha_in[2], sha_out[2];
        if (pipe(sha_in) < 0) die("pipe");
        if (pipe(sha_out) < 0) die("pipe");
        /* Flush stdio buffers before fork to avoid duplication. */
        fflush(stdout); fflush(stderr);
        pid_t sha_pid = fork();
        if (sha_pid < 0) die("fork");
        if (sha_pid == 0) {
            dup2(sha_in[0], STDIN_FILENO);
            dup2(sha_out[1], STDOUT_FILENO);
            close(sha_in[0]); close(sha_in[1]);
            close(sha_out[0]); close(sha_out[1]);
            /* Don't keep batch pipes open in sha256sum child. */
            close(fileno(cat_w));
            close(fileno(cat_r));
            execlp("sha256sum", "sha256sum", (char *)NULL);
            perror("execlp sha256sum (is it on PATH?)");
            _exit(127);
        }
        close(sha_in[0]);
        close(sha_out[1]);

        long long remaining = size;
        int copy_err = 0;
        while (remaining > 0) {
            size_t want = remaining > (long long)sizeof buf
                          ? sizeof buf : (size_t)remaining;
            size_t got = fread(buf, 1, want, cat_r);
            if (got == 0) {
                if (ferror(cat_r)) perror("read batch content");
                else fprintf(stderr, "unexpected EOF from git cat-file\n");
                copy_err = 1;
                break;
            }
            if (write_all(sha_in[1], buf, got) < 0) {
                perror("write to sha256sum");
                copy_err = 1;
                break;
            }
            remaining -= (long long)got;
        }
        /* consume trailing newline after batch content */
        if (!copy_err) {
            int c = fgetc(cat_r);
            if (c != '\n') {
                fprintf(stderr, "%s: bad batch terminator\n", entries[i].commit);
                copy_err = 1;
            }
        }
        close(sha_in[1]);

        char outline[512] = {0};
        FILE *sha_fp = fdopen(sha_out[0], "r");
        if (!sha_fp) die("fdopen sha256sum");
        if (!copy_err && fgets(outline, sizeof outline, sha_fp)) {
            char hash[129] = {0};
            if (sscanf(outline, "%128s", hash) == 1)
                printf("%s %s\n", entries[i].commit, hash);
            else
                fprintf(stderr, "%s: bad sha256sum output: %s\n",
                        entries[i].commit, outline);
        } else if (!copy_err) {
            fprintf(stderr, "%s: no output from sha256sum\n", entries[i].commit);
        }
        fclose(sha_fp);
        int sha_st = 0;
        waitpid(sha_pid, &sha_st, 0);
        if (!WIFEXITED(sha_st) || WEXITSTATUS(sha_st) != 0)
            fprintf(stderr, "%s: sha256sum exited with status %d\n",
                    entries[i].commit,
                    WIFEXITED(sha_st) ? WEXITSTATUS(sha_st) : -1);
        if (copy_err) continue;
    }

    fclose(cat_w);  /* EOF to git cat-file */
    fclose(cat_r);
    waitpid(cat_pid, NULL, 0);
    free(entries);
    return 0;
}
