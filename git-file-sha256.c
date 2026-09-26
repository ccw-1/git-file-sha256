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
 *   3. Internal SHA-256 over each blob, streamed in 64 KiB chunks
 *      (no temp files, no full-file buffering, binary-safe, no
 *      external hasher needed -- works with or without zopen).
 */
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#ifdef __MVS__
#include <fcntl.h>
#endif

typedef struct {
    char commit[41];
    char blob[41];
} Entry;

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

/* z/OS: pipes to ASCII programs (git) get mangled by automatic
 * EBCDIC conversion (program CCSID defaults vary, pipe tag is
 * deferred). Force CCSID 819/819 so bytes pass through unchanged.
 * No-op elsewhere. */
static void pipe_ascii(int fd) {
#ifdef __MVS__
    struct f_cnvrt c;
    c.cvtcmd = SETCVTON;
    c.pccsid = 819;
    c.fccsid = 819;
    fcntl(fd, F_CONTROL_CVT, &c);
#else
    (void)fd;
#endif
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

/* ---- SHA-256 (FIPS 180-4, public domain style implementation) ---- */
typedef struct {
    uint32_t h[8];
    uint64_t total;
    unsigned char buf[64];
    size_t buflen;
} Sha256;

static uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(Sha256 *c, const unsigned char *p) {
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) |
               ((uint32_t)p[4*i+2] << 8) | (uint32_t)p[4*i+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i-15], 7) ^ rotr32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr32(w[i-2], 17) ^ rotr32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], dd = c->h[3];
    uint32_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + k[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = dd + t1;
        dd = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += dd;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_init(Sha256 *c) {
    c->h[0]=0x6a09e667; c->h[1]=0xbb67ae85; c->h[2]=0x3c6ef372; c->h[3]=0xa54ff53a;
    c->h[4]=0x510e527f; c->h[5]=0x9b05688c; c->h[6]=0x1f83d9ab; c->h[7]=0x5be0cd19;
    c->total = 0;
    c->buflen = 0;
}

static void sha256_update(Sha256 *c, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    c->total += (uint64_t)len;
    while (len > 0) {
        size_t take = 64 - c->buflen;
        if (take > len) take = len;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take;
        p += take;
        len -= take;
        if (c->buflen == 64) {
            sha256_block(c, c->buf);
            c->buflen = 0;
        }
    }
}

static void sha256_final(Sha256 *c, unsigned char out[32]) {
    uint64_t bits = c->total * 8;
    unsigned char pad = 0x80;
    sha256_update(c, &pad, 1);
    unsigned char zero = 0;
    while (c->buflen != 56)
        sha256_update(c, &zero, 1);
    unsigned char lenbuf[8];
    for (int i = 0; i < 8; i++)
        lenbuf[i] = (unsigned char)(bits >> (56 - 8 * i));
    /* append length without affecting total */
    memcpy(c->buf + 56, lenbuf, 8);
    sha256_block(c, c->buf);
    for (int i = 0; i < 8; i++) {
        out[4*i]   = (unsigned char)(c->h[i] >> 24);
        out[4*i+1] = (unsigned char)(c->h[i] >> 16);
        out[4*i+2] = (unsigned char)(c->h[i] >> 8);
        out[4*i+3] = (unsigned char)c->h[i];
    }
}

/* Strip trailing CR/LF. */

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
    pipe_ascii(log_pipe[0]);
    pipe_ascii(log_pipe[1]);
    pid_t log_pid = fork();
    if (log_pid < 0) die("fork");
    if (log_pid == 0) {
        dup2(log_pipe[1], STDOUT_FILENO);
        close(log_pipe[0]);
        close(log_pipe[1]);
        pipe_ascii(STDOUT_FILENO);
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
    pipe_ascii(to_cat[0]);
    pipe_ascii(to_cat[1]);
    pipe_ascii(from_cat[0]);
    pipe_ascii(from_cat[1]);
    pid_t cat_pid = fork();
    if (cat_pid < 0) die("fork");
    if (cat_pid == 0) {
        dup2(to_cat[0], STDIN_FILENO);
        dup2(from_cat[1], STDOUT_FILENO);
        close(to_cat[0]); close(to_cat[1]);
        close(from_cat[0]); close(from_cat[1]);
        pipe_ascii(STDIN_FILENO);
        pipe_ascii(STDOUT_FILENO);
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

    /* ---- 3. hash each blob internally (no external hasher) ---- */
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

        Sha256 ctx;
        sha256_init(&ctx);
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
            sha256_update(&ctx, buf, got);
            remaining -= (long long)got;
        }
        /* consume trailing newline after batch content */
        if (!copy_err) {
            int ch = fgetc(cat_r);
            if (ch != '\n') {
                fprintf(stderr, "%s: bad batch terminator\n", entries[i].commit);
                copy_err = 1;
            }
        }
        if (copy_err) continue;
        unsigned char digest[32];
        sha256_final(&ctx, digest);
        static const char hexd[] = "0123456789abcdef";
        char hash[65];
        for (int b = 0; b < 32; b++) {
            hash[2*b] = hexd[digest[b] >> 4];
            hash[2*b+1] = hexd[digest[b] & 0xf];
        }
        hash[64] = '\0';
        printf("%s %s\n", entries[i].commit, hash);
    }

    fclose(cat_w);  /* EOF to git cat-file */
    fclose(cat_r);
    waitpid(cat_pid, NULL, 0);
    free(entries);
    return 0;
}
