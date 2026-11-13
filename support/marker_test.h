#ifndef TASK_MARKER_TEST_H
#define TASK_MARKER_TEST_H
#include <stddef.h>
#include "../src/nanosvg.h"
#include "../src/nanosvgrast.h"
typedef struct MarkerTest { size_t checks, failures; } MarkerTest;
typedef struct MarkerCase { const char* group; const char* name; void (*run)(MarkerTest*); } MarkerCase;
extern const MarkerCase marker_cases[];
extern const size_t marker_case_count;
void marker_check_(MarkerTest*,int,const char*,const char*,int);
#define MARKER_CHECK(ctx,cond) marker_check_((ctx),!!(cond),#cond,__FILE__,__LINE__)
#endif
