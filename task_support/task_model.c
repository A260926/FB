#include "task_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- deterministic random source ---------------- */

void task_rng_seed(task_rng *r, unsigned long long seed)
{
    r->s = seed ? seed : 0x9e3779b97f4a7c15ULL;
}

unsigned long long task_rng_next(task_rng *r)
{
    /* splitmix64: self contained, no library or global state */
    unsigned long long z = (r->s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

unsigned task_rng_below(task_rng *r, unsigned n)
{
    return (unsigned)(task_rng_next(r) % n);
}

/* ---------------- growable byte buffers ---------------- */

static void die(const char *what)
{
    fprintf(stderr, "harness failure: %s\n", what);
    exit(1);
}

void task_buf_init(task_buf *b)
{
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
}

static void buf_reserve(task_buf *b, size_t extra)
{
    if (b->len + extra <= b->cap) return;
    size_t cap = b->cap ? b->cap : 16;
    while (cap < b->len + extra) {
        if (cap > (size_t)-1 / 2) die("buffer too large");
        cap *= 2;
    }
    unsigned char *p = (unsigned char *)realloc(b->p, cap);
    if (p == NULL) die("out of memory");
    b->p = p;
    b->cap = cap;
}

void task_buf_append(task_buf *b, const void *data, size_t n)
{
    if (n == 0) return;
    buf_reserve(b, n);
    memcpy(b->p + b->len, data, n);
    b->len += n;
}

void task_buf_take(task_buf *dst, const task_buf *src)
{
    task_buf_init(dst);
    if (src->len) {
        buf_reserve(dst, src->len);
        memcpy(dst->p, src->p, src->len);
        dst->len = src->len;
    }
}

int task_buf_equal(const task_buf *a, const task_buf *b)
{
    return a->len == b->len && (a->len == 0 || memcmp(a->p, b->p, a->len) == 0);
}

void task_buf_reset(task_buf *b)
{
    b->len = 0;
}

void task_buf_free(task_buf *b)
{
    free(b->p);
    task_buf_init(b);
}

/* ---------------- independent plaintext model ---------------- */

void task_model_bytes(task_rng *r, task_buf *out, size_t n)
{
    unsigned family = task_rng_below(r, 5);
    buf_reserve(out, n ? n : 1);
    size_t i = 0;
    switch (family) {
    case 0: /* uniformly spread bytes */
        for (; i < n; i++)
            out->p[out->len + i] = (unsigned char)(task_rng_next(r) >> 32);
        break;
    case 1: /* tiny alphabet, highly compressible */
        for (; i < n; i++)
            out->p[out->len + i] = (unsigned char)task_rng_below(r, 4);
        break;
    case 2: /* printable text-like data */
        for (; i < n; i++)
            out->p[out->len + i] = (unsigned char)(' ' + task_rng_below(r, 95));
        break;
    case 3: { /* variable length runs */
        unsigned char v = 0;
        size_t run = 0;
        for (; i < n; i++) {
            if (run == 0) {
                v = (unsigned char)task_rng_next(r);
                run = 1 + task_rng_below(r, 8);
            }
            out->p[out->len + i] = v;
            run--;
        }
        break;
    }
    default: { /* two-symbol backbone with rare excursions */
        unsigned char a = (unsigned char)task_rng_next(r);
        unsigned char b = (unsigned char)task_rng_next(r);
        for (; i < n; i++) {
            unsigned char v = task_rng_below(r, 16) == 0
                                  ? (unsigned char)task_rng_next(r)
                                  : (task_rng_below(r, 2) ? a : b);
            out->p[out->len + i] = v;
        }
        break;
    }
    }
    out->len += n;
}

/* ---------------- accounting allocator ---------------- */

static unsigned long acct_live;
static unsigned long acct_total;

void task_acct_reset(void)
{
    acct_live = 0;
    acct_total = 0;
}

unsigned long task_acct_live(void)  { return acct_live; }
unsigned long task_acct_total(void) { return acct_total; }

voidpf task_zalloc(voidpf opaque, unsigned items, unsigned size)
{
    (void)opaque;
    size_t bytes = (size_t)items * (size_t)size;
    void *p = malloc(bytes ? bytes : 1);
    if (p != NULL) {
        acct_live++;
        acct_total++;
    }
    return p;
}

void task_zfree(voidpf opaque, voidpf address)
{
    (void)opaque;
    if (address == NULL) return;
    if (acct_live == 0) die("allocator underflow");
    acct_live--;
    free(address);
}

/* ---------------- caller-owned branch bookkeeping ---------------- */

void task_branch_init(task_branch *b)
{
    task_buf_init(&b->expect);
    task_buf_init(&b->prefix);
    task_buf_init(&b->store);
    b->cursor = 0;
    b->out = NULL;
    b->out_cap = 0;
    b->finished = 0;
    b->ended = 0;
}

void task_branch_duplicate(const task_branch *src, task_branch *dst)
{
    task_branch_init(dst);
    /* deep, independent copies of every caller-owned model store */
    task_buf_take(&dst->expect, &src->expect);
    task_buf_take(&dst->prefix, &src->prefix);
    task_buf_take(&dst->store, &src->store);
    dst->cursor = src->cursor;
    dst->out_cap = src->out_cap;
    if (dst->out_cap) {
        dst->out = (unsigned char *)malloc(dst->out_cap);
        if (dst->out == NULL) die("out of memory");
    }
    dst->finished = src->finished;
    dst->ended = src->ended;
}

void task_branch_bind_out(task_rng *r, task_branch *b, z_streamp z)
{
    /* distinct output store per branch, deliberately small so every stream
     * drains through several slots, but large enough to stay linear */
    free(b->out);
    b->out_cap = 32 + (size_t)task_rng_below(r, 224);
    b->out = (unsigned char *)malloc(b->out_cap);
    if (b->out == NULL) die("out of memory");
    z->next_out = b->out;
    z->avail_out = (uInt)b->out_cap;
}

void task_branch_drain(task_branch *b, z_streamp z)
{
    size_t produced = b->out_cap - z->avail_out;
    if (produced)
        task_buf_append(&b->prefix, b->out, produced);
    z->next_out = b->out;
    z->avail_out = (uInt)b->out_cap;
}

void task_branch_free(task_branch *b)
{
    task_buf_free(&b->expect);
    task_buf_free(&b->prefix);
    task_buf_free(&b->store);
    free(b->out);
    task_branch_init(b);
}

/* ---------------- reference codec (no copy API ever used) ---------------- */

static void ref_drain(z_streamp s, task_buf *out, unsigned char *slot, size_t cap)
{
    size_t produced = cap - s->avail_out;
    if (produced)
        task_buf_append(out, slot, produced);
    s->next_out = slot;
    s->avail_out = (uInt)cap;
}

int task_ref_deflate(const task_buf *plain, task_buf *compressed,
                     int window_bits, int level,
                     const unsigned char *dict, unsigned dict_len,
                     int mid_flush, unsigned chunk)
{
    z_stream s;
    memset(&s, 0, sizeof(s));
    int rc = deflateInit2(&s, level, Z_DEFLATED, window_bits, 8,
                          Z_DEFAULT_STRATEGY);
    if (rc != Z_OK) return 1;

    if (dict_len) {
        rc = deflateSetDictionary(&s, dict, (uInt)dict_len);
        if (rc != Z_OK) { deflateEnd(&s); return 2; }
    }

    unsigned char slot[256];
    s.next_out = slot;
    s.avail_out = sizeof(slot);

    size_t off = 0;
    int flushed = 0;
    while (off < plain->len) {
        size_t take = chunk ? chunk : plain->len - off;
        if (take > plain->len - off) take = plain->len - off;
        s.next_in = plain->p + off;
        s.avail_in = (uInt)take;
        do {
            rc = deflate(&s, Z_NO_FLUSH);
            ref_drain(&s, compressed, slot, sizeof(slot));
        } while (rc == Z_OK && s.avail_out == 0);
        if (rc != Z_OK) { deflateEnd(&s); return 3; }
        off += take - s.avail_in;

        if (mid_flush && !flushed && off >= plain->len / 2 && plain->len) {
            s.next_in = NULL;
            s.avail_in = 0;
            do {
                rc = deflate(&s, mid_flush);
                ref_drain(&s, compressed, slot, sizeof(slot));
            } while (rc == Z_OK && s.avail_out == 0);
            if (rc != Z_OK) { deflateEnd(&s); return 4; }
            flushed = 1;
        }
    }

    s.next_in = NULL;
    s.avail_in = 0;
    do {
        rc = deflate(&s, Z_FINISH);
        ref_drain(&s, compressed, slot, sizeof(slot));
    } while (rc == Z_OK);
    if (rc != Z_STREAM_END) { deflateEnd(&s); return 5; }
    deflateEnd(&s);
    return 0;
}

int task_ref_inflate(const task_buf *compressed, task_buf *plain,
                     int window_bits,
                     const unsigned char *dict, unsigned dict_len,
                     unsigned in_chunk, unsigned out_chunk)
{
    z_stream s;
    memset(&s, 0, sizeof(s));
    int rc = inflateInit2(&s, window_bits);
    if (rc != Z_OK) return 1;

    size_t cap = out_chunk ? (size_t)out_chunk
                           : (compressed->len * 4 + 64);
    if (cap < 64) cap = 64;
    unsigned char *slot = (unsigned char *)malloc(cap);
    if (slot == NULL) { inflateEnd(&s); return 2; }
    s.next_out = slot;
    s.avail_out = (uInt)cap;

    if (window_bits < 0 && dict_len) {
        rc = inflateSetDictionary(&s, dict, (uInt)dict_len);
        if (rc != Z_OK) { free(slot); inflateEnd(&s); return 3; }
    }

    size_t off = 0;
    int dict_applied = (window_bits < 0 && dict_len != 0);
    for (;;) {
        size_t take = in_chunk ? (size_t)in_chunk : compressed->len - off;
        if (take > compressed->len - off) take = compressed->len - off;
        s.next_in = compressed->p + off;
        s.avail_in = (uInt)take;

        rc = inflate(&s, Z_NO_FLUSH);
        ref_drain(&s, plain, slot, cap);
        off = (size_t)(s.next_in - compressed->p);

        if (rc == Z_NEED_DICT) {
            if (dict_applied || dict_len == 0) {
                free(slot); inflateEnd(&s); return 4;
            }
            if (s.adler != adler32(adler32(0L, Z_NULL, 0),
                                   dict, (uInt)dict_len)) {
                free(slot); inflateEnd(&s); return 5;
            }
            rc = inflateSetDictionary(&s, dict, (uInt)dict_len);
            if (rc != Z_OK) { free(slot); inflateEnd(&s); return 6; }
            dict_applied = 1;
            continue;
        }
        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK && rc != Z_BUF_ERROR) {
            free(slot); inflateEnd(&s); return 7;
        }
        if (rc == Z_BUF_ERROR && s.avail_in == 0 && off >= compressed->len) {
            free(slot); inflateEnd(&s); return 8;
        }
    }

    free(slot);
    inflateEnd(&s);
    return 0;
}
