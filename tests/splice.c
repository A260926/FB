/*
 * Regression tests for pjmedia_rtp_splice: the timed in-memory RTP
 * source continuation / handover buffer.
 *
 * Registers the timing, handover and ownership groups consumed by
 * task_support/driver.c.
 */
#include "task_support/check.h"

#include <pjmedia/rtp.h>
#include <pj/sock.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>


#define PT_X        96
#define SSRC_A      UINT32_C(0x11111111)
#define SSRC_B      UINT32_C(0x22222222)
#define SSRC_C      UINT32_C(0x33333333)
#define SSRC_OUT    UINT32_C(0xdddddddd)

#define CLOCK_RATE  8000
#define FRAME_TICKS 160
#define IV_US       UINT64_C(20000)
#define DELAY_US    UINT64_C(40000)
#define WIN         16


/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint16_t rd16(const uint8_t *b)
{
    return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}

static uint32_t rd32(const uint8_t *b)
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

/* Build a plain (no CSRC/extension/padding) RTP packet. */
static size_t make_rtp(uint8_t *b, size_t cap,
                       uint16_t seq, uint32_t ts, uint32_t ssrc,
                       int pt, const char *pl)
{
    size_t plen = pl ? strlen(pl) : 0;
    size_t n = 12 + plen;
    if (cap < n)
        return 0;
    memset(b, 0, n);
    b[0] = 0x80;
    b[1] = (uint8_t)(pt & 0x7f);
    b[2] = (uint8_t)(seq >> 8);
    b[3] = (uint8_t)(seq & 0xff);
    b[4] = (uint8_t)(ts >> 24);
    b[5] = (uint8_t)((ts >> 16) & 0xff);
    b[6] = (uint8_t)((ts >> 8) & 0xff);
    b[7] = (uint8_t)(ts & 0xff);
    b[8] = (uint8_t)(ssrc >> 24);
    b[9] = (uint8_t)((ssrc >> 16) & 0xff);
    b[10] = (uint8_t)((ssrc >> 8) & 0xff);
    b[11] = (uint8_t)(ssrc & 0xff);
    if (plen)
        memcpy(b + 12, pl, plen);
    return n;
}

static pjmedia_rtp_splice_cfg cfg_default(void)
{
    pjmedia_rtp_splice_cfg c;
    memset(&c, 0, sizeof(c));
    c.ssrc = SSRC_A;
    c.out_ssrc = SSRC_OUT;
    c.pt = PT_X;
    c.clock_rate = CLOCK_RATE;
    c.frame_ticks = FRAME_TICKS;
    c.delay_us = DELAY_US;
    c.window = WIN;
    c.out_seq = 100;
    c.out_ts = 2000;
    return c;
}

static int push_seq(pjmedia_rtp_splice *sp,
                    uint16_t seq, uint32_t ts, uint32_t ssrc,
                    uint64_t now, const char *pl)
{
    uint8_t b[256];
    size_t n = make_rtp(b, sizeof(b), seq, ts, ssrc, PT_X, pl);
    return pjmedia_rtp_splice_push(sp, b, n, now);
}

/* Validate the rewritten header and payload. */
static int hdr_ok(const void *p, size_t n,
                  uint16_t seq, uint32_t ts, uint32_t ssrc,
                  const char *pl)
{
    const uint8_t *b = (const uint8_t *)p;
    size_t plen = pl ? strlen(pl) : 0;
    if (!p || n < 12)
        return 0;
    if ((b[0] >> 6) != 2)
        return 0;
    if ((b[1] & 0x7f) != PT_X)
        return 0;
    if (rd16(b + 2) != seq)
        return 0;
    if (rd32(b + 4) != ts)
        return 0;
    if (rd32(b + 8) != ssrc)
        return 0;
    if (pl) {
        if (n != 12 + plen)
            return 0;
        if (memcmp(b + 12, pl, plen) != 0)
            return 0;
    }
    return 1;
}

static uint16_t out_seq_k(unsigned k)
{
    return (uint16_t)(100u + k);
}

static uint32_t out_ts_k(unsigned k)
{
    return (uint32_t)(2000u + (uint32_t)k * FRAME_TICKS);
}


/* ================================================================== */
/* timing group                                                        */
/* ================================================================== */

static int t_config_validation(void)
{
    pjmedia_rtp_splice *sp = NULL;
    pjmedia_rtp_splice_cfg c;

    TASK_CHECK(pjmedia_rtp_splice_alloc(NULL, cfg_default()) == EINVAL);

    c = cfg_default();
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, NULL) == EINVAL);
    TASK_CHECK(sp == NULL);

    c = cfg_default(); c.pt = 128;
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == EINVAL);
    c = cfg_default(); c.pt = -1;
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == EINVAL);

    c = cfg_default(); c.clock_rate = 0;
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == EINVAL);
    c = cfg_default(); c.clock_rate = -8000;
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == EINVAL);
    c = cfg_default(); c.frame_ticks = 0;
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == EINVAL);
    c = cfg_default(); c.frame_ticks = -160;
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == EINVAL);

    /* 160*1e6 is not divisible by 44100. */
    c = cfg_default(); c.clock_rate = 44100; c.frame_ticks = 160;
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == EINVAL);

    c = cfg_default(); c.delay_us = -1;
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == EINVAL);

    c = cfg_default(); c.window = 3;
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == EINVAL);
    c = cfg_default(); c.window = 513;
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == EINVAL);

    /* Valid extremes. */
    c = cfg_default(); c.window = 4;
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    pjmedia_rtp_splice_destroy(sp); sp = NULL;

    c = cfg_default(); c.window = 512;
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    pjmedia_rtp_splice_destroy(sp); sp = NULL;

    /* 441*1e6 / 44100 = 10000 us exactly. */
    c = cfg_default(); c.clock_rate = 44100; c.frame_ticks = 441;
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    pjmedia_rtp_splice_destroy(sp); sp = NULL;

    /* Zero delay is allowed. */
    c = cfg_default(); c.delay_us = 0;
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    pjmedia_rtp_splice_destroy(sp); sp = NULL;

    /* By-value configuration through the generic wrapper. */
    c = cfg_default();
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, c) == 0);
    TASK_CHECK(sp != NULL);
    pjmedia_rtp_splice_destroy(sp);

    return 0;
}

static int t_start_boundary(void)
{
    pjmedia_rtp_splice *sp;
    pjmedia_rtp_splice_cfg c = cfg_default();
    void *out = (void *)(uintptr_t)0x1234;
    size_t out_len = 77;

    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);

    TASK_CHECK(pjmedia_rtp_splice_head(sp) == 0);
    TASK_CHECK(pjmedia_rtp_splice_active(sp) == SSRC_A);

    /* No anchor yet: EAGAIN, head stays 0, outputs untouched. */
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 0, &out, &out_len) == EAGAIN);
    TASK_CHECK(pjmedia_rtp_splice_head(sp) == 0);
    TASK_CHECK(out == (void *)(uintptr_t)0x1234);
    TASK_CHECK(out_len == 77);

    /* Unknown SSRC does not anchor. */
    TASK_CHECK(push_seq(sp, 7000, 500000, SSRC_B, 500, "x") == ENOENT);
    TASK_CHECK(pjmedia_rtp_splice_head(sp) == 0);

    /* First legal initial-source packet: S=7000 T=500000 A=1000. */
    TASK_CHECK(push_seq(sp, 7000, 500000, SSRC_A, 1000, "zero") == 0);
    TASK_CHECK(pjmedia_rtp_splice_head(sp) == 0);
    TASK_CHECK(pjmedia_rtp_splice_active(sp) == SSRC_A);

    /* Deadline slot0 = 1000 + 40000 = 41000. */
    out = (void *)(uintptr_t)0x1234;
    out_len = 77;
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 40999, &out, &out_len)
               == EAGAIN);
    TASK_CHECK(pjmedia_rtp_splice_head(sp) == 0);
    TASK_CHECK(out == (void *)(uintptr_t)0x1234);
    TASK_CHECK(out_len == 77);

    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 41000, &out, &out_len) == 0);
    TASK_CHECK(hdr_ok(out, out_len, 100, 2000, SSRC_OUT, "zero"));
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_head(sp) == 1);

    /* Deadline slot1 = 61000. */
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 60999, &out, &out_len)
               == EAGAIN);
    TASK_CHECK(pjmedia_rtp_splice_head(sp) == 1);

    pjmedia_rtp_splice_destroy(sp);
    return 0;
}

static int t_deadline_edges(void)
{
    pjmedia_rtp_splice *sp;
    pjmedia_rtp_splice_cfg c = cfg_default();
    void *out = NULL;
    size_t n = 0;

    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    /* A=1000, deadlines 41000 + k*20000. */
    TASK_CHECK(push_seq(sp, 7000, 500000, SSRC_A, 1000, "p0") == 0);

    /* Exactly on slot1 deadline is still acceptable. */
    TASK_CHECK(push_seq(sp, 7001, 500160, SSRC_A, 61000, "p1") == 0);

    /* Exactly on slot2 deadline accepted, one microsecond late rejected. */
    TASK_CHECK(push_seq(sp, 7002, 500320, SSRC_A, 81000, "p2") == 0);
    TASK_CHECK(push_seq(sp, 7002, 500320, SSRC_A, 81001, "p2x")
               == ETIMEDOUT);

    /* Slot3 strictly late is refused and leaves no packet. */
    TASK_CHECK(push_seq(sp, 7003, 500480, SSRC_A, 101001, "p3")
               == ETIMEDOUT);

    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 200000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(0), out_ts_k(0), SSRC_OUT, "p0"));
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 200000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(1), out_ts_k(1), SSRC_OUT, "p1"));
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 200000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(2), out_ts_k(2), SSRC_OUT, "p2"));
    free(out);

    /* Slot3 was never stored: ENODATA but head still advances. */
    {
        void *sent = (void *)(uintptr_t)0xbeef;
        size_t slen = 31337;
        TASK_CHECK(pjmedia_rtp_splice_pop(sp, 200000, &sent, &slen)
                   == ENODATA);
        TASK_CHECK(pjmedia_rtp_splice_head(sp) == 4);
        TASK_CHECK(sent == (void *)(uintptr_t)0xbeef);
        TASK_CHECK(slen == 31337);
    }

    pjmedia_rtp_splice_destroy(sp);
    return 0;
}

static int t_old_far_slots(void)
{
    pjmedia_rtp_splice *sp;
    pjmedia_rtp_splice_cfg c = cfg_default();
    void *out = NULL;
    size_t n = 0;

    c.window = 8;
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);

    /* S=1000 T=100000 A=0. */
    TASK_CHECK(push_seq(sp, 1000, 100000, SSRC_A, 0, "s0") == 0);

    /* k=-1 with matching timestamp: too old -> ENOENT. */
    TASK_CHECK(push_seq(sp, 999, 99840, SSRC_A, 0, "old") == ENOENT);

    /* k=8 == window: too far -> ENOBUFS. */
    TASK_CHECK(push_seq(sp, 1008, 101280, SSRC_A, 0, "far") == ENOBUFS);

    /* k=7 is the furthest accepted slot. */
    TASK_CHECK(push_seq(sp, 1007, 101120, SSRC_A, 0, "s7") == 0);

    /* Bad timestamp for a known slot -> EBADMSG. */
    TASK_CHECK(push_seq(sp, 1001, 123456, SSRC_A, 0, "badts")
               == EBADMSG);

    /* Consume slot0. */
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 40000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(0), out_ts_k(0), SSRC_OUT, "s0"));
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_head(sp) == 1);

    /* Slot0 is now behind head. */
    TASK_CHECK(push_seq(sp, 1000, 100000, SSRC_A, 40000, "dup")
               == ENOENT);

    pjmedia_rtp_splice_destroy(sp);
    return 0;
}

static int t_lost_slot_seq(void)
{
    pjmedia_rtp_splice *sp;
    pjmedia_rtp_splice_cfg c = cfg_default();
    void *out = NULL;
    size_t n = 0;
    void *sent = (void *)(uintptr_t)0x9999;
    size_t slen = 555;

    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    TASK_CHECK(push_seq(sp, 500, 9000, SSRC_A, 0, "a") == 0);

    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 40000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(0), out_ts_k(0), SSRC_OUT, "a"));
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_head(sp) == 1);

    /* Slot1 missing: ENODATA advances head. */
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 60000, &sent, &slen)
               == ENODATA);
    TASK_CHECK(pjmedia_rtp_splice_head(sp) == 2);
    TASK_CHECK(sent == (void *)(uintptr_t)0x9999);
    TASK_CHECK(slen == 555);

    /* Slot2 packet still gets output identity k=2. */
    TASK_CHECK(push_seq(sp, 502, 9320, SSRC_A, 70000, "c") == 0);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 80000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(2), out_ts_k(2), SSRC_OUT, "c"));
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_head(sp) == 3);

    pjmedia_rtp_splice_destroy(sp);
    return 0;
}

static int t_double_wrap(void)
{
    pjmedia_rtp_splice *sp;
    pjmedia_rtp_splice_cfg c = cfg_default();
    enum { KMAX = 10 };
    unsigned order[KMAX + 1] = { 0, 5, 2, 9, 1, 10, 3, 4, 6, 7, 8 };
    unsigned i;

    c.delay_us = 0;
    c.out_seq = 65534;
    c.out_ts = UINT32_C(0xffffff80);
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);

    /* Input coordinates themselves sit just below both wrap points. */
    for (i = 0; i <= KMAX; ++i) {
        unsigned k = order[i];
        char pl[8];
        uint16_t s = (uint16_t)(65530u + k);
        uint32_t t = UINT32_C(0xffffff00) + (uint32_t)k * FRAME_TICKS;
        snprintf(pl, sizeof(pl), "k%u", k);
        TASK_CHECK(push_seq(sp, s, t, SSRC_A, 0, pl) == 0);
    }

    for (i = 0; i <= KMAX; ++i) {
        char pl[8];
        void *out = NULL;
        size_t n = 0;
        uint16_t es = (uint16_t)(65534u + i);
        uint32_t et = UINT32_C(0xffffff80) + (uint32_t)i * FRAME_TICKS;
        snprintf(pl, sizeof(pl), "k%u", i);
        TASK_CHECK(pjmedia_rtp_splice_pop(sp, UINT64_C(1000000),
                                         &out, &n) == 0);
        TASK_CHECK(hdr_ok(out, n, es, et, SSRC_OUT, pl));
        free(out);
    }
    TASK_CHECK(pjmedia_rtp_splice_head(sp) == KMAX + 1);

    pjmedia_rtp_splice_destroy(sp);
    return 0;
}

static int t_malformed(void)
{
    pjmedia_rtp_splice *sp;
    pjmedia_rtp_splice_cfg c = cfg_default();
    uint8_t b[64];
    size_t n;
    void *out = NULL;
    size_t on = 0;

    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);

    /* Wrong version. */
    n = make_rtp(b, sizeof(b), 1, 1, SSRC_A, PT_X, "x");
    b[0] = 0x40;
    TASK_CHECK(pjmedia_rtp_splice_push(sp, b, n, 0) == EBADMSG);

    /* Truncated base header. */
    TASK_CHECK(pjmedia_rtp_splice_push(sp, b, 10, 0) == EBADMSG);

    /* CC=1 but no CSRC bytes. */
    n = make_rtp(b, sizeof(b), 1, 1, SSRC_A, PT_X, "x");
    b[0] = 0x81;
    TASK_CHECK(pjmedia_rtp_splice_push(sp, b, 12, 0) == EBADMSG);

    /* Extension bit set but extension header truncated. */
    memset(b, 0, sizeof(b));
    b[0] = 0x90;
    b[1] = PT_X;
    TASK_CHECK(pjmedia_rtp_splice_push(sp, b, 14, 0) == EBADMSG);

    /* Extension length runs past the packet. */
    memset(b, 0, 16);
    b[0] = 0x90;
    b[1] = PT_X;
    b[14] = 0; b[15] = 100;
    TASK_CHECK(pjmedia_rtp_splice_push(sp, b, 16, 0) == EBADMSG);

    /* Illegal padding: pad count larger than the packet tail. */
    n = make_rtp(b, sizeof(b), 1, 1, SSRC_A, PT_X, "x");
    b[0] = 0xa0;
    b[n - 1] = 5;
    TASK_CHECK(pjmedia_rtp_splice_push(sp, b, n, 0) == EBADMSG);

    /* Illegal padding: zero pad count. */
    n = make_rtp(b, sizeof(b), 1, 1, SSRC_A, PT_X, "x");
    b[0] = 0xa0;
    b[n - 1] = 0;
    TASK_CHECK(pjmedia_rtp_splice_push(sp, b, n, 0) == EBADMSG);

    /* Wrong PT. */
    TASK_CHECK(push_seq(sp, 1, 1, SSRC_A, 0, "x") == 0); /* anchors? */
    /* The previous line is a legal packet and anchors; so use a fresh
     * instance for the wrong-PT-before-anchor assertion.
     */
    pjmedia_rtp_splice_destroy(sp);

    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    n = make_rtp(b, sizeof(b), 1, 1, SSRC_A, 7, "x");
    TASK_CHECK(pjmedia_rtp_splice_push(sp, b, n, 0) == EBADMSG);
    TASK_CHECK(pjmedia_rtp_splice_head(sp) == 0);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 100000, &out, &on) == EAGAIN);

    /* A legal one-byte-padding packet is accepted and preserved. */
    n = make_rtp(b, sizeof(b), 7000, 500000, SSRC_A, PT_X, "pd");
    b[0] |= 0x20;            /* P bit */
    b[n] = 1;                /* one padding octet */
    ++n;
    TASK_CHECK(pjmedia_rtp_splice_push(sp, b, n, 1000) == 0);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 41000, &out, &on) == 0);
    TASK_CHECK(on == n);
    TASK_CHECK(((uint8_t *)out)[0] == b[0]);
    TASK_CHECK(((uint8_t *)out)[n - 1] == 1);
    free(out);

    pjmedia_rtp_splice_destroy(sp);
    return 0;
}


/* Odd (non power of two) window ring wrap. */
static int t_odd_window(void)
{
    pjmedia_rtp_splice *sp;
    pjmedia_rtp_splice_cfg c = cfg_default();
    void *out = NULL;
    size_t n = 0;
    unsigned k;

    c.window = 5;
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    TASK_CHECK(push_seq(sp, 1000, 100000, SSRC_A, 0, "p0") == 0);
    for (k = 1; k <= 4; ++k) {
        char pl[8];
        snprintf(pl, sizeof(pl), "p%u", k);
        TASK_CHECK(push_seq(sp, (uint16_t)(1000u + k),
                            100000u + k * FRAME_TICKS,
                            SSRC_A, 0, pl) == 0);
    }
    /* k=5 is outside the initial [0,5) window. */
    TASK_CHECK(push_seq(sp, 1005, 100800, SSRC_A, 0, "p5") == ENOBUFS);

    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 40000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(0), out_ts_k(0), SSRC_OUT, "p0"));
    free(out);

    /* Head is 1: k=5 now fits [1,6) and reuses ring index 0. */
    TASK_CHECK(push_seq(sp, 1005, 100800, SSRC_A, 40000, "p5") == 0);

    for (k = 1; k <= 5; ++k) {
        char pl[8];
        snprintf(pl, sizeof(pl), "p%u", k);
        TASK_CHECK(pjmedia_rtp_splice_pop(sp, 40000 + k * IV_US,
                                         &out, &n) == 0);
        TASK_CHECK(hdr_ok(out, n, out_seq_k(k), out_ts_k(k), SSRC_OUT,
                          pl));
        free(out);
    }
    TASK_CHECK(pjmedia_rtp_splice_head(sp) == 6);

    /* Window arm bound for win=5: max dist = 2. */
    TASK_CHECK(pjmedia_rtp_splice_arm(sp, SSRC_B, 6) == 0);
    pjmedia_rtp_splice_cancel(sp);

    pjmedia_rtp_splice_destroy(sp);
    return 0;
}


/* ================================================================== */
/* handover group                                                      */
/* ================================================================== */

/* Push old-source packets a0..a(max) at S=1000 T=500000. */
static void push_old_range(pjmedia_rtp_splice *sp, unsigned max)
{
    unsigned k;
    for (k = 0; k <= max; ++k) {
        char pl[8];
        snprintf(pl, sizeof(pl), "a%u", k);
        if (push_seq(sp, (uint16_t)(1000u + k), 500000u + k * FRAME_TICKS,
                     SSRC_A, 0, pl) != 0) {
            /* Tests check results individually where it matters. */
        }
    }
}

static int ho_basic(void)
{
    pjmedia_rtp_splice *sp;
    pjmedia_rtp_splice_cfg c = cfg_default();
    void *out = NULL;
    size_t n = 0;

    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    push_old_range(sp, 5);

    TASK_CHECK(pjmedia_rtp_splice_arm(sp, SSRC_B, 2) == 0);

    /* Candidate coordinates: seq 9000 / ts 700000 at cut slot 2. */
    TASK_CHECK(push_seq(sp, 9000, 700000, SSRC_B, 0, "b2") == 0);
    TASK_CHECK(push_seq(sp, 9001, 700160, SSRC_B, 0, "b3") == 0);
    TASK_CHECK(push_seq(sp, 9002, 700320, SSRC_B, 0, "b4") == 0);

    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 40000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(0), out_ts_k(0), SSRC_OUT, "a0"));
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 60000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(1), out_ts_k(1), SSRC_OUT, "a1"));
    free(out);

    /* At cut 2 the ready candidate wins atomically. */
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 80000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(2), out_ts_k(2), SSRC_OUT, "b2"));
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_active(sp) == SSRC_B);
    TASK_CHECK(pjmedia_rtp_splice_head(sp) == 3);

    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 100000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(3), out_ts_k(3), SSRC_OUT, "b3"));
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 120000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(4), out_ts_k(4), SSRC_OUT, "b4"));
    free(out);

    /* Old SSRC is rejected after the switch. */
    TASK_CHECK(push_seq(sp, 1005, 500800, SSRC_A, 120000, "lateA")
               == ENOENT);

    /* Candidate coordinates continue as the active source. */
    TASK_CHECK(push_seq(sp, 9003, 700480, SSRC_B, 120000, "b5") == 0);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 140000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(5), out_ts_k(5), SSRC_OUT, "b5"));
    free(out);

    pjmedia_rtp_splice_destroy(sp);
    return 0;
}

static int ho_reorder_dup(void)
{
    pjmedia_rtp_splice *sp;
    pjmedia_rtp_splice_cfg c = cfg_default();
    void *out = NULL;
    size_t n = 0;

    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    push_old_range(sp, 4);
    TASK_CHECK(pjmedia_rtp_splice_arm(sp, SSRC_B, 2) == 0);

    /* First candidate packet defines cut coordinates. */
    TASK_CHECK(push_seq(sp, 9000, 700000, SSRC_B, 0, "first") == 0);
    /* A timestamp-bad proof is rejected and does not occupy the slot. */
    TASK_CHECK(push_seq(sp, 9001, 777777, SSRC_B, 0, "badts")
               == EBADMSG);
    /* Malformed candidate bytes never establish candidate coordinates. */
    {
        uint8_t bad[13];
        size_t bn = make_rtp(bad, sizeof(bad), 9500, 1, SSRC_B, PT_X, "z");
        TASK_CHECK(bn == 13);
        bad[0] = 0x40; /* wrong version */
        TASK_CHECK(pjmedia_rtp_splice_push(sp, bad, bn, 0) == EBADMSG);
    }
    /* Duplicate cut packet with different content: first wins. */
    TASK_CHECK(push_seq(sp, 9000, 700000, SSRC_B, 1000, "second") == 0);
    /* Remaining proofs arrive out of order: c+2 then c+1. The corrected
     * c+1 proof after the bad-timestamp attempt is accepted. */
    TASK_CHECK(push_seq(sp, 9002, 700320, SSRC_B, 0, "b4") == 0);
    TASK_CHECK(push_seq(sp, 9001, 700160, SSRC_B, 0, "b3") == 0);
    /* Repeated proof does not break anything. */
    TASK_CHECK(push_seq(sp, 9001, 700160, SSRC_B, 0, "b3") == 0);

    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 40000, &out, &n) == 0);
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 60000, &out, &n) == 0);
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 80000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(2), out_ts_k(2), SSRC_OUT,
                      "first"));
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_active(sp) == SSRC_B);

    pjmedia_rtp_splice_destroy(sp);
    return 0;
}

static int ho_insufficient_fallback(void)
{
    pjmedia_rtp_splice *sp;
    pjmedia_rtp_splice_cfg c = cfg_default();
    void *out = NULL;
    size_t n = 0;

    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    push_old_range(sp, 3);
    TASK_CHECK(pjmedia_rtp_splice_arm(sp, SSRC_B, 2) == 0);

    /* Only c and c+2: proof incomplete. */
    TASK_CHECK(push_seq(sp, 9000, 700000, SSRC_B, 0, "b2") == 0);
    TASK_CHECK(push_seq(sp, 9002, 700320, SSRC_B, 0, "b4") == 0);

    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 40000, &out, &n) == 0);
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 60000, &out, &n) == 0);
    free(out);

    /* Cut pop falls back to old source packet a2. */
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 80000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(2), out_ts_k(2), SSRC_OUT, "a2"));
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_active(sp) == SSRC_A);
    TASK_CHECK(pjmedia_rtp_splice_head(sp) == 3);

    /* Late candidate can no longer switch. */
    TASK_CHECK(push_seq(sp, 9001, 700160, SSRC_B, 80000, "late")
               == ENOENT);
    TASK_CHECK(push_seq(sp, 9000, 700000, SSRC_B, 80000, "late2")
               == ENOENT);

    /* The old source coordinates continue normally after fallback. */
    TASK_CHECK(push_seq(sp, 1003, 500480, SSRC_A, 80000, "a3") == 0);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 100000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(3), out_ts_k(3), SSRC_OUT, "a3"));
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_active(sp) == SSRC_A);

    pjmedia_rtp_splice_destroy(sp);
    return 0;
}

static int ho_cut_endata(void)
{
    pjmedia_rtp_splice *sp;
    pjmedia_rtp_splice_cfg c = cfg_default();
    void *sent = (void *)(uintptr_t)0x777;
    size_t slen = 42;

    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    /* Only the anchor slot0 packet exists. */
    TASK_CHECK(push_seq(sp, 1000, 500000, SSRC_A, 0, "a0") == 0);
    TASK_CHECK(pjmedia_rtp_splice_arm(sp, SSRC_B, 2) == 0);
    /* Incomplete candidate proof. */
    TASK_CHECK(push_seq(sp, 9000, 700000, SSRC_B, 0, "b2") == 0);

    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 40000, &sent, &slen) == 0);
    free(sent);
    sent = (void *)(uintptr_t)0x777; slen = 42;

    /* Missing slot1 -> ENODATA, outputs untouched. */
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 60000, &sent, &slen)
               == ENODATA);
    TASK_CHECK(pjmedia_rtp_splice_head(sp) == 2);
    TASK_CHECK(sent == (void *)(uintptr_t)0x777 && slen == 42);

    /* At the cut the candidate is not ready: fallback + ENODATA. */
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 80000, &sent, &slen)
               == ENODATA);
    TASK_CHECK(pjmedia_rtp_splice_head(sp) == 3);
    TASK_CHECK(pjmedia_rtp_splice_active(sp) == SSRC_A);
    TASK_CHECK(sent == (void *)(uintptr_t)0x777 && slen == 42);

    pjmedia_rtp_splice_destroy(sp);
    return 0;
}

static int ho_cancel(void)
{
    pjmedia_rtp_splice *sp;
    pjmedia_rtp_splice_cfg c = cfg_default();
    void *out = NULL;
    size_t n = 0;

    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    push_old_range(sp, 2);
    TASK_CHECK(pjmedia_rtp_splice_arm(sp, SSRC_B, 2) == 0);
    TASK_CHECK(push_seq(sp, 9000, 700000, SSRC_B, 0, "b2") == 0);
    TASK_CHECK(push_seq(sp, 9001, 700160, SSRC_B, 0, "b3") == 0);
    TASK_CHECK(push_seq(sp, 9002, 700320, SSRC_B, 0, "b4") == 0);

    pjmedia_rtp_splice_cancel(sp);

    /* Candidate SSRC is unknown again. */
    TASK_CHECK(push_seq(sp, 9000, 700000, SSRC_B, 0, "b2") == ENOENT);

    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 40000, &out, &n) == 0);
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 60000, &out, &n) == 0);
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 80000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(2), out_ts_k(2), SSRC_OUT, "a2"));
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_active(sp) == SSRC_A);

    /* cancel/reset/destroy accept NULL. */
    pjmedia_rtp_splice_cancel(NULL);
    pjmedia_rtp_splice_reset(NULL);

    pjmedia_rtp_splice_destroy(sp);
    pjmedia_rtp_splice_destroy(NULL);
    return 0;
}

static int ho_replace_arm_fail(void)
{
    pjmedia_rtp_splice *sp;
    pjmedia_rtp_splice_cfg c = cfg_default();
    void *out = NULL;
    size_t n = 0;
    unsigned k;

    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    push_old_range(sp, 6);

    TASK_CHECK(pjmedia_rtp_splice_arm(sp, SSRC_B, 2) == 0);
    TASK_CHECK(push_seq(sp, 9000, 700000, SSRC_B, 0, "B") == 0);

    /* Replace B/cut2 with C/cut4. */
    TASK_CHECK(pjmedia_rtp_splice_arm(sp, SSRC_C, 4) == 0);
    TASK_CHECK(push_seq(sp, 9000, 700000, SSRC_B, 0, "B") == ENOENT);

    /* Invalid arms return EINVAL. */
    TASK_CHECK(pjmedia_rtp_splice_arm(sp, SSRC_A, 4) == EINVAL); /* active */
    TASK_CHECK(pjmedia_rtp_splice_arm(sp, SSRC_B, 100) == EINVAL); /* far */

    /* Consume slot0, then cut < head is invalid and must preserve C. */
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 40000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(0), out_ts_k(0), SSRC_OUT, "a0"));
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_arm(sp, SSRC_B, 0) == EINVAL);

    /* C proofs for slots 4,5,6 (first defines cut coordinates). */
    TASK_CHECK(push_seq(sp, 5000, 800000, SSRC_C, 40000, "c4") == 0);
    TASK_CHECK(push_seq(sp, 5001, 800160, SSRC_C, 40000, "c5") == 0);
    TASK_CHECK(push_seq(sp, 5002, 800320, SSRC_C, 40000, "c6") == 0);

    for (k = 1; k <= 3; ++k) {
        char pl[8];
        snprintf(pl, sizeof(pl), "a%u", k);
        TASK_CHECK(pjmedia_rtp_splice_pop(sp, 40000 + k * IV_US,
                                         &out, &n) == 0);
        TASK_CHECK(hdr_ok(out, n, out_seq_k(k), out_ts_k(k), SSRC_OUT,
                          pl));
        free(out);
    }

    /* k=4: preserved C arm is ready and wins. */
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 120000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(4), out_ts_k(4), SSRC_OUT, "c4"));
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_active(sp) == SSRC_C);

    pjmedia_rtp_splice_destroy(sp);
    return 0;
}

static int ho_two_handovers(void)
{
    pjmedia_rtp_splice *sp;
    pjmedia_rtp_splice_cfg c = cfg_default();
    void *out = NULL;
    size_t n = 0;

    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    push_old_range(sp, 7);

    /* First splice A -> B at cut 2. */
    TASK_CHECK(pjmedia_rtp_splice_arm(sp, SSRC_B, 2) == 0);
    TASK_CHECK(push_seq(sp, 9000, 700000, SSRC_B, 0, "b2") == 0);
    TASK_CHECK(push_seq(sp, 9001, 700160, SSRC_B, 0, "b3") == 0);
    TASK_CHECK(push_seq(sp, 9002, 700320, SSRC_B, 0, "b4") == 0);

    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 40000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(0), out_ts_k(0), SSRC_OUT, "a0"));
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 60000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(1), out_ts_k(1), SSRC_OUT, "a1"));
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 80000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(2), out_ts_k(2), SSRC_OUT, "b2"));
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_active(sp) == SSRC_B);

    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 100000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(3), out_ts_k(3), SSRC_OUT, "b3"));
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 120000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(4), out_ts_k(4), SSRC_OUT, "b4"));
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_head(sp) == 5);

    /* Second splice B -> C at cut 5. */
    TASK_CHECK(pjmedia_rtp_splice_arm(sp, SSRC_C, 5) == 0);
    TASK_CHECK(push_seq(sp, 5000, 800000, SSRC_C, 120000, "c5") == 0);
    TASK_CHECK(push_seq(sp, 5001, 800160, SSRC_C, 120000, "c6") == 0);
    TASK_CHECK(push_seq(sp, 5002, 800320, SSRC_C, 120000, "c7") == 0);

    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 140000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(5), out_ts_k(5), SSRC_OUT, "c5"));
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_active(sp) == SSRC_C);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 160000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(6), out_ts_k(6), SSRC_OUT, "c6"));
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 180000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(7), out_ts_k(7), SSRC_OUT, "c7"));
    free(out);

    /* Output sequence/timestamp continuity across both splices. */
    TASK_CHECK(pjmedia_rtp_splice_head(sp) == 8);

    pjmedia_rtp_splice_destroy(sp);
    return 0;
}

static int ho_switch_back(void)
{
    pjmedia_rtp_splice *sp;
    pjmedia_rtp_splice_cfg c = cfg_default();
    void *out = NULL;
    size_t n = 0;

    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    push_old_range(sp, 6);
    TASK_CHECK(pjmedia_rtp_splice_arm(sp, SSRC_B, 2) == 0);
    TASK_CHECK(push_seq(sp, 9000, 700000, SSRC_B, 0, "b2") == 0);
    TASK_CHECK(push_seq(sp, 9001, 700160, SSRC_B, 0, "b3") == 0);
    TASK_CHECK(push_seq(sp, 9002, 700320, SSRC_B, 0, "b4") == 0);

    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 40000, &out, &n) == 0);
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 60000, &out, &n) == 0);
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 80000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(2), out_ts_k(2), SSRC_OUT, "b2"));
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_active(sp) == SSRC_B);
    TASK_CHECK(pjmedia_rtp_splice_head(sp) == 3);

    /* The switched-away SSRC is rejected until re-armed. */
    TASK_CHECK(push_seq(sp, 3000, 400000, SSRC_A, 80000, "aX")
               == ENOENT);
    TASK_CHECK(pjmedia_rtp_splice_arm(sp, SSRC_A, 4) == 0);

    /* New candidate coordinates are arbitrary and anchored at cut 4. */
    TASK_CHECK(push_seq(sp, 2000, 810000, SSRC_A, 80000, "A4") == 0);
    TASK_CHECK(push_seq(sp, 2001, 810160, SSRC_A, 80000, "A5") == 0);
    TASK_CHECK(push_seq(sp, 2002, 810320, SSRC_A, 80000, "A6") == 0);

    /* k=3 still plays B. */
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 100000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(3), out_ts_k(3), SSRC_OUT, "b3"));
    free(out);

    /* k=4 switches back to the re-armed A. */
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 120000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(4), out_ts_k(4), SSRC_OUT, "A4"));
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_active(sp) == SSRC_A);

    pjmedia_rtp_splice_destroy(sp);
    return 0;
}

static int ho_candidate_deadline(void)
{
    pjmedia_rtp_splice *sp;
    pjmedia_rtp_splice_cfg c = cfg_default();
    void *out = NULL;
    size_t n = 0;

    /* Late first candidate proof. */
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    TASK_CHECK(push_seq(sp, 1000, 500000, SSRC_A, 0, "a0") == 0);
    TASK_CHECK(pjmedia_rtp_splice_arm(sp, SSRC_B, 0) == 0);
    TASK_CHECK(push_seq(sp, 9000, 700000, SSRC_B, 40001, "b0")
               == ETIMEDOUT);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 40001, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(0), out_ts_k(0), SSRC_OUT, "a0"));
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_active(sp) == SSRC_A);
    pjmedia_rtp_splice_destroy(sp);

    /* Exactly on the cut deadline all three proofs are accepted. */
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    TASK_CHECK(push_seq(sp, 1000, 500000, SSRC_A, 0, "a0") == 0);
    TASK_CHECK(pjmedia_rtp_splice_arm(sp, SSRC_B, 0) == 0);
    TASK_CHECK(push_seq(sp, 9000, 700000, SSRC_B, 40000, "b0") == 0);
    TASK_CHECK(push_seq(sp, 9001, 700160, SSRC_B, 40000, "b1") == 0);
    TASK_CHECK(push_seq(sp, 9002, 700320, SSRC_B, 40000, "b2") == 0);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 40000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(0), out_ts_k(0), SSRC_OUT, "b0"));
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_active(sp) == SSRC_B);
    pjmedia_rtp_splice_destroy(sp);

    /* Arm overflow / impossible cuts are EINVAL. */
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    TASK_CHECK(pjmedia_rtp_splice_arm(sp, SSRC_B, 0) == EINVAL); /* no anchor */
    TASK_CHECK(push_seq(sp, 1000, 500000, SSRC_A, 0, "a0") == 0);
    TASK_CHECK(pjmedia_rtp_splice_arm(sp, SSRC_B, UINT64_MAX) == EINVAL);
    /* window 16: cut+2 < 16 -> valid cuts are 0..13. */
    TASK_CHECK(pjmedia_rtp_splice_arm(sp, SSRC_B, 14) == EINVAL);
    TASK_CHECK(pjmedia_rtp_splice_arm(sp, SSRC_B, 13) == 0);
    pjmedia_rtp_splice_destroy(sp);
    return 0;
}


/* ================================================================== */
/* ownership group                                                     */
/* ================================================================== */

static int own_input_copied(void)
{
    pjmedia_rtp_splice *sp;
    pjmedia_rtp_splice_cfg c = cfg_default();
    uint8_t *heap;
    size_t n;
    void *out = NULL;
    size_t on = 0;
    const char *word = "ownership-payload";

    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);

    heap = (uint8_t *)malloc(64);
    TASK_CHECK(heap != NULL);
    n = make_rtp(heap, 64, 4242, 777, SSRC_A, PT_X, word);
    TASK_CHECK(n == 12 + strlen(word));
    TASK_CHECK(pjmedia_rtp_splice_push(sp, heap, n, 1000) == 0);

    /* The caller may mutate and free its bytes immediately. */
    memset(heap, 0xab, 64);
    free(heap);

    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 41000, &out, &on) == 0);
    TASK_CHECK(hdr_ok(out, on, out_seq_k(0), out_ts_k(0), SSRC_OUT, word));
    free(out);

    /* Stack buffer mutation after push must likewise be isolated. */
    {
        uint8_t st[128];
        size_t sn = make_rtp(st, sizeof(st), 4243, 937, SSRC_A, PT_X,
                             "stack-word");
        TASK_CHECK(pjmedia_rtp_splice_push(sp, st, sn, 1000) == 0);
        memset(st, 0, sizeof(st));
        TASK_CHECK(pjmedia_rtp_splice_pop(sp, 61000, &out, &on) == 0);
        TASK_CHECK(hdr_ok(out, on, out_seq_k(1), out_ts_k(1), SSRC_OUT,
                          "stack-word"));
        free(out);
    }

    pjmedia_rtp_splice_destroy(sp);
    return 0;
}

static int own_output_malloc_widths(void)
{
    pjmedia_rtp_splice *sp;
    pjmedia_rtp_splice_cfg c = cfg_default();
    void *p0 = NULL, *p1 = NULL, *p2 = NULL, *p3 = NULL;
    size_t l0 = 0;
    unsigned int lu = 0;
    int li = 0;
    unsigned short ls = 0;

    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    TASK_CHECK(push_seq(sp, 1, 100, SSRC_A, 0, "0") == 0);
    TASK_CHECK(push_seq(sp, 2, 260, SSRC_A, 0, "1") == 0);
    TASK_CHECK(push_seq(sp, 3, 420, SSRC_A, 0, "22") == 0);
    TASK_CHECK(push_seq(sp, 4, 580, SSRC_A, 0, "333") == 0);

    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 40000, &p0, &l0) == 0);
    TASK_CHECK(l0 == 13);
    TASK_CHECK(p0 != NULL);

    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 60000, &p1, &lu) == 0);
    TASK_CHECK(lu == 13);
    TASK_CHECK(p1 != NULL);

    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 80000, &p2, &li) == 0);
    TASK_CHECK(li == 14);
    TASK_CHECK(p2 != NULL);

    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 100000, &p3, &ls) == 0);
    TASK_CHECK(ls == 15);
    TASK_CHECK(p3 != NULL);

    /* Independent allocations. */
    TASK_CHECK(p0 != p1 && p1 != p2 && p2 != p3);

    free(p0);
    free(p1);
    free(p2);
    free(p3);
    pjmedia_rtp_splice_destroy(sp);
    return 0;
}

static int own_pop_pointer_types(void)
{
    pjmedia_rtp_splice *sp;
    pjmedia_rtp_splice_cfg c = cfg_default();
    unsigned char *ub = NULL;
    char *cb = NULL;
    size_t ul = 0;
    long ll = 0;

    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    TASK_CHECK(push_seq(sp, 1, 100, SSRC_A, 0, "u") == 0);
    TASK_CHECK(push_seq(sp, 2, 260, SSRC_A, 0, "c") == 0);

    /* unsigned char ** output + size_t length. */
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 40000, &ub, &ul) == 0);
    TASK_CHECK(ul == 13 && ub != NULL);
    TASK_CHECK(hdr_ok(ub, ul, out_seq_k(0), out_ts_k(0), SSRC_OUT, "u"));
    free(ub);

    /* char ** output + long length exercises both casts/dispatch. */
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 60000, &cb, &ll) == 0);
    TASK_CHECK(ll == 13 && cb != NULL);
    TASK_CHECK(hdr_ok(cb, (size_t)ll, out_seq_k(1), out_ts_k(1),
                      SSRC_OUT, "c"));
    free(cb);

    pjmedia_rtp_splice_destroy(sp);
    return 0;
}

static int own_first_content_wins(void)
{
    pjmedia_rtp_splice *sp;
    pjmedia_rtp_splice_cfg c = cfg_default();
    void *out = NULL;
    size_t n = 0;

    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    TASK_CHECK(push_seq(sp, 80, 900, SSRC_A, 0, "FIRST") == 0);
    TASK_CHECK(push_seq(sp, 80, 900, SSRC_A, 0, "SECOND-LONG") == 0);

    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 40000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, out_seq_k(0), out_ts_k(0), SSRC_OUT,
                      "FIRST"));
    free(out);

    pjmedia_rtp_splice_destroy(sp);
    return 0;
}

static int own_reset(void)
{
    pjmedia_rtp_splice *sp;
    pjmedia_rtp_splice_cfg c = cfg_default();
    void *out = NULL;
    size_t n = 0;

    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    TASK_CHECK(push_seq(sp, 1000, 100000, SSRC_A, 0, "old") == 0);
    TASK_CHECK(pjmedia_rtp_splice_arm(sp, SSRC_B, 2) == 0);
    TASK_CHECK(push_seq(sp, 9000, 700000, SSRC_B, 0, "b2") == 0);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 40000, &out, &n) == 0);
    free(out);
    TASK_CHECK(pjmedia_rtp_splice_head(sp) == 1);

    pjmedia_rtp_splice_reset(sp);
    TASK_CHECK(pjmedia_rtp_splice_head(sp) == 0);
    TASK_CHECK(pjmedia_rtp_splice_active(sp) == SSRC_A);

    /* Candidate state is gone. */
    TASK_CHECK(push_seq(sp, 9000, 700000, SSRC_B, 0, "b2") == ENOENT);
    TASK_CHECK(pjmedia_rtp_splice_arm(sp, SSRC_B, 0) == EINVAL);

    /* Re-anchor with fresh coordinates; output origin is restored. */
    TASK_CHECK(push_seq(sp, 4242, 777, SSRC_A, 5000, "new0") == 0);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 45000, &out, &n) == 0);
    TASK_CHECK(hdr_ok(out, n, 100, 2000, SSRC_OUT, "new0"));
    free(out);

    pjmedia_rtp_splice_destroy(sp);

    TASK_CHECK(pjmedia_rtp_splice_head(NULL) == 0);
    TASK_CHECK(pjmedia_rtp_splice_active(NULL) == 0);
    return 0;
}

static int own_preserve_options(void)
{
    pjmedia_rtp_splice *sp;
    pjmedia_rtp_splice_cfg c = cfg_default();
    uint8_t in[40];
    void *out = NULL;
    size_t n = 0;
    size_t len = 32;
    unsigned i;

    memset(in, 0, sizeof(in));
    /* V=2, X=1, CC=1, PT. */
    in[0] = 0x91;
    in[1] = PT_X;
    in[2] = 0x12; in[3] = 0x34;       /* seq 0x1234 */
    in[4] = 0xaa; in[5] = 0xbb; in[6] = 0xcc; in[7] = 0xdd;
    in[8] = 0x11; in[9] = 0x11; in[10] = 0x11; in[11] = 0x11;
    in[12] = 0xde; in[13] = 0xad; in[14] = 0xbe; in[15] = 0xef; /* CSRC */
    in[16] = 0xbe; in[17] = 0xef;   /* ext profile */
    in[18] = 0x00; in[19] = 0x01;   /* ext length = 1 word */
    in[20] = 0xca; in[21] = 0xfe; in[22] = 0xba; in[23] = 0xbe;
    memcpy(in + 24, "PAYLOAD8", 8);

    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    TASK_CHECK(pjmedia_rtp_splice_push(sp, in, len, 0) == 0);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, 40000, &out, &n) == 0);
    TASK_CHECK(n == len);

    /* Everything except seq/ts/ssrc bytes survives verbatim. */
    for (i = 0; i < len; ++i) {
        if ((i >= 2 && i <= 11))
            continue;
        TASK_CHECK(((uint8_t *)out)[i] == in[i]);
    }
    TASK_CHECK(hdr_ok(out, n, 100, 2000, SSRC_OUT, NULL));
    TASK_CHECK(memcmp((uint8_t *)out + 12, in + 12, len - 12) == 0);
    free(out);

    pjmedia_rtp_splice_destroy(sp);
    return 0;
}

/* Ordinary production RTP header encode/decode round trip stays intact. */
static int own_rtp_roundtrip(void)
{
    pjmedia_rtp_session s;
    pjmedia_rtp_session_setting cfg;
    const void *encoded = NULL;
    const void *payload = NULL;
    const pjmedia_rtp_hdr *hdr = NULL;
    int hlen = 0;
    unsigned plen = 0;
    uint8_t raw[16];
    unsigned n;

    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = 1 | 2 | 4 | 8;
    cfg.default_pt = 111;
    cfg.sender_ssrc = 0x12345678;
    cfg.seq = 65534;
    cfg.ts = UINT32_C(0xfffffc40);

    TASK_CHECK(pjmedia_rtp_session_init2(&s, cfg) == 0);

    for (n = 0; n < 3; ++n) {
        TASK_CHECK(pjmedia_rtp_encode_rtp(&s, 111, n == 2, 4, 960,
                                          &encoded, &hlen) == 0);
        TASK_CHECK(hlen == 12);
        memcpy(raw, encoded, 12);
        raw[12] = (uint8_t)('A' + n);
        raw[13] = 0x42;
        raw[14] = 0x43;
        raw[15] = 0x44;

        TASK_CHECK(pjmedia_rtp_decode_rtp(&s, raw, sizeof(raw), &hdr,
                                          &payload, &plen) == 0);
        TASK_CHECK(plen == 4);
        TASK_CHECK(((const uint8_t *)payload)[0] == (uint8_t)('A' + n));
        TASK_CHECK(pj_ntohs(hdr->seq) == (uint16_t)(65535u + n));
        TASK_CHECK(pj_ntohl(hdr->ts) ==
                   (uint32_t)(UINT32_C(0xfffffc40) + 960u * (n + 1)));
        TASK_CHECK(pj_ntohl(hdr->ssrc) == 0x12345678u);
        TASK_CHECK(hdr->pt == 111);
        TASK_CHECK(hdr->m == (n == 2));
    }
    return 0;
}


/* ------------------------------------------------------------------ */
/* Registration (consumed by task_support/driver.c)                    */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    { "timing", "config_validation",  t_config_validation },
    { "timing", "start_boundary",     t_start_boundary },
    { "timing", "deadline_edges",     t_deadline_edges },
    { "timing", "old_far_slots",      t_old_far_slots },
    { "timing", "lost_slot_seq",      t_lost_slot_seq },
    { "timing", "double_wrap",        t_double_wrap },
    { "timing", "malformed",          t_malformed },
    { "timing", "odd_window",         t_odd_window },

    { "handover", "basic",                ho_basic },
    { "handover", "reorder_dup",          ho_reorder_dup },
    { "handover", "insufficient_fallback", ho_insufficient_fallback },
    { "handover", "cut_endata",           ho_cut_endata },
    { "handover", "cancel",               ho_cancel },
    { "handover", "replace_arm_fail",     ho_replace_arm_fail },
    { "handover", "two_handovers",        ho_two_handovers },
    { "handover", "switch_back",          ho_switch_back },
    { "handover", "candidate_deadline",   ho_candidate_deadline },

    { "ownership", "input_copied",          own_input_copied },
    { "ownership", "output_malloc_widths",  own_output_malloc_widths },
    { "ownership", "pop_pointer_types",     own_pop_pointer_types },
    { "ownership", "first_content_wins",    own_first_content_wins },
    { "ownership", "reset",                 own_reset },
    { "ownership", "preserve_options",      own_preserve_options },
    { "ownership", "rtp_roundtrip",         own_rtp_roundtrip },
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
