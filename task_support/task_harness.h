#ifndef TASK_SUPPORT_TASK_HARNESS_H
#define TASK_SUPPORT_TASK_HARNESS_H
/* Property-suite orchestration: drives generator -> oracle -> real engines,
 * compares results in scalar space, keeps real coverage counters, and records
 * the first divergence. Also contains a predetermined logical-difference
 * simulator that proves the checker flags and retains the first bad case
 * without touching any upstream PCRE2 code. */
#include "task_ast.h"
#include "task_oracle.h"
#include "task_engine.h"

#define TASK_DEFAULT_CASES 500
#define TASK_DIFF_MAX 1024

typedef struct {
    uint64_t seed;
    int cases;
    int matched_cases, nomatch_cases;
    int compile_errors, runtime_errors, bad_boundaries, oracle_limits;
    int depth_hist[TASK_MAX_DEPTH + 1];      /* index by tree depth 1..5     */
    int cap_hist[TASK_MAX_CAPS + 1];         /* index by capture count 0..8  */
    int with_supp;                           /* pattern or subject           */
    int with_alt, with_rep;
    int with_empty_participating;            /* cap matched ""               */
    int with_unset_capture;                  /* cap never participated       */
    int diffs;
    int diff_case;                           /* first bad case, -1 if none   */
    uint64_t diff_seed;
    char diff_text[TASK_DIFF_MAX];
    /* replay fingerprint: updated every case, identical for identical seeds */
    uint64_t fingerprint;
} TaskReport;

void task_report_init(TaskReport *r, uint64_t seed);

/* Run the full property suite. Verbose prints one line per case plus the
 * first-diff block. Returns nonzero if any property failed. */
int task_run_suite(uint64_t seed, int ncases, int verbose, TaskReport *r);

/* Compare one oracle result against one engine result in scalar space.
 * Returns 0 when equal, nonzero otherwise, writing a description to why. */
int task_compare_result(const TaskOracleResult *o, const TaskEngResult *e,
                        char *why, int whycap);

/* Predetermined logical-difference simulator: corrupts a copy of an oracle
 * result and confirms the comparator detects it and that the harness keeps
 * the first failing case. Returns 0 on success. */
int task_simulate_logical_difference(void);

/* Test-only fault injection into OUR oracle copy inside the harness (never
 * into PCRE2). The next suite run flips the match verdict of case caseidx,
 * which the comparator must flag; used to prove first-diff retention. */
void task_test_inject_fault(int caseidx);

/* Regenerate one case of a seed stream and print its full block: seed,
 * logical AST, scalar subject, oracle and all three width results. */
void task_print_one_case(uint64_t seed, int caseidx);

/* Hand-built semantic regressions for the backtracking rules that are easy to
 * get wrong: nested greedy quantifiers, alternation fall-through, and the
 * distinction between an empty participating capture and an unset one.
 * Returns 0 when the oracle matches both the documented expectation and every
 * real width. */
int task_targeted_semantics(void);

/* Resource-boundary probes; return 0 when boundaries are enforced. */
int task_probe_resource_bounds(void);

typedef struct {
    uint64_t seed;
    int cases;
    int applied[4];              /* one counter per TaskMetaKind            */
    int not_applicable[4];
    int failures;
    int diff_case;               /* first transform divergence, -1 if none  */
    char diff_text[TASK_DIFF_MAX];
} TaskMetaReport;

void task_meta_report_init(TaskMetaReport *r, uint64_t seed);

/* Run every applicable metamorphic transform over ncases generated inputs and
 * validate match-span invariance plus the explicit capture mappings, against
 * both the transformed oracle and all three real engines. */
int task_run_metamorphic(uint64_t seed, int ncases, int verbose,
                         TaskMetaReport *r);
#endif
