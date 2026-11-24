/*
 * Public ownership-replay entry point.
 *
 * The wire format and the semantics reported here are defined in
 * task_support/contract.md. The executor only calls the public QuickJS C
 * API; it never reads internal ref counts and never feeds the engine's
 * real finalization results back into the expected model.
 */
#ifndef FUZZ_OWNERSHIP_REPLAY_H
#define FUZZ_OWNERSHIP_REPLAY_H

#include <stddef.h>
#include <stdint.h>

struct qjs_ownership_report {
    size_t steps;        /* executed records */
    size_t created;      /* logical tracked objects created this run */
    size_t finalized;    /* real, identity-by-identity finalizations */
    size_t comparisons;  /* ownership responsibility checks performed */
    size_t exceptions;   /* entries into the pending exception slot */
};

/* Replay one serialized plan in a brand new runtime/context and perform
 * the full cleanup itself.
 *  - returns 0 on success,
 *  - 1 for a truncated or otherwise invalid whole plan (engine not called),
 *  - 2 on a model/engine divergence, after printing the first difference.
 * On return 0, *out is zeroed then filled with the real observations.
 * The process is never terminated by this function. */
int qjs_ownership_run(const uint8_t *data, size_t size,
                      struct qjs_ownership_report *out);

#endif /* FUZZ_OWNERSHIP_REPLAY_H */
