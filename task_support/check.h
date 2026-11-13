#pragma once
#include <cstdio>
#include <cstdlib>
#include <vector>
struct RetargetCase { const char* name; void(*fn)(); };
inline std::vector<RetargetCase>& retarget_cases(){ static std::vector<RetargetCase> v; return v; }
inline int retarget_assertions=0;
struct RetargetRegister { RetargetRegister(const char* n,void(*f)()){retarget_cases().push_back({n,f});} };
#define RETARGET_CASE(name) static void name(); static RetargetRegister reg_##name(#name,name); static void name()
#define RETARGET_CHECK(expr) do { ++retarget_assertions; if(!(expr)){std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#expr);std::exit(1);} } while(0)
int main(){if(retarget_cases().size()<6){std::fprintf(stderr,"at least six cases required\n");return 2;} for(auto c:retarget_cases()){auto before=retarget_assertions;c.fn();if(retarget_assertions==before)return 3;std::printf("retarget case %s ok\n",c.name);}std::printf("RETARGET_OK cases=%zu assertions=%d\n",retarget_cases().size(),retarget_assertions);}
