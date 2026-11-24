/*
 * libFuzzer harness for the recoverable reassembly path.  Both the
 * test entry point and the custom mutator delegate to the same
 * rxcheck_run / rxcheck_mutate implementation used by the replay
 * driver.
 */
#include <stddef.h>
#include <stdint.h>

#include <cstddef>
#include <cstdint>

extern "C" {
#include "reassembly.h"
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  rxcheck_result result;

  int rv = rxcheck_run(data, size, &result, NULL);

  /* Malformed inputs are ignored; any model difference is fatal. */
  if (rv == 1) {
    __builtin_trap();
  }

  return 0;
}

extern "C" size_t LLVMFuzzerCustomMutator(uint8_t *data, size_t size,
                                          size_t max_size,
                                          unsigned int seed) {
  return rxcheck_mutate(data, size, max_size, seed);
}
