#pragma once
#include <vector>
#include <string>
#include <stdexcept>
struct LegacyCase {const char* name; void (*run)();};
inline std::vector<LegacyCase>& legacy_cases(){static std::vector<LegacyCase> c; return c;}
struct LegacyRegister {LegacyRegister(const char* n, void(*f)()){legacy_cases().push_back({n,f});}};
#define TEST(S,N) void S##_##N(); static LegacyRegister reg_##S##_##N(#S "." #N,S##_##N); void S##_##N()
#define EXPECT_TRUE(x) do {if(!(x)) throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+": " #x);}while(0)
#include <cmath>
#define EXPECT_NEAR(x,y,e) do {if(!(std::isfinite(x)&&std::isfinite(y)&&std::abs((x)-(y))<=(e))) throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+": EXPECT_NEAR");}while(0)
