#include "suite.h"
int unit_checks=0;
int main(){
unit_checks=0; { int n=unit_run_group("packing"); if(n<=0 || n!=unit_checks) return 3; std::printf("GROUP_OK packing %d\n",n); }
unit_checks=0; { int n=unit_run_group("execution"); if(n<=0 || n!=unit_checks) return 3; std::printf("GROUP_OK execution %d\n",n); }
unit_checks=0; { int n=unit_run_group("compat"); if(n<=0 || n!=unit_checks) return 3; std::printf("GROUP_OK compat %d\n",n); }
std::puts("TARGET_OK"); return 0; }
