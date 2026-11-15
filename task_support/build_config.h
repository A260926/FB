/* Headless build configuration; enable upstream error reporting for API callers. */
#include "zfeatures.h"
#undef TGL_FEATURE_ERROR_CHECK
#define TGL_FEATURE_ERROR_CHECK 1
