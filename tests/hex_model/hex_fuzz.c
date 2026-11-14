/*
 * libFuzzer entry point. This is a separate translation unit from
 * hex_driver.c so the ordinary driver never links libFuzzer. Build it with
 * clang -fsanitize=fuzzer[,address,undefined]. A model/scanner mismatch
 * already prints a replay-ready report in hex_scan.c; here it is turned into
 * an abort so the fuzzer keeps the crashing input.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "hex_runner.h"
#include "hex_scan.h"

int LLVMFuzzerInitialize(int* argc, char*** argv) {
  (void) argc;
  (void) argv;
  return hm_scan_init();
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  int rc = hm_fuzz_one(data, (unsigned long) size);
  if (rc != 0) abort();
  return 0;
}
