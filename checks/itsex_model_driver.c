#include "itsex_model_contract.h"
#include <stdio.h>
#include <string.h>
struct required_case { const char *name; int bits; size_t minimum; int public_io; };
static const struct required_case required[] = {
    {"core-8-214-short",8,61,0}, {"core-8-214-full",8,32768,0},
    {"core-8-214-tail",8,32785,0}, {"core-8-215-short",8,61,0},
    {"core-8-215-full",8,32768,0}, {"core-8-215-tail",8,32785,0},
    {"core-16-214-short",16,61,0}, {"core-16-214-full",16,16384,0},
    {"core-16-214-tail",16,16401,0}, {"core-16-215-short",16,61,0},
    {"core-16-215-full",16,16384,0}, {"core-16-215-tail",16,16401,0},
    {"public-8-214-stereo",8,131140,1}, {"public-8-215-stereo",8,131140,1},
    {"public-16-214-stereo",16,65604,1}, {"public-16-215-stereo",16,65604,1}
};
int main(int argc, char **argv)
{
    const struct itsex_model_case *cases = NULL;
    size_t count = itsex_model_register(&cases), i, j, executed = 0;
    struct itsex_model_result totals[2];
    const char *selected = NULL;
    int mismatch = 0;
    memset(totals, 0, sizeof(totals));
    if (argc == 3 && !strcmp(argv[1], "--expect-pcm-mismatch")) selected = argv[2];
    else if (argc != 1) return 2;
    if (!cases || count != sizeof(required)/sizeof(required[0])) {
        fprintf(stderr, "REGISTRY_ERROR expected=16 actual=%lu\n", (unsigned long)count);
        return 2;
    }
    /* Validate the complete registry before any callback can claim a pass. */
    for (i = 0; i < count; i++) {
        int found = 0;
        if (!cases[i].name || !cases[i].run) return 2;
        for (j = 0; j < count; j++) if (!strcmp(cases[i].name, required[j].name)) found++;
        if (found != 1) return 2;
        for (j = 0; j < i; j++) if (!strcmp(cases[i].name, cases[j].name)) return 2;
    }
    for (i = 0; i < count; i++) {
        struct itsex_model_result result;
        const struct itsex_model_case *test = NULL;
        int rc, k, group = required[i].bits == 16;
        for (j = 0; j < count; j++) if (!strcmp(required[i].name, cases[j].name)) test = &cases[j];
        if (!test) return 2;
        if (selected && strcmp(selected, test->name)) continue;
        memset(&result, 0, sizeof(result));
        rc = test->run(&result);  /* Actual function call, not a test-owned marker. */
        executed++;
        if (selected && rc == 1 && result.samples > 0 && result.expected != result.actual) {
            printf("PCM_MISMATCH %s sample=%lu expected=%u actual=%u\n", test->name,
                   (unsigned long)result.first_sample, result.expected, result.actual);
            mismatch = 1;
            continue;
        }
        if (rc != 0 || result.samples < required[i].minimum ||
            (required[i].public_io && (!result.memory_loads || !result.callback_loads))) {
            fprintf(stderr, "CASE_ERROR %s rc=%d compared=%lu minimum=%lu\n", test->name,
                    rc, (unsigned long)result.samples, (unsigned long)required[i].minimum);
            return 2;
        }
        printf("PASS %s compared=%lu memory=%lu callbacks=%lu\n", test->name,
               (unsigned long)result.samples, (unsigned long)result.memory_loads,
               (unsigned long)result.callback_loads);
        for (k = 1; k <= required[i].bits + 1; k++) totals[group].width_data[k] += result.width_data[k];
        for (k = 0; k < 3; k++) totals[group].switches[k] += result.switches[k];
        totals[group].control_byte_crossings += result.control_byte_crossings;
        totals[group].control_cache_crossings += result.control_cache_crossings;
    }
    if (selected) {
        if (executed != 1 || !mismatch) { fprintf(stderr,"EXPECTED_PCM_DIFFERENCE_MISSING\n"); return 2; }
        printf("ITSEX_MODEL_DIFFERENCE_OK case=%s\n", selected);
        return 0;
    }
    if (executed != 16) return 2;
    for (i = 0; i < 2; i++) {
        int width, bits = i ? 16 : 8;
        for (width = 1; width <= bits + 1; width++) {
            if (!totals[i].width_data[width]) { fprintf(stderr,"ZERO_WIDTH_COVERAGE bits=%d width=%d\n",bits,width); return 2; }
            printf("WIDTH bits=%d width=%d data=%lu\n",bits,width,(unsigned long)totals[i].width_data[width]);
        }
        for (j = 0; j < 3; j++) {
            if (!totals[i].switches[j]) return 2;
            printf("SWITCH bits=%d class=%lu count=%lu\n",bits,(unsigned long)j,(unsigned long)totals[i].switches[j]);
        }
        if (!totals[i].control_byte_crossings || !totals[i].control_cache_crossings) return 2;
        printf("CONTROL_CROSS bits=%d byte=%lu cache32=%lu\n",bits,
               (unsigned long)totals[i].control_byte_crossings,(unsigned long)totals[i].control_cache_crossings);
    }
    puts("ITSEX_MODEL_CASES_OK cases=16");
    return 0;
}
