// Standalone libFuzzer entry point for the MQTT5 property decoder.
//
// Shares mqtt_property_fuzz_core.h with test/mqtt_property_contract.c, so
// both use the same independent model, encoder and expectation checks. Fuzzer
// bytes are mapped to at most a few KB of local buffer, a bounded property
// sequence and bounded length perturbations; no network requests and no files
// are read. Build with, for example:
//   clang -std=c99 -fsanitize=fuzzer,address,undefined -I. \
//       mongoose.c test/mqtt_property_fuzz.c -o mqtt_property_fuzzer
// Running the binary with no arguments performs the fixed-seed smoke run.

#include "mqtt_property_fuzz_core.h"
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  struct mp_case c;
  struct mp_fail f;
  mp_case_from_data(data, size, &c);
  if (!mp_run_case(&c, &f)) {
    fprintf(stderr,
            "FUZZ_FAIL size=%lu seed=%u at=%lu rule_line=%d msg=%s "
            "snippet=%s\n",
            (unsigned long) size, (unsigned) f.seed, (unsigned long) f.at,
            f.line, f.msg, f.snippet);
    abort();  // libFuzzer records this input as a crash
  }
  return 0;
}

#ifdef MP_FUZZ_STANDALONE
int main(void) {
  unsigned seed;
  unsigned executed = 0;
  for (seed = 1; seed <= 300; seed++) {
    struct mp_case c;
    struct mp_fail f;
    mp_case_from_seed(seed, &c);
    executed++;
    if (!mp_run_case(&c, &f)) {
      fprintf(stderr, "FUZZ_SEED seed=%u at=%lu rule_line=%d msg=%s snippet=%s\n",
              seed, (unsigned long) f.at, f.line, f.msg, f.snippet);
      return 1;
    }
  }
  printf("FUZZ_EXECUTED seeds=%u\n", executed);
  return 0;
}
#endif
