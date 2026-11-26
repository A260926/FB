/*
 * Copyright (C) 2026 Teluu Inc. (http://www.teluu.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * pjmedia_rtp_splice: timed in-memory RTP source continuation / handover
 * buffer.  Driven by a caller maintained microsecond clock; no sockets,
 * no threads.
 */
#include <pjmedia/rtp.h>
#include <pj/sock.h>
#include <pj/string.h>

#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* The header installs source-compatibility _Generic wrappers over these
 * names; the definitions below are the real exported symbols.
 */
#undef pjmedia_rtp_splice_alloc
#undef pjmedia_rtp_splice_pop


#define SPLICE_RTP_HDR_LEN  12u

struct splice_pkt
{
    uint64_t            slot;   /**< Logical slot of the packet.        */
    size_t              len;    /**< Full packet length.                */
    uint8_t             data[];  /**< Full packet bytes.                 */
};

struct pjmedia_rtp_splice
{
    pjmedia_rtp_splice_cfg cfg;

    uint64_t            interval_us;/**< Per slot duration in us.        */

    int                 anchored;   /**< First initial pkt seen.        */
    uint64_t            anchor_A;   /**< Arrival time A of slot 0.      */

    uint64_t            base_slot;  /**< Logical slot of coordinate base.*/
    uint16_t            base_seq;   /**< Input seq at base_slot.        */
    uint32_t            base_ts;     /**< Input ts at base_slot.         */

    uint64_t            head;       /**< Next slot to pop.              */
    uint32_t            active_ssrc;/**< Current source SSRC.           */

    /* Armed candidate. */
    int                 cand_armed;
    uint32_t            cand_ssrc;
    uint64_t            cand_cut;
    int                 cand_known; /**< First candidate pkt mapped.    */
    uint16_t            cand_seq;   /**< Seq at cand_cut.              */
    uint32_t            cand_ts;    /**< Ts at cand_cut.               */

    struct splice_pkt **cur;        /**< Active source ring (window).   */
    struct splice_pkt **cand;       /**< Candidate ring (window).       */
};


/*
 * Validate configuration.  Returns 0 or EINVAL.
 */
static int splice_check_cfg(const pjmedia_rtp_splice_cfg *cfg)
{
    int64_t prod;

    if (cfg->pt < 0 || cfg->pt > 127)
        return EINVAL;
    if (cfg->clock_rate <= 0 || cfg->frame_ticks <= 0)
        return EINVAL;
    if (cfg->delay_us < 0)
        return EINVAL;
    if (cfg->window < 4 || cfg->window > 512)
        return EINVAL;

    /* frame_ticks * 1e6 must be representable and exactly divisible by
     * clock_rate, and the resulting packet interval must be positive.
     */
    if (cfg->frame_ticks > (INT64_MAX / 1000000))
        return EINVAL;
    prod = cfg->frame_ticks * 1000000;
    if (prod % cfg->clock_rate != 0)
        return EINVAL;
    if (prod / cfg->clock_rate <= 0)
        return EINVAL;

    return 0;
}


PJ_DEF(int) pjmedia_rtp_splice_alloc(pjmedia_rtp_splice **p_sp,
                                    const pjmedia_rtp_splice_cfg *cfg)
{
    struct pjmedia_rtp_splice *sp;
    unsigned win;
    int err;

    if (p_sp)
        *p_sp = NULL;

    if (!p_sp || !cfg)
        return EINVAL;

    err = splice_check_cfg(cfg);
    if (err != 0)
        return err;

    sp = (struct pjmedia_rtp_splice *)calloc(1, sizeof(*sp));
    if (!sp)
        return ENOMEM;

    win = (unsigned)cfg->window;
    sp->cur = (struct splice_pkt **)calloc(win, sizeof(sp->cur[0]));
    sp->cand = (struct splice_pkt **)calloc(win, sizeof(sp->cand[0]));
    if (!sp->cur || !sp->cand) {
        free(sp->cur);
        free(sp->cand);
        free(sp);
        return ENOMEM;
    }

    /* Copy the whole descriptor (all aliases share storage). */
    memcpy(&sp->cfg, cfg, sizeof(sp->cfg));

    sp->interval_us = (uint64_t)(cfg->frame_ticks * 1000000 /
                                 cfg->clock_rate);
    sp->active_ssrc = cfg->ssrc;
    sp->head = 0;

    *p_sp = sp;
    return 0;
}


PJ_DEF(int) pjmedia_rtp_splice_allocv(pjmedia_rtp_splice **p_sp,
                                      pjmedia_rtp_splice_cfg cfg)
{
    return pjmedia_rtp_splice_alloc(p_sp, &cfg);
}


static void ring_free_all(struct splice_pkt **ring, unsigned win)
{
    unsigned i;
    if (!ring)
        return;
    for (i = 0; i < win; ++i) {
        free(ring[i]);
        ring[i] = NULL;
    }
}


PJ_DEF(void) pjmedia_rtp_splice_destroy(pjmedia_rtp_splice *sp)
{
    if (!sp)
        return;
    ring_free_all(sp->cur, (unsigned)sp->cfg.window);
    ring_free_all(sp->cand, (unsigned)sp->cfg.window);
    free(sp->cur);
    free(sp->cand);
    free(sp);
}


PJ_DEF(void) pjmedia_rtp_splice_reset(pjmedia_rtp_splice *sp)
{
    unsigned win;
    if (!sp)
        return;

    win = (unsigned)sp->cfg.window;
    ring_free_all(sp->cur, win);
    ring_free_all(sp->cand, win);

    sp->interval_us = (uint64_t)(sp->cfg.frame_ticks * 1000000 /
                                 sp->cfg.clock_rate);
    sp->anchored = 0;
    sp->anchor_A = 0;
    sp->base_slot = 0;
    sp->base_seq = 0;
    sp->base_ts = 0;
    sp->head = 0;
    sp->active_ssrc = sp->cfg.ssrc;

    sp->cand_armed = 0;
    sp->cand_ssrc = 0;
    sp->cand_cut = 0;
    sp->cand_known = 0;
    sp->cand_seq = 0;
    sp->cand_ts = 0;
}


PJ_DEF(uint64_t) pjmedia_rtp_splice_head(pjmedia_rtp_splice *sp)
{
    return sp ? sp->head : 0;
}


PJ_DEF(uint32_t) pjmedia_rtp_splice_active(pjmedia_rtp_splice *sp)
{
    return sp ? sp->active_ssrc : 0;
}


PJ_DEF(int) pjmedia_rtp_splice_arm(pjmedia_rtp_splice *sp,
                                   uint32_t cand_ssrc, uint64_t cut)
{
    uint64_t dist;

    if (!sp)
        return EINVAL;

    /* Existing anchor is required. */
    if (!sp->anchored)
        return EINVAL;

    /* cut >= head and cut + 2 < head + window, expressed without any
     * risk of uint64 wrap as: dist = cut - head < window - 2, i.e.
     * dist <= window - 3.
     */
    if (cut < sp->head)
        return EINVAL;
    dist = cut - sp->head;
    if (dist >= (uint64_t)sp->cfg.window - 2u)
        return EINVAL;

    /* Candidate must differ from the current active source. */
    if (cand_ssrc == sp->active_ssrc)
        return EINVAL;

    /* Valid: replace any previous candidate. */
    ring_free_all(sp->cand, (unsigned)sp->cfg.window);
    sp->cand_armed = 1;
    sp->cand_ssrc = cand_ssrc;
    sp->cand_cut = cut;
    sp->cand_known = 0;
    sp->cand_seq = 0;
    sp->cand_ts = 0;
    return 0;
}


PJ_DEF(void) pjmedia_rtp_splice_cancel(pjmedia_rtp_splice *sp)
{
    if (!sp)
        return;
    ring_free_all(sp->cand, (unsigned)sp->cfg.window);
    sp->cand_armed = 0;
    sp->cand_ssrc = 0;
    sp->cand_cut = 0;
    sp->cand_known = 0;
    sp->cand_seq = 0;
    sp->cand_ts = 0;
}


/*
 * Deadline of logical slot k: A + delay_us + k * interval.
 * Callers guarantee no int64 overflow; uint64 arithmetic is used.
 */
static uint64_t slot_deadline(const struct pjmedia_rtp_splice *sp,
                              uint64_t k)
{
    return sp->anchor_A + (uint64_t)sp->cfg.delay_us +
           k * sp->interval_us;
}


/*
 * Signed modular sequence delta in (-32768 .. 32767), valid because the
 * active sequence span is assumed < 32768.
 */
static int64_t seq_delta(uint16_t seq, uint16_t base)
{
    uint16_t d = (uint16_t)(seq - base);
    if (d < 32768)
        return (int64_t)d;
    return (int64_t)d - 65536;
}


/*
 * Reuse the production RTP header decoder and additionally enforce the
 * padding rules that pjmedia_rtp_decode_rtp2() leaves lax.
 *
 * Returns 0 and fills @a hdr / @a dec / @a data_off, or EBADMSG.
 */
static int splice_parse(const void *pkt, size_t len,
                        const pjmedia_rtp_hdr **hdr,
                        pjmedia_rtp_dec_hdr *dec,
                        size_t *data_off)
{
    pj_status_t st;
    const void *payload = NULL;
    unsigned payloadlen = 0;
    unsigned off;

    if (len < SPLICE_RTP_HDR_LEN || len > (size_t)INT_MAX)
        return EBADMSG;

    st = pjmedia_rtp_decode_rtp2(NULL, pkt, (int)len, hdr, dec,
                                 &payload, &payloadlen);
    if (st != PJ_SUCCESS)
        return EBADMSG;

    off = SPLICE_RTP_HDR_LEN + 4u * (unsigned)(*hdr)->cc;
    if ((*hdr)->x) {
        uint16_t ext_words = pj_ntohs(dec->ext_hdr->length);
        off += (unsigned)(ext_words + 1u) * 4u;
    }
    /* decode_rtp2 already guarantees off <= len. */

    if ((*hdr)->p) {
        unsigned pad_len;
        if (len <= off)
            return EBADMSG;       /* no padding count byte */
        pad_len = (unsigned)((const uint8_t *)pkt)[len - 1];
        if (pad_len < 1 || (size_t)pad_len > len - off)
            return EBADMSG;
    }

    if (data_off)
        *data_off = off;
    return 0;
}


/*
 * First-content-wins store into a window ring.  A stale ring entry with a
 * different slot (should not normally happen) is reclaimed.
 */
static int ring_store(struct splice_pkt **ring, unsigned win,
                      uint64_t slot, const void *bytes, size_t len)
{
    size_t idx = (size_t)(slot % win);
    struct splice_pkt *p = ring[idx];

    if (p != NULL) {
        if (p->slot == slot)
            return 0;
        free(p);
        ring[idx] = NULL;
    }

    p = (struct splice_pkt *)malloc(sizeof(*p) + len);
    if (!p)
        return ENOMEM;
    p->slot = slot;
    p->len = len;
    memcpy(p->data, bytes, len);
    ring[idx] = p;
    return 0;
}


static int candidate_ready(const struct pjmedia_rtp_splice *sp)
{
    uint64_t j;
    unsigned n;

    if (!sp->cand_armed || !sp->cand_known)
        return 0;

    for (n = 0, j = sp->cand_cut; n < 3; ++n, ++j) {
        struct splice_pkt *p =
            sp->cand[(size_t)(j % (unsigned)sp->cfg.window)];
        if (!p || p->slot != j)
            return 0;
    }
    return 1;
}


/*
 * Atomic handover at slot cut: free active packets at/after cut, move
 * candidate packets into the active ring, adopt candidate coordinates.
 */
static void do_handover(struct pjmedia_rtp_splice *sp)
{
    unsigned win = (unsigned)sp->cfg.window;
    unsigned i;

    /* Release every old-source cache entry at slot >= cut. */
    for (i = 0; i < win; ++i) {
        struct splice_pkt *p = sp->cur[i];
        if (p && p->slot >= sp->cand_cut) {
            free(p);
            sp->cur[i] = NULL;
        }
    }

    /* Move candidate cache across. */
    for (i = 0; i < win; ++i) {
        struct splice_pkt *p = sp->cand[i];
        sp->cand[i] = NULL;
        if (!p)
            continue;
        if (p->slot < sp->cand_cut) {
            /* Behind the cut, never played: discard. */
            free(p);
            continue;
        }
        /* The matching active slot was freed above; defensively release
         * any unexpected stale occupant of this ring index.
         */
        if (sp->cur[i] && sp->cur[i] != p)
            free(sp->cur[i]);
        sp->cur[i] = p;
    }

    sp->active_ssrc = sp->cand_ssrc;
    sp->base_slot = sp->cand_cut;
    sp->base_seq = sp->cand_seq;
    sp->base_ts = sp->cand_ts;

    sp->cand_armed = 0;
    sp->cand_ssrc = 0;
    sp->cand_cut = 0;
    sp->cand_known = 0;
    sp->cand_seq = 0;
    sp->cand_ts = 0;
}


/*
 * Abandon the candidate and all of its proofs/packets.
 */
static void discard_candidate(struct pjmedia_rtp_splice *sp)
{
    ring_free_all(sp->cand, (unsigned)sp->cfg.window);
    sp->cand_armed = 0;
    sp->cand_ssrc = 0;
    sp->cand_cut = 0;
    sp->cand_known = 0;
    sp->cand_seq = 0;
    sp->cand_ts = 0;
}


PJ_DEF(int) pjmedia_rtp_splice_push(pjmedia_rtp_splice *sp,
                                    const void *pkt, size_t len,
                                    uint64_t now_us)
{
    const pjmedia_rtp_hdr *hdr = NULL;
    pjmedia_rtp_dec_hdr dec;
    size_t data_off = 0;
    int err;
    uint16_t seq16;
    uint32_t ts32, ssrc;

    if (!sp || !pkt)
        return EINVAL;

    pj_bzero(&dec, sizeof(dec));

    /* Structural validation first; never mutates state. */
    err = splice_parse(pkt, len, &hdr, &dec, &data_off);
    if (err != 0)
        return err;

    if ((int)hdr->pt != sp->cfg.pt)
        return EBADMSG;

    seq16 = pj_ntohs(hdr->seq);
    ts32 = pj_ntohl(hdr->ts);
    ssrc = pj_ntohl(hdr->ssrc);

    if (ssrc == sp->active_ssrc) {
        /* ---------------- Active/current source ---------------- */
        int64_t k;
        uint64_t ku;
        uint32_t exp_ts;

        if (!sp->anchored) {
            /* First legal initial-source packet establishes slot 0. */
            sp->anchored = 1;
            sp->anchor_A = now_us;
            sp->base_slot = 0;
            sp->base_seq = seq16;
            sp->base_ts = ts32;
            sp->head = 0;

            /* Its deadline is A + delay which is >= now_us. */
            return ring_store(sp->cur, (unsigned)sp->cfg.window, 0,
                              pkt, len);
        }

        k = (int64_t)sp->base_slot + seq_delta(seq16, sp->base_seq);

        /* Timestamp must track the slot coordinates. */
        exp_ts = sp->base_ts +
                 (uint32_t)((k - (int64_t)sp->base_slot) *
                            sp->cfg.frame_ticks);
        if (ts32 != exp_ts)
            return EBADMSG;

        /* Slot range relative to head. */
        if (k < 0 || (uint64_t)k < sp->head)
            return ENOENT;
        ku = (uint64_t)k;
        if (ku >= sp->head + (uint64_t)sp->cfg.window)
            return ENOBUFS;

        /* Deadline (exact deadline still accepted). */
        if (now_us > slot_deadline(sp, ku))
            return ETIMEDOUT;

        return ring_store(sp->cur, (unsigned)sp->cfg.window, ku,
                          pkt, len);
    }

    if (sp->cand_armed && ssrc == sp->cand_ssrc) {
        /* ---------------- Candidate source ---------------- */
        uint64_t cut_deadline = slot_deadline(sp, sp->cand_cut);

        /* Every candidate push must arrive no later than the cut
         * slot deadline.
         */
        if (now_us > cut_deadline)
            return ETIMEDOUT;

        if (!sp->cand_known) {
            /* First legal candidate packet maps to the cut slot. */
            sp->cand_known = 1;
            sp->cand_seq = seq16;
            sp->cand_ts = ts32;
            return ring_store(sp->cand, (unsigned)sp->cfg.window,
                              sp->cand_cut, pkt, len);
        } else {
            int64_t j = (int64_t)sp->cand_cut +
                        seq_delta(seq16, sp->cand_seq);
            uint64_t ju;
            uint32_t exp_ts;

            exp_ts = sp->cand_ts +
                     (uint32_t)((j - (int64_t)sp->cand_cut) *
                                sp->cfg.frame_ticks);
            if (ts32 != exp_ts)
                return EBADMSG;

            if (j < 0 || (uint64_t)j < sp->head)
                return ENOENT;
            ju = (uint64_t)j;
            if (ju >= sp->head + (uint64_t)sp->cfg.window)
                return ENOBUFS;

            if (now_us > slot_deadline(sp, ju))
                return ETIMEDOUT;

            return ring_store(sp->cand, (unsigned)sp->cfg.window, ju,
                              pkt, len);
        }
    }

    /* Unknown SSRC. */
    return ENOENT;
}


/*
 * Core pop.  On success returns 0 and fills caller-local buf/len.
 */
static int pop_core(struct pjmedia_rtp_splice *sp, uint64_t now_us,
                    void **out_pkt, size_t *out_len)
{
    uint64_t k;
    size_t idx;
    struct splice_pkt *p;
    uint8_t *out;
    uint16_t oseq;
    uint32_t ots, ossrc;

    if (!sp || !out_pkt || !out_len)
        return EINVAL;

    if (!sp->anchored)
        return EAGAIN;

    k = sp->head;
    if (now_us < slot_deadline(sp, k))
        return EAGAIN;

    /* First pop advancing onto the cut makes the handover decision. */
    if (sp->cand_armed && sp->cand_cut == k) {
        if (candidate_ready(sp))
            do_handover(sp);
        else
            discard_candidate(sp);
    }

    idx = (size_t)(k % (unsigned)sp->cfg.window);
    p = sp->cur[idx];
    if (!p || p->slot != k) {
        if (p && p->slot != k) {
            free(p);
            sp->cur[idx] = NULL;
        }
        ++sp->head;
        return ENODATA;
    }

    out = (uint8_t *)malloc(p->len);
    if (!out)
        return ENOMEM;
    memcpy(out, p->data, p->len);

    /* Rewrite only SSRC / sequence / timestamp of the output identity.
     * Network byte order (big endian), written byte-wise.
     */
    oseq = (uint16_t)(sp->cfg.out_seq + k);
    ots = (uint32_t)(sp->cfg.out_ts +
                     k * (uint64_t)sp->cfg.frame_ticks);
    ossrc = sp->cfg.out_ssrc;

    out[2] = (uint8_t)(oseq >> 8);
    out[3] = (uint8_t)(oseq & 0xff);
    out[4] = (uint8_t)(ots >> 24);
    out[5] = (uint8_t)((ots >> 16) & 0xff);
    out[6] = (uint8_t)((ots >> 8) & 0xff);
    out[7] = (uint8_t)(ots & 0xff);
    out[8] = (uint8_t)(ossrc >> 24);
    out[9] = (uint8_t)((ossrc >> 16) & 0xff);
    out[10] = (uint8_t)((ossrc >> 8) & 0xff);
    out[11] = (uint8_t)(ossrc & 0xff);

    *out_pkt = out;
    *out_len = p->len;

    free(p);
    sp->cur[idx] = NULL;
    ++sp->head;
    return 0;
}


PJ_DEF(int) pjmedia_rtp_splice_pop_z(pjmedia_rtp_splice *sp,
                                     uint64_t now_us,
                                     void **out_pkt, size_t *out_len)
{
    void *buf = NULL;
    size_t len = 0;
    int rc = pop_core(sp, now_us, &buf, &len);
    if (rc == 0) {
        /* memcpy keeps this aliasing-safe if the caller's pointer
         * variable has a different (but ABI identical) object-pointer
         * type.
         */
        if (out_pkt)
            memcpy(out_pkt, &buf, sizeof(buf));
        if (out_len)
            *out_len = len;
    }
    return rc;
}

PJ_DEF(int) pjmedia_rtp_splice_pop(pjmedia_rtp_splice *sp,
                                   uint64_t now_us,
                                   void **out_pkt, size_t *out_len)
{
    return pjmedia_rtp_splice_pop_z(sp, now_us, out_pkt, out_len);
}

#define SPLICE_DEF_NARROW_POP(suffix, ctype) \
PJ_DEF(int) pjmedia_rtp_splice_pop_##suffix( \
        pjmedia_rtp_splice *sp, uint64_t now_us, \
        void **out_pkt, ctype *out_len) \
{ \
    void *buf = NULL; \
    size_t len = 0; \
    int rc = pop_core(sp, now_us, &buf, &len); \
    if (rc == 0) { \
        if (out_pkt) \
            memcpy(out_pkt, &buf, sizeof(buf)); \
        if (out_len) \
            *out_len = (ctype)len; \
    } \
    return rc; \
}

SPLICE_DEF_NARROW_POP(u, unsigned int)
SPLICE_DEF_NARROW_POP(i, int)
SPLICE_DEF_NARROW_POP(ull, unsigned long long)
SPLICE_DEF_NARROW_POP(ll, long long)
SPLICE_DEF_NARROW_POP(us, unsigned short)
SPLICE_DEF_NARROW_POP(s, short)
SPLICE_DEF_NARROW_POP(l, long)
