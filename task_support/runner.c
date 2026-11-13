#include "check.h"
static unsigned cases,checks;
static const char *current;
void task_case(const char *name) { if(!name || !*name)exit(78); ++cases; printf("CASE %s %s\n",current,name); }
void task_check(int ok,const char *expr,const char *file,int line) {
  if(!cases)exit(78);
  ++checks;
  if(!ok) { fprintf(stderr,"CHECK %s:%d %s\n",file,line,expr);exit(41); }
}
void contract_medians(void);
void contract_stream(void);
void contract_stems(void);
static void run(const char *name,void (*f)(void)) {
  current=name;cases=checks=0;
  printf("BEGIN %s\n",name);fflush(stdout);
  f();
  if(!cases||!checks)exit(78);
  printf("DONE %s cases=%u checks=%u\n",name,cases,checks);fflush(stdout);
}
int main(void) {
  run("medians",contract_medians);
  run("stems",contract_stems);
  run("stream",contract_stream);
  puts("HPSS_ALL_DONE groups=3");return 0;
}
