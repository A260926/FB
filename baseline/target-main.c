#include <stdio.h>
int contract_effects(void);
int contract_cycles(void);
int contract_repetition(void);
int contract_ownership(void);
int main(void) {
  int n;
  n = contract_effects(); if (n <= 0) return 2;
  printf("PASS effects assertions=%d\n", n);
  n = contract_cycles(); if (n <= 0) return 2;
  printf("PASS cycles assertions=%d\n", n);
  n = contract_repetition(); if (n <= 0) return 2;
  printf("PASS repetition assertions=%d\n", n);
  n = contract_ownership(); if (n <= 0) return 2;
  printf("PASS ownership assertions=%d\n", n);
  puts("MPC_PROGRESS_CONTRACT_OK groups=4");
  return 0;
}
