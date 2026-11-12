#include <stdio.h>
int contract_model(void);
int contract_realloc(void);
int contract_pool_topology(void);
int contract_replay(void);
int main(void){int n;
n=contract_model(); if(n<0) return 42; if(n==0) return 2; printf("PASS model assertions=%d\n",n);
n=contract_realloc(); if(n<0) return 42; if(n==0) return 2; printf("PASS realloc assertions=%d\n",n);
n=contract_pool_topology(); if(n<0) return 42; if(n==0) return 2; printf("PASS pool_topology assertions=%d\n",n);
n=contract_replay(); if(n<0) return 42; if(n==0) return 2; printf("PASS replay assertions=%d\n",n);
return 0;}
