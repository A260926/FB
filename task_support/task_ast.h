#ifndef TASK_SUPPORT_AST_H
#define TASK_SUPPORT_AST_H
/* Logical Unicode-scalar AST for the restricted, fully supported grammar:
 *
 *   literal, non-empty explicit character class, concatenation,
 *   alternation, non-capturing group, capturing group (max 8),
 *   bounded repetition {0..4}, pattern depth <= 5.
 *
 * Every node stores Unicode scalar values (uint32_t); encoding to UTF-8/16/32
 * happens in a separate module. No surrogate code points are ever produced. */
#include <stdint.h>
#include "task_rng.h"

#define TASK_MAX_DEPTH   5
#define TASK_MAX_CAPS    8
#define TASK_MAX_SUBJ    128
#define TASK_MAX_NODES   400
#define TASK_MAX_PATLEN  8192   /* scalar/source units for a rendered pattern */
#define TASK_ARENA_CAP   420

typedef enum {
    TASK_LIT = 0,   /* single literal scalar                          */
    TASK_CLASS,     /* explicit, non-empty character class [..]/[^..] */
    TASK_CAT,       /* concatenation of 2..3 children                 */
    TASK_ALT,       /* alternation of 2..3 branches, left first       */
    TASK_GROUP,     /* non-capturing group (?: ... )                   */
    TASK_CAP,       /* capturing group, capno is 1-based, PCRE order  */
    TASK_REP        /* greedy bounded repetition rmin..rmax, 0..4     */
} TaskKind;

typedef struct TaskNode TaskNode;
struct TaskNode {
    TaskKind kind;
    uint32_t cp;                 /* TASK_LIT                          */
    int neg;                     /* TASK_CLASS: negated class         */
    uint32_t chars[8];           /* TASK_CLASS members                */
    int nchars;
    TaskNode *ch[4];             /* TASK_CAT/TASK_ALT/TASK_GROUP/CAP  */
    int nch;
    int capno;                   /* TASK_CAP: 1-based capture number  */
    char name[8];                /* TASK_CAP: optional name, "" none  */
    int rmin, rmax;              /* TASK_REP: inclusive bounds 0..4   */
};

/* Arena plus generation bookkeeping. An arena is reused by resetting 'used';
 * transformed trees live in their own arena so originals stay valid. */
typedef struct {
    TaskNode nodes[TASK_ARENA_CAP];
    int used;
    int caps;                    /* capture numbers handed out so far  */
    int depth_seen;              /* deepest compound nesting generated */
    TaskRng rng;
    /* per-tree feature flags, refreshed by task_gen_tree */
    int has_alt, has_rep, has_supp, has_bmp_nonascii;
} TaskArena;

/* Scalar pools: ASCII, non-ASCII BMP, supplementary plane. All legal Unicode
 * scalar values; the surrogate range D800..DFFF is deliberately absent. */
#define TASK_POOL_ASCII_N 5
#define TASK_POOL_BMP_N   4
#define TASK_POOL_SUP_N   3
extern const uint32_t task_pool_ascii[TASK_POOL_ASCII_N];
extern const uint32_t task_pool_bmp[TASK_POOL_BMP_N];
extern const uint32_t task_pool_supp[TASK_POOL_SUP_N];
#define TASK_POOL_N (TASK_POOL_ASCII_N + TASK_POOL_BMP_N + TASK_POOL_SUP_N)
const uint32_t *task_pool_all(void);

void task_arena_init(TaskArena *a, uint64_t seed);
void task_arena_reset(TaskArena *a);            /* keeps the RNG state     */
TaskNode *task_alloc_node(TaskArena *a);
int task_is_supplementary(uint32_t cp);
int task_is_surrogate(uint32_t cp);
int task_tree_depth(const TaskNode *n);
int task_tree_caps(const TaskNode *n);

#endif
