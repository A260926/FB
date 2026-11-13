#ifndef TASK_SUPPORT_TASK_GEN_H
#define TASK_SUPPORT_TASK_GEN_H
/* Restricted random generator. Produces a logical Unicode-scalar AST and a
 * matching scalar subject, all inside the declared grammar bounds. */
#include "task_ast.h"

/* Generate one tree in the arena (depth <= TASK_MAX_DEPTH, captures <=
 * TASK_MAX_CAPS). Returns NULL only if the arena is exhausted, which the
 * bounds make impossible for a fresh arena. */
TaskNode *task_gen_tree(TaskArena *a);

/* Collect the distinct scalars referenced by a tree into out (cap n),
 * returning how many were written. */
int task_tree_alphabet(const TaskNode *n, uint32_t *out, int cap);

/* Generate a subject of 0..TASK_MAX_SUBJ scalars, biased toward the tree
 * alphabet so that both matching and failing inputs occur. */
int task_gen_subject(TaskArena *a, const TaskNode *tree,
                     uint32_t *out, int cap);

/* Structural predicates used by the replay/resource boundary checks. */
int task_tree_has_kind(const TaskNode *n, TaskKind k);
int task_tree_has_supplementary(const TaskNode *n);
#endif
