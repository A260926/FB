/*
 * Reassembly fuzzing support.
 *
 * This header is consumable from both C11 and C++17 translation
 * units.  It drives the real ngtcp2 stream/ROB reassembly path with
 * locally generated legal bodies and offsets, and checks the observed
 * behavior against an independent byte-presence model.
 */
#ifndef RXA_REASSEMBLY_H
#define RXA_REASSEMBLY_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif /* defined(__cplusplus) */

/*
 * rxcheck_result is the accounting of one executed schedule.  Every
 * counter starts from zero.
 */
typedef struct rxcheck_result {
  /* operations is the number of parsed records, including SKIPed
     ones.  Internal finalization is not counted. */
  uint64_t operations;
  /* accepted_bytes counts uniquely received bytes across all streams,
     including bytes accepted while finalizing surviving streams. */
  uint64_t accepted_bytes;
  /* delivered_bytes counts bytes actually delivered through the
     production consumption path. */
  uint64_t delivered_bytes;
  /* canceled is the number of streams released by CANCEL. */
  uint64_t canceled;
  /* oom_retries counts FEED calls which failed with NOMEM and then
     succeeded on retry against the same live object. */
  uint64_t oom_retries;
} rxcheck_result;

/*
 * rxcheck_run parses and executes one RXA1 schedule.  |result| must be
 * non-NULL; |trace| may be NULL, in which case no trace is written.
 *
 * It returns 0 when the production objects match the independent
 * model at every operation boundary, 1 on the first model difference
 * and 2 when the input is malformed.
 */
int rxcheck_run(const uint8_t *data, size_t size, rxcheck_result *result,
                FILE *trace);

/*
 * rxcheck_mutate derives a bounded, syntactically legal RXA1 schedule
 * from |data| of length |size|, using |seed| as the deterministic
 * randomness source.  The output, including the header, never exceeds
 * |max_size| bytes.  An invalid input restarts from an empty schedule.
 * It returns 0 when |max_size| is smaller than the 5-byte header.
 */
size_t rxcheck_mutate(uint8_t *data, size_t size, size_t max_size,
                      unsigned int seed);

#ifdef __cplusplus
}
#endif /* defined(__cplusplus) */

#endif /* !defined(RXA_REASSEMBLY_H) */
