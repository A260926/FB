#include "check.h"
#include "task_harness.h"
#include "task_meta.h"
#include "task_gen.h"
#include <stdio.h>
#include <string.h>

#define TASK_SEED       UINT64_C(0x514771C9A5ED)
#define TASK_NCASES     500
#define TASK_META_CASES 250

static void print_report(const TaskReport *r) {
    int i;
    printf("seed=%llu cases=%d match=%d nomatch=%d\n",
           (unsigned long long)r->seed, r->cases,
           r->matched_cases, r->nomatch_cases);
    printf("engine: compile_errors=%d runtime_errors=%d bad_boundaries=%d "
           "oracle_limits=%d diffs=%d\n", r->compile_errors,
           r->runtime_errors, r->bad_boundaries, r->oracle_limits, r->diffs);
    printf("coverage depth[1..%d]:", TASK_MAX_DEPTH);
    for (i = 1; i <= TASK_MAX_DEPTH; i++) printf(" %d", r->depth_hist[i]);
    printf("\ncoverage captures[0..%d]:", TASK_MAX_CAPS);
    for (i = 0; i <= TASK_MAX_CAPS; i++) printf(" %d", r->cap_hist[i]);
    printf("\ncoverage: supplementary=%d alternation=%d repetition=%d "
           "empty_participating=%d unset_capture=%d\n",
           r->with_supp, r->with_alt, r->with_rep,
           r->with_empty_participating, r->with_unset_capture);
    printf("replay fingerprint=%016llx\n",
           (unsigned long long)r->fingerprint);
    if (r->diff_case < 0)
        printf("first difference: none\n");
    else
        printf("%s", r->diff_text);
}

static int group_model(void) {
    TaskReport r;
    int rc, i, total_depth = 0, total_caps = 0, max_cap_seen = 0,
        max_depth_seen = 0;

    rc = task_run_suite(TASK_SEED, TASK_NCASES, 0, &r);
    print_report(&r);
    /* A few fully rendered example cases: seed, AST, scalar subject and every
     * width's result. */
    task_print_one_case(TASK_SEED, 0);
    task_print_one_case(TASK_SEED, 17);
    task_print_one_case(TASK_SEED, 200);

    TASK_CHECK(rc == 0);
    TASK_CHECK(r.cases == TASK_NCASES);
    TASK_CHECK(r.diffs == 0);
    TASK_CHECK(r.compile_errors == 0);
    TASK_CHECK(r.runtime_errors == 0);
    TASK_CHECK(r.bad_boundaries == 0);
    TASK_CHECK(r.oracle_limits == 0);
    TASK_CHECK(r.matched_cases > 0);
    TASK_CHECK(r.nomatch_cases > 0);
    /* real feature coverage, not hard-coded constants */
    TASK_CHECK(r.with_alt > 0);
    TASK_CHECK(r.with_rep > 0);
    TASK_CHECK(r.with_supp > 0);
    TASK_CHECK(r.with_empty_participating > 0);
    TASK_CHECK(r.with_unset_capture > 0);
    for (i = 1; i <= TASK_MAX_DEPTH; i++) total_depth += r.depth_hist[i];
    for (i = 0; i <= TASK_MAX_CAPS; i++) total_caps += r.cap_hist[i];
    TASK_CHECK(total_depth == TASK_NCASES);
    TASK_CHECK(total_caps == TASK_NCASES);
    TASK_CHECK(r.depth_hist[TASK_MAX_DEPTH] > 0);
    for (i = 1; i <= TASK_MAX_DEPTH; i++)
        if (r.depth_hist[i] > 0) max_depth_seen = i;
    for (i = 0; i <= TASK_MAX_CAPS; i++)
        if (r.cap_hist[i] > 0) max_cap_seen = i;
    /* the bounded grammar is actually exercised near its limits */
    TASK_CHECK(max_depth_seen == TASK_MAX_DEPTH);
    TASK_CHECK(max_cap_seen >= 4);
    /* hand-built regressions for the tricky backtracking/capture rules */
    TASK_CHECK(task_targeted_semantics() == 0);
    /* the predetermined divergence must be caught by the comparator */
    TASK_CHECK(task_simulate_logical_difference() == 0);
    return 0;
}

static int group_metamorphic(void) {
    TaskMetaReport r;
    int rc;
    rc = task_run_metamorphic(TASK_SEED ^ UINT64_C(0x9e3779b97f4a7c15),
                              TASK_META_CASES, 1, &r);
    printf("meta seed=%llu applied=[%d,%d,%d,%d] na=[%d,%d,%d,%d] "
           "failures=%d\n", (unsigned long long)r.seed,
           r.applied[0], r.applied[1], r.applied[2], r.applied[3],
           r.not_applicable[0], r.not_applicable[1],
           r.not_applicable[2], r.not_applicable[3], r.failures);
    TASK_CHECK(rc == 0);
    TASK_CHECK(r.failures == 0);
    TASK_CHECK(r.applied[TASK_META_ADD_GROUPS] > 0);
    TASK_CHECK(r.applied[TASK_META_REMOVE_GROUPS] > 0);
    TASK_CHECK(r.applied[TASK_META_CAP_WRAP] > 0);
    TASK_CHECK(r.applied[TASK_META_RENAME] > 0);
    /* every transform was either applied or explicitly not applicable */
    TASK_CHECK(r.applied[0] + r.not_applicable[0] == TASK_META_CASES);
    TASK_CHECK(r.applied[3] + r.not_applicable[3] == TASK_META_CASES);
    return 0;
}

static int group_replay(void) {
    TaskReport a, b, injected;
    const int n = 60;
    const int fault_at = 7;

    /* fixed-seed determinism: identical fingerprints and counters */
    TASK_CHECK(task_run_suite(TASK_SEED, n, 0, &a) == 0);
    TASK_CHECK(task_run_suite(TASK_SEED, n, 0, &b) == 0);
    TASK_CHECK(a.fingerprint == b.fingerprint);
    TASK_CHECK(a.matched_cases == b.matched_cases);
    TASK_CHECK(a.nomatch_cases == b.nomatch_cases);

    /* inject a predetermined logical difference at a known case; the harness
     * must flag it and retain that first failing case */
    task_test_inject_fault(fault_at);
    TASK_CHECK(task_run_suite(TASK_SEED, n, 0, &injected) != 0);
    TASK_CHECK(injected.diffs > 0);
    TASK_CHECK(injected.diff_case == fault_at);
    printf("%s", injected.diff_text);

    /* after the one-shot injection fires, a replay is clean again */
    TASK_CHECK(task_run_suite(TASK_SEED, n, 0, &a) == 0);
    TASK_CHECK(a.diffs == 0);

    /* structural/resource boundaries and boundary-table mapping */
    TASK_CHECK(task_probe_resource_bounds() == 0);
    return 0;
}

int task_cases(const char *group) {
    if (!group) return 1;
    if (strcmp(group, "model") == 0) return group_model();
    if (strcmp(group, "metamorphic") == 0) return group_metamorphic();
    if (strcmp(group, "replay") == 0) return group_replay();
    return 1;
}
