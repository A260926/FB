#ifndef TASK_SUPPORT_TASK_ENGINE_H
#define TASK_SUPPORT_TASK_ENGINE_H
/* Real PCRE2 execution across all three code-unit widths. Each width is
 * compiled and matched through the genuine library API (interpretive match,
 * PCRE2_UTF, no JIT). Offsets returned in code units are mapped back to scalar
 * indices through that width's own boundary table; an offset that lands inside
 * a code point is reported as an explicit failure. */
#include "task_ast.h"
#include "task_enc.h"
#include "task_oracle.h"

typedef enum {
    TASK_ENG_MATCH = 0,
    TASK_ENG_NOMATCH,
    TASK_ENG_COMPILE_ERROR,   /* surfaced, never swallowed/filtered */
    TASK_ENG_RUNTIME_ERROR,
    TASK_ENG_BAD_BOUNDARY     /* an offset fell inside a code point */
} TaskEngStatus;

typedef struct {
    TaskEngStatus status;
    int errcode;                /* PCRE2 negative code when applicable */
    int start, end;             /* scalar span, valid on MATCH */
    int ncaps;                  /* capture count reported by the pattern */
    TaskCap cap[TASK_MAX_CAPS + 1];
} TaskEngResult;

/* Run one width. pattern is the rendered scalar pattern source; text holds
 * the encoded subject and boundary tables. width is 8, 16 or 32. */
TaskEngResult task_engine_run(int width, const uint32_t *pattern, int patlen,
                              const TaskText *text);

/* Compile the pattern and resolve a capture name. Returns the 1-based capture
 * number when the name exists, or -1 when it does not / on compile failure. */
int task_engine_name_resolves(int width, const uint32_t *pattern, int patlen,
                              const char *name);
#endif
