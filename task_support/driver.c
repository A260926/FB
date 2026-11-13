#include "check.h"
#include <stdio.h>
#include <string.h>
static unsigned checks;
int task_check(int value, const char *expr, const char *file, int line) {
    ++checks;
    if (!value) fprintf(stderr, "FAIL %s:%d %s\n", file, line, expr);
    return value;
}
int main(void) {
    const char *groups[] = {"layout","limits","failure","model"};
    size_t g, i;
    unsigned total = 0;
    if (!task_case_count || task_case_count > 10000) return 2;
    for (g = 0; g < sizeof(groups)/sizeof(groups[0]); ++g) {
        unsigned ran = 0, n = 0;
        for (i = 0; i < task_case_count; ++i) {
            if (!task_cases[i].group || !task_cases[i].name || !task_cases[i].run) return 3;
            if (strcmp(groups[g], task_cases[i].group)) continue;
            checks = 0;
            if (task_cases[i].run() || !checks) return 4;
            printf("CASE %s %s checks=%u\n", groups[g], task_cases[i].name, checks);
            ++ran; n += checks; ++total;
        }
        if (!ran || !n) return 5;
        printf("GROUP %s cases=%u checks=%u COMPLETE\n", groups[g], ran, n);
    }
    if (total != task_case_count) return 6;
    printf("TARGET_COMPLETE cases=%u\n", total);
    return 0;
}
