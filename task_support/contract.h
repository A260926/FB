#ifndef CHECK_EMITTER_CONTRACT_H
#define CHECK_EMITTER_CONTRACT_H
#include "tests/gtest/gtest.h"
namespace emitter_contract { void Check(bool value, const char *expr, const char *file, int line); }
#define TASK_CHECK(expr) ::emitter_contract::Check(bool(expr), #expr, __FILE__, __LINE__)
#endif
