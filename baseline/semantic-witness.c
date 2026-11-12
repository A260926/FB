#include "mpc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static int calls;
static void noop(void *p) { (void)p; }
static int reject(mpc_val_t **p) { (void)p; return 0; }
static int bounded(mpc_val_t **p) { (void)p; return ++calls < 3; }
static int accept(mpc_val_t **p) { (void)p; ++calls; return 1; }
static mpc_parser_t *rejected_char(void) {
  return mpc_check(mpc_char('a'), free, reject, "reject");
}
static void position(const char *name, mpc_parser_t *child, long expected) {
  mpc_result_t r;
  mpc_parser_t *p = mpc_and(2, mpcf_snd, child, mpc_state(), noop);
  assert(mpc_parse("witness", "a", p, &r));
  printf("%s position=%ld\n", name, ((mpc_state_t*)r.output)->pos);
  assert(((mpc_state_t*)r.output)->pos == expected);
  free(r.output); mpc_delete(p);
}
int main(void) {
  mpc_parser_t *p;
  mpc_result_t r;
  int ok;
  position("NOT_CHECK_SUCCESS", mpc_not(rejected_char(), free), 1);
  position("MAYBE_CHECK_SUCCESS", mpc_maybe(rejected_char()), 1);
  position("MANY_CHECK_STOP", mpc_many(mpcf_null, rejected_char()), 1);
  position("OR_CHECK_PASS", mpc_or(2, rejected_char(), mpc_pass()), 1);
  position("AND_REWIND", mpc_maybe(mpc_and(2, mpcf_null,
    mpc_char('a'), mpc_fail("stop"), free)), 0);
  calls = 0;
  /* Two successes and a failure keep this inside the fixed stack slots. */
  p = mpc_count(0, mpcf_null,
    mpc_check(mpc_pass(), noop, bounded, "bounded-stop"), noop);
  ok = mpc_parse("witness", "", p, &r);
  printf("COUNT_ZERO success=%d body_calls=%d\n", ok, calls);
  assert(!ok && calls == 3); mpc_err_delete(r.error); mpc_delete(p);
  calls = 0;
  p = mpc_count(3, mpcf_null,
    mpc_check(mpc_pass(), noop, accept, "unused"), noop);
  ok = mpc_parse("witness", "", p, &r);
  printf("COUNT_POSITIVE success=%d body_calls=%d\n", ok, calls);
  assert(ok && calls == 3); mpc_delete(p);
  puts("MPC_SEMANTICS_OK cases=7");
  return 0;
}
