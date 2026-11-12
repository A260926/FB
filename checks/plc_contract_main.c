#include "plc_contract.h"
#include <stdio.h>
#include <string.h>
int main(void) {
 if (plc_case_count < 12) return 40;
 for (size_t i=0;i<plc_case_count;++i) {
  if (!plc_cases[i].name || !*plc_cases[i].name || !plc_cases[i].run) return 41;
  for (size_t j=0;j<i;++j) if (!strcmp(plc_cases[i].name,plc_cases[j].name)) return 42;
  int rc=plc_cases[i].run();
  if (rc!=1) { fprintf(stderr,"G722_PLC_CASE_FAIL name=%s return=%d\n",plc_cases[i].name,rc);return 43; }
  printf("G722_PLC_CASE_PASS %s\n",plc_cases[i].name);
 }
 printf("G722_PLC_CONTRACT_OK cases=%zu\n",plc_case_count);
 return 0;
}
