/*
 * Shared test runner: fixed hand-checked cases, seeded generated cases and
 * explicit unsupported-input rejection. The ordinary driver and the
 * libFuzzer entry point both call into here.
 */

#ifndef HEX_RUNNER_H
#define HEX_RUNNER_H

#include <stdbool.h>

#include "hex_check.h"

// Runs the fixed cases, checking the hand-computed lists against the oracle
// and the oracle against the scanner.
bool hm_run_fixed(int threshold, hm_stats* stats);

// Generates `cases` patterns and inputs from `seed`, plants at least one
// derivation in every buffer and interleaves fully random buffers.
bool hm_run_generated(unsigned long seed, int cases, int threshold,
                      hm_stats* stats);

// Confirms the model refuses every unsupported pattern and that the
// grammar-illegal subset is also refused by the YARA compiler.
bool hm_run_rejections(int threshold, hm_stats* stats);

// Fuzzer entry: one arbitrary byte string drives one model/scanner round.
int hm_fuzz_one(const unsigned char* data, unsigned long size);

#endif  // HEX_RUNNER_H
