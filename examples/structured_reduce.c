/*
  Structural reduction example.

  Drives a real afl_struct_session with more than one fragment, proves that
  deleting a fragment also removes the fragments that depend on it, and checks
  the final accepted bytes.  Prints STRUCT_EXAMPLE_OK on a line of its own only
  after every assertion passes.
*/

#include "afl-struct.h"

#include <stdio.h>
#include <string.h>

/* Acceptance: the candidate must still contain the marker byte 'K'. */

typedef struct {
  int calls;
  size_t first_candidate_len;
} example_ctx;

static int example_observe(const unsigned char *data, size_t len, void *opaque) {
  example_ctx *ctx = (example_ctx *)opaque;
  size_t i;

  ++ctx->calls;
  if (ctx->calls == 1) ctx->first_candidate_len = len;

  for (i = 0; i < len; ++i)
    if (data[i] == 'K') return 1;

  return 0;
}

int main(void) {
  /*
     input: "HdbgK"   (5 bytes)

     frag 1: [0,1) 'H'
     frag 2: [1,4) 'dbg'   needs 1   -> deleting 1 must also delete 2
     frag 3: [4,5) 'K'

     The scan starts with the lowest active ID (1).  Its dependency closure is
     {1, 2}, so the first candidate is just "K" (one byte).  Without the
     closure the candidate would still carry "dbg" and be four bytes long, so
     the one-byte observation proves the closure was applied.
  */

  static const unsigned char input[] = {'H', 'd', 'b', 'g', 'K'};
  const char *table =
      "1 0 1 : -\n"
      "2 1 4 : 1\n"
      "3 4 5 : -\n";

  afl_struct_session *session;
  example_ctx ctx;
  const unsigned char *out;
  size_t out_len = 0;
  int rc;

  memset(&ctx, 0, sizeof(ctx));

  session = afl_struct_create(input, sizeof(input), table);
  if (!session) {
    fprintf(stderr, "example: session creation failed\n");
    return 1;
  }

  rc = afl_struct_step(session, (size_t)-1, example_observe, &ctx);
  if (rc != 0) {
    fprintf(stderr, "example: step did not complete (%d)\n", rc);
    afl_struct_destroy(session);
    return 1;
  }

  if (ctx.calls < 1 || ctx.first_candidate_len != 1) {
    fprintf(stderr, "example: dependency closure not applied "
                    "(first candidate %zu bytes)\n", ctx.first_candidate_len);
    afl_struct_destroy(session);
    return 1;
  }

  out = afl_struct_data(session, &out_len);

  if (out_len != 1 || out[0] != 'K') {
    fprintf(stderr, "example: unexpected result (%zu bytes)\n", out_len);
    afl_struct_destroy(session);
    return 1;
  }

  /* A second candidate (removing the marker itself) must have been tried. */

  if (ctx.calls < 2) {
    fprintf(stderr, "example: search stopped early\n");
    afl_struct_destroy(session);
    return 1;
  }

  afl_struct_destroy(session);

  printf("STRUCT_EXAMPLE_OK\n");
  return 0;
}
