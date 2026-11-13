#include "suite.h"
int unit_checks=0;
int main(){
unit_checks=0; { int n=unit_run_group("quads"); if(n<=0 || n!=unit_checks) return 3; std::printf("GROUP_OK quads %d\n",n); }
unit_checks=0; { int n=unit_run_group("clipping"); if(n<=0 || n!=unit_checks) return 3; std::printf("GROUP_OK clipping %d\n",n); }
unit_checks=0; { int n=unit_run_group("compat"); if(n<=0 || n!=unit_checks) return 3; std::printf("GROUP_OK compat %d\n",n); }
std::puts("TARGET_OK"); return 0; }
