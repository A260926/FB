/* The upstream uses only ISO C malloc/free. macOS exposes them in stdlib.h. */
#ifndef TASK_MALLOC_COMPAT_H
#define TASK_MALLOC_COMPAT_H
#include <stdlib.h>
#endif
