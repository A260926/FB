#ifndef TASK_SUPPORT_TASK_ORACLE_H
#define TASK_SUPPORT_TASK_ORACLE_H
/* Independent reference interpreter. It works only on logical scalar arrays
 * and the restricted AST; it never calls PCRE2, so agreement is a genuine
 * cross-check rather than a self-test.
 *
 * Semantics modelled: leftmost overall match, leftmost alternation branch
 * first, greedy bounded repetition, PCRE capture overwrite rules. A capture
 * that never participates stays unset; one that participates but matches the
 * empty string is recorded as (p,p), which is distinct from unset. */
#include "task_ast.h"

#define TASK_CAP_UNSET (-1)

typedef struct {
    int set;                    /* 1 if the group participated              */
    int start, end;             /* scalar indices; start==end means empty   */
} TaskCap;

typedef struct {
    int matched;                /* 1 on a match, 0 on no match              */
    int start, end;             /* whole-match scalar span (group 0)        */
    int ncaps;                  /* number of capturing groups in the tree   */
    TaskCap cap[TASK_MAX_CAPS + 1]; /* [1..ncaps]; [0] is the whole match  */
} TaskOracleResult;

#define TASK_ORACLE_OK        0
#define TASK_ORACLE_NO_MATCH  1
#define TASK_ORACLE_LIMIT     2  /* harness resource bound hit, not a pass */

/* Run the interpreter. ncaps is the tree's capture count (use
 * task_tree_caps). Returns TASK_ORACLE_*; on OK or NO_MATCH *out is filled. */
int task_oracle_run(const TaskNode *root, int ncaps,
                    const uint32_t *subj, int n, TaskOracleResult *out);
#endif
