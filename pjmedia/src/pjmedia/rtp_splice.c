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
#include <pjmedia/rtp.h>
#include <pj/sock.h>
#include <pj/string.h>

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>


#define SPLICE_MIN_WINDOW   4
#define SPLICE_MAX_WINDOW   512
#define SPLICE_PROOF_SLOTS  3

typedef struct splice_slot
{
    void       *buf;
    pj_size_t   len;
} splice_slot;

struct pjmedia_rtp_splice
{
    pjmedia_rtp_splice_cfg  cfg;
    pj_uint64_t             interval_us;

    splice_slot            *slots;      /* current source, by slot % window */
    splice_slot            *cand_slots; /* candidate source                */

    pj_bool_t               anchored;
    pj_uint64_t             anchor_us;  /* arrival time of logical slot 0  */
    pj_uint64_t             head;
    pj_uint16_t             head_seq;   /* sequence of slot 'head'         */
    pj_uint32_t             head_ts;    /* timestamp of slot 'head'        */
    pj_uint32_t             active_ssrc;

    pj_bool_t               armed;
    pj_uint32_t             cand_ssrc;
    pj_uint64_t             cand_cut;
    pj_bool_t               cand_coord; /* candidate coordinate established */
    pj_uint16_t             cand_seq0;  /* sequence at slot 'cand_cut'      */
    pj_uint32_t             cand_ts0;   /* timestamp at slot 'cand_cut'     */
};


/* Deadline of a logical slot. */
static pj_uint64_t splice_deadline(const pjmedia_rtp_splice *sp,
                                   pj_uint64_t slot)
{
    return sp->anchor_us + (pj_uint64_t)sp->cfg.delay_us +
           slot * sp->interval_us;
}


/* Expected timestamp of a slot at 'delta' slots from a reference slot. */
static pj_uint32_t splice_ts_at(pj_uint32_t base_ts, pj_int64_t delta,
                                pj_uint32_t frame_ticks)
{
    pj_int64_t ts = (pj_int64_t)base_ts + delta * (pj_int64_t)frame_ticks;
    return (pj_uint32_t)((pj_uint64_t)ts & PJ_UINT64(0xffffffff));
}


/*
 * Decode a complete RTP packet using the production RTP parser and apply
 * the additional strictness required by the splice object (complete header,
 * valid padding).  This function never modifies the packet or the splice
 * state.
 */
static int splice_parse(const void *pkt, pj_size_t len,
                        pj_uint16_t *seq, pj_uint32_t *ts,
                        pj_uint32_t *ssrc, pj_uint8_t *pt)
{
    pjmedia_rtp_session ses;
    pjmedia_rtp_dec_hdr dec_hdr;
    const pjmedia_rtp_hdr *hdr = NULL;
    const void *payload = NULL;
    unsigned payloadlen = 0;
    pj_size_t offset;
    pj_status_t status;

    if (len < sizeof(pjmedia_rtp_hdr) || len > (pj_size_t)INT_MAX)
        return EBADMSG;

    pj_bzero(&ses, sizeof(ses));
    status = pjmedia_rtp_decode_rtp2(&ses, pkt, (int)len, &hdr, &dec_hdr,
                                     &payload, &payloadlen);
    if (status != PJ_SUCCESS)
        return EBADMSG;

    /* The production decoder already rejects a wrong version and an
     * incomplete header or extension.  Recompute the payload offset so
     * that padding can be checked strictly.
     */
    offset = sizeof(pjmedia_rtp_hdr) + (pj_size_t)hdr->cc * 4;
    if (hdr->x)
        offset += sizeof(pjmedia_rtp_ext_hdr) + (pj_size_t)dec_hdr.ext_len * 4;

    if (hdr->p) {
        pj_size_t raw_len = len - offset;
        unsigned pad_len;

        /* A padding flag with no room for the padding count byte, a zero
         * count, or a count covering more than the payload is invalid.
         */
        if (raw_len == 0)
            return EBADMSG;
        pad_len = ((const pj_uint8_t*)pkt)[len - 1];
        if (pad_len == 0 || (pj_size_t)pad_len > raw_len)
            return EBADMSG;
    }

    *seq = pj_ntohs(hdr->seq);
    *ts = pj_ntohl(hdr->ts);
    *ssrc = pj_ntohl(hdr->ssrc);
    *pt = (pj_uint8_t)hdr->pt;
    return 0;
}


/* Store a copy of the packet in an empty slot, first contents winning. */
static int splice_slot_put(splice_slot *slot, const void *pkt, pj_size_t len)
{
    void *copy;

    if (slot->buf != NULL)
        return 0;

    copy = malloc(len);
    if (copy == NULL)
        return ENOMEM;
    memcpy(copy, pkt, len);
    slot->buf = copy;
    slot->len = len;
    return 0;
}


/* Release every packet held by a slot array. */
static void splice_slots_clear(splice_slot *slots, pj_uint32_t window)
{
    pj_uint32_t i;

    if (slots == NULL)
        return;
    for (i = 0; i < window; ++i) {
        free(slots[i].buf);
        slots[i].buf = NULL;
        slots[i].len = 0;
    }
}


/* Drop the candidate source and all of its queued packets. */
static void splice_candidate_clear(pjmedia_rtp_splice *sp)
{
    splice_slots_clear(sp->cand_slots, sp->cfg.window);
    sp->armed = PJ_FALSE;
    sp->cand_coord = PJ_FALSE;
    sp->cand_ssrc = 0;
    sp->cand_cut = 0;
    sp->cand_seq0 = 0;
    sp->cand_ts0 = 0;
}


/*
 * Commit a ready candidate.  Packets queued for the old source at the cut
 * slot and beyond are dropped, candidate packets take their place and the
 * candidate coordinate becomes the current source coordinate.
 */
static void splice_commit(pjmedia_rtp_splice *sp)
{
    pj_uint64_t slot, limit = sp->head + sp->cfg.window;
    pj_uint32_t idx;

    for (slot = sp->cand_cut; slot < limit; ++slot) {
        idx = (pj_uint32_t)(slot % sp->cfg.window);
        free(sp->slots[idx].buf);
        sp->slots[idx].buf = NULL;
        sp->slots[idx].len = 0;
    }

    for (slot = sp->cand_cut; slot < limit; ++slot) {
        idx = (pj_uint32_t)(slot % sp->cfg.window);
        sp->slots[idx] = sp->cand_slots[idx];
        sp->cand_slots[idx].buf = NULL;
        sp->cand_slots[idx].len = 0;
    }

    sp->head_seq = sp->cand_seq0;
    sp->head_ts = sp->cand_ts0;
    sp->active_ssrc = sp->cand_ssrc;
    splice_candidate_clear(sp);
}


/* Current candidate has collected slots cut, cut+1 and cut+2. */
static pj_bool_t splice_candidate_ready(const pjmedia_rtp_splice *sp)
{
    pj_uint32_t i, idx;

    if (!sp->armed || !sp->cand_coord)
        return PJ_FALSE;
    for (i = 0; i < SPLICE_PROOF_SLOTS; ++i) {
        idx = (pj_uint32_t)((sp->cand_cut + i) % sp->cfg.window);
        if (sp->cand_slots[idx].buf == NULL)
            return PJ_FALSE;
    }
    return PJ_TRUE;
}


PJ_DEF(int) pjmedia_rtp_splice_alloc(pjmedia_rtp_splice **obj,
                                     const pjmedia_rtp_splice_cfg *cfg)
{
    pjmedia_rtp_splice *sp;
    pj_uint64_t scaled, interval;

    if (obj == NULL || cfg == NULL)
        return EINVAL;

    if (cfg->pt > 127)
        return EINVAL;
    if (cfg->clock_rate == 0 || cfg->frame_ticks == 0)
        return EINVAL;
    if (cfg->delay_us < 0)
        return EINVAL;
    if (cfg->window < SPLICE_MIN_WINDOW || cfg->window > SPLICE_MAX_WINDOW)
        return EINVAL;

    scaled = (pj_uint64_t)cfg->frame_ticks * PJ_UINT64(1000000);
    if (scaled % cfg->clock_rate != 0)
        return EINVAL;
    interval = scaled / cfg->clock_rate;
    if (interval == 0)
        return EINVAL;

    sp = (pjmedia_rtp_splice*)calloc(1, sizeof(*sp));
    if (sp == NULL)
        return ENOMEM;

    sp->slots = (splice_slot*)calloc(cfg->window, sizeof(splice_slot));
    sp->cand_slots = (splice_slot*)calloc(cfg->window, sizeof(splice_slot));
    if (sp->slots == NULL || sp->cand_slots == NULL) {
        free(sp->slots);
        free(sp->cand_slots);
        free(sp);
        return ENOMEM;
    }

    sp->cfg = *cfg;
    sp->interval_us = interval;
    sp->active_ssrc = cfg->init_ssrc;

    *obj = sp;
    return 0;
}


PJ_DEF(void) pjmedia_rtp_splice_destroy(pjmedia_rtp_splice *obj)
{
    if (obj == NULL)
        return;

    splice_slots_clear(obj->slots, obj->cfg.window);
    splice_slots_clear(obj->cand_slots, obj->cfg.window);
    free(obj->slots);
    free(obj->cand_slots);
    free(obj);
}


PJ_DEF(int) pjmedia_rtp_splice_push(pjmedia_rtp_splice *obj,
                                    const void *pkt, pj_size_t len,
                                    pj_uint64_t now)
{
    pj_uint16_t seq;
    pj_uint32_t ts, ssrc;
    pj_uint8_t pt;
    pj_int16_t delta;
    pj_int64_t slot64;
    pj_uint64_t slot;
    pj_uint32_t idx;
    int rc;

    if (obj == NULL || pkt == NULL)
        return EINVAL;

    rc = splice_parse(pkt, len, &seq, &ts, &ssrc, &pt);
    if (rc != 0)
        return rc;
    if ((pj_uint32_t)pt != obj->cfg.pt)
        return EBADMSG;

    /* The first acceptable initial-source packet establishes slot zero. */
    if (!obj->anchored) {
        if (ssrc != obj->cfg.init_ssrc)
            return ENOENT;

        /* Anchor only once the packet copy is safely stored. */
        rc = splice_slot_put(&obj->slots[0], pkt, len);
        if (rc != 0)
            return rc;

        obj->anchored = PJ_TRUE;
        obj->anchor_us = now;
        obj->head = 0;
        obj->head_seq = seq;
        obj->head_ts = ts;
        obj->active_ssrc = obj->cfg.init_ssrc;
        return 0;
    }

    if (ssrc == obj->active_ssrc) {
        delta = (pj_int16_t)(pj_uint16_t)(seq - obj->head_seq);
        slot64 = (pj_int64_t)obj->head + delta;
        if (slot64 < 0)
            return ENOENT;
        slot = (pj_uint64_t)slot64;
        if (slot < obj->head)
            return ENOENT;
        if (slot - obj->head >= obj->cfg.window)
            return ENOBUFS;
        if (now > splice_deadline(obj, slot))
            return ETIMEDOUT;
        if (ts != splice_ts_at(obj->head_ts, delta, obj->cfg.frame_ticks))
            return EBADMSG;

        idx = (pj_uint32_t)(slot % obj->cfg.window);
        return splice_slot_put(&obj->slots[idx], pkt, len);
    }

    if (!obj->armed || ssrc != obj->cand_ssrc)
        return ENOENT;

    /* A candidate may only be proven while slot c's deadline is open. */
    if (now > splice_deadline(obj, obj->cand_cut))
        return ETIMEDOUT;

    if (!obj->cand_coord) {
        /* The first candidate packet defines the coordinate at slot c. */
        idx = (pj_uint32_t)(obj->cand_cut % obj->cfg.window);
        rc = splice_slot_put(&obj->cand_slots[idx], pkt, len);
        if (rc != 0)
            return rc;
        obj->cand_coord = PJ_TRUE;
        obj->cand_seq0 = seq;
        obj->cand_ts0 = ts;
        return 0;
    }

    delta = (pj_int16_t)(pj_uint16_t)(seq - obj->cand_seq0);
    slot64 = (pj_int64_t)obj->cand_cut + delta;
    if (slot64 < (pj_int64_t)obj->cand_cut)
        return ENOENT;
    slot = (pj_uint64_t)slot64;
    if (slot - obj->head >= obj->cfg.window)
        return ENOBUFS;
    if (ts != splice_ts_at(obj->cand_ts0, delta, obj->cfg.frame_ticks))
        return EBADMSG;

    idx = (pj_uint32_t)(slot % obj->cfg.window);
    return splice_slot_put(&obj->cand_slots[idx], pkt, len);
}


PJ_DEF(int) pjmedia_rtp_splice_pop(pjmedia_rtp_splice *obj, pj_uint64_t now,
                                   void **pkt, pj_size_t *len)
{
    splice_slot *slot;
    pj_uint32_t idx;
    pj_uint64_t num;
    pjmedia_rtp_hdr *hdr;

    if (obj == NULL || pkt == NULL || len == NULL)
        return EINVAL;

    if (!obj->anchored)
        return EAGAIN;

    if (now < splice_deadline(obj, obj->head))
        return EAGAIN;

    /* The first time the head reaches the cut slot, decide atomically. */
    if (obj->armed && obj->cand_cut == obj->head) {
        if (splice_candidate_ready(obj))
            splice_commit(obj);
        else
            splice_candidate_clear(obj);
    }

    num = obj->head;
    idx = (pj_uint32_t)(num % obj->cfg.window);
    slot = &obj->slots[idx];

    if (slot->buf != NULL) {
        void *out = slot->buf;
        pj_size_t out_len = slot->len;

        slot->buf = NULL;
        slot->len = 0;

        /* Advance after detaching the packet. */
        obj->head_seq = (pj_uint16_t)(obj->head_seq + 1);
        obj->head_ts += obj->cfg.frame_ticks;
        obj->head = num + 1;

        hdr = (pjmedia_rtp_hdr*)out;
        hdr->ssrc = pj_htonl(obj->cfg.out_ssrc);
        hdr->seq = pj_htons((pj_uint16_t)(obj->cfg.out_seq + (pj_uint16_t)num));
        hdr->ts = pj_htonl((pj_uint32_t)
                           ((pj_uint64_t)obj->cfg.out_ts +
                            num * (pj_uint64_t)obj->cfg.frame_ticks));

        *pkt = out;
        *len = out_len;
        return 0;
    }

    /* Empty due slot: consume it, output pointers stay untouched. */
    obj->head_seq = (pj_uint16_t)(obj->head_seq + 1);
    obj->head_ts += obj->cfg.frame_ticks;
    obj->head = num + 1;
    return ENODATA;
}


PJ_DEF(int) pjmedia_rtp_splice_arm(pjmedia_rtp_splice *obj,
                                   pj_uint32_t ssrc, pj_uint64_t cut)
{
    pj_uint64_t limit;

    if (obj == NULL || !obj->anchored)
        return EINVAL;
    if (ssrc == obj->active_ssrc)
        return EINVAL;
    if (cut < obj->head)
        return EINVAL;

    /* cut + 2 must still fit strictly inside the window. */
    limit = obj->head + obj->cfg.window;
    if (cut > limit - SPLICE_PROOF_SLOTS)
        return EINVAL;

    /* Everything is valid; replacing a candidate is now safe. */
    splice_candidate_clear(obj);

    obj->armed = PJ_TRUE;
    obj->cand_ssrc = ssrc;
    obj->cand_cut = cut;
    obj->cand_coord = PJ_FALSE;
    return 0;
}


PJ_DEF(void) pjmedia_rtp_splice_cancel(pjmedia_rtp_splice *obj)
{
    if (obj == NULL)
        return;

    splice_candidate_clear(obj);
}


PJ_DEF(void) pjmedia_rtp_splice_reset(pjmedia_rtp_splice *obj)
{
    if (obj == NULL)
        return;

    splice_slots_clear(obj->slots, obj->cfg.window);
    splice_candidate_clear(obj);

    obj->anchored = PJ_FALSE;
    obj->anchor_us = 0;
    obj->head = 0;
    obj->head_seq = 0;
    obj->head_ts = 0;
    obj->active_ssrc = obj->cfg.init_ssrc;
}


PJ_DEF(pj_uint64_t) pjmedia_rtp_splice_head(const pjmedia_rtp_splice *obj)
{
    return obj ? obj->head : 0;
}


PJ_DEF(pj_uint32_t) pjmedia_rtp_splice_active(
                                        const pjmedia_rtp_splice *obj)
{
    return obj ? obj->active_ssrc : 0;
}
