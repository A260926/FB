#include "section_cases.h"
#include <stdio.h>
#include <string.h>

static const char *const required[] = {
    "witness_244_243", "long_31", "short_7", "short_14",
    "group_restart", "zero_identity", "pns_identity", "intensity_identity",
    "signed_spectrum", "escape_16_plus", "escape_only", "quantize_frame"
};

int main(void)
{
    size_t count = 0, passed = 0;
    const size_t needed = sizeof(required) / sizeof(required[0]);
    const struct section_case *cases = section_optimality_cases(&count);
    if (!cases || count != needed) {
        fprintf(stderr, "section registry: expected %zu cases, got %zu\n", needed, count);
        return 1;
    }
    /* Validate the whole table before invoking any callback. */
    for (size_t i = 0; i < needed; ++i) {
        size_t matches = 0;
        for (size_t j = 0; j < count; ++j) {
            if (!cases[j].name || !cases[j].run) {
                fprintf(stderr, "section registry: null entry %zu\n", j);
                return 1;
            }
            matches += strcmp(cases[j].name, required[i]) == 0;
        }
        if (matches != 1) {
            fprintf(stderr, "section registry: %s must occur once\n", required[i]);
            return 1;
        }
    }
    for (size_t i = 0; i < needed; ++i) {
        for (size_t j = 0; j < count; ++j) {
            if (strcmp(cases[j].name, required[i])) continue;
            struct section_result result = {-1, -1, 0};
            printf("SECTION_RUN %s\n", required[i]);
            fflush(stdout);
            int status = cases[j].run(&result);
            if (status != 0 || result.written_bits < 0 || result.minimum_bits < 0 ||
                result.written_bits != result.minimum_bits || result.roundtrip_ok != 1) {
                fprintf(stderr, "SECTION_FAIL %s status=%d written=%d minimum=%d roundtrip=%d\n",
                        required[i], status, result.written_bits, result.minimum_bits,
                        result.roundtrip_ok);
                return 1;
            }
            ++passed;
            printf("SECTION_PASS %s written=%d minimum=%d roundtrip=1\n",
                   required[i], result.written_bits, result.minimum_bits);
        }
    }
    if (passed != needed) return 1;
    printf("FAAC_SECTION_OPTIMAL_OK cases=%zu\n", passed);
    return 0;
}
