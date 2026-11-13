#include "task_meta.h"
#include <string.h>
#include <stdio.h>

static TaskNode *clone_into(const TaskNode *s, TaskArena *dst) {
    TaskNode *n;
    int i;
    if (!s) return NULL;
    n = task_alloc_node(dst);
    if (!n) return NULL;
    *n = *s;                         /* copies scalars, names and bounds     */
    n->nch = 0;
    for (i = 0; i < s->nch; i++) {
        n->ch[i] = clone_into(s->ch[i], dst);
        if (!n->ch[i]) return NULL;
        n->nch++;
    }
    return n;
}

int task_meta_renumber(TaskNode *n, int next) {
    int i;
    if (!n) return next;
    if (n->kind == TASK_CAP) {
        n->capno = next++;
    }
    for (i = 0; i < n->nch; i++)
        next = task_meta_renumber(n->ch[i], next);
    return next;
}

static void init_identity_map(TaskMetaMap *map, TaskMetaKind kind, int ncaps) {
    int i;
    memset(map, 0, sizeof(*map));
    map->kind = kind;
    map->old_ncaps = map->new_ncaps = ncaps;
    map->num[0] = 0;
    for (i = 1; i <= TASK_MAX_CAPS; i++) map->num[i] = -1;
    for (i = 1; i <= ncaps; i++) map->num[i] = i;
}

static int count_groups(const TaskNode *n) {
    int i, c = 0;
    if (!n) return 0;
    if (n->kind == TASK_GROUP) c = 1;
    for (i = 0; i < n->nch; i++) c += count_groups(n->ch[i]);
    return c;
}

static int count_named(const TaskNode *n) {
    int i, c = 0;
    if (!n) return 0;
    if (n->kind == TASK_CAP && n->name[0]) c = 1;
    for (i = 0; i < n->nch; i++) c += count_named(n->ch[i]);
    return c;
}

/* Wrap every direct child of every concatenation in a non-capturing group. */
static int wrap_cat_children(TaskNode *n, TaskArena *dst) {
    int i;
    if (!n) return 0;
    if (n->kind == TASK_CAT) {
        for (i = 0; i < n->nch; i++) {
            TaskNode *g = task_alloc_node(dst);
            if (!g) return -1;
            g->kind = TASK_GROUP;
            g->ch[0] = n->ch[i];
            g->nch = 1;
            n->ch[i] = g;
        }
    }
    for (i = 0; i < n->nch; i++)
        if (wrap_cat_children(n->ch[i], dst)) return -1;
    return 0;
}

TaskNode *task_meta_add_groups(const TaskNode *src, TaskArena *dst,
                               TaskMetaMap *map) {
    TaskNode *n = clone_into(src, dst);
    int ncaps;
    if (!n) return NULL;
    if (wrap_cat_children(n, dst)) return NULL;
    ncaps = task_meta_renumber(n, 1) - 1;
    init_identity_map(map, TASK_META_ADD_GROUPS, ncaps);
    return n;
}

/* Clone while eliding every non-capturing group; such a node is semantically
 * transparent, so the accepted language and capture order do not change. */
static TaskNode *clone_skip_groups(const TaskNode *s, TaskArena *dst) {
    TaskNode *n;
    int i;
    if (!s) return NULL;
    if (s->kind == TASK_GROUP) return clone_skip_groups(s->ch[0], dst);
    n = task_alloc_node(dst);
    if (!n) return NULL;
    *n = *s;
    n->nch = 0;
    for (i = 0; i < s->nch; i++) {
        n->ch[i] = clone_skip_groups(s->ch[i], dst);
        if (!n->ch[i]) return NULL;
        n->nch++;
    }
    return n;
}

TaskNode *task_meta_remove_groups(const TaskNode *src, TaskArena *dst,
                                  TaskMetaMap *map) {
    TaskNode *n;
    int ncaps;
    if (count_groups(src) == 0) return NULL;   /* transform not applicable */
    n = clone_skip_groups(src, dst);
    if (!n) return NULL;
    ncaps = task_meta_renumber(n, 1) - 1;
    init_identity_map(map, TASK_META_REMOVE_GROUPS, ncaps);
    return n;
}

TaskNode *task_meta_wrap_capture(const TaskNode *src, TaskArena *dst,
                                 TaskMetaMap *map) {
    TaskNode *inner, *root;
    int old_ncaps, i;
    old_ncaps = task_tree_caps(src);
    if (old_ncaps >= TASK_MAX_CAPS) return NULL;     /* no capture budget   */
    inner = clone_into(src, dst);
    if (!inner) return NULL;
    /* Existing groups keep their relative order but shift by one: the new
     * outer group is visited first in preorder and becomes capture 1. */
    task_meta_renumber(inner, 2);
    root = task_alloc_node(dst);
    if (!root) return NULL;
    root->kind = TASK_CAP;
    root->capno = 1;
    snprintf(root->name, sizeof(root->name), "wrap");
    root->ch[0] = inner;
    root->nch = 1;
    memset(map, 0, sizeof(*map));
    map->kind = TASK_META_CAP_WRAP;
    map->old_ncaps = old_ncaps;
    map->new_ncaps = old_ncaps + 1;
    map->num[0] = 0;
    for (i = 1; i <= TASK_MAX_CAPS; i++) map->num[i] = -1;
    for (i = 1; i <= old_ncaps; i++) map->num[i] = i + 1;
    return root;
}

static int rename_named(TaskNode *n, int *counter, TaskMetaMap *map) {
    int i;
    if (!n) return 0;
    if (n->kind == TASK_CAP && n->name[0]) {
        size_t len = strlen(n->name);
        if (len >= sizeof(map->old_name[n->capno])) return -1;
        memcpy(map->old_name[n->capno], n->name, len + 1);
        snprintf(n->name, sizeof(n->name), "m%d", ++(*counter));
        memcpy(map->new_name[n->capno], n->name, sizeof(n->name));
    }
    for (i = 0; i < n->nch; i++)
        if (rename_named(n->ch[i], counter, map)) return -1;
    return 0;
}

TaskNode *task_meta_rename(const TaskNode *src, TaskArena *dst,
                           TaskMetaMap *map) {
    TaskNode *n;
    int ncaps, counter = 0;
    if (count_named(src) == 0) return NULL;         /* nothing to rename    */
    n = clone_into(src, dst);
    if (!n) return NULL;
    ncaps = task_meta_renumber(n, 1) - 1;
    init_identity_map(map, TASK_META_RENAME, ncaps);
    if (rename_named(n, &counter, map)) return NULL;
    return n;
}
