#include "suite.h"
int unit_checks=0;
int main(){
unit_checks=0; { int n=unit_run_group("replay"); if(n<=0 || n!=unit_checks) return 3; std::printf("GROUP_OK replay %d\n",n); }
unit_checks=0; { int n=unit_run_group("large_mesh"); if(n<=0 || n!=unit_checks) return 3; std::printf("GROUP_OK large_mesh %d\n",n); }
unit_checks=0; { int n=unit_run_group("mutations"); if(n<=0 || n!=unit_checks) return 3; std::printf("GROUP_OK mutations %d\n",n); }
std::puts("TARGET_OK"); return 0; }
