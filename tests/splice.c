/*
 * Regression tests for the timed RTP source splice object.
 */
#include <pjmedia/rtp.h>
#include <pj/sock.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../task_support/check.h"


#define IV          ((pj_uint64_t)20000)   /* 160 ticks at 8 kHz */
#define DELAY       ((pj_uint64_t)100000)
#define A0          ((pj_uint64_t)1000000)

#define INIT_SSRC   ((pj_uint32_t)0x11111111u)
#define OUT_SSRC    ((pj_uint32_t)0x99999999u)
#define B_SSRC      ((pj_uint32_t)0x22222222u)
#define C_SSRC      ((pj_uint32_t)0x33333333u)
#define D_SSRC      ((pj_uint32_t)0x44444444u)

#define PT_DEFAULT  96u
#define OUT_SEQ     ((pj_uint16_t)0xFFFE)
#define OUT_TS      ((pj_uint32_t)0xFFFFF000u)

#define BASE_SEQ    ((pj_uint16_t)65530)
#define BASE_TS     ((pj_uint32_t)0x00001000u)

#define B_SEQ       ((pj_uint16_t)40000)
#define B_TS        ((pj_uint32_t)0x00002000u)
#define C_SEQ       ((pj_uint16_t)50000)
#define C_TS        ((pj_uint32_t)0x00004000u)

#define WINDOW      8u


typedef struct fixture
{
    pjmedia_rtp_splice *sp;
} fixture;


static pj_uint64_t dl(pj_uint64_t anchor, unsigned slot)
{
    return anchor + DELAY + IV * slot;
}

static pj_uint16_t out_seq_at(unsigned slot)
{
    return (pj_uint16_t)(OUT_SEQ + (pj_uint16_t)slot);
}

static pj_uint32_t out_ts_at(unsigned slot)
{
    return (pj_uint32_t)(OUT_TS + (pj_uint32_t)(160u * slot));
}

static pj_uint16_t be16(const void *p)
{
    const unsigned char *b = (const unsigned char*)p;
    return (pj_uint16_t)(((pj_uint16_t)b[0] << 8) | b[1]);
}

static pj_uint32_t be32(const void *p)
{
    const unsigned char *b = (const unsigned char*)p;
    return ((pj_uint32_t)b[0] << 24) | ((pj_uint32_t)b[1] << 16) |
           ((pj_uint32_t)b[2] << 8) | (pj_uint32_t)b[3];
}

static pj_size_t mkpkt(unsigned char *b, pj_uint32_t ssrc, pj_uint16_t seq,
                       pj_uint32_t ts, unsigned pt, unsigned marker,
                       const char *payload)
{
    pj_size_t plen = payload ? strlen(payload) : 0;

    b[0] = 0x80;
    b[1] = (unsigned char)((marker ? 0x80 : 0) | (pt & 0x7f));
    b[2] = (unsigned char)(seq >> 8);
    b[3] = (unsigned char)seq;
    b[4] = (unsigned char)(ts >> 24);
    b[5] = (unsigned char)(ts >> 16);
    b[6] = (unsigned char)(ts >> 8);
    b[7] = (unsigned char)ts;
    b[8] = (unsigned char)(ssrc >> 24);
    b[9] = (unsigned char)(ssrc >> 16);
    b[10] = (unsigned char)(ssrc >> 8);
    b[11] = (unsigned char)ssrc;
    if (plen != 0)
        memcpy(b + 12, payload, plen);
    return 12 + plen;
}

static pj_size_t mkraw(unsigned char *b, pj_uint32_t ssrc, pj_uint16_t seq,
                       pj_uint32_t ts, unsigned pt, unsigned marker,
                       const void *payload, pj_size_t plen)
{
    mkpkt(b, ssrc, seq, ts, pt, marker, NULL);
    if (plen != 0)
        memcpy(b + 12, payload, plen);
    return 12 + plen;
}

static pjmedia_rtp_splice_cfg base_cfg(void)
{
    pjmedia_rtp_splice_cfg c;

    c.init_ssrc = INIT_SSRC;
    c.out_ssrc = OUT_SSRC;
    c.pt = PT_DEFAULT;
    c.clock_rate = 8000;
    c.frame_ticks = 160;
    c.delay_us = (pj_int64_t)DELAY;
    c.window = WINDOW;
    c.out_seq = OUT_SEQ;
    c.out_ts = OUT_TS;
    return c;
}

static int fx_init(fixture *f)
{
    pjmedia_rtp_splice_cfg c = base_cfg();

    f->sp = NULL;
    return pjmedia_rtp_splice_alloc(&f->sp, &c);
}

static int pushp(pjmedia_rtp_splice *sp, pj_uint32_t ssrc, pj_uint16_t seq,
                 pj_uint32_t ts, pj_uint64_t now, const char *payload)
{
    unsigned char b[128];
    pj_size_t n = mkpkt(b, ssrc, seq, ts, PT_DEFAULT, 0, payload);

    return pjmedia_rtp_splice_push(sp, b, n, now);
}

/* Push slot 'slot' of a source whose coordinate has 'seq_base'/'ts_base'
 * at logical slot 'base_slot'.
 */
static int push_at(pjmedia_rtp_splice *sp, const char *prefix,
                   pj_uint32_t ssrc, unsigned base_slot,
                   pj_uint16_t seq_base, pj_uint32_t ts_base,
                   unsigned slot, pj_uint64_t now)
{
    unsigned char b[128];
    char tag[32];
    pj_uint16_t seq;
    pj_uint32_t ts;
    pj_size_t n;

    seq = (pj_uint16_t)(seq_base + (pj_uint16_t)(slot - base_slot));
    ts = ts_base + (pj_uint32_t)(160u * (slot - base_slot));
    snprintf(tag, sizeof(tag), "%s%u", prefix, slot);
    n = mkpkt(b, ssrc, seq, ts, PT_DEFAULT, 0, tag);
    return pjmedia_rtp_splice_push(sp, b, n, now);
}

static int push_a(pjmedia_rtp_splice *sp, unsigned slot, pj_uint64_t now)
{
    return push_at(sp, "a", INIT_SSRC, 0, BASE_SEQ, BASE_TS, slot, now);
}

static int push_b(pjmedia_rtp_splice *sp, unsigned cut, unsigned slot,
                  pj_uint64_t now)
{
    return push_at(sp, "b", B_SSRC, cut, B_SEQ, B_TS, slot, now);
}

static int push_c(pjmedia_rtp_splice *sp, unsigned cut, unsigned slot,
                  pj_uint64_t now)
{
    return push_at(sp, "c", C_SSRC, cut, C_SEQ, C_TS, slot, now);
}

/* Check a normal output packet against the configured output identity. */
static int check_out(const void *pkt, pj_size_t len, unsigned slot,
                     const char *payload)
{
    const unsigned char *b = (const unsigned char*)pkt;
    pj_size_t plen = strlen(payload);

    TASK_CHECK(pkt != NULL);
    TASK_CHECK(len == 12 + plen);
    TASK_CHECK(b[0] == 0x80);
    TASK_CHECK(b[1] == PT_DEFAULT);
    TASK_CHECK(be16(b + 2) == out_seq_at(slot));
    TASK_CHECK(be32(b + 4) == out_ts_at(slot));
    TASK_CHECK(be32(b + 8) == OUT_SSRC);
    TASK_CHECK(memcmp(b + 12, payload, plen) == 0);
    return 0;
}

/* Pop a slot that is due and must contain a packet. */
static int pop_ok(pjmedia_rtp_splice *sp, pj_uint64_t now, unsigned slot,
                  const char *payload)
{
    void *pkt = NULL;
    pj_size_t len = 0;
    int rc;

    rc = pjmedia_rtp_splice_pop(sp, now, &pkt, &len);
    if (rc != 0)
        return 1;
    if (check_out(pkt, len, slot, payload) != 0) {
        free(pkt);
        return 1;
    }
    free(pkt);
    return 0;
}

/* Pop a due slot that must be empty; output pointers must stay untouched. */
static int pop_empty(pjmedia_rtp_splice *sp, pj_uint64_t now)
{
    int dummy = 0;
    void *sentinel = &dummy;
    void *pkt = sentinel;
    pj_size_t len = 12345;
    int rc;

    rc = pjmedia_rtp_splice_pop(sp, now, &pkt, &len);
    TASK_CHECK(rc == ENODATA);
    TASK_CHECK(pkt == sentinel);
    TASK_CHECK(len == 12345);
    return 0;
}


/* ------------------------------------------------------------------ */
/* timing                                                             */
/* ------------------------------------------------------------------ */

static int case_config_validation(void)
{
    pjmedia_rtp_splice *sp = NULL;
    pjmedia_rtp_splice_cfg c;

    c = base_cfg();
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    pjmedia_rtp_splice_destroy(sp);

    TASK_CHECK(pjmedia_rtp_splice_alloc(NULL, &c) == EINVAL);
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, NULL) == EINVAL);

    c = base_cfg(); c.pt = 128;
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == EINVAL);
    c.pt = 0;
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    pjmedia_rtp_splice_destroy(sp);

    c = base_cfg(); c.clock_rate = 0;
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == EINVAL);

    c = base_cfg(); c.frame_ticks = 0;
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == EINVAL);

    c = base_cfg(); c.clock_rate = 44100; c.frame_ticks = 1;
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == EINVAL);

    c = base_cfg(); c.frame_ticks = 1;   /* 8000 Hz: 125 us interval */
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    pjmedia_rtp_splice_destroy(sp);

    c = base_cfg(); c.delay_us = -1;
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == EINVAL);

    c = base_cfg(); c.window = 3;
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == EINVAL);
    c.window = 4;
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);
    pjmedia_rtp_splice_destroy(sp);
    c.window = 513;
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == EINVAL);

    return 0;
}

static int case_anchor_and_deadline(void)
{
    fixture f;
    void *pkt = NULL;
    pj_size_t len = 0;

    TASK_CHECK(fx_init(&f) == 0);

    /* No stream yet. */
    TASK_CHECK(pjmedia_rtp_splice_head(f.sp) == 0);
    TASK_CHECK(pjmedia_rtp_splice_active(f.sp) == INIT_SSRC);
    TASK_CHECK(pjmedia_rtp_splice_pop(f.sp, A0, &pkt, &len) == EAGAIN);
    TASK_CHECK(pkt == NULL && len == 0);

    /* First initial-source packet anchors slot zero. */
    TASK_CHECK(push_a(f.sp, 0, A0) == 0);
    TASK_CHECK(pjmedia_rtp_splice_head(f.sp) == 0);

    TASK_CHECK(pjmedia_rtp_splice_pop(f.sp, A0, &pkt, &len) == EAGAIN);
    TASK_CHECK(pjmedia_rtp_splice_pop(f.sp, A0 + 99999, &pkt, &len) == EAGAIN);
    TASK_CHECK(pjmedia_rtp_splice_head(f.sp) == 0);

    TASK_CHECK(pjmedia_rtp_splice_pop(f.sp, A0 + 100000, &pkt, &len) == 0);
    TASK_CHECK(check_out(pkt, len, 0, "a0") == 0);
    free(pkt);
    TASK_CHECK(pjmedia_rtp_splice_head(f.sp) == 1);

    /* Slot 1 has its own deadline. */
    TASK_CHECK(push_a(f.sp, 1, A0 + 100000) == 0);
    TASK_CHECK(pjmedia_rtp_splice_pop(f.sp, dl(A0, 1) - 1, &pkt, &len)
                == EAGAIN);
    TASK_CHECK(pjmedia_rtp_splice_pop(f.sp, dl(A0, 1), &pkt, &len) == 0);
    TASK_CHECK(check_out(pkt, len, 1, "a1") == 0);
    free(pkt);
    TASK_CHECK(pjmedia_rtp_splice_head(f.sp) == 2);

    TASK_CHECK(push_a(f.sp, 2, dl(A0, 1)) == 0);
    TASK_CHECK(pjmedia_rtp_splice_pop(f.sp, dl(A0, 2), &pkt, &len) == 0);
    TASK_CHECK(check_out(pkt, len, 2, "a2") == 0);
    free(pkt);
    TASK_CHECK(pjmedia_rtp_splice_head(f.sp) == 3);
    TASK_CHECK(pjmedia_rtp_splice_active(f.sp) == INIT_SSRC);

    pjmedia_rtp_splice_destroy(f.sp);
    return 0;
}

static int case_slot_bounds(void)
{
    fixture f;
    void *pkt = NULL;
    pj_size_t len = 0;

    TASK_CHECK(fx_init(&f) == 0);
    TASK_CHECK(push_a(f.sp, 0, A0) == 0);

    /* Too old and too far. */
    TASK_CHECK(pushp(f.sp, INIT_SSRC, (pj_uint16_t)(BASE_SEQ - 1),
                     BASE_TS - 160u, A0, "old") == ENOENT);
    TASK_CHECK(push_a(f.sp, 8, A0) == ENOBUFS);
    TASK_CHECK(push_a(f.sp, 7, A0) == 0);

    /* First content wins for the same source and slot. */
    TASK_CHECK(pushp(f.sp, INIT_SSRC, (pj_uint16_t)(BASE_SEQ + 1),
                     BASE_TS + 160u, A0, "dup") == 0);
    TASK_CHECK(push_a(f.sp, 1, A0) == 0);

    TASK_CHECK(pjmedia_rtp_splice_head(f.sp) == 0);

    TASK_CHECK(pjmedia_rtp_splice_pop(f.sp, dl(A0, 0), &pkt, &len) == 0);
    TASK_CHECK(check_out(pkt, len, 0, "a0") == 0);
    free(pkt);
    TASK_CHECK(pjmedia_rtp_splice_head(f.sp) == 1);

    /* Slot behind head is too old. */
    TASK_CHECK(pushp(f.sp, INIT_SSRC, BASE_SEQ, BASE_TS,
                     dl(A0, 0), "old0") == ENOENT);
    TASK_CHECK(pjmedia_rtp_splice_head(f.sp) == 1);
    TASK_CHECK(pjmedia_rtp_splice_active(f.sp) == INIT_SSRC);

    TASK_CHECK(pjmedia_rtp_splice_pop(f.sp, dl(A0, 1), &pkt, &len) == 0);
    TASK_CHECK(check_out(pkt, len, 1, "dup") == 0);
    free(pkt);

    /* Strictly after the deadline is rejected; exactly at it is accepted. */
    TASK_CHECK(pushp(f.sp, INIT_SSRC, (pj_uint16_t)(BASE_SEQ + 2),
                     BASE_TS + 320u, dl(A0, 2) + 1, "late") == ETIMEDOUT);
    TASK_CHECK(push_a(f.sp, 2, dl(A0, 2)) == 0);
    TASK_CHECK(pjmedia_rtp_splice_pop(f.sp, dl(A0, 2), &pkt, &len) == 0);
    TASK_CHECK(check_out(pkt, len, 2, "a2") == 0);
    free(pkt);
    TASK_CHECK(pjmedia_rtp_splice_head(f.sp) == 3);

    /* Slot 3 was never accepted. */
    TASK_CHECK(push_a(f.sp, 3, dl(A0, 3) + 1) == ETIMEDOUT);
    TASK_CHECK(pop_empty(f.sp, dl(A0, 3) + 1) == 0);
    TASK_CHECK(pjmedia_rtp_splice_head(f.sp) == 4);

    /* Still bounded by the window after head advanced. */
    TASK_CHECK(pushp(f.sp, INIT_SSRC,
                     (pj_uint16_t)(BASE_SEQ + 4 + WINDOW),
                     BASE_TS + 160u * (4 + WINDOW),
                     dl(A0, 3) + 1, "far") == ENOBUFS);

    pjmedia_rtp_splice_destroy(f.sp);
    return 0;
}

static int case_bad_packets(void)
{
    fixture f;
    unsigned char b[128];
    const unsigned char pad_bad[] = {'x', 'y', 0x05};
    const unsigned char pad_zero[] = {'x', 0x00};
    const unsigned char pad_ok[] = {'x', 'y', 0x02};
    unsigned char ext[20];
    pj_size_t n;
    void *pkt = NULL;
    pj_size_t len = 0;

    TASK_CHECK(fx_init(&f) == 0);
    TASK_CHECK(push_a(f.sp, 0, A0) == 0);

    /* Wrong RTP version. */
    n = mkpkt(b, INIT_SSRC, BASE_SEQ + 1, BASE_TS + 160u, PT_DEFAULT, 0, "v");
    b[0] = 0x40;
    TASK_CHECK(pjmedia_rtp_splice_push(f.sp, b, n, A0) == EBADMSG);

    /* Truncated header. */
    n = mkpkt(b, INIT_SSRC, BASE_SEQ + 1, BASE_TS + 160u, PT_DEFAULT, 0, "");
    TASK_CHECK(pjmedia_rtp_splice_push(f.sp, b, 11, A0) == EBADMSG);

    /* Wrong payload type. */
    n = mkpkt(b, INIT_SSRC, BASE_SEQ + 1, BASE_TS + 160u, 97, 0, "pt");
    TASK_CHECK(pjmedia_rtp_splice_push(f.sp, b, n, A0) == EBADMSG);

    /* Timestamp does not match the slot. */
    n = mkpkt(b, INIT_SSRC, BASE_SEQ + 1, BASE_TS + 161u, PT_DEFAULT, 0, "ts");
    TASK_CHECK(pjmedia_rtp_splice_push(f.sp, b, n, A0) == EBADMSG);

    /* Padding that claims more bytes than the payload holds. */
    n = mkraw(b, INIT_SSRC, BASE_SEQ + 1, BASE_TS + 160u, PT_DEFAULT, 0,
              pad_bad, sizeof(pad_bad));
    b[0] |= 0x20;
    TASK_CHECK(pjmedia_rtp_splice_push(f.sp, b, n, A0) == EBADMSG);

    /* Zero padding length. */
    n = mkraw(b, INIT_SSRC, BASE_SEQ + 1, BASE_TS + 160u, PT_DEFAULT, 0,
              pad_zero, sizeof(pad_zero));
    b[0] |= 0x20;
    TASK_CHECK(pjmedia_rtp_splice_push(f.sp, b, n, A0) == EBADMSG);

    /* Extension header promises a word that is not present. */
    memset(ext, 0, sizeof(ext));
    n = mkpkt(b, INIT_SSRC, BASE_SEQ + 1, BASE_TS + 160u, PT_DEFAULT, 0, NULL);
    b[0] |= 0x10;
    b[12] = 0xBE; b[13] = 0xDE; b[14] = 0x00; b[15] = 0x02;
    b[16] = 0x01; b[17] = 0x02;
    TASK_CHECK(pjmedia_rtp_splice_push(f.sp, b, 18, A0) == EBADMSG);

    /* Unknown SSRC. */
    TASK_CHECK(pushp(f.sp, 0x77777777u, BASE_SEQ + 1, BASE_TS + 160u,
                     A0, "who") == ENOENT);

    /* A well-formed padded packet is accepted. */
    n = mkraw(b, INIT_SSRC, BASE_SEQ + 1, BASE_TS + 160u, PT_DEFAULT, 0,
              pad_ok, sizeof(pad_ok));
    b[0] |= 0x20;
    TASK_CHECK(pjmedia_rtp_splice_push(f.sp, b, n, A0) == 0);

    /* None of the rejections disturbed the anchor or the cache. */
    TASK_CHECK(pjmedia_rtp_splice_pop(f.sp, dl(A0, 0), &pkt, &len) == 0);
    TASK_CHECK(check_out(pkt, len, 0, "a0") == 0);
    free(pkt);

    TASK_CHECK(pjmedia_rtp_splice_pop(f.sp, dl(A0, 1), &pkt, &len) == 0);
    TASK_CHECK(len == 15);
    {
        const unsigned char *o = (const unsigned char*)pkt;
        TASK_CHECK((o[0] & 0x20) != 0);
        TASK_CHECK(be16(o + 2) == out_seq_at(1));
        TASK_CHECK(be32(o + 4) == out_ts_at(1));
        TASK_CHECK(be32(o + 8) == OUT_SSRC);
        TASK_CHECK(memcmp(o + 12, pad_ok, sizeof(pad_ok)) == 0);
    }
    free(pkt);

    pjmedia_rtp_splice_destroy(f.sp);
    return 0;
}

static int case_output_wrap(void)
{
    pjmedia_rtp_splice_cfg c = base_cfg();
    pjmedia_rtp_splice *sp = NULL;
    void *pkt = NULL;
    pj_size_t len = 0;
    unsigned k;

    c.out_seq = OUT_SEQ;
    c.out_ts = 0xFFFFFF00u;
    TASK_CHECK(pjmedia_rtp_splice_alloc(&sp, &c) == 0);

    /* Anchor with input numbers that wrap early. */
    TASK_CHECK(pushp(sp, INIT_SSRC, 65534, 0xFFFFFF00u, A0, "0") == 0);
    for (k = 1; k <= 4; ++k)
        TASK_CHECK(pushp(sp, INIT_SSRC, (pj_uint16_t)(65534 + k),
                         0xFFFFFF00u + 160u * k, A0, "x") == 0);

    {
        static const pj_uint16_t seqs[5] = {0xFFFE, 0xFFFF, 0x0000, 0x0001,
                                            0x0002};
        static const pj_uint32_t tss[5] = {0xFFFFFF00u, 0xFFFFFFA0u,
                                           0x00000040u, 0x000000E0u,
                                           0x00000180u};
        for (k = 0; k <= 4; ++k) {
            TASK_CHECK(pjmedia_rtp_splice_pop(sp, dl(A0, k), &pkt, &len) == 0);
            TASK_CHECK(be16((unsigned char*)pkt + 2) == seqs[k]);
            TASK_CHECK(be32((unsigned char*)pkt + 4) == tss[k]);
            TASK_CHECK(be32((unsigned char*)pkt + 8) == OUT_SSRC);
            free(pkt);
        }
    }
    TASK_CHECK(pjmedia_rtp_splice_head(sp) == 5);

    /* A dropped slot still consumes an output sequence number and timestamp. */
    TASK_CHECK(pushp(sp, INIT_SSRC, (pj_uint16_t)(65534 + 6),
                     0xFFFFFF00u + 160u * 6, dl(A0, 4), "6") == 0);
    TASK_CHECK(pushp(sp, INIT_SSRC, (pj_uint16_t)(65534 + 7),
                     0xFFFFFF00u + 160u * 7, dl(A0, 4), "7") == 0);

    TASK_CHECK(pop_empty(sp, dl(A0, 5)) == 0);
    TASK_CHECK(pjmedia_rtp_splice_head(sp) == 6);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, dl(A0, 6), &pkt, &len) == 0);
    TASK_CHECK(be16((unsigned char*)pkt + 2) == (pj_uint16_t)(OUT_SEQ + 6));
    TASK_CHECK(be32((unsigned char*)pkt + 4) == 0x000002C0u);
    free(pkt);
    TASK_CHECK(pjmedia_rtp_splice_pop(sp, dl(A0, 7), &pkt, &len) == 0);
    TASK_CHECK(be16((unsigned char*)pkt + 2) == (pj_uint16_t)(OUT_SEQ + 7));
    TASK_CHECK(be32((unsigned char*)pkt + 4) == 0x00000360u);
    free(pkt);
    TASK_CHECK(pjmedia_rtp_splice_head(sp) == 8);

    pjmedia_rtp_splice_destroy(sp);
    return 0;
}


/* ------------------------------------------------------------------ */
/* handover                                                           */
/* ------------------------------------------------------------------ */

static int case_arm_validation(void)
{
    fixture f;
    void *pkt = NULL;
    pj_size_t len = 0;

    TASK_CHECK(fx_init(&f) == 0);

    /* No anchor yet. */
    TASK_CHECK(pjmedia_rtp_splice_arm(f.sp, B_SSRC, 0) == EINVAL);

    TASK_CHECK(push_a(f.sp, 0, A0) == 0);
    TASK_CHECK(pjmedia_rtp_splice_arm(f.sp, INIT_SSRC, 2) == EINVAL);
    TASK_CHECK(pjmedia_rtp_splice_arm(f.sp, B_SSRC, 2) == 0);
    pjmedia_rtp_splice_cancel(f.sp);

    TASK_CHECK(pjmedia_rtp_splice_pop(f.sp, dl(A0, 0), &pkt, &len) == 0);
    free(pkt);
    TASK_CHECK(pjmedia_rtp_splice_head(f.sp) == 1);

    /* cut must not be behind the head. */
    TASK_CHECK(pjmedia_rtp_splice_arm(f.sp, B_SSRC, 0) == EINVAL);
    /* cut + 2 must stay strictly inside the window. */
    TASK_CHECK(pjmedia_rtp_splice_arm(f.sp, B_SSRC, 7) == EINVAL);
    TASK_CHECK(pjmedia_rtp_splice_arm(f.sp, B_SSRC, 6) == 0);

    pjmedia_rtp_splice_destroy(f.sp);
    return 0;
}

static int case_out_of_order_proof(void)
{
    fixture f;
    unsigned k;

    TASK_CHECK(fx_init(&f) == 0);
    for (k = 0; k <= 5; ++k)
        TASK_CHECK(push_a(f.sp, k, A0) == 0);

    TASK_CHECK(pjmedia_rtp_splice_arm(f.sp, B_SSRC, 2) == 0);

    /* First candidate packet defines the coordinate at slot 2, the rest
     * arrive out of order.  Slot 5 goes beyond the three proof slots.
     */
    TASK_CHECK(push_b(f.sp, 2, 2, A0) == 0);
    TASK_CHECK(push_b(f.sp, 2, 4, A0) == 0);
    TASK_CHECK(push_b(f.sp, 2, 3, A0) == 0);
    TASK_CHECK(push_b(f.sp, 2, 5, A0) == 0);

    TASK_CHECK(pop_ok(f.sp, dl(A0, 0), 0, "a0") == 0);
    TASK_CHECK(pjmedia_rtp_splice_active(f.sp) == INIT_SSRC);
    TASK_CHECK(pop_ok(f.sp, dl(A0, 1), 1, "a1") == 0);
    TASK_CHECK(pjmedia_rtp_splice_active(f.sp) == INIT_SSRC);

    /* At the cut the ready candidate takes over atomically. */
    TASK_CHECK(pop_ok(f.sp, dl(A0, 2), 2, "b2") == 0);
    TASK_CHECK(pjmedia_rtp_splice_active(f.sp) == B_SSRC);
    TASK_CHECK(pjmedia_rtp_splice_head(f.sp) == 3);

    TASK_CHECK(pop_ok(f.sp, dl(A0, 3), 3, "b3") == 0);
    TASK_CHECK(pop_ok(f.sp, dl(A0, 4), 4, "b4") == 0);
    TASK_CHECK(pop_ok(f.sp, dl(A0, 5), 5, "b5") == 0);

    /* The old source is no longer accepted. */
    TASK_CHECK(push_a(f.sp, 6, dl(A0, 5)) == ENOENT);

    /* The candidate coordinate now drives the active source. */
    TASK_CHECK(push_b(f.sp, 2, 6, dl(A0, 5)) == 0);
    TASK_CHECK(pop_ok(f.sp, dl(A0, 6), 6, "b6") == 0);
    TASK_CHECK(pjmedia_rtp_splice_active(f.sp) == B_SSRC);

    pjmedia_rtp_splice_destroy(f.sp);
    return 0;
}

static int case_missing_proof_abandons(void)
{
    fixture f;
    unsigned k;

    TASK_CHECK(fx_init(&f) == 0);
    for (k = 0; k <= 4; ++k)
        TASK_CHECK(push_a(f.sp, k, A0) == 0);

    TASK_CHECK(pjmedia_rtp_splice_arm(f.sp, B_SSRC, 2) == 0);
    TASK_CHECK(push_b(f.sp, 2, 2, A0) == 0);
    TASK_CHECK(push_b(f.sp, 2, 3, A0) == 0);

    TASK_CHECK(pop_ok(f.sp, dl(A0, 0), 0, "a0") == 0);
    TASK_CHECK(pop_ok(f.sp, dl(A0, 1), 1, "a1") == 0);

    /* Slot 4 is missing, so the candidate is dropped and the old source
     * keeps playing at and after the cut.
     */
    TASK_CHECK(pop_ok(f.sp, dl(A0, 2), 2, "a2") == 0);
    TASK_CHECK(pjmedia_rtp_splice_active(f.sp) == INIT_SSRC);

    /* The late packet cannot revive the abandoned candidate. */
    TASK_CHECK(push_b(f.sp, 2, 4, dl(A0, 2)) == ENOENT);
    TASK_CHECK(push_b(f.sp, 2, 2, dl(A0, 2)) == ENOENT);

    TASK_CHECK(pop_ok(f.sp, dl(A0, 3), 3, "a3") == 0);
    TASK_CHECK(pop_ok(f.sp, dl(A0, 4), 4, "a4") == 0);
    TASK_CHECK(pjmedia_rtp_splice_active(f.sp) == INIT_SSRC);

    pjmedia_rtp_splice_destroy(f.sp);
    return 0;
}

static int case_duplicate_not_proof(void)
{
    fixture f;
    unsigned k;

    TASK_CHECK(fx_init(&f) == 0);
    for (k = 0; k <= 4; ++k)
        TASK_CHECK(push_a(f.sp, k, A0) == 0);

    TASK_CHECK(pjmedia_rtp_splice_arm(f.sp, B_SSRC, 2) == 0);
    TASK_CHECK(push_b(f.sp, 2, 2, A0) == 0);
    /* Duplicate of slot 2 with different payload: accepted but ignored. */
    TASK_CHECK(pushp(f.sp, B_SSRC, (pj_uint16_t)(B_SEQ),
                     B_TS, A0, "dup2") == 0);
    TASK_CHECK(push_b(f.sp, 2, 3, A0) == 0);

    TASK_CHECK(pop_ok(f.sp, dl(A0, 0), 0, "a0") == 0);
    TASK_CHECK(pop_ok(f.sp, dl(A0, 1), 1, "a1") == 0);
    /* Two packets, one of them a duplicate, are not enough proof. */
    TASK_CHECK(pop_ok(f.sp, dl(A0, 2), 2, "a2") == 0);
    TASK_CHECK(pjmedia_rtp_splice_active(f.sp) == INIT_SSRC);
    TASK_CHECK(push_b(f.sp, 2, 4, dl(A0, 2)) == ENOENT);

    pjmedia_rtp_splice_destroy(f.sp);
    return 0;
}

static int case_cancel_replace_rollback(void)
{
    fixture f;
    unsigned k;

    TASK_CHECK(fx_init(&f) == 0);
    for (k = 0; k <= 4; ++k)
        TASK_CHECK(push_a(f.sp, k, A0) == 0);

    /* Cancel drops only the candidate. */
    TASK_CHECK(pjmedia_rtp_splice_arm(f.sp, B_SSRC, 2) == 0);
    TASK_CHECK(push_b(f.sp, 2, 2, A0) == 0);
    TASK_CHECK(push_b(f.sp, 2, 3, A0) == 0);
    pjmedia_rtp_splice_cancel(f.sp);
    TASK_CHECK(push_b(f.sp, 2, 4, A0) == ENOENT);

    /* Arming again replaces any previous candidate. */
    TASK_CHECK(pjmedia_rtp_splice_arm(f.sp, B_SSRC, 2) == 0);
    TASK_CHECK(push_b(f.sp, 2, 2, A0) == 0);
    TASK_CHECK(push_b(f.sp, 2, 3, A0) == 0);
    TASK_CHECK(pjmedia_rtp_splice_arm(f.sp, C_SSRC, 1) == 0);
    TASK_CHECK(push_b(f.sp, 2, 4, A0) == ENOENT);

    TASK_CHECK(push_c(f.sp, 1, 1, A0) == 0);
    TASK_CHECK(push_c(f.sp, 1, 2, A0) == 0);
    TASK_CHECK(push_c(f.sp, 1, 3, A0) == 0);

    /* A failing arm must leave the current candidate and its proof intact. */
    TASK_CHECK(pjmedia_rtp_splice_arm(f.sp, D_SSRC, 7) == EINVAL);
    TASK_CHECK(push_c(f.sp, 1, 4, A0) == 0);

    TASK_CHECK(pop_ok(f.sp, dl(A0, 0), 0, "a0") == 0);
    TASK_CHECK(pjmedia_rtp_splice_active(f.sp) == INIT_SSRC);
    TASK_CHECK(pop_ok(f.sp, dl(A0, 1), 1, "c1") == 0);
    TASK_CHECK(pjmedia_rtp_splice_active(f.sp) == C_SSRC);
    TASK_CHECK(pop_ok(f.sp, dl(A0, 2), 2, "c2") == 0);
    TASK_CHECK(pop_ok(f.sp, dl(A0, 3), 3, "c3") == 0);
    TASK_CHECK(pop_ok(f.sp, dl(A0, 4), 4, "c4") == 0);

    /* B was replaced and never armed again. */
    TASK_CHECK(push_b(f.sp, 2, 5, dl(A0, 4)) == ENOENT);

    pjmedia_rtp_splice_destroy(f.sp);
    return 0;
}

static int case_cut_contention(void)
{
    fixture f;
    unsigned k;

    TASK_CHECK(fx_init(&f) == 0);
    for (k = 0; k <= 6; ++k)
        TASK_CHECK(push_a(f.sp, k, A0) == 0);

    TASK_CHECK(pjmedia_rtp_splice_arm(f.sp, B_SSRC, 2) == 0);
    TASK_CHECK(push_b(f.sp, 2, 2, A0) == 0);
    TASK_CHECK(push_b(f.sp, 2, 3, A0) == 0);
    TASK_CHECK(push_b(f.sp, 2, 4, A0) == 0);

    /* Before the cut the old source still plays even though the
     * candidate is already proven.
     */
    TASK_CHECK(pop_ok(f.sp, dl(A0, 0), 0, "a0") == 0);
    TASK_CHECK(pop_ok(f.sp, dl(A0, 1), 1, "a1") == 0);
    TASK_CHECK(pjmedia_rtp_splice_active(f.sp) == INIT_SSRC);

    /* A later old-source packet is still accepted before the decision, but
     * is discarded when the switch happens.
     */
    TASK_CHECK(push_a(f.sp, 5, dl(A0, 1)) == 0);

    TASK_CHECK(pop_ok(f.sp, dl(A0, 2), 2, "b2") == 0);
    TASK_CHECK(pjmedia_rtp_splice_active(f.sp) == B_SSRC);
    TASK_CHECK(pop_ok(f.sp, dl(A0, 3), 3, "b3") == 0);
    TASK_CHECK(pop_ok(f.sp, dl(A0, 4), 4, "b4") == 0);

    /* The old cached slot 5 must be gone. */
    TASK_CHECK(push_b(f.sp, 2, 5, dl(A0, 4)) == 0);
    TASK_CHECK(pop_ok(f.sp, dl(A0, 5), 5, "b5") == 0);
    TASK_CHECK(push_a(f.sp, 7, dl(A0, 5)) == ENOENT);

    pjmedia_rtp_splice_destroy(f.sp);
    return 0;
}

static int case_double_handover(void)
{
    fixture f;
    unsigned k;

    TASK_CHECK(fx_init(&f) == 0);
    for (k = 0; k <= 6; ++k)
        TASK_CHECK(push_a(f.sp, k, A0) == 0);

    /* First continuation: A to B at slot 1. */
    TASK_CHECK(pjmedia_rtp_splice_arm(f.sp, B_SSRC, 1) == 0);
    TASK_CHECK(push_b(f.sp, 1, 1, A0) == 0);
    TASK_CHECK(push_b(f.sp, 1, 2, A0) == 0);
    TASK_CHECK(push_b(f.sp, 1, 3, A0) == 0);

    TASK_CHECK(pop_ok(f.sp, dl(A0, 0), 0, "a0") == 0);
    TASK_CHECK(pop_ok(f.sp, dl(A0, 1), 1, "b1") == 0);
    TASK_CHECK(pjmedia_rtp_splice_active(f.sp) == B_SSRC);
    TASK_CHECK(pop_ok(f.sp, dl(A0, 2), 2, "b2") == 0);
    TASK_CHECK(pjmedia_rtp_splice_head(f.sp) == 3);

    /* The cut source is unknown until armed again. */
    TASK_CHECK(push_a(f.sp, 3, dl(A0, 2)) == ENOENT);

    /* Second continuation: B to A at slot 4, using fresh coordinates. */
    TASK_CHECK(pjmedia_rtp_splice_arm(f.sp, INIT_SSRC, 4) == 0);
    TASK_CHECK(push_at(f.sp, "d", INIT_SSRC, 4, 1000, 0x8000,
                       4, dl(A0, 2)) == 0);
    TASK_CHECK(push_at(f.sp, "d", INIT_SSRC, 4, 1000, 0x8000,
                       5, dl(A0, 2)) == 0);
    TASK_CHECK(push_at(f.sp, "d", INIT_SSRC, 4, 1000, 0x8000,
                       6, dl(A0, 2)) == 0);

    TASK_CHECK(pop_ok(f.sp, dl(A0, 3), 3, "b3") == 0);
    TASK_CHECK(pop_ok(f.sp, dl(A0, 4), 4, "d4") == 0);
    TASK_CHECK(pjmedia_rtp_splice_active(f.sp) == INIT_SSRC);
    TASK_CHECK(pjmedia_rtp_splice_head(f.sp) == 5);

    /* Third continuation: A to C at slot 5. */
    TASK_CHECK(pjmedia_rtp_splice_arm(f.sp, C_SSRC, 5) == 0);
    TASK_CHECK(push_c(f.sp, 5, 5, dl(A0, 4)) == 0);
    TASK_CHECK(push_c(f.sp, 5, 6, dl(A0, 4)) == 0);
    TASK_CHECK(push_c(f.sp, 5, 7, dl(A0, 4)) == 0);

    TASK_CHECK(pop_ok(f.sp, dl(A0, 5), 5, "c5") == 0);
    TASK_CHECK(pjmedia_rtp_splice_active(f.sp) == C_SSRC);
    TASK_CHECK(pop_ok(f.sp, dl(A0, 6), 6, "c6") == 0);
    TASK_CHECK(pop_ok(f.sp, dl(A0, 7), 7, "c7") == 0);
    TASK_CHECK(pjmedia_rtp_splice_head(f.sp) == 8);

    /* Every SSRC that was cut away is unknown unless armed. */
    TASK_CHECK(push_a(f.sp, 8, dl(A0, 7)) == ENOENT);
    TASK_CHECK(push_b(f.sp, 2, 8, dl(A0, 7)) == ENOENT);

    pjmedia_rtp_splice_destroy(f.sp);
    return 0;
}


/* ------------------------------------------------------------------ */
/* ownership                                                          */
/* ------------------------------------------------------------------ */

static int case_push_copies(void)
{
    fixture f;
    unsigned char buf[128], saved[128];
    pj_size_t n;
    void *pkt = NULL;
    pj_size_t len = 0;

    TASK_CHECK(fx_init(&f) == 0);

    n = mkpkt(buf, INIT_SSRC, BASE_SEQ, BASE_TS, PT_DEFAULT, 0, "orig");
    memcpy(saved, buf, n);
    TASK_CHECK(pjmedia_rtp_splice_push(f.sp, buf, n, A0) == 0);
    TASK_CHECK(memcmp(buf, saved, n) == 0);   /* caller bytes untouched */

    memset(buf, 0xAA, n);                     /* caller reuses its buffer */
    TASK_CHECK(pjmedia_rtp_splice_pop(f.sp, dl(A0, 0), &pkt, &len) == 0);
    TASK_CHECK(len == n);
    TASK_CHECK(memcmp((unsigned char*)pkt + 12, "orig", 4) == 0);
    TASK_CHECK(((unsigned char*)pkt)[0] == saved[0]);
    free(pkt);

    /* A second slot behaves the same. */
    n = mkpkt(buf, INIT_SSRC, (pj_uint16_t)(BASE_SEQ + 1), BASE_TS + 160u,
              PT_DEFAULT, 0, "next");
    TASK_CHECK(pjmedia_rtp_splice_push(f.sp, buf, n, A0) == 0);
    memset(buf, 0, n);
    TASK_CHECK(pjmedia_rtp_splice_pop(f.sp, dl(A0, 1), &pkt, &len) == 0);
    TASK_CHECK(check_out(pkt, len, 1, "next") == 0);
    free(pkt);

    pjmedia_rtp_splice_destroy(f.sp);
    return 0;
}

static int case_pop_pointer_rules(void)
{
    fixture f;
    int dummy = 0;
    void *sentinel = &dummy;
    void *pkt;
    pj_size_t len;
    unsigned k;

    TASK_CHECK(fx_init(&f) == 0);

    /* Not started yet. */
    pkt = sentinel; len = 4242;
    TASK_CHECK(pjmedia_rtp_splice_pop(f.sp, A0, &pkt, &len) == EAGAIN);
    TASK_CHECK(pkt == sentinel && len == 4242);

    TASK_CHECK(push_a(f.sp, 0, A0) == 0);
    pkt = sentinel; len = 4242;
    TASK_CHECK(pjmedia_rtp_splice_pop(f.sp, A0, &pkt, &len) == EAGAIN);
    TASK_CHECK(pkt == sentinel && len == 4242);

    for (k = 1; k <= 4; ++k) {
        if (k == 2)
            continue;   /* slot 2 will be deliberately empty */
        TASK_CHECK(push_a(f.sp, k, A0) == 0);
    }

    TASK_CHECK(pop_ok(f.sp, dl(A0, 0), 0, "a0") == 0);
    TASK_CHECK(pop_ok(f.sp, dl(A0, 1), 1, "a1") == 0);

    /* Slot 2 is due but empty: pointers are untouched. */
    TASK_CHECK(pop_empty(f.sp, dl(A0, 2)) == 0);
    TASK_CHECK(pjmedia_rtp_splice_head(f.sp) == 3);

    /* Consecutive packets come from independent allocations. */
    {
        void *p3 = NULL, *p4 = NULL;
        pj_size_t l3 = 0, l4 = 0;

        TASK_CHECK(pjmedia_rtp_splice_pop(f.sp, dl(A0, 3), &p3, &l3) == 0);
        TASK_CHECK(pjmedia_rtp_splice_pop(f.sp, dl(A0, 4), &p4, &l4) == 0);
        TASK_CHECK(p3 != NULL && p4 != NULL && p3 != p4);
        TASK_CHECK(check_out(p3, l3, 3, "a3") == 0);
        TASK_CHECK(check_out(p4, l4, 4, "a4") == 0);
        free(p3);
        TASK_CHECK(check_out(p4, l4, 4, "a4") == 0);
        free(p4);
    }

    /* Invalid output arguments. */
    TASK_CHECK(pjmedia_rtp_splice_pop(f.sp, dl(A0, 5), NULL, &len) == EINVAL);
    TASK_CHECK(pjmedia_rtp_splice_pop(f.sp, dl(A0, 5), &pkt, NULL) == EINVAL);

    pjmedia_rtp_splice_destroy(f.sp);
    return 0;
}

static int case_output_bytes_preserved(void)
{
    fixture f;
    unsigned char pkt_in[64];
    pj_size_t n;
    void *pkt = NULL;
    pj_size_t len = 0;

    TASK_CHECK(fx_init(&f) == 0);

    /* CSRC count 1 plus a one-word header extension, marker set. */
    n = mkpkt(pkt_in, INIT_SSRC, BASE_SEQ, BASE_TS, PT_DEFAULT, 1, NULL);
    pkt_in[0] = 0x91;                       /* V=2, X=1, CC=1 */
    pkt_in[12] = 0xA1; pkt_in[13] = 0xB2;
    pkt_in[14] = 0xC3; pkt_in[15] = 0xD4;   /* CSRC */
    pkt_in[16] = 0xBE; pkt_in[17] = 0xDE;   /* extension profile */
    pkt_in[18] = 0x00; pkt_in[19] = 0x01;   /* extension length: 1 word */
    pkt_in[20] = 0x01; pkt_in[21] = 0x02;
    pkt_in[22] = 0x03; pkt_in[23] = 0x04;   /* extension data */
    memcpy(pkt_in + 24, "hello", 5);
    n = 29;

    TASK_CHECK(pjmedia_rtp_splice_push(f.sp, pkt_in, n, A0) == 0);
    TASK_CHECK(pjmedia_rtp_splice_pop(f.sp, dl(A0, 0), &pkt, &len) == 0);
    TASK_CHECK(len == n);
    {
        const unsigned char *o = (const unsigned char*)pkt;
        TASK_CHECK(o[0] == 0x91);
        TASK_CHECK(o[1] == 0xE0);           /* marker and PT unchanged */
        TASK_CHECK(be16(o + 2) == out_seq_at(0));
        TASK_CHECK(be32(o + 4) == out_ts_at(0));
        TASK_CHECK(be32(o + 8) == OUT_SSRC);
        /* CSRC, extension and payload bytes are preserved verbatim. */
        TASK_CHECK(memcmp(o + 12, pkt_in + 12, n - 12) == 0);
    }
    free(pkt);

    /* A padded packet keeps its padding bytes on output too. */
    {
        unsigned char pad[128];
        const unsigned char tail[3] = {'x', 'y', 0x02};

        n = mkraw(pad, INIT_SSRC, (pj_uint16_t)(BASE_SEQ + 1),
                  BASE_TS + 160u, PT_DEFAULT, 0, tail, sizeof(tail));
        pad[0] |= 0x20;
        TASK_CHECK(pjmedia_rtp_splice_push(f.sp, pad, n, A0) == 0);
        TASK_CHECK(pjmedia_rtp_splice_pop(f.sp, dl(A0, 1), &pkt, &len) == 0);
        TASK_CHECK(len == n);
        TASK_CHECK(memcmp((unsigned char*)pkt + 12, tail, sizeof(tail)) == 0);
        free(pkt);
    }

    pjmedia_rtp_splice_destroy(f.sp);
    return 0;
}

static int case_reset_and_destroy(void)
{
    fixture f;
    void *pkt = NULL;
    pj_size_t len = 0;
    pj_uint64_t a2 = 2000000;

    TASK_CHECK(fx_init(&f) == 0);

    TASK_CHECK(push_a(f.sp, 0, A0) == 0);
    TASK_CHECK(push_a(f.sp, 1, A0) == 0);
    TASK_CHECK(pjmedia_rtp_splice_arm(f.sp, B_SSRC, 2) == 0);
    TASK_CHECK(push_b(f.sp, 2, 2, A0) == 0);

    pjmedia_rtp_splice_reset(f.sp);

    TASK_CHECK(pjmedia_rtp_splice_head(f.sp) == 0);
    TASK_CHECK(pjmedia_rtp_splice_active(f.sp) == INIT_SSRC);
    TASK_CHECK(pjmedia_rtp_splice_pop(f.sp, a2, &pkt, &len) == EAGAIN);
    TASK_CHECK(pkt == NULL && len == 0);

    /* The old candidate is gone and no anchor exists yet. */
    TASK_CHECK(push_b(f.sp, 2, 2, a2) == ENOENT);

    /* Re-anchoring starts a fresh stream at slot zero. */
    TASK_CHECK(push_a(f.sp, 0, a2) == 0);
    TASK_CHECK(pjmedia_rtp_splice_pop(f.sp, a2 + DELAY, &pkt, &len) == 0);
    TASK_CHECK(check_out(pkt, len, 0, "a0") == 0);
    free(pkt);
    TASK_CHECK(pjmedia_rtp_splice_head(f.sp) == 1);

    /* A new candidate must be armed again. */
    TASK_CHECK(pjmedia_rtp_splice_arm(f.sp, B_SSRC, 1) == 0);
    TASK_CHECK(push_b(f.sp, 1, 1, a2 + DELAY) == 0);
    TASK_CHECK(push_b(f.sp, 1, 2, a2 + DELAY) == 0);
    TASK_CHECK(push_b(f.sp, 1, 3, a2 + DELAY) == 0);
    TASK_CHECK(pop_ok(f.sp, a2 + DELAY + IV, 1, "b1") == 0);
    TASK_CHECK(pjmedia_rtp_splice_active(f.sp) == B_SSRC);

    pjmedia_rtp_splice_destroy(f.sp);
    pjmedia_rtp_splice_destroy(NULL);
    return 0;
}

static int case_rtp_header_roundtrip(void)
{
    pjmedia_rtp_session ses;
    pjmedia_rtp_session_setting st;
    const void *encoded = NULL;
    const void *payload = NULL;
    const pjmedia_rtp_hdr *hdr = NULL;
    unsigned char raw[16];
    int hlen = 0;
    unsigned plen = 0;
    unsigned i;

    memset(&st, 0, sizeof(st));
    st.flags = 1 | 2 | 4 | 8;
    st.default_pt = 111;
    st.sender_ssrc = 0x12345678u;
    st.seq = 65534;
    st.ts = 0xFFFFFC40u;

    TASK_CHECK(pjmedia_rtp_session_init2(&ses, st) == 0);

    for (i = 0; i < 3; ++i) {
        TASK_CHECK(pjmedia_rtp_encode_rtp(&ses, 111, i == 2, 4, 960,
                                          &encoded, &hlen) == 0);
        TASK_CHECK(hlen == 12);
        memcpy(raw, encoded, 12);
        raw[12] = (unsigned char)('A' + i);
        raw[13] = 0x42;
        raw[14] = 0x43;
        raw[15] = 0x44;
        TASK_CHECK(pjmedia_rtp_decode_rtp(&ses, raw, sizeof(raw), &hdr,
                                          &payload, &plen) == 0);
        TASK_CHECK(plen == 4);
        TASK_CHECK(((const unsigned char*)payload)[0] == (unsigned char)('A' + i));
        TASK_CHECK(pj_ntohs(hdr->seq) == (pj_uint16_t)(65535 + i));
        TASK_CHECK(pj_ntohl(hdr->ts) == (pj_uint32_t)(0xFFFFFC40u + 960u * (i + 1)));
        TASK_CHECK(pj_ntohl(hdr->ssrc) == 0x12345678u);
        TASK_CHECK(hdr->pt == 111);
        TASK_CHECK(!!hdr->m == (i == 2));
    }

    return 0;
}


const struct task_case task_cases[] = {
    {"timing", "config_validation", case_config_validation},
    {"timing", "anchor_and_deadline", case_anchor_and_deadline},
    {"timing", "slot_bounds", case_slot_bounds},
    {"timing", "bad_packets", case_bad_packets},
    {"timing", "output_wrap", case_output_wrap},

    {"handover", "arm_validation", case_arm_validation},
    {"handover", "out_of_order_proof", case_out_of_order_proof},
    {"handover", "missing_proof_abandons", case_missing_proof_abandons},
    {"handover", "duplicate_not_proof", case_duplicate_not_proof},
    {"handover", "cancel_replace_rollback", case_cancel_replace_rollback},
    {"handover", "cut_contention", case_cut_contention},
    {"handover", "double_handover", case_double_handover},

    {"ownership", "push_copies", case_push_copies},
    {"ownership", "pop_pointer_rules", case_pop_pointer_rules},
    {"ownership", "output_bytes_preserved", case_output_bytes_preserved},
    {"ownership", "reset_and_destroy", case_reset_and_destroy},
    {"ownership", "rtp_header_roundtrip", case_rtp_header_roundtrip},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
