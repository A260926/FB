#ifndef TASK_SUPPORT_TASK_RENDER_H
#define TASK_SUPPORT_TASK_RENDER_H
/* Rendering of the logical AST into PCRE pattern source (as a scalar stream)
 * and into a stable textual form for replay output/diffs. */
#include "task_ast.h"

/* Render the tree as PCRE2 pattern source scalars into out (capacity outcap,
 * measured in scalars). Returns the length, or -1 if it would not fit. */
int task_render_pattern(const TaskNode *n, uint32_t *out, int outcap);

/* Serialize the logical AST into text (depth-first, fully parenthesized),
 * e.g. CAT(ALT(LIT(a),LIT(\x{e9})),CAP1[ n1 ](REP{0,2}(...))). */
int task_serialize_ast(const TaskNode *n, char *out, int outcap);

/* Serialize a scalar subject as space-separated U+XXXX tokens. */
int task_serialize_scalars(const uint32_t *s, int n, char *out, int outcap);
#endif
