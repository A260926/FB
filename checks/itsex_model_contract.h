#ifndef ITSEX_MODEL_CONTRACT_H
#define ITSEX_MODEL_CONTRACT_H
#include <stddef.h>
/* Status: 0=all compared PCM equal; 1=PCM differs; 2=setup/decode/load error;
 * 77=skipped. Only status 0 passes the normal run. No test-owned main(). */
struct itsex_model_result {
    size_t samples;             /* Actual PCM comparisons, not generated size. */
    size_t width_data[18];       /* Data symbols only; index is code width. */
    size_t switches[3];          /* Low sentinel, middle range, high-bit control. */
    size_t control_byte_crossings, control_cache_crossings;
    size_t memory_loads, callback_loads;
    size_t first_sample;
    unsigned expected, actual;  /* Unsigned PCM bit patterns on mismatch. */
};
struct itsex_model_case {
    const char *name;
    int (*run)(struct itsex_model_result *);
};
/* Defined by test-dev/test_itsex_model.c. Storage lives for the whole run. */
size_t itsex_model_register(const struct itsex_model_case **cases);
#endif
