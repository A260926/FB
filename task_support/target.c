#include "check.h"
#include "task_harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* task_assertions is defined once by driver.c in the normal build; the
 * standalone fault-simulator binary has no driver, so define it there. */
#ifdef TASK_FAULT_MAIN
unsigned task_assertions = 0;
#endif

/* ================================================================== */
/* Shared corpus: the fixed 500-program seed set. The first eight are  */
/* hand pinned so every required lifecycle path is guaranteed; the     */
/* rest are generated deterministically from the program index.        */
/* ================================================================== */

static void run_corpus(int capture)
{
    unsigned i;
    for (i = 0; i < TASK_PROGRAMS; i++) {
        task_program p;
        task_generate(&p, i);
        TASK_CHECK(p.nact >= 2 && p.nact <= TASK_MAX_ACTIONS);
        TASK_CHECK(p.plain_total <= TASK_MAX_PLAIN);
        task_run_deflate(&p, capture);
        task_program_free(&p);
    }
}

/* ================================================================== */
/* Group 1: model. The independent splitmix/plaintext model and the    */
/* non-copy reference codec explain everything the real library does.  */
/* ================================================================== */

static void group_model(void)
{
    task_rng r1, r2, r3;
    task_buf a, b, comp, back, comp2, dict;
    unsigned i;

    task_buf_init(&a);
    task_buf_init(&b);
    task_buf_init(&comp);
    task_buf_init(&back);
    task_buf_init(&comp2);
    task_buf_init(&dict);

    /* 1. the model is deterministic and seed-sensitive, with no globals */
    task_rng_seed(&r1, 12345);
    task_rng_seed(&r2, 12345);
    task_rng_seed(&r3, 12346);
    for (i = 0; i < 64; i++) {
        unsigned long long v1 = task_rng_next(&r1);
        unsigned long long v2 = task_rng_next(&r2);
        unsigned long long v3 = task_rng_next(&r3);
        TASK_CHECK(v1 == v2);          /* equal seeds, equal call counts */
        TASK_CHECK(v1 != v3);          /* splitmix64 is a state bijection */
    }

    /* 2. model plaintext round-trips through the real streaming API in
     *    both wrappers, and the reference decoder reproduces it exactly */
    task_rng_seed(&r1, 0xabcdef);
    for (i = 0; i < 12; i++) {
        int wb = (i & 1) ? -15 : 15;
        task_buf_reset(&a);
        task_buf_reset(&comp);
        task_buf_reset(&back);
        task_model_bytes(&r1, &a, 1 + task_rng_below(&r1, 4096));
        TASK_CHECK(task_ref_deflate(&a, &comp, wb, 1 + (i % 9),
                                    NULL, 0, 0,
                                    1 + task_rng_below(&r1, 200)) == 0);
        TASK_CHECK(comp.len > 0);
        TASK_CHECK(task_ref_inflate(&comp, &back, wb, NULL, 0,
                                    1 + task_rng_below(&r1, 150),
                                    1 + task_rng_below(&r1, 90)) == 0);
        TASK_CHECK(task_buf_equal(&a, &back));
    }

    /* 3. dictionary paths: zlib wrapper exercises Z_NEED_DICT handling,
     *    raw deflate exercises an explicit pre-stream dictionary */
    task_rng_seed(&r2, 0xd1c7);
    task_buf_reset(&a);
    task_buf_reset(&comp);
    task_buf_reset(&back);
    task_buf_reset(&comp2);
    task_model_bytes(&r2, &dict, 128);
    task_model_bytes(&r2, &a, 3000);
    TASK_CHECK(task_ref_deflate(&a, &comp, 15, 6, dict.p,
                                (unsigned)dict.len, Z_SYNC_FLUSH, 97) == 0);
    task_buf_reset(&back);
    TASK_CHECK(task_ref_inflate(&comp, &back, 15, dict.p,
                                (unsigned)dict.len, 64, 48) == 0);
    TASK_CHECK(task_buf_equal(&a, &back));

    task_buf_reset(&comp);
    task_buf_reset(&back);
    TASK_CHECK(task_ref_deflate(&a, &comp, -15, 6, dict.p,
                                (unsigned)dict.len, 0, 130) == 0);
    TASK_CHECK(task_ref_inflate(&comp, &back, -15, dict.p,
                                (unsigned)dict.len, 50, 77) == 0);
    TASK_CHECK(task_buf_equal(&a, &back));

    /* 4. the reference encoder is itself a deterministic function of the
     *    model input: identical programs yield identical byte streams */
    TASK_CHECK(task_ref_deflate(&a, &comp2, -15, 6, dict.p,
                                (unsigned)dict.len, 0, 130) == 0);
    TASK_CHECK(task_buf_equal(&comp, &comp2));

    /* 5. model stores are deep and independent: duplicated buffers do not
     *    share backing memory, so a forked branch cannot alias its sibling */
    task_buf_take(&b, &a);
    TASK_CHECK(task_buf_equal(&a, &b));
    TASK_CHECK(b.p != a.p);
    b.p[0] = (unsigned char)(b.p[0] ^ 1);
    TASK_CHECK(!task_buf_equal(&a, &b));

    task_buf_free(&a);
    task_buf_free(&b);
    task_buf_free(&comp);
    task_buf_free(&comp2);
    task_buf_free(&back);
    task_buf_free(&dict);
}

/* ================================================================== */
/* Group 2: metamorphic. State/representation changes (flushes, both   */
/* copy APIs, partial input/output, forked suffixes) must keep every   */
/* complete stream explainable by the independent model.               */
/* ================================================================== */

static void group_metamorphic(void)
{
    task_rng r;
    unsigned i;

    /* divergent suffixes after one shared prefix, checked before coverage is
     * reset so the printed corpus numbers stay exact (500 programs) */
    {
        task_program p;
        unsigned cap_count = 0, j;
        task_generate(&p, 0); /* pinned fork: slot 0 and slot 1 diverge */
        task_run_deflate(&p, 1);
        for (j = 0; j < p.nact; j++)
            cap_count += p.has_cap_act[j];
        TASK_CHECK(cap_count >= 2);
        {
            task_buf first = {0}, second = {0};
            task_buf_init(&first);
            task_buf_init(&second);
            for (j = 0; j < p.nact && second.len == 0; j++) {
                if (p.has_cap_act[j]) {
                    if (first.len == 0)
                        task_buf_take(&first, &p.cap_act[j]);
                    else
                        task_buf_take(&second, &p.cap_act[j]);
                }
            }
            TASK_CHECK(first.len && second.len);
            TASK_CHECK(!task_buf_equal(&first, &second));
            task_buf_free(&first);
            task_buf_free(&second);
        }
        task_program_free(&p);
    }

    task_cov_reset();
    task_acct_reset();
    run_corpus(1); /* capture complete compressed streams per branch */

    /* the whole bounded grammar was actually exercised on real paths */
    TASK_CHECK(task_coverage.programs == TASK_PROGRAMS);
    TASK_CHECK(task_coverage.raw_programs > 0);
    TASK_CHECK(task_coverage.zlib_programs > 0);
    TASK_CHECK(task_coverage.dict_paths > 0);
    TASK_CHECK(task_coverage.sync_flushes > 0);
    TASK_CHECK(task_coverage.full_flushes > 0);
    TASK_CHECK(task_coverage.deflate_copies > 0);
    TASK_CHECK(task_coverage.copy_partial_input > 0);
    TASK_CHECK(task_coverage.copy_partial_output > 0);
    TASK_CHECK(task_coverage.branch_forks > 0);
    TASK_CHECK(task_coverage.finish_original_first > 0);
    TASK_CHECK(task_coverage.active_peak == TASK_MAX_STREAMS);
    TASK_CHECK(task_acct_live() == 0);   /* every stream released its state */
    TASK_CHECK(task_acct_total() > 0);   /* and the copies really allocated */

    /* inflateCopy with partially consumed input and a saturated output
     * slot, across raw/zlib wrappers and both dictionary modes */
    task_rng_seed(&r, 0x111f1a7eULL);
    for (i = 0; i < 8; i++) {
        int raw = (i >> 1) & 1;
        int use_dict = i & 1;
        unsigned long before = task_coverage.inflate_copies;
        task_run_inflate_copy(&r, raw, use_dict);
        TASK_CHECK(task_coverage.inflate_copies == before + 1);
        if (use_dict)
            TASK_CHECK(task_coverage.inflate_dict_paths > 0);
    }
    TASK_CHECK(task_coverage.inflate_copies == 8);
    TASK_CHECK(task_coverage.copy_partial_input > 0);
    TASK_CHECK(task_coverage.copy_partial_output > 0);
    TASK_CHECK(task_acct_live() == 0);

    printf("COVERAGE programs=%lu raw=%lu zlib=%lu dict=%lu sync=%lu "
           "full=%lu dcopy=%lu icopy=%lu part_in=%lu part_out=%lu "
           "forks=%lu finish_first=%lu idict=%lu peak=%lu\n",
           task_coverage.programs, task_coverage.raw_programs,
           task_coverage.zlib_programs, task_coverage.dict_paths,
           task_coverage.sync_flushes, task_coverage.full_flushes,
           task_coverage.deflate_copies, task_coverage.inflate_copies,
           task_coverage.copy_partial_input,
           task_coverage.copy_partial_output, task_coverage.branch_forks,
           task_coverage.finish_original_first,
           task_coverage.inflate_dict_paths, task_coverage.active_peak);
}

/* ================================================================== */
/* Group 3: replay / resource boundaries. Repeated replay is byte for  */
/* byte identical, depends on no prior address or global state, and    */
/* returns every allocation.                                           */
/* ================================================================== */

static void group_replay(void)
{
    unsigned i;

    /* 1. replay every fixed program against the captures taken on the
     *    first run; two extra replays must match byte for byte */
    task_acct_reset();
    for (i = 0; i < TASK_PROGRAMS; i++) {
        task_program p;
        task_generate(&p, i);
        task_run_deflate(&p, 1);
        task_run_deflate(&p, 0);
        task_run_deflate(&p, 0);
        task_program_free(&p);
        TASK_CHECK(task_acct_live() == 0);
    }

    /* 2. regenerate from scratch (fresh heap layout, reset globals) and
     *    confirm the captured compressed bytes are stable */
    for (i = 0; i < 16u; i++) {
        task_program p, q;
        unsigned s;
        task_generate(&p, i);
        task_run_deflate(&p, 1);
        task_generate(&q, i);
        task_run_deflate(&q, 1);
        for (s = 0; s < TASK_MAX_ACTIONS; s++) {
            TASK_CHECK(p.has_cap_act[s] == q.has_cap_act[s]);
            if (p.has_cap_act[s])
                TASK_CHECK(task_buf_equal(&p.cap_act[s], &q.cap_act[s]));
        }
        task_program_free(&p);
        task_program_free(&q);
        TASK_CHECK(task_acct_live() == 0);
    }

    /* 3. allocation accounting survives a full reset/replay cycle */
    {
        unsigned long first_total = task_acct_total();
        TASK_CHECK(first_total > 0);
        task_acct_reset();
        TASK_CHECK(task_acct_live() == 0 && task_acct_total() == 0);
        run_corpus(1);
        TASK_CHECK(task_acct_live() == 0);
        TASK_CHECK(task_acct_total() > 0);
    }

    /* 4. every replay run still honours the action/plaintext bounds */
    for (i = 0; i < TASK_PROGRAMS; i += 37) {
        task_program p;
        task_generate(&p, i);
        TASK_CHECK(p.nact <= TASK_MAX_ACTIONS);
        TASK_CHECK(p.plain_total <= TASK_MAX_PLAIN);
        task_program_free(&p);
    }
}

int task_cases(const char *group)
{
    if (strcmp(group, "model") == 0) {
        group_model();
        return 0;
    }
    if (strcmp(group, "metamorphic") == 0) {
        group_metamorphic();
        return 0;
    }
    if (strcmp(group, "replay") == 0) {
        group_replay();
        return 0;
    }
    return 1;
}

/* Predetermined logic-difference simulator. Built as a separate binary
 * (-DTASK_FAULT_MAIN): the oracle is deliberately perturbed at the first
 * case so we can prove TASK_CHECK reports it and stops on that case. The
 * upstream library is never modified. */
#ifdef TASK_FAULT_MAIN
int main(void)
{
    task_program p;
    fprintf(stderr, "FAULT_SIM first case: program 0\n");
    task_fault_inject = 1;
    task_cov_reset();
    task_generate(&p, 0);
    task_run_deflate(&p, 1); /* expected to exit(1) inside the first oracle */
    task_program_free(&p);
    return 0;
}
#endif