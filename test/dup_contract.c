/*
 *  dup_contract.c - open identity / dup / dup2 contract tests for libzzuf
 *
 *  This program is built against the task_support link adapter: the
 *  original src/libzzuf/lib-fd.c wrappers are compiled under zz_test_*
 *  names and wired to the real libc, so every I/O call below exercises
 *  the production fd.c bookkeeping without LD_PRELOAD.
 *
 *  Expected bytes are always obtained from an independent sequential
 *  read of a fresh open identity configured with the same seed, never
 *  from a cached copy of the file contents.
 *
 *  Usage: dup_contract identity|position|lifetime
 *  Each mode prints its DUP_*_OK marker on success and exits non-zero
 *  on the first failed assertion.
 */

#include "adapter.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define FILE_SIZE 4096

/* Must match STATIC_FILES in src/common/fd.c. */
enum { STATIC_CAPACITY = 32 };

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
            return 1; \
        } \
    } while (0)

/* Create a real temporary regular file holding a deterministic byte
 * pattern. The path is written into path (must hold at least 4096). */
static int make_fixture(char *path, size_t n)
{
    const char *tmp = getenv("TMPDIR");
    if (tmp == NULL || *tmp == '\0')
        tmp = "/tmp";

    snprintf(path, 4096, "%s/zzuf-dup-XXXXXX", tmp);

    int fd = mkstemp(path);
    if (fd < 0)
        return -1;

    unsigned char *buf = malloc(n);
    if (buf == NULL)
    {
        close(fd);
        return -1;
    }

    for (size_t i = 0; i < n; ++i)
        buf[i] = (unsigned char)(i * 13 + 7);

    ssize_t written = write(fd, buf, n);
    free(buf);
    close(fd);

    return written == (ssize_t)n ? 0 : -1;
}

/* Independent sequential reference: open a brand new identity with the
 * given seed and read the whole file in order. */
static int sequential_reference(char const *path, uint32_t s,
                                unsigned char *buf, size_t n)
{
    zzuf_set_seed((int32_t)s);
    int fd = zz_test_open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    size_t done = 0;
    while (done < n)
    {
        ssize_t r = zz_test_read(fd, buf + done, n - done);
        if (r <= 0)
        {
            zz_test_close(fd);
            return -1;
        }
        done += (size_t)r;
    }

    zz_test_close(fd);
    return 0;
}

/* DUP_IDENTITY_OK: aliases inherit seed, position and active state,
 * consume no auto-increment seed, and dup2 follows the real call. */
static int test_identity(char const *path)
{
    unsigned char want[64], got[64], dummy[16];

    /* 1. dup() shares the open identity of its source. */
    zzuf_set_seed(41);
    int a = zz_test_open(path, O_RDONLY);
    CHECK(a >= 0, "open a");
    CHECK(zz_test_read(a, dummy, 16) == 16, "read 16 on a");

    int b = zz_test_dup(a);
    CHECK(b >= 0, "dup a");
    CHECK(_zz_getpos(b) == 16, "alias tracks shared position");
    CHECK(lseek(b, 0, SEEK_CUR) == 16, "kernel position matches");
    CHECK(_zz_identity_seed(a) == 41, "source seed");
    CHECK(_zz_identity_seed(b) == 41, "alias inherits seed");

    int ref = sequential_reference_open(path, 41);
    CHECK(ref >= 0 && zz_test_read(ref, dummy, 16) == 16, "ref skip 16");
    CHECK(zz_test_read(ref, want, 64) == 64, "ref sequential 64");
    zz_test_close(ref);

    CHECK(zz_test_pread(a, got, 64, 16) == 64, "pread on a");
    CHECK(memcmp(want, got, 64) == 0, "pread matches sequential identity");
    CHECK(_zz_getpos(a) == 16, "pread does not move shared position");
    CHECK(zz_test_read(b, got, 64) == 64, "read on alias");
    CHECK(memcmp(want, got, 64) == 0, "alias read matches sequential identity");
    CHECK(_zz_getpos(a) == 80 && _zz_getpos(b) == 80, "shared position 80");
    zz_test_close(a);
    zz_test_close(b);

    /* 2. Watched source dup2() over a watched target: the target loses
     * its old identity and becomes an alias of the source. */
    zzuf_set_seed(55);
    int c = zz_test_open(path, O_RDONLY);
    zzuf_set_seed(77);
    int d = zz_test_open(path, O_RDONLY);
    CHECK(_zz_identity_seed(c) == 55 && _zz_identity_seed(d) == 77,
          "independent opens have distinct seeds");
    CHECK(zz_test_read(c, dummy, 10) == 10, "advance c to 10");

    CHECK(zz_test_dup2(c, d) == d, "dup2 c -> d");
    CHECK(_zz_identity_seed(d) == 55, "target inherits source seed");
    CHECK(_zz_getpos(d) == 10, "target inherits shared position");

    ref = sequential_reference_open(path, 55);
    CHECK(ref >= 0 && zz_test_read(ref, dummy, 10) == 10, "ref skip 10");
    CHECK(zz_test_read(ref, want, 8) == 8, "ref 8 bytes");
    zz_test_close(ref);
    CHECK(zz_test_read(d, got, 8) == 8, "read through overwritten target");
    CHECK(memcmp(want, got, 8) == 0, "overwritten target uses source identity");
    CHECK(_zz_getpos(c) == 18, "source sees the alias's progress");
    zz_test_close(c);
    zz_test_close(d);

    /* 3. Unwatched source dup2() over a watched target: the real call
     * succeeds, so the old monitoring must be torn down with no stale
     * seed left behind; failed calls and oldfd == newfd keep state. */
    zzuf_set_seed(88);
    int e = zz_test_open(path, O_RDONLY);
    CHECK(_zz_iswatched(e), "e is watched");
    int u = zz_test_open(path, O_RDONLY);
    _zz_unregister(u);
    CHECK(!_zz_iswatched(u), "u is deliberately unwatched");

    CHECK(zz_test_dup2(u, e) == e, "dup2 unwatched -> watched");
    CHECK(!_zz_iswatched(e), "stale monitoring removed from target");
    errno = 0;
    int rawfd = open(path, O_RDONLY);
    CHECK(rawfd >= 0, "real libc open");
    unsigned char raw[8];
    CHECK(pread(rawfd, raw, 8, 0) == 8, "real raw read");
    close(rawfd);
    CHECK(zz_test_read(e, got, 8) == 8, "read after losing monitoring");
    CHECK(memcmp(raw, got, 8) == 0, "unwatched alias returns raw bytes");
    zz_test_close(u);
    zz_test_close(e);

    zzuf_set_seed(90);
    int t = zz_test_open(path, O_RDONLY);
    uint32_t tseed = _zz_identity_seed(t);
    CHECK(zz_test_read(t, dummy, 5) == 5, "advance t to 5");

    int invalid = zz_test_open(path, O_RDONLY);
    CHECK(invalid >= 0, "open sacrificial fd");
    zz_test_close(invalid);
    errno = 0;
    CHECK(zz_test_dup2(invalid, t) == -1, "dup2 with invalid oldfd fails");
    CHECK(errno == EBADF, "errno is EBADF");
    CHECK(_zz_iswatched(t) && _zz_identity_seed(t) == tseed,
          "target survives failed dup2 with same seed");
    CHECK(_zz_getpos(t) == 5, "target position survives failed dup2");

    errno = 0;
    CHECK(zz_test_dup2(t, -1) == -1, "dup2 with negative newfd fails");
    CHECK(_zz_iswatched(t) && _zz_identity_seed(t) == tseed
          && _zz_getpos(t) == 5, "negative newfd changes nothing");

    CHECK(zz_test_dup2(t, t) == t, "dup2 same descriptor");
    CHECK(_zz_iswatched(t) && _zz_identity_seed(t) == tseed
          && _zz_getpos(t) == 5, "same-fd dup2 keeps the association");
    zz_test_close(t);

    /* 4. Auto-increment: aliases consume neither a seed nor a file
     * selection index; later independent opens keep their order. */
    zzuf_set_auto_increment();
    zzuf_set_seed(100);
    int x = zz_test_open(path, O_RDONLY);
    CHECK(_zz_identity_seed(x) == 100, "first auto seed");
    int y = zz_test_dup(x);
    CHECK(_zz_identity_seed(y) == 100, "dup does not consume a seed");
    int z = zz_test_open(path, O_RDONLY);
    CHECK(_zz_identity_seed(z) == 101, "next open gets the next seed");
    int w = zz_test_open(path, O_RDONLY);
    CHECK(_zz_identity_seed(w) == 102, "second next open gets seed 102");
    int w2 = zz_test_dup2(w, z);
    CHECK(w2 == z && _zz_identity_seed(z) == 102, "dup2 alias keeps seed 102");
    int q = zz_test_open(path, O_RDONLY);
    CHECK(_zz_identity_seed(q) == 103, "aliases never delay later opens");
    zz_test_close(x);
    zz_test_close(y);
    zz_test_close(z);
    zz_test_close(w);
    zz_test_close(q);

    printf("DUP_IDENTITY_OK\n");
    return 0;
}

/* DUP_POSITION_OK: mixed read/readv/pread/lseek sequences on aliases
 * equal one absolute-position mutation of the file, including 1024-byte
 * chunk boundaries, short readv across iovecs and failed lseek. */
static int test_position(char const *path)
{
    static unsigned char want[FILE_SIZE];
    unsigned char buf[128];

    CHECK(sequential_reference(path, 41, want, FILE_SIZE) == 0,
          "sequential reference read");

    zzuf_set_seed(41);
    int fd = zz_test_open(path, O_RDONLY);
    CHECK(fd >= 0, "open working identity");

    /* Plain read straddling the first 1024-byte chunk boundary. */
    CHECK(zz_test_lseek(fd, 1000, SEEK_SET) == 1000, "lseek 1000");
    CHECK(zz_test_read(fd, buf, 48) == 48, "read 48 across boundary");
    CHECK(memcmp(buf, want + 1000, 48) == 0, "cross-chunk read");
    CHECK(_zz_getpos(fd) == 1048, "position after cross-chunk read");

    /* pread() mutates at the explicit offset and leaves the shared
     * sequential position untouched. */
    CHECK(zz_test_pread(fd, buf, 100, 2048) == 100, "pread at 2048");
    CHECK(memcmp(buf, want + 2048, 100) == 0, "pread bytes");
    CHECK(_zz_getpos(fd) == 1048, "pread preserves shared position");

    /* The next read continues exactly where the stream was before. */
    CHECK(zz_test_read(fd, buf, 16) == 16, "read after pread");
    CHECK(memcmp(buf, want + 1048, 16) == 0, "read resumes at shared position");

    /* readv across a chunk boundary, split over two iovecs. */
    CHECK(zz_test_lseek(fd, 1010, SEEK_SET) == 1010, "lseek 1010");
    unsigned char p1[20], p2[20];
    struct iovec iov[2];
    iov[0].iov_base = p1;
    iov[0].iov_len = sizeof p1;
    iov[1].iov_base = p2;
    iov[1].iov_len = sizeof p2;
    CHECK(zz_test_readv(fd, iov, 2) == 40, "readv 40 in two iovecs");
    CHECK(memcmp(p1, want + 1010, 20) == 0 && memcmp(p2, want + 1030, 20) == 0,
          "readv stays contiguous across iovec and chunk boundaries");
    CHECK(_zz_getpos(fd) == 1050, "position after readv");

    /* Short read at EOF crossing an iovec boundary: 4 bytes in the
     * first vector, 2 in the second. */
    CHECK(zz_test_lseek(fd, FILE_SIZE - 6, SEEK_SET) == FILE_SIZE - 6,
          "lseek near EOF");
    unsigned char q1[4], q2[32];
    iov[0].iov_base = q1;
    iov[0].iov_len = 4;
    iov[1].iov_base = q2;
    iov[1].iov_len = sizeof q2;
    CHECK(zz_test_readv(fd, iov, 2) == 6, "short readv at EOF");
    CHECK(memcmp(q1, want + FILE_SIZE - 6, 4) == 0, "short readv first vector");
    CHECK(memcmp(q2, want + FILE_SIZE - 2, 2) == 0,
          "short readv spills into second vector");
    CHECK(_zz_getpos(fd) == FILE_SIZE, "short readv lands on EOF");

    /* A failed lseek() must not mutate any tracked state. */
    errno = 0;
    CHECK(zz_test_lseek(fd, -1, SEEK_SET) == -1, "negative lseek fails");
    CHECK(_zz_getpos(fd) == FILE_SIZE, "failed lseek keeps position");

    /* A successful lseek on one alias is visible through every alias,
     * in both the tracked state and the kernel. */
    int al = zz_test_dup(fd);
    CHECK(al >= 0, "dup for shared seek");
    CHECK(zz_test_lseek(al, 500, SEEK_SET) == 500, "seek via alias");
    CHECK(_zz_getpos(fd) == 500, "other alias sees the new position");
    CHECK(lseek(fd, 0, SEEK_CUR) == 500, "kernel shares the position too");

    /* Back-and-forth seeks across chunk boundaries stay deterministic. */
    static int const offsets[] = { 0, 1023, 1024, 1025, 3000, 7, 2047, 4095 };
    for (size_t i = 0; i < sizeof offsets / sizeof offsets[0]; ++i)
    {
        off_t off = offsets[i];
        CHECK(zz_test_lseek(al, off, SEEK_SET) == off, "seek to offset");
        CHECK(_zz_getpos(fd) == off, "alias position propagates");
        CHECK(zz_test_read(fd, buf, 1) == 1, "read one byte");
        CHECK(buf[0] == want[off], "byte matches absolute-position mutation");
    }

    zz_test_close(al);
    zz_test_close(fd);
    printf("DUP_POSITION_OK\n");
    return 0;
}

/* DUP_LIFETIME_OK: closing one alias keeps the others alive, the last
 * close releases the shared identity, reused fd numbers start a new
 * identity, and growth past the static 32-entry tables stays sound. */
static int test_lifetime(char const *path)
{
    unsigned char want[32], got[32], dummy[20];

    /* 1. One alias closing does not invalidate the other. */
    zzuf_set_seed(33);
    int a = zz_test_open(path, O_RDONLY);
    int b = zz_test_dup(a);
    CHECK(zz_test_read(a, dummy, 20) == 20, "advance identity to 20");
    zz_test_close(a);
    CHECK(_zz_iswatched(b), "remaining alias stays watched");
    CHECK(_zz_getpos(b) == 20, "remaining alias keeps position");

    int ref = sequential_reference_open(path, 33);
    CHECK(ref >= 0 && zz_test_read(ref, dummy, 20) == 20, "ref skip 20");
    CHECK(zz_test_read(ref, want, 16) == 16, "ref 16 bytes");
    zz_test_close(ref);
    CHECK(zz_test_read(b, got, 16) == 16, "read after sibling close");
    CHECK(memcmp(want, got, 16) == 0, "surviving alias output unchanged");
    zz_test_close(b);
    CHECK(!_zz_iswatched(b), "last close releases the identity");

    /* 2. Reusing the freed descriptor number creates a new identity. */
    zzuf_set_auto_increment();
    zzuf_set_seed(200);
    int n = zz_test_open(path, O_RDONLY);
    int reused = n;
    CHECK(_zz_identity_seed(n) == 200, "reused-number open seed 200");
    int m = zz_test_dup(n);
    CHECK(_zz_identity_seed(m) == 200, "alias seed 200");
    zz_test_close(m);
    zz_test_close(n);
    CHECK(!_zz_iswatched(reused), "old number fully released");

    int p = zz_test_open(path, O_RDONLY);
    CHECK(p == reused, "kernel hands the same fd number out again");
    CHECK(_zz_identity_seed(p) == 201, "reused number gets a fresh seed");
    CHECK(_zz_getpos(p) == 0, "reused number starts at position zero");

    zzuf_set_seed(201);
    ref = zz_test_open(path, O_RDONLY);
    CHECK(_zz_identity_seed(ref) == 201 && zz_test_read(ref, want, 32) == 32,
          "reference for the new identity");
    CHECK(zz_test_read(p, got, 32) == 32, "read through reused number");
    CHECK(memcmp(want, got, 32) == 0, "reused identity mutates on its own seed");
    zz_test_close(ref);
    zz_test_close(p);

    /* 3. Exceed the static 32-entry descriptor and identity tables. */
    enum { MANY = 40 };
    int many[MANY];
    for (int i = 0; i < MANY; ++i)
    {
        many[i] = zz_test_open(path, O_RDONLY);
        CHECK(many[i] >= 0, "open past static capacity");
        CHECK(_zz_iswatched(many[i]), "high fd is watched after table growth");
    }

    int high = many[MANY - 1];
    CHECK(high >= STATIC_CAPACITY, "a descriptor lives past the static table");
    uint32_t high_seed = _zz_identity_seed(high);
    int hd = zz_test_dup(high);
    CHECK(hd >= 0, "dup after table growth");
    CHECK(_zz_identity_seed(hd) == high_seed, "alias survives table growth");

    unsigned char h1[16], h2[16];
    CHECK(zz_test_read(high, h1, 16) == 16, "read on high fd");
    CHECK(_zz_getpos(hd) == 16, "high-fd alias shares position");
    CHECK(zz_test_read(hd, h2, 16) == 16, "read on high-fd alias");

    zzuf_set_seed((int32_t)high_seed);
    ref = zz_test_open(path, O_RDONLY);
    CHECK(zz_test_read(ref, want, 32) == 32, "high-fd sequential reference");
    zz_test_close(ref);
    CHECK(memcmp(h1, want, 16) == 0 && memcmp(h2, want + 16, 16) == 0,
          "grown-table aliases match the sequential identity");

    zz_test_close(hd);
    for (int i = 0; i < MANY; ++i)
        zz_test_close(many[i]);

    /* Freed slots and the grown arrays must still serve new opens. */
    int again = zz_test_open(path, O_RDONLY);
    CHECK(again >= 0 && _zz_iswatched(again), "tables reusable after mass close");
    zz_test_close(again);

    printf("DUP_LIFETIME_OK\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s identity|position|lifetime\n", argv[0]);
        return 2;
    }

    setup();

    char path[4096];
    if (make_fixture(path, FILE_SIZE) != 0)
    {
        fprintf(stderr, "cannot create temporary fixture\n");
        return 2;
    }

    int ret;
    if (!strcmp(argv[1], "identity"))
        ret = test_identity(path);
    else if (!strcmp(argv[1], "position"))
        ret = test_position(path);
    else if (!strcmp(argv[1], "lifetime"))
        ret = test_lifetime(path);
    else
    {
        fprintf(stderr, "unknown mode: %s\n", argv[1]);
        ret = 2;
    }

    unlink(path);
    return ret;
}
