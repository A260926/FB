#include "suite.h"
int unit_checks=0;
int main(){
unit_checks=0; { int n=unit_run_group("lowering"); if(n<=0 || n!=unit_checks) return 3; std::printf("GROUP_OK lowering %d\n",n); }
unit_checks=0; { int n=unit_run_group("evaluation"); if(n<=0 || n!=unit_checks) return 3; std::printf("GROUP_OK evaluation %d\n",n); }
unit_checks=0; { int n=unit_run_group("compat"); if(n<=0 || n!=unit_checks) return 3; std::printf("GROUP_OK compat %d\n",n); }
std::puts("TARGET_OK"); return 0; }
