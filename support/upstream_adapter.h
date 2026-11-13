#pragma once
#include "check.h"
#include <cal3d/streamops.h>
#include <cal3d/memory.h>
#include <cal3d/vector4.h>
#define TEST(name) CASE(legacy,name)
#define FIXTURE(F) struct F
#define SETUP(F) F()
#define TEST_F(F,N) struct F##_##N : F { void run(); }; \
    static check::Register reg_##F##_##N("legacy",#F "/" #N,[]{F##_##N().run();}); \
    void F##_##N::run()
#define ABSTRACT_TEST(N) template<class F> struct abstract_##N : F { void run(); }; \
    template<class F> void abstract_##N<F>::run()
#define APPLY_TEST_F(F,N) static check::Register reg_##F##_##N("legacy",#F "/" #N,[]{abstract_##N<F>().run();});
#define CHECK_EQUAL(a,b) CHECK((a)==(b))
#define CHECK_CLOSE(a,b,t) CHECK(std::fabs(double(a)-double(b))<=double(t))
