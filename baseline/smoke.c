#include "mpc.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
int main(void) {
  mpc_parser_t *p = mpc_many(mpcf_strfold, mpc_char('a'));
  mpc_result_t r;
  assert(mpc_parse("local", "aaa", p, &r));
  assert(strcmp(r.output, "aaa") == 0); free(r.output);
  mpc_delete(p);
  p = mpc_or(2, mpc_pass(), mpc_fail("unused"));
  assert(mpc_parse("local", "", p, &r));
  mpc_delete(p);
  puts("MPC_BASELINE_OK consume=3 ordered-empty=1");
  return 0;
}
