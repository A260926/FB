#include <cstdio>
#include <exception>
struct task_case { const char *group; const char *name; int (*run)(void); };
extern const struct task_case task_cases[];
extern const size_t task_case_count;
int task_check(int v,const char*,const char*,int){return v;}
int main(){
 for(size_t i=0;i<task_case_count;i++){
   std::fprintf(stderr,"RUN %s::%s\n",task_cases[i].group,task_cases[i].name);
   try { int r=task_cases[i].run(); std::fprintf(stderr,"  -> %d\n",r); }
   catch(const std::exception&e){ std::fprintf(stderr,"  EXC %s\n",e.what()); }
 }
 return 0;
}
