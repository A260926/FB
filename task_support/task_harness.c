#include "task_harness.h"
#include "task_gen.h"
#include "task_enc.h"
#include "task_render.h"
#include "task_meta.h"
#include <stdio.h>
#include <string.h>

/* ---- FNV-1a fingerprinting ------------------------------------------------ */

static uint64_t fnv_u64(uint64_t h, uint64_t v) {
    int i;
    for (i = 0; i < 8; i++) {
        h ^= (uint8_t)(v & 0xFFu);
        h *= UINT64_C(1099511628211);
        v >>= 8;
    }
    return h;
}

static uint64_t fp_eng(uint64_t h, const TaskEngResult *e) {
    int g;
    h = fnv_u64(h, (uint64_t)e->status);
    h = fnv_u64(h, (uint64_t)(unsigned)e->start);
    h = fnv_u64(h, (uint64_t)(unsigned)e->end);
    h = fnv_u64(h, (uint64_t)(unsigned)e->ncaps);
    for (g = 0; g <= TASK_MAX_CAPS; g++) {
        h = fnv_u64(h, (uint64_t)e->cap[g].set);
        h = fnv_u64(h, (uint64_t)e->cap[g].start);
        h = fnv_u64(h, (uint64_t)e->cap[g].end);
    }
    return h;
}

static uint64_t fp_orc(uint64_t h, const TaskOracleResult *o) {
    int g;
    h = fnv_u64(h, (uint64_t)o->matched);
    h = fnv_u64(h, (uint64_t)(unsigned)o->start);
    h = fnv_u64(h, (uint64_t)(unsigned)o->end);
    for (g = 0; g <= TASK_MAX_CAPS; g++) {
        h = fnv_u64(h, (uint64_t)o->cap[g].set);
        h = fnv_u64(h, (uint64_t)o->cap[g].start);
        h = fnv_u64(h, (uint64_t)o->cap[g].end);
    }
    return h;
}

void task_report_init(TaskReport *r, uint64_t seed) {
    memset(r, 0, sizeof(*r));
    r->seed = seed;
    r->diff_case = -1;
    r->fingerprint = UINT64_C(1469598103934665603);
}

/* Test-only fault injection state. -1 means no injection; this never touches
 * PCRE2, only the harness's in-memory oracle copy. */
static int inject_fault_case = -1;
void task_test_inject_fault(int caseidx) { inject_fault_case = caseidx; }

/* ---- comparison ----------------------------------------------------------- */

int task_compare_result(const TaskOracleResult *o, const TaskEngResult *e,
                        char *why, int whycap) {
    int g;
    if (e->status == TASK_ENG_COMPILE_ERROR) {
        snprintf(why, whycap, "engine compile error %d not predicted by oracle",
                 e->errcode);
        return 1;
    }
    if (e->status == TASK_ENG_RUNTIME_ERROR) {
        snprintf(why, whycap, "engine runtime error %d", e->errcode);
        return 1;
    }
    if (e->status == TASK_ENG_BAD_BOUNDARY) {
        snprintf(why, whycap, "offset of capture %d lands inside a code point",
                 e->errcode);
        return 1;
    }
    if (!o->matched) {
        if (e->status != TASK_ENG_NOMATCH) {
            snprintf(why, whycap, "oracle says no match but engine matched "
                     "[%d,%d)", e->start, e->end);
            return 1;
        }
        return 0;
    }
    if (e->status != TASK_ENG_MATCH) {
        snprintf(why, whycap, "oracle matched [%d,%d) but engine status=%d",
                 o->start, o->end, (int)e->status);
        return 1;
    }
    if (o->start != e->start || o->end != e->end) {
        snprintf(why, whycap, "whole-match span oracle [%d,%d) != engine "
                 "[%d,%d)", o->start, o->end, e->start, e->end);
        return 1;
    }
    if (o->ncaps != e->ncaps) {
        snprintf(why, whycap, "capture count oracle %d != engine %d",
                 o->ncaps, e->ncaps);
        return 1;
    }
    for (g = 1; g <= o->ncaps; g++) {
        if (o->cap[g].set != e->cap[g].set) {
            snprintf(why, whycap, "capture %d participation oracle=%d engine=%d",
                     g, o->cap[g].set, e->cap[g].set);
            return 1;
        }
        if (o->cap[g].set &&
            (o->cap[g].start != e->cap[g].start ||
             o->cap[g].end != e->cap[g].end)) {
            snprintf(why, whycap,
                     "capture %d span oracle [%d,%d) != engine [%d,%d)", g,
                     o->cap[g].start, o->cap[g].end,
                     e->cap[g].start, e->cap[g].end);
            return 1;
        }
    }
    return 0;
}

/* ---- one case end to end -------------------------------------------------- */

typedef struct {
    TaskOracleResult oracle;
    TaskEngResult eng[3];   /* widths 8,16,32 at indices 0..2 */
    int oracle_code;
} CaseResult;

static int run_case(const TaskNode *tree,
                    const uint32_t *subj, int nsubj, CaseResult *cr,
                    TaskReport *rep) {
    static uint32_t pat[TASK_MAX_PATLEN + 1];
    TaskText text;
    int patlen, rc, w;
    patlen = task_render_pattern(tree, pat, (int)(sizeof(pat)/sizeof(pat[0])));
    if (patlen < 0) return -1;
    if (task_text_encode(&text, subj, nsubj) < 0) return -2;
    rc = task_oracle_run(tree, task_tree_caps(tree), subj, nsubj, &cr->oracle);
    cr->oracle_code = rc;
    if (rc == TASK_ORACLE_LIMIT) return -3;
    for (w = 0; w < 3; w++) {
        cr->eng[w] = task_engine_run(w == 0 ? 8 : w == 1 ? 16 : 32,
                                     pat, patlen, &text);
        rep->fingerprint = fp_eng(rep->fingerprint, &cr->eng[w]);
    }
    rep->fingerprint = fp_orc(rep->fingerprint, &cr->oracle);
    return 0;
}

static const char *eng_status_name(TaskEngStatus s) {
    switch (s) {
    case TASK_ENG_MATCH: return "MATCH";
    case TASK_ENG_NOMATCH: return "NOMATCH";
    case TASK_ENG_COMPILE_ERROR: return "COMPILE_ERROR";
    case TASK_ENG_RUNTIME_ERROR: return "RUNTIME_ERROR";
    case TASK_ENG_BAD_BOUNDARY: return "BAD_BOUNDARY";
    }
    return "?";
}

static void print_eng(const char *label, const TaskEngResult *e) {
    int g;
    printf("    %s %s", label, eng_status_name(e->status));
    if (e->status == TASK_ENG_MATCH) {
        printf(" span=[%d,%d) caps={", e->start, e->end);
        for (g = 1; g <= e->ncaps; g++) {
            if (!e->cap[g].set) printf("%s-", g == 1 ? "" : ",");
            else printf("%s[%d,%d)%s", g == 1 ? "" : ",",
                        e->cap[g].start, e->cap[g].end,
                        e->cap[g].start == e->cap[g].end ? "(empty)" : "");
        }
        printf("}");
    } else if (e->status != TASK_ENG_NOMATCH) {
        printf(" errcode=%d", e->errcode);
    }
    printf("\n");
}

int task_run_suite(uint64_t seed, int ncases, int verbose, TaskReport *rep) {
    TaskArena arena;
    int ci, w, failures = 0;
    task_report_init(rep, seed);
    rep->cases = ncases;
    task_arena_init(&arena, seed);

    if (verbose)
        printf("== property suite seed=%llu cases=%d ==\n",
               (unsigned long long)seed, ncases);

    for (ci = 0; ci < ncases; ci++) {
        TaskNode *tree;
        uint32_t subj[TASK_MAX_SUBJ];
        int nsubj, ncaps, depth, g;
        CaseResult cr;
        char ast[TASK_DIFF_MAX], subjs[TASK_DIFF_MAX];
        char why[256];

        tree = task_gen_tree(&arena);
        if (!tree) { fprintf(stderr, "generator: arena exhausted\n"); return 2; }
        nsubj = task_gen_subject(&arena, tree, subj,
                                 (int)(sizeof(subj)/sizeof(subj[0])));
        if (nsubj < 0) { fprintf(stderr, "generator: subject failure\n"); return 2; }

        if (run_case(tree, subj, nsubj, &cr, rep) < 0) {
            fprintf(stderr, "case %d: harness resource failure\n", ci);
            return 2;
        }
        if (inject_fault_case == ci) {
            /* Predetermined divergence injected into our oracle copy. */
            cr.oracle.matched = cr.oracle.matched ? 0 : 1;
            inject_fault_case = -1;
        }

        ncaps = task_tree_caps(tree);
        depth = task_tree_depth(tree);
        if (depth >= 1 && depth <= TASK_MAX_DEPTH) rep->depth_hist[depth]++;
        if (ncaps <= TASK_MAX_CAPS) rep->cap_hist[ncaps]++;
        if (task_tree_has_supplementary(tree)) rep->with_supp++;
        for (g = 0; g < nsubj; g++)
            if (task_is_supplementary(subj[g])) { rep->with_supp++; break; }
        if (task_tree_has_kind(tree, TASK_ALT)) rep->with_alt++;
        if (task_tree_has_kind(tree, TASK_REP)) rep->with_rep++;
        for (g = 1; g <= ncaps; g++) {
            if (!cr.oracle.cap[g].set) rep->with_unset_capture++;
            else if (cr.oracle.cap[g].start == cr.oracle.cap[g].end)
                rep->with_empty_participating++;
        }
        if (cr.oracle.matched) rep->matched_cases++;
        else rep->nomatch_cases++;
        if (cr.oracle_code == TASK_ORACLE_LIMIT) rep->oracle_limits++;

        for (w = 0; w < 3; w++) {
            if (cr.eng[w].status == TASK_ENG_COMPILE_ERROR) rep->compile_errors++;
            if (cr.eng[w].status == TASK_ENG_RUNTIME_ERROR) rep->runtime_errors++;
            if (cr.eng[w].status == TASK_ENG_BAD_BOUNDARY) rep->bad_boundaries++;
        }

        if (verbose) {
            task_serialize_ast(tree, ast, sizeof(ast));
            task_serialize_scalars(subj, nsubj, subjs, sizeof(subjs));
            printf("case %d seed=%llu\n  ast: %s\n  subj: %s\n  oracle: %s",
                   ci, (unsigned long long)seed, ast,
                   nsubj ? subjs : "(empty)",
                   cr.oracle.matched ? "MATCH" : "NOMATCH");
            if (cr.oracle.matched) {
                printf(" span=[%d,%d) caps={", cr.oracle.start, cr.oracle.end);
                for (g = 1; g <= ncaps; g++) {
                    if (!cr.oracle.cap[g].set)
                        printf("%s-", g == 1 ? "" : ",");
                    else
                        printf("%s[%d,%d)%s", g == 1 ? "" : ",",
                               cr.oracle.cap[g].start, cr.oracle.cap[g].end,
                               cr.oracle.cap[g].start == cr.oracle.cap[g].end
                                   ? "(empty)" : "");
                }
                printf("}");
            }
            printf("\n");
            print_eng("width8 :", &cr.eng[0]);
            print_eng("width16:", &cr.eng[1]);
            print_eng("width32:", &cr.eng[2]);
        }

        for (w = 0; w < 3; w++) {
            if (task_compare_result(&cr.oracle, &cr.eng[w], why, sizeof(why))) {
                rep->diffs++;
                if (rep->diff_case < 0) {
                    rep->diff_case = ci;
                    rep->diff_seed = seed;
                    task_serialize_ast(tree, ast, sizeof(ast));
                    task_serialize_scalars(subj, nsubj, subjs, sizeof(subjs));
                    snprintf(rep->diff_text, sizeof(rep->diff_text),
                             "FIRST DIFF case=%d seed=%llu width=%d\n"
                             "  ast : %s\n  subj: %s\n  why : %s\n",
                             ci, (unsigned long long)seed,
                             w == 0 ? 8 : w == 1 ? 16 : 32, ast,
                             nsubj ? subjs : "(empty)", why);
                }
                failures++;
                if (verbose)
                    printf("  >>> width %d differs: %s\n",
                           w == 0 ? 8 : w == 1 ? 16 : 32, why);
            }
        }
    }

    if (verbose) {
        if (rep->diff_case >= 0)
            printf("%s", rep->diff_text);
        else
            printf("no differences across %d cases x 3 widths\n", ncases);
    }
    return failures ? 1 : 0;
}

/* ---- metamorphic transforms ----------------------------------------------- */

void task_meta_report_init(TaskMetaReport *r, uint64_t seed) {
    memset(r, 0, sizeof(*r));
    r->seed = seed;
    r->diff_case = -1;
}

/* Compare captures through an explicit mapping: every old capture must appear
 * at new cap number map->num[i] with identical participation and span. */
static int mapped_caps_equal(const TaskCap *oldc, int old_n,
                             const TaskCap *newc, int new_n,
                             const TaskMetaMap *map, char *why, int whycap) {
    int i, j;
    for (i = 1; i <= old_n; i++) {
        j = map->num[i];
        if (j < 1 || j > new_n) {
            snprintf(why, whycap, "old cap %d has no mapped new capture", i);
            return 1;
        }
        if (oldc[i].set != newc[j].set) {
            snprintf(why, whycap,
                     "mapped cap %d->%d participation %d != %d",
                     i, j, oldc[i].set, newc[j].set);
            return 1;
        }
        if (oldc[i].set && (oldc[i].start != newc[j].start ||
                            oldc[i].end != newc[j].end)) {
            snprintf(why, whycap,
                     "mapped cap %d->%d span [%d,%d) != [%d,%d)", i, j,
                     oldc[i].start, oldc[i].end, newc[j].start, newc[j].end);
            return 1;
        }
    }
    return 0;
}

static int record_meta_diff(TaskMetaReport *r, int ci, TaskMetaKind kind,
                            const char *why, const TaskNode *tree,
                            const uint32_t *subj, int nsubj) {
    char ast[TASK_DIFF_MAX], subjs[TASK_DIFF_MAX];
    static const char *kn[] = {"ADD_GROUPS", "REMOVE_GROUPS",
                               "CAP_WRAP", "RENAME"};
    r->failures++;
    if (r->diff_case < 0) {
        r->diff_case = ci;
        task_serialize_ast(tree, ast, sizeof(ast));
        task_serialize_scalars(subj, nsubj, subjs, sizeof(subjs));
        snprintf(r->diff_text, sizeof(r->diff_text),
                 "FIRST META DIFF case=%d transform=%s\n  ast : %s\n"
                 "  subj: %s\n  why : %s\n", ci, kn[kind], ast,
                 nsubj ? subjs : "(empty)", why);
    }
    return 0;
}

int task_run_metamorphic(uint64_t seed, int ncases, int verbose,
                         TaskMetaReport *r) {
    TaskArena gen, work;
    int ci;
    task_meta_report_init(r, seed);
    task_arena_init(&gen, seed);
    if (verbose)
        printf("== metamorphic suite seed=%llu cases=%d ==\n",
               (unsigned long long)seed, ncases);

    for (ci = 0; ci < ncases; ci++) {
        TaskNode *tree;
        uint32_t subj[TASK_MAX_SUBJ];
        int nsubj, t;
        tree = task_gen_tree(&gen);
        if (!tree) return 2;
        nsubj = task_gen_subject(&gen, tree, subj,
                                 (int)(sizeof(subj)/sizeof(subj[0])));
        if (nsubj < 0) return 2;

        for (t = 0; t < 4; t++) {
            TaskNode *t2;
            TaskMetaMap map;
            TaskOracleResult o0, o2;
            TaskEngResult e2[3];
            static uint32_t pat0[TASK_MAX_PATLEN + 1];
            static uint32_t pat2[TASK_MAX_PATLEN + 1];
            TaskText text;
            int patlen0, patlen2, rc0, rc2, w, g;
            char why[256];

            task_arena_init(&work, seed + 1000003u + (uint64_t)ci * 7u + t);
            if (t == TASK_META_ADD_GROUPS)
                t2 = task_meta_add_groups(tree, &work, &map);
            else if (t == TASK_META_REMOVE_GROUPS)
                t2 = task_meta_remove_groups(tree, &work, &map);
            else if (t == TASK_META_CAP_WRAP)
                t2 = task_meta_wrap_capture(tree, &work, &map);
            else
                t2 = task_meta_rename(tree, &work, &map);
            if (!t2) { r->not_applicable[t]++; continue; }
            r->applied[t]++;

            patlen0 = task_render_pattern(tree, pat0,
                        (int)(sizeof(pat0)/sizeof(pat0[0])));
            patlen2 = task_render_pattern(t2, pat2,
                        (int)(sizeof(pat2)/sizeof(pat2[0])));
            if (patlen0 < 0 || patlen2 < 0 ||
                task_text_encode(&text, subj, nsubj) < 0)
                return 2;

            rc0 = task_oracle_run(tree, task_tree_caps(tree),
                                  subj, nsubj, &o0);
            rc2 = task_oracle_run(t2, task_tree_caps(t2),
                                  subj, nsubj, &o2);
            if (rc0 == TASK_ORACLE_LIMIT || rc2 == TASK_ORACLE_LIMIT)
                return 2;

            /* whole-match language must be invariant, oracle level */
            if (o0.matched != o2.matched ||
                (o0.matched && (o0.start != o2.start || o0.end != o2.end))) {
                snprintf(why, sizeof(why),
                         "whole-match span changed under transform");
                record_meta_diff(r, ci, (TaskMetaKind)t, why, tree,
                                 subj, nsubj);
                continue;
            }
            if (mapped_caps_equal(o0.cap, o0.ncaps, o2.cap, o2.ncaps,
                                  &map, why, sizeof(why))) {
                record_meta_diff(r, ci, (TaskMetaKind)t, why, tree,
                                 subj, nsubj);
                continue;
            }
            /* a wrapping capture must record exactly the whole-match span */
            if (t == TASK_META_CAP_WRAP && o2.matched &&
                (!o2.cap[1].set || o2.cap[1].start != o2.start ||
                 o2.cap[1].end != o2.end)) {
                snprintf(why, sizeof(why),
                         "wrap capture does not equal whole match");
                record_meta_diff(r, ci, (TaskMetaKind)t, why, tree,
                                 subj, nsubj);
                continue;
            }

            for (w = 0; w < 3; w++) {
                e2[w] = task_engine_run(w == 0 ? 8 : w == 1 ? 16 : 32,
                                        pat2, patlen2, &text);
                if (e2[w].status == TASK_ENG_COMPILE_ERROR ||
                    e2[w].status == TASK_ENG_RUNTIME_ERROR ||
                    e2[w].status == TASK_ENG_BAD_BOUNDARY) {
                    snprintf(why, sizeof(why), "engine status=%d code=%d",
                             (int)e2[w].status, e2[w].errcode);
                    record_meta_diff(r, ci, (TaskMetaKind)t, why, tree,
                                     subj, nsubj);
                    break;
                }
                if (o2.matched && (o2.start != e2[w].start ||
                                   o2.end != e2[w].end)) {
                    snprintf(why, sizeof(why),
                             "engine whole-match disagrees with oracle");
                    record_meta_diff(r, ci, (TaskMetaKind)t, why, tree,
                                     subj, nsubj);
                    break;
                }
                if (o2.matched) {
                    /* transformed oracle vs engine share the same numbering */
                    TaskMetaMap id;
                    int k;
                    memset(&id, 0, sizeof(id));
                    id.old_ncaps = id.new_ncaps = o2.ncaps;
                    for (k = 0; k <= TASK_MAX_CAPS; k++) id.num[k] = k;
                    if (mapped_caps_equal(o2.cap, o2.ncaps, e2[w].cap,
                                          e2[w].ncaps, &id,
                                          why, sizeof(why))) {
                        record_meta_diff(r, ci, (TaskMetaKind)t, why, tree,
                                         subj, nsubj);
                        break;
                    }
                }
            }

            /* rename: the new name must resolve to the same number, and the
             * old name must be gone, in every real width */
            if (t == TASK_META_RENAME) {
                for (g = 1; g <= map.old_ncaps; g++) {
                    if (!map.old_name[g][0]) continue;
                    for (w = 0; w < 3; w++) {
                        int width = w == 0 ? 8 : w == 1 ? 16 : 32;
                        if (task_engine_name_resolves(width, pat2, patlen2,
                                map.new_name[g]) != g ||
                            task_engine_name_resolves(width, pat2, patlen2,
                                map.old_name[g]) != -1) {
                            snprintf(why, sizeof(why),
                                     "rename resolution wrong for cap %d", g);
                            record_meta_diff(r, ci, (TaskMetaKind)t, why,
                                             tree, subj, nsubj);
                            break;
                        }
                    }
                }
            }
        }
    }
    if (verbose) {
        if (r->diff_case >= 0) printf("%s", r->diff_text);
        else printf("metamorphic: applied=[%d,%d,%d,%d] n/a=[%d,%d,%d,%d]\n",
                    r->applied[0], r->applied[1], r->applied[2], r->applied[3],
                    r->not_applicable[0], r->not_applicable[1],
                    r->not_applicable[2], r->not_applicable[3]);
    }
    return r->failures ? 1 : 0;
}

/* ---- predetermined logical-difference simulator --------------------------- */

int task_simulate_logical_difference(void) {
    /* A fixed, hand-built case: pattern (a|b)c with one capture. */
    TaskArena a;
    TaskNode *alt, *cap, *cat, *lita, *litb, *litc;
    uint32_t subj[3] = { 0x61u, 0x63u, 0x78u };   /* "acx" */
    static uint32_t pat[TASK_MAX_PATLEN + 1];
    TaskText text;
    TaskOracleResult oracle, corrupted;
    TaskEngResult eng;
    char why[256];
    int patlen;

    task_arena_init(&a, 424242u);
    task_arena_reset(&a);
    lita = task_alloc_node(&a); lita->kind = TASK_LIT; lita->cp = 0x61u;
    litb = task_alloc_node(&a); litb->kind = TASK_LIT; litb->cp = 0x62u;
    litc = task_alloc_node(&a); litc->kind = TASK_LIT; litc->cp = 0x63u;
    alt = task_alloc_node(&a); alt->kind = TASK_ALT;
    alt->ch[0] = lita; alt->ch[1] = litb; alt->nch = 2;
    cap = task_alloc_node(&a); cap->kind = TASK_CAP; cap->capno = 1;
    cap->ch[0] = alt; cap->nch = 1;
    cat = task_alloc_node(&a); cat->kind = TASK_CAT;
    cat->ch[0] = cap; cat->ch[1] = litc; cat->nch = 2;

    patlen = task_render_pattern(cat, pat,
                (int)(sizeof(pat)/sizeof(pat[0])));
    if (patlen < 0) return 1;
    if (task_text_encode(&text, subj, 2) < 0) return 1;
    if (task_oracle_run(cat, 1, subj, 2, &oracle) != TASK_ORACLE_OK)
        return 1;
    if (!oracle.matched || oracle.start != 0 || oracle.end != 2 ||
        !oracle.cap[1].set || oracle.cap[1].start != 0 ||
        oracle.cap[1].end != 1)
        return 1;
    eng = task_engine_run(8, pat, patlen, &text);
    if (task_compare_result(&oracle, &eng, why, sizeof(why))) return 1;

    /* Sanity: the honest results agree. Now inject a predetermined logical
     * difference (shift the capture end) and require the comparator to flag
     * it. This simulates an oracle/engine divergence without modifying any
     * upstream PCRE2 source. */
    corrupted = oracle;
    corrupted.cap[1].end = 2;                    /* claim capture = "ac" */
    if (!task_compare_result(&corrupted, &eng, why, sizeof(why))) return 1;

    /* Second, distinct corruption: pretend there was no match at all. */
    corrupted = oracle;
    corrupted.matched = 0;
    if (!task_compare_result(&corrupted, &eng, why, sizeof(why))) return 1;

    /* Third: flip participation so an unset/empty distinction is lost. */
    corrupted = oracle;
    corrupted.cap[1].set = 0;
    corrupted.cap[1].start = corrupted.cap[1].end = TASK_CAP_UNSET;
    if (!task_compare_result(&corrupted, &eng, why, sizeof(why))) return 1;
    return 0;
}

/* ---- resource and replay boundary probes ---------------------------------- */

int task_probe_resource_bounds(void) {
    TaskArena a;
    TaskReport r1, r2;
    int ci, max_depth = 0, max_caps = 0, max_subj = 0;
    int saw_surrogate = 0;
    const int probe_cases = 120;

    /* 1. Fixed-seed replay must be byte-identical: same fingerprint. */
    if (task_run_suite(987654321u, probe_cases, 0, &r1)) return 1;
    if (task_run_suite(987654321u, probe_cases, 0, &r2)) return 1;
    if (r1.fingerprint != r2.fingerprint) return 1;
    if (r1.matched_cases != r2.matched_cases ||
        r1.nomatch_cases != r2.nomatch_cases || r1.diffs != r2.diffs)
        return 1;

    /* 2. A different seed must produce a different fingerprint (the stream is
     * not pinned to one constant). */
    {
        TaskReport r3;
        if (task_run_suite(123456789u, probe_cases, 0, &r3)) return 1;
        if (r3.fingerprint == r1.fingerprint) return 1;
    }

    /* 3-5. Structural bounds over a fresh generated stream. */
    task_arena_init(&a, 55555u);
    for (ci = 0; ci < probe_cases; ci++) {
        TaskNode *tree = task_gen_tree(&a);
        uint32_t subj[TASK_MAX_SUBJ];
        int nsubj, g;
        if (!tree) return 1;
        if (task_tree_depth(tree) > TASK_MAX_DEPTH) return 1;
        if (task_tree_caps(tree) > TASK_MAX_CAPS) return 1;
        nsubj = task_gen_subject(&a, tree, subj,
                    (int)(sizeof(subj)/sizeof(subj[0])));
        if (nsubj < 0 || nsubj > TASK_MAX_SUBJ) return 1;
        for (g = 0; g < nsubj; g++)
            if (task_is_surrogate(subj[g])) saw_surrogate = 1;
        if (task_tree_depth(tree) > max_depth)
            max_depth = task_tree_depth(tree);
        if (task_tree_caps(tree) > max_caps)
            max_caps = task_tree_caps(tree);
        if (nsubj > max_subj) max_subj = nsubj;
    }
    if (max_depth > TASK_MAX_DEPTH || max_caps > TASK_MAX_CAPS ||
        max_subj > TASK_MAX_SUBJ || saw_surrogate)
        return 1;
    /* bounds must actually be exercised, not trivially zero */
    if (max_depth < 2 || max_caps < 1 || max_subj < 1) return 1;

    /* 6. Boundary-table round trip on a mixed-width text. Every scalar start
     * maps back to its own index; every interior code unit maps to -1. */
    {
        uint32_t mixed[5] = { 0x61u, 0x00E9u, 0x1F600u, 0x4E2Du, 0x20000u };
        TaskText t;
        int k, expect = 0;
        if (task_text_encode(&t, mixed, 5) < 0) return 1;
        if (task_map_offset_8(&t, t.u8_len) != 5) return 1;
        if (task_map_offset_16(&t, t.u16_len) != 5) return 1;
        if (task_map_offset_32(&t, t.u32_len) != 5) return 1;
        if (task_map_offset_8(&t, -1) != -1) return 1;
        for (k = 0; k < t.u8_len; k++) {
            int mapped = task_map_offset_8(&t, k);
            if (t.b8[k] == -1) { if (mapped != -1) return 1; }
            else { if (mapped != expect) return 1; expect = mapped + 1; }
        }
        /* UTF-16 surrogate interior of the supplementary scalar is detected */
        for (k = 0; k < t.u16_len; k++)
            if (t.b16[k] == -1 && task_map_offset_16(&t, k) != -1)
                return 1;
    }

    /* 7. Compile errors must be surfaced, not swallowed. Feed an invalid
     * pattern (unbalanced parenthesis) and require an explicit status. */
    {
        static uint32_t bad[2] = { (uint32_t)'(', 0 };
        uint32_t ok_subj[1] = { 0x61u };
        TaskText t;
        TaskEngResult e;
        if (task_text_encode(&t, ok_subj, 1) < 0) return 1;
        e = task_engine_run(8, bad, 1, &t);
        if (e.status != TASK_ENG_COMPILE_ERROR) return 1;
    }

    /* 8. The generator never returns an empty/filtered universe: across the
     * replay suite above both match and no-match cases were produced. */
    if (r1.cases == 0 || r1.matched_cases == 0 || r1.nomatch_cases == 0)
        return 1;
    return 0;
}

void task_print_one_case(uint64_t seed, int caseidx) {
    TaskArena arena;
    TaskNode *tree = NULL;
    uint32_t subj[TASK_MAX_SUBJ];
    int nsubj = 0, ci;
    CaseResult cr;
    TaskReport rep;
    char ast[TASK_DIFF_MAX], subjs[TASK_DIFF_MAX];
    int g, ncaps;

    task_report_init(&rep, seed);
    task_arena_init(&arena, seed);
    /* Replay the exact RNG stream up to the requested case. */
    for (ci = 0; ci <= caseidx; ci++) {
        tree = task_gen_tree(&arena);
        nsubj = task_gen_subject(&arena, tree, subj,
                    (int)(sizeof(subj)/sizeof(subj[0])));
    }
    if (!tree || nsubj < 0) {
        printf("case %d unavailable\n", caseidx);
        return;
    }
    if (run_case(tree, subj, nsubj, &cr, &rep) < 0) {
        printf("case %d resource failure\n", caseidx);
        return;
    }
    ncaps = task_tree_caps(tree);
    task_serialize_ast(tree, ast, sizeof(ast));
    task_serialize_scalars(subj, nsubj, subjs, sizeof(subjs));
    printf("CASE seed=%llu index=%d\n  ast : %s\n  subj: %s\n  oracle: %s",
           (unsigned long long)seed, caseidx, ast,
           nsubj ? subjs : "(empty)",
           cr.oracle.matched ? "MATCH" : "NOMATCH");
    if (cr.oracle.matched) {
        printf(" span=[%d,%d) caps={", cr.oracle.start, cr.oracle.end);
        for (g = 1; g <= ncaps; g++) {
            if (!cr.oracle.cap[g].set)
                printf("%s-", g == 1 ? "" : ",");
            else
                printf("%s[%d,%d)%s", g == 1 ? "" : ",",
                       cr.oracle.cap[g].start, cr.oracle.cap[g].end,
                       cr.oracle.cap[g].start == cr.oracle.cap[g].end
                           ? "(empty)" : "");
        }
        printf("}");
    }
    printf("\n");
    print_eng("width8 :", &cr.eng[0]);
    print_eng("width16:", &cr.eng[1]);
    print_eng("width32:", &cr.eng[2]);
}

/* Run oracle and all three real widths on one hand-built tree/subject. */
static int builtin_run(const TaskNode *tree, const uint32_t *subj, int nsubj,
                       TaskOracleResult *orc, TaskEngResult eng[3]) {
    static uint32_t pat[TASK_MAX_PATLEN + 1];
    TaskText text;
    int patlen = task_render_pattern(tree, pat,
                    (int)(sizeof(pat)/sizeof(pat[0])));
    int w, rc;
    if (patlen < 0 || task_text_encode(&text, subj, nsubj) < 0) return 1;
    rc = task_oracle_run(tree, task_tree_caps(tree), subj, nsubj, orc);
    if (rc != TASK_ORACLE_OK && rc != TASK_ORACLE_NO_MATCH) return 1;
    for (w = 0; w < 3; w++)
        eng[w] = task_engine_run(w == 0 ? 8 : w == 1 ? 16 : 32,
                                 pat, patlen, &text);
    return 0;
}

static TaskNode *b_lit(TaskArena *a, uint32_t cp) {
    TaskNode *n = task_alloc_node(a);
    n->kind = TASK_LIT; n->cp = cp; return n;
}
static TaskNode *b_cat2(TaskArena *a, TaskNode *x, TaskNode *y) {
    TaskNode *n = task_alloc_node(a);
    n->kind = TASK_CAT; n->ch[0] = x; n->ch[1] = y; n->nch = 2; return n;
}
static TaskNode *b_alt2(TaskArena *a, TaskNode *x, TaskNode *y) {
    TaskNode *n = task_alloc_node(a);
    n->kind = TASK_ALT; n->ch[0] = x; n->ch[1] = y; n->nch = 2; return n;
}
static TaskNode *b_cap(TaskArena *a, int no, TaskNode *x) {
    TaskNode *n = task_alloc_node(a);
    n->kind = TASK_CAP; n->capno = no; n->ch[0] = x; n->nch = 1; return n;
}
static TaskNode *b_rep(TaskArena *a, int lo, int hi, TaskNode *x) {
    TaskNode *n = task_alloc_node(a);
    n->kind = TASK_REP; n->rmin = lo; n->rmax = hi;
    n->ch[0] = x; n->nch = 1; return n;
}

int task_targeted_semantics(void) {
    TaskArena a;
    TaskOracleResult orc;
    TaskEngResult eng[3];
    char why[256];
    int w;

    /* 1. Nested greedy quantifiers: ( ( c{2,3} ){1,3} ) on seven c's must
     *    consume six, and the inner capture keeps its last iteration [3,6). */
    task_arena_init(&a, 1);
    {
        TaskNode *inner = b_rep(&a, 2, 3, b_lit(&a, 0x63u));
        TaskNode *cap2  = b_cap(&a, 2, inner);
        TaskNode *outer = b_rep(&a, 1, 3, cap2);
        TaskNode *root  = b_cap(&a, 1, outer);
        uint32_t subj[7] = {0x63,0x63,0x63,0x63,0x63,0x63,0x63};
        if (builtin_run(root, subj, 7, &orc, eng)) return 1;
        if (!orc.matched || orc.start != 0 || orc.end != 6) return 1;
        if (!orc.cap[1].set || orc.cap[1].start != 0 || orc.cap[1].end != 6)
            return 1;
        if (!orc.cap[2].set || orc.cap[2].start != 3 || orc.cap[2].end != 6)
            return 1;
        for (w = 0; w < 3; w++)
            if (task_compare_result(&orc, &eng[w], why, sizeof(why))) return 1;
    }

    /* 2. Alternation fall-through: (a|ab)c on "abc" must abandon the first
     *    branch when the trailing c cannot match, then take "ab". */
    task_arena_init(&a, 2);
    {
        TaskNode *branch1 = b_lit(&a, 0x61u);
        TaskNode *branch2 = b_cat2(&a, b_lit(&a, 0x61u), b_lit(&a, 0x62u));
        TaskNode *grp     = b_cap(&a, 1, b_alt2(&a, branch1, branch2));
        TaskNode *root    = b_cat2(&a, grp, b_lit(&a, 0x63u));
        uint32_t subj[3] = {0x61u, 0x62u, 0x63u};
        if (builtin_run(root, subj, 3, &orc, eng)) return 1;
        if (!orc.matched || orc.start != 0 || orc.end != 3) return 1;
        if (!orc.cap[1].set || orc.cap[1].start != 0 || orc.cap[1].end != 2)
            return 1;
        for (w = 0; w < 3; w++)
            if (task_compare_result(&orc, &eng[w], why, sizeof(why))) return 1;
    }

    /* 3a. Unset capture: (a)|b on "b" leaves group 1 never participating. */
    task_arena_init(&a, 3);
    {
        TaskNode *root = b_alt2(&a, b_cap(&a, 1, b_lit(&a, 0x61u)),
                                b_lit(&a, 0x62u));
        uint32_t subj[1] = {0x62u};
        if (builtin_run(root, subj, 1, &orc, eng)) return 1;
        if (!orc.matched || orc.cap[1].set) return 1;
        if (orc.cap[1].start != TASK_CAP_UNSET) return 1;
        for (w = 0; w < 3; w++)
            if (task_compare_result(&orc, &eng[w], why, sizeof(why))) return 1;
    }

    /* 3b. Empty participating capture: (z{0,1}) on "a" matches the empty
     *     string, so group 1 is set with start==end, not unset. */
    task_arena_init(&a, 4);
    {
        TaskNode *root = b_cap(&a, 1, b_rep(&a, 0, 1, b_lit(&a, 0x7Au)));
        uint32_t subj[1] = {0x61u};
        if (builtin_run(root, subj, 1, &orc, eng)) return 1;
        if (!orc.matched || orc.start != 0 || orc.end != 0) return 1;
        if (!orc.cap[1].set || orc.cap[1].start != 0 || orc.cap[1].end != 0)
            return 1;
        for (w = 0; w < 3; w++)
            if (task_compare_result(&orc, &eng[w], why, sizeof(why))) return 1;
    }
    return 0;
}
