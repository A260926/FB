/* Local test registration ABI. The runner, not the tests, counts checks. */
#ifndef CHECK_CLIP_TEST_H
#define CHECK_CLIP_TEST_H
#include <stddef.h>
#include "nanosvg.h"
#include "nanosvgrast.h"
typedef struct ClipTest ClipTest;
typedef struct ClipCase { const char* group; const char* name; void (*run)(ClipTest*); } ClipCase;
extern const ClipCase clip_cases[];
extern const size_t clip_case_count;
void clip_check(ClipTest*, int condition, const char* expression, const char* file, int line);
#define CLIP_CHECK(ctx, expression) clip_check((ctx), !!(expression), #expression, __FILE__, __LINE__)
#endif
