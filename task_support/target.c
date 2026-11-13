/*
 * Replayable property tests for c-ares DNS response parsing.
 *
 * The logical model, the three legal wire layouts and all expected
 * values live in model.c and are produced without consulting c-ares.
 * This file drives 500 fixed-seed cases, each in all three layouts,
 * through the real ares_parse_a_reply / ares_parse_aaaa_reply and
 * ares_expand_name interfaces and checks the results against the
 * independent model.
 *
 * Groups:
 *   model       - independent-model conformance and enclen accounting
 *   metamorphic - legal RR reordering, query casing, irrelevant extra
 *                 records leave Q/A identity and the address set intact
 *   replay      - deterministic seed replay, DNS size/pointer limits,
 *                 truncation boundaries, root-name semantics and the
 *                 predetermined logic-difference simulator
 */

#include "check.h"
#include "model.h"
#include "ares.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <netdb.h>

#define MASTER_SEED 0x5eed0001u

typedef struct {
    int cases;
    int layouts;
    int builds;
    int build_fail;
    int parse_fail;
    int expand_fail;
    int skipped;
    int chain_len[EM_MAX_CHAIN + 1];   /* index by nhops 1..4 */
    int a_replies;
    int aaaa_replies;
    int root_names;
    int ptr_depth_max;
    int ptr_names;                      /* names using compression */
    int multilevel_names;              /* names with >=2 ptr hops */
    int uncompressed_pointers;         /* must stay 0 for layout 0 */
    int extra_records;
    int first_case;
    int first_layout;
    unsigned first_seed;
    char first_diff[256];
    int have_diff;
} run_stats;

static void render_chain(const em_model *m, char *buf, size_t n)
{
    char name[EM_NAMEMAX];
    size_t pos = 0;
    int i;
    em_render(&m->qname, name, sizeof(name));
    pos += (size_t)snprintf(buf, n, "%s", name);
    for (i = 0; i < m->nhops; i++)
    {
        em_render(&m->cname[i], name, sizeof(name));
        pos += (size_t)snprintf(buf + pos, n - pos, " -> %s", name);
    }
    snprintf(buf + pos, n - pos, " [%s x%d]",
             m->qtype == 1 ? "A" : "AAAA", m->naddrs);
}

static void note_name_coverage(run_stats *s, const em_model *m,
                               const em_wire *w)
{
    int i;
    if (em_name_is_root(&m->qname))
        s->root_names++;
    for (i = 0; i < m->nhops; i++)
        if (em_name_is_root(&m->cname[i]))
            s->root_names++;
    for (i = 0; i < w->nnames; i++)
    {
        if (w->names[i].ptrs > 0)
            s->ptr_names++;
        if (w->names[i].ptrs >= 2)
            s->multilevel_names++;
        if (w->names[i].ptrs > s->ptr_depth_max)
            s->ptr_depth_max = w->names[i].ptrs;
    }
}

static void record_diff(run_stats *s, const em_model *m, int case_idx,
                        int layout, const char *diff)
{
    if (!s->have_diff)
    {
        s->have_diff = 1;
        s->first_case = case_idx;
        s->first_layout = layout;
        s->first_seed = m->seed;
        snprintf(s->first_diff, sizeof(s->first_diff), "%s", diff);
    }
}

/* ------------------------------------------------------------------ */
/* Group 1: independent model over 500 cases x 3 layouts.             */
/* ------------------------------------------------------------------ */

static int run_model_group(run_stats *s)
{
    int n, lay;
    memset(s, 0, sizeof(*s));
    s->first_case = -1;

    for (n = 0; n < EM_CASES; n++)
    {
        em_model m = em_generate(MASTER_SEED, n);
        char chain[EM_NAMEMAX * 2];
        s->cases++;
        s->chain_len[m.nhops]++;
        if (m.qtype == 1) s->a_replies++; else s->aaaa_replies++;
        if (m.has_extra) s->extra_records++;
        render_chain(&m, chain, sizeof(chain));
        printf("seed=0x%08x case=%d chain=[%s]\n", m.seed, n, chain);

        for (lay = 0; lay < 3; lay++)
        {
            em_wire w;
            char diff[256];
            int rc;
            s->layouts++;
            memset(diff, 0, sizeof(diff));
            rc = em_build(&m, (em_layout)lay, 0, 0, 0, &w);
            if (rc != 0)
            {
                s->build_fail++;
                record_diff(s, &m, n, lay, "em_build rejected layout");
                continue;
            }
            s->builds++;
            note_name_coverage(s, &m, &w);
            if (lay == EM_LAYOUT_UNCOMPRESSED && w.ptr_depth != 0)
                s->uncompressed_pointers++;

            printf("  layout=%d q=%s msglen=%d ptrdepth=%d names=%d\n",
                   lay, m.qtype == 1 ? "A" : "AAAA",
                   w.msglen, w.ptr_depth, w.nnames);

            memset(diff, 0, sizeof(diff));
            if (em_check_reply(&m, &w, diff, sizeof(diff)) != 0)
            {
                s->parse_fail++;
                record_diff(s, &m, n, lay, diff);
                continue;
            }
            memset(diff, 0, sizeof(diff));
            if (em_check_expand(&w, diff, sizeof(diff)) != 0)
            {
                s->expand_fail++;
                record_diff(s, &m, n, lay, diff);
            }
        }
    }

    if (s->have_diff)
    {
        em_model m = em_generate(MASTER_SEED, s->first_case);
        char chain[EM_NAMEMAX * 2];
        render_chain(&m, chain, sizeof(chain));
        printf("FIRST DEVIATION seed=0x%08x case=%d layout=%d chain=[%s]\n"
               "  detail: %s\n",
               s->first_seed, s->first_case, s->first_layout, chain,
               s->first_diff);
    }
    else
        puts("no deviation across all cases and layouts");
    return s->have_diff ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Group 2: metamorphic transformations.                              */
/* ------------------------------------------------------------------ */

static int hostent_name_sets_equal(const struct hostent *a,
                                   const struct hostent *b)
{
    int na = 0, nb = 0, i, j;
    while (a->h_aliases[na]) na++;
    while (b->h_aliases[nb]) nb++;
    if (na != nb) return 0;
    if (strcasecmp(a->h_name, b->h_name) != 0) return 0;
    for (i = 0; i < na; i++)
    {
        int found = 0;
        for (j = 0; j < nb; j++)
            if (strcasecmp(a->h_aliases[i], b->h_aliases[j]) == 0)
                { found = 1; break; }
        if (!found) return 0;
    }
    return 1;
}

static int hostent_addr_sets_equal(const struct hostent *a,
                                   const struct hostent *b)
{
    int na = 0, nb = 0, i, j;
    while (a->h_addr_list[na]) na++;
    while (b->h_addr_list[nb]) nb++;
    if (na != nb || a->h_length != b->h_length) return 0;
    for (i = 0; i < na; i++)
    {
        int found = 0;
        for (j = 0; j < nb; j++)
            if (memcmp(a->h_addr_list[i], b->h_addr_list[j],
                       (size_t)a->h_length) == 0)
                { found = 1; break; }
        if (!found) return 0;
    }
    return 1;
}

static int parse_one(const em_wire *w, int qtype, struct hostent **out)
{
    if (qtype == 1)
        return ares_parse_a_reply(w->msg, w->msglen, out, NULL, NULL);
    return ares_parse_aaaa_reply(w->msg, w->msglen, out, NULL, NULL);
}

static int run_metamorphic_group(run_stats *s)
{
    run_stats local;
    int n, variant_runs = 0, variant_fail = 0;
    int reorder_seen = 0, casing_seen = 0, extra_seen = 0;
    int identity_mismatch = 0, addrset_mismatch = 0, qname_mismatch = 0;

    /* Keep this group's bookkeeping local so the model coverage in the
     * shared stats object is preserved across groups. */
    memset(&local, 0, sizeof(local));
    s = &local;
    for (n = 0; n < EM_CASES; n++)
    {
        em_model m = em_generate(MASTER_SEED, n);
        em_wire base, alt;
        struct hostent *hb = NULL, *ha = NULL;
        char *qbase = NULL, *qalt = NULL;
        long elen = 0;
        int v;

        if (em_build(&m, EM_LAYOUT_MULTI, 0, 0, 0, &base) != 0)
        {
            s->build_fail++;
            continue;
        }
        if (parse_one(&base, m.qtype, &hb) != ARES_SUCCESS)
        {
            variant_fail++;
            continue;
        }
        if (ares_expand_name(base.msg + base.names[0].offset, base.msg,
                             base.msglen, &qbase, &elen) != ARES_SUCCESS)
        {
            variant_fail++;
            ares_free_hostent(hb);
            continue;
        }

        /* Axis 1: legal terminal-RR order adjustments. */
        for (v = 1; v <= 2; v++)
        {
            if (em_build(&m, EM_LAYOUT_MULTI, v, 0, 0, &alt) != 0)
            { s->build_fail++; continue; }
            variant_runs++;
            if (parse_one(&alt, m.qtype, &ha) != ARES_SUCCESS)
            { variant_fail++; continue; }
            reorder_seen++;
            if (!hostent_name_sets_equal(hb, ha)) identity_mismatch++;
            if (!hostent_addr_sets_equal(hb, ha)) addrset_mismatch++;
            ares_free_hostent(ha);
            ha = NULL;
        }

        /* Axis 2: query-name casing changes. */
        for (v = 1; v <= 2; v++)
        {
            if (em_build(&m, EM_LAYOUT_MULTI, 0, v, 0, &alt) != 0)
            { s->build_fail++; continue; }
            variant_runs++;
            if (parse_one(&alt, m.qtype, &ha) != ARES_SUCCESS)
            { variant_fail++; continue; }
            casing_seen++;
            if (!hostent_name_sets_equal(hb, ha)) identity_mismatch++;
            if (!hostent_addr_sets_equal(hb, ha)) addrset_mismatch++;
            if (ares_expand_name(alt.msg + alt.names[0].offset, alt.msg,
                                 alt.msglen, &qalt, &elen) != ARES_SUCCESS)
                variant_fail++;
            else
            {
                if (strcasecmp(qbase, qalt) != 0) qname_mismatch++;
                ares_free_string(qalt);
            }
            ares_free_hostent(ha);
            ha = NULL;
        }

        /* Axis 3: irrelevant extra record in three legal placements. */
        if (m.has_extra)
        {
            for (v = 0; v <= 2; v++)
            {
                if (em_build(&m, EM_LAYOUT_MULTI, 0, 0, v, &alt) != 0)
                { s->build_fail++; continue; }
                variant_runs++;
                if (parse_one(&alt, m.qtype, &ha) != ARES_SUCCESS)
                { variant_fail++; continue; }
                extra_seen++;
                if (!hostent_name_sets_equal(hb, ha)) identity_mismatch++;
                if (!hostent_addr_sets_equal(hb, ha)) addrset_mismatch++;
                ares_free_hostent(ha);
                ha = NULL;
            }
        }
        ares_free_string(qbase);
        ares_free_hostent(hb);
    }

    printf("metamorphic variants=%d failures=%d reorder=%d casing=%d "
           "extra=%d identity_diff=%d addrset_diff=%d qname_diff=%d\n",
           variant_runs, variant_fail, reorder_seen, casing_seen,
           extra_seen, identity_mismatch, addrset_mismatch, qname_mismatch);

    TASK_CHECK(s->build_fail == 0);
    TASK_CHECK(variant_runs >= EM_CASES * 4);
    TASK_CHECK(variant_fail == 0);
    TASK_CHECK(reorder_seen > 0 && casing_seen > 0 && extra_seen > 0);
    TASK_CHECK(identity_mismatch == 0);
    TASK_CHECK(addrset_mismatch == 0);
    TASK_CHECK(qname_mismatch == 0);
    /* The order permutation must be a real bijection over the set. */
    {
        int k, seen[EM_MAX_ADDRS] = {0};
        for (k = 0; k < EM_MAX_ADDRS; k++)
        {
            int p = em_addr_perm(2, k, EM_MAX_ADDRS);
            TASK_CHECK(p >= 0 && p < EM_MAX_ADDRS && !seen[p]);
            seen[p] = 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Group 3: replay determinism and resource/protocol boundaries.      */
/* ------------------------------------------------------------------ */

static int run_replay_group(run_stats *s)
{
    int n, lay, root_hits = 0, trunc_parse_fail = 0, trunc_expand_fail = 0;
    int depth_violations = 0, name_limit_violations = 0, msg_limit_violations = 0;
    int first_msglen[3], rerun_msglen[3];
    em_model sample;
    em_wire sample_wire[3];
    em_wire root_wire;
    int have_root_wire = 0;

    /* This group keeps no aggregate stats; leave the shared object alone. */
    (void)s;
    memset(&root_wire, 0, sizeof(root_wire));

    /* Replay 1: regenerating the same (seed,n) yields an identical model. */
    for (n = 0; n < EM_CASES; n++)
    {
        em_model a = em_generate(MASTER_SEED, n);
        em_model b = em_generate(MASTER_SEED, n);
        TASK_CHECK(memcmp(&a, &b, sizeof(a)) == 0);
    }

    /* Replay 2: rebuild every layout twice; bytes must be identical and
     * the rerun over the whole seed sequence must reproduce lengths. */
    for (n = 0; n < EM_CASES; n++)
    {
        em_model m = em_generate(MASTER_SEED, n);
        for (lay = 0; lay < 3; lay++)
        {
            em_wire w1, w2;
            int i;
            TASK_CHECK(em_build(&m, (em_layout)lay, 0, 0, 0, &w1) == 0);
            TASK_CHECK(em_build(&m, (em_layout)lay, 0, 0, 0, &w2) == 0);
            TASK_CHECK(w1.msglen == w2.msglen);
            TASK_CHECK(memcmp(w1.msg, w2.msg, (size_t)w1.msglen) == 0);
            TASK_CHECK(w1.ptr_depth == w2.ptr_depth);

            if (w1.msglen > 65535 || w1.msglen > EM_MSGMAX)
                msg_limit_violations++;
            for (i = 0; i < w1.nnames; i++)
            {
                const em_wname *wn = &w1.names[i];
                const unsigned char *p, *end;
                if (wn->enclen > EM_NAMEMAX)
                    name_limit_violations++;
                if (em_name_wirelen(&wn->identity) > EM_NAMEMAX)
                    name_limit_violations++;
                if (wn->ptrs > 50) /* c-ares MAX_INDIRS */
                    depth_violations++;
                p = w1.msg + wn->offset;
                end = p + wn->enclen;
                while (p < end && *p != 0 && (*p & 0xc0) != 0xc0)
                    p += 1 + *p;
                if (p < end && (*p & 0xc0) == 0xc0)
                {
                    int off = ((p[0] & 0x3f) << 8) | p[1];
                    if (off >= wn->offset || off >= w1.msglen)
                        depth_violations++;
                }
                if (em_name_is_root(&wn->identity))
                {
                    root_hits++;
                    if (!have_root_wire)
                    {
                        root_wire = w1;
                        have_root_wire = 1;
                    }
                }
            }
            if (n == 7)
            {
                sample = m;
                sample_wire[lay] = w1;
                first_msglen[lay] = w1.msglen;
            }
        }
    }

    /* Replay 3: rerun the fixed sequence later and compare. */
    for (lay = 0; lay < 3; lay++)
    {
        em_model m = em_generate(MASTER_SEED, 7);
        em_wire w;
        TASK_CHECK(em_build(&m, (em_layout)lay, 0, 0, 0, &w) == 0);
        rerun_msglen[lay] = w.msglen;
        TASK_CHECK(rerun_msglen[lay] == first_msglen[lay]);
    }

    /* Root-name expand semantics on a real occurrence: ares_expand_name
     * renders the root as "" and still reports the true enclen. */
    TASK_CHECK(root_hits > 0);
    TASK_CHECK(have_root_wire);
    {
        int found_root = 0, i;
        for (i = 0; i < root_wire.nnames && !found_root; i++)
        {
            em_wname *wn = &root_wire.names[i];
            char *rs = NULL;
            long rlen = 0;
            if (!em_name_is_root(&wn->identity)) continue;
            TASK_CHECK(ares_expand_name(root_wire.msg + wn->offset,
                                        root_wire.msg, root_wire.msglen,
                                        &rs, &rlen) == ARES_SUCCESS);
            TASK_CHECK(rs != NULL && rs[0] == '\0');
            TASK_CHECK(rlen == wn->enclen);
            ares_free_string(rs);
            found_root = 1;
        }
        TASK_CHECK(found_root);
    }

    /* Boundary: a truncated message must be rejected, never interpreted.
     * Use a case with no additional-section record so the cut lands inside
     * an answer RR that the parser actually walks. */
    {
        em_model bm;
        em_wire bw;
        struct hostent *h = NULL;
        char *s = NULL;
        long elen = 0;
        int bn = 0;
        do
        {
            bm = em_generate(MASTER_SEED, bn++);
        }
        while (em_name_is_root(&bm.qname) && bn < EM_CASES);
        bm.has_extra = 0;
        TASK_CHECK(em_build(&bm, EM_LAYOUT_UNCOMPRESSED, 0, 0, 0, &bw) == 0);
        TASK_CHECK(bw.msglen > 2);
        TASK_CHECK(parse_one(&bw, bm.qtype, &h) == ARES_SUCCESS);
        if (h) ares_free_hostent(h);
        h = NULL;
        /* drop one trailing byte: the final answer RR now overruns */
        if (bm.qtype == 1)
            trunc_parse_fail =
                ares_parse_a_reply(bw.msg, bw.msglen - 1, &h, NULL, NULL);
        else
            trunc_parse_fail =
                ares_parse_aaaa_reply(bw.msg, bw.msglen - 1, &h, NULL, NULL);
        if (h) ares_free_hostent(h);
        TASK_CHECK(trunc_parse_fail != ARES_SUCCESS);
        /* expand a name whose buffer ends before the name completes */
        trunc_expand_fail = ares_expand_name(
            bw.msg + bw.names[0].offset, bw.msg,
            bw.names[0].offset + 1, &s, &elen);
        if (s) ares_free_string(s);
        TASK_CHECK(trunc_expand_fail != ARES_SUCCESS);
        (void)sample; (void)sample_wire;
    }

    /* Predetermined logic-difference simulator: oracle must catch it
     * and keep the first case, without touching upstream code. */
    TASK_CHECK(em_diff_simulator() == 0);

    TASK_CHECK(msg_limit_violations == 0);
    TASK_CHECK(name_limit_violations == 0);
    TASK_CHECK(depth_violations == 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Coverage summary printed once after the model group.               */
/* ------------------------------------------------------------------ */

static void print_coverage(const run_stats *s)
{
    int k;
    puts("COVERAGE");
    printf("  cases=%d layouts=%d builds=%d skipped=%d\n",
           s->cases, s->layouts, s->builds, s->skipped);
    for (k = 1; k <= EM_MAX_CHAIN; k++)
        printf("  chain_len_%d=%d\n", k, s->chain_len[k]);
    printf("  A=%d AAAA=%d root_names=%d\n",
           s->a_replies, s->aaaa_replies, s->root_names);
    printf("  ptr_depth_max=%d compressed_names=%d multilevel_names=%d\n",
           s->ptr_depth_max, s->ptr_names, s->multilevel_names);
    printf("  uncompressed_layout_pointers=%d extra_records=%d\n",
           s->uncompressed_pointers, s->extra_records);
}

int task_cases(const char *group)
{
    static int initialized = 0;
    static run_stats model_stats;
    if (!initialized)
    {
        if (ares_library_init(ARES_LIB_INIT_ALL) != ARES_SUCCESS)
            return 1;
        initialized = 1;
    }

    if (strcmp(group, "model") == 0)
    {
        if (run_model_group(&model_stats) != 0)
        {
            print_coverage(&model_stats);
            TASK_CHECK(0);
        }
        print_coverage(&model_stats);
        TASK_CHECK(model_stats.cases == EM_CASES);
        TASK_CHECK(model_stats.layouts == EM_CASES * 3);
        TASK_CHECK(model_stats.builds == EM_CASES * 3);
        TASK_CHECK(model_stats.skipped == 0);
        TASK_CHECK(model_stats.parse_fail == 0);
        TASK_CHECK(model_stats.expand_fail == 0);
        TASK_CHECK(model_stats.uncompressed_pointers == 0);
        {
            int k, covered = 1;
            for (k = 1; k <= EM_MAX_CHAIN; k++)
                if (model_stats.chain_len[k] == 0) covered = 0;
            TASK_CHECK(covered);
        }
        TASK_CHECK(model_stats.a_replies > 0 && model_stats.aaaa_replies > 0);
        TASK_CHECK(model_stats.root_names > 0);
        TASK_CHECK(model_stats.ptr_depth_max >= 2);
        TASK_CHECK(model_stats.multilevel_names > 0);
        TASK_CHECK(model_stats.ptr_names > 0);
        TASK_CHECK(em_diff_simulator() == 0);
        return 0;
    }
    if (strcmp(group, "metamorphic") == 0)
        return run_metamorphic_group(&model_stats);
    if (strcmp(group, "replay") == 0)
        return run_replay_group(&model_stats);
    return 1;
}
