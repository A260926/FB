#pragma once
#include <cmath>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
namespace check {
struct Case { std::string group, name; std::function<void()> fn; };
inline std::vector<Case>& cases() { static std::vector<Case> v; return v; }
inline unsigned& checks() { static unsigned n = 0; return n; }
struct Register {
    Register(const char* g, const char* n, std::function<void()> f) { cases().push_back({g,n,f}); }
};
inline void require(bool value, const char* expr, const char* file, int line) {
    ++checks();
    if (!value) throw std::runtime_error(std::string(file)+":"+std::to_string(line)+" "+expr);
}
}
#define CHECK(x) ::check::require(bool(x), #x, __FILE__, __LINE__)
#define CASE(group, name) static void case_##group##_##name(); \
    static ::check::Register reg_##group##_##name(#group,#name,case_##group##_##name); \
    static void case_##group##_##name()
