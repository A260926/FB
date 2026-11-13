#ifndef CHECK_CONTENT_CHECK_H
#define CHECK_CONTENT_CHECK_H
#include <stddef.h>
#include <stdio.h>
extern size_t check_check_count;
#define CHECK_CHECK(expr) do { ++check_check_count; if (!(expr)) { fprintf(stderr,"content check failed %s:%d: %s\n",__FILE__,__LINE__,#expr); return -1; } } while(0)
#endif
