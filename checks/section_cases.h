/* Test registration ABI only; test bodies belong in tests/section_optimality.c. */
#ifndef FAAC_SECTION_CASES_H
#define FAAC_SECTION_CASES_H
#include <stddef.h>

struct section_result {
    int written_bits;
    int minimum_bits;
    int roundtrip_ok;
};

struct section_case {
    const char *name;
    /* 0: assertions completed; 77: skipped; any other result: failed. */
    int (*run)(struct section_result *result);
};

const struct section_case *section_optimality_cases(size_t *count);
#endif
