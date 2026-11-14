#ifndef PBRT_TRANSIENT_CONTRACT_H
#define PBRT_TRANSIENT_CONTRACT_H
#include "tests/gtest/gtest.h"
namespace transient_contract { void Check(bool value,const char*expr,const char*file,int line); }
#define TASK_CHECK(expr) ::transient_contract::Check(bool(expr),#expr,__FILE__,__LINE__)
#endif
