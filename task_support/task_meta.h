#ifndef TASK_SUPPORT_TASK_META_H
#define TASK_SUPPORT_TASK_META_H
/* Metamorphic transformations over the logical AST. Each transform returns a
 * freshly built tree in a separate arena and an explicit capture mapping;
 * callers must compare through that mapping rather than assuming identical
 * ovector layout. The whole-match language is preserved, so the leftmost
 * match span on the same subject must be unchanged. */
#include "task_ast.h"

typedef enum {
    TASK_META_ADD_GROUPS = 0,  /* wrap concatenands in (?: .. )           */
    TASK_META_REMOVE_GROUPS,   /* drop redundant non-capturing groups      */
    TASK_META_CAP_WRAP,        /* wrap the root in a new capturing group   */
    TASK_META_RENAME           /* rename named captures, keep numbering    */
} TaskMetaKind;

typedef struct {
    TaskMetaKind kind;
    int old_ncaps, new_ncaps;
    /* num[old_capno] = new_capno, or -1 when the old group has no image.
     * Group 0 (the whole match) always maps to 0. */
    int num[TASK_MAX_CAPS + 1];
    /* rename only: old/new names indexed by old capture number, "" unused */
    char old_name[TASK_MAX_CAPS + 1][8];
    char new_name[TASK_MAX_CAPS + 1][8];
} TaskMetaMap;

/* Each returns a transformed tree, or NULL when the transform is not
 * applicable to this tree (nothing removable / no named group / no capture
 * budget left). On success *map is fully initialized. */
TaskNode *task_meta_add_groups(const TaskNode *src, TaskArena *dst,
                               TaskMetaMap *map);
TaskNode *task_meta_remove_groups(const TaskNode *src, TaskArena *dst,
                                  TaskMetaMap *map);
TaskNode *task_meta_wrap_capture(const TaskNode *src, TaskArena *dst,
                                 TaskMetaMap *map);
TaskNode *task_meta_rename(const TaskNode *src, TaskArena *dst,
                           TaskMetaMap *map);

/* Reassign capture numbers in preorder, which is exactly PCRE2's numbering. */
int task_meta_renumber(TaskNode *n, int next);
#endif
