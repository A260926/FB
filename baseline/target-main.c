#include <stdio.h>
int contract_vectors(void);
int contract_mapping(void);
int contract_streaming(void);
int contract_errors(void);
int main(void){int n;
n=contract_vectors(); if(n<=0) return 2; printf("PASS vectors assertions=%d\n",n);
n=contract_mapping(); if(n<=0) return 2; printf("PASS mapping assertions=%d\n",n);
n=contract_streaming(); if(n<=0) return 2; printf("PASS streaming assertions=%d\n",n);
n=contract_errors(); if(n<=0) return 2; printf("PASS errors assertions=%d\n",n);
return 0;}
