// libFuzzer entry point for the MQTT5 property decode contract.
//
// Shares test/mqtt_property_core.h with test/mqtt_property_contract.c.
// The only mapping is from the fuzzer bytes to a local buffer of at most
// MQTT_PROP_CORE_MAX_INPUT bytes; no network or filesystem access.
//
// Build with:
//   cc -std=c99 -g -O1 -fsanitize=fuzzer,address,undefined \
//      -I. mongoose.c test/mqtt_property_fuzz.c -o mqtt_property_fuzzer
#include "../mongoose.h"

#include <stdint.h>
#include <stdlib.h>

#include "../task_support/check.h"
#include "mqtt_property_core.h"

// The core reports assertions through task_check; record them and let the
// caller turn the returned failure into a reproducible abort.
static unsigned fuzz_checks;
int task_check(int value, const char *expr, const char *file, int line) {
  ++fuzz_checks;
  if (!value) fprintf(stderr, "FUZZ_CHECK_FAIL %s:%d %s\n", file, line, expr);
  return value;
}

// Same seed derivation as core_generate(), so a crash can be replayed.
static uint32_t fuzz_seed_of(const uint8_t *data, size_t size) {
  uint32_t seed = 2166136261u;
  size_t i;
  for (i = 0; i < size; i++) seed = (seed ^ data[i]) * 16777619u;
  seed ^= (uint32_t) size * 2654435761u;
  return seed ? seed : 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  uint8_t local[MQTT_PROP_CORE_MAX_INPUT];
  size_t n = size < MQTT_PROP_CORE_MAX_INPUT ? size : MQTT_PROP_CORE_MAX_INPUT;
  unsigned before = fuzz_checks;

  if (size > 0 && n > 0) memcpy(local, data, n);
  if (core_run(local, n) != 0) {
    // A failed assertion (not a sanitizer report) must be reproducible too:
    // print the seed and a minimized but still-failing byte fragment.
    uint8_t work[MQTT_PROP_CORE_MAX_INPUT];
    size_t shrunk;
    if (n > 0) memcpy(work, local, n);
    shrunk = n > 0 ? core_shrink(work, n) : 0;
    core_report_failure(fuzz_seed_of(data, size), data, size, 64);
    fprintf(stderr, "FUZZ_MINIMIZED size=%lu bytes=",
            (unsigned long) (shrunk ? shrunk : n));
    if (shrunk) {
      size_t i;
      for (i = 0; i < shrunk; i++) fprintf(stderr, "%02x", work[i]);
    } else if (n > 0) {
      size_t i;
      for (i = 0; i < n; i++) fprintf(stderr, "%02x", local[i]);
    }
    fprintf(stderr, "\nFUZZ_CHECKS_BEFORE=%u AFTER=%u\n", before, fuzz_checks);
    abort();
  }
  return 0;
}

#ifdef MQTT_PROP_FUZZ_SELFTEST
// Optional standalone smoke test (not used by the contract driver):
//   cc -D MQTT_PROP_FUZZ_SELFTEST ... mqtt_property_fuzz.c
// It paints lengths, start offset, and payload bytes with a small state
// machine so that all seven representations, multi-property lists, and
// 127/128 length edges show up without libFuzzer.
int main(void) {
  uint8_t buf[512];
  unsigned seed, executed = 0, decodes = 0;
  for (seed = 0; seed < 2048; seed++) {
    uint32_t s = seed * 2654435761u + 1;
    size_t i, n = 3 + (seed % 300);
    unsigned d0 = core_decodes;
    s ^= s << 13, s ^= s >> 17, s ^= s << 5;
    buf[0] = (uint8_t) (s % 8);        // property count - 1
    buf[1] = (uint8_t) ((s >> 8) % 7);  // starting representation
    buf[2] = (uint8_t) ((s >> 16) % 8);  // bytes before the property section
    for (i = 3; i < n; i++) {
      s ^= s << 13, s ^= s >> 17, s ^= s << 5;
      buf[i] = (uint8_t) s;
    }
    LLVMFuzzerTestOneInput(buf, n);
    TASK_CHECK(core_decodes >= d0);
    decodes += core_decodes - d0;
    executed++;
  }
  // The run is only meaningful if properties were actually compared.
  TASK_CHECK(decodes > executed);
  printf("FUZZ_SELFTEST_OK executed=%u decodes=%u checks=%u\n", executed,
         decodes, fuzz_checks);
  return 0;
}
#endif
