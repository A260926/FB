#include <stdio.h>
#include <string.h>
#include "contract.h"
int main(void) {
    const char *groups[] = {"view-mapping", "clip-and-occlusion", "recursion-and-state"};
    unsigned n = 0, g, i, j, total_cases = 0, total_checks = 0;
    const PortalCase *cases = portal_cases(&n);
    if (!cases || !n || n > 10000) return 2;
    for (i = 0; i < n; i++) {
        int known = 0;
        if (!cases[i].group || !cases[i].name || !*cases[i].name || !cases[i].run) return 3;
        for (g = 0; g < 3; g++) if (!strcmp(cases[i].group, groups[g])) known = 1;
        if (!known) return 4;
        for (j = 0; j < i; j++) if (!strcmp(cases[i].name, cases[j].name)) return 5;
    }
    for (g = 0; g < 3; g++) {
        unsigned count = 0, checks = 0;
        for (i = 0; i < n; i++) if (!strcmp(cases[i].group, groups[g])) {
            PortalCheck check = {0, 0};
            int status;
            printf("CASE_START %s %s\n", groups[g], cases[i].name); fflush(stdout);
            status = cases[i].run(&check);
            if (status || !check.checks || check.failures) return 6;
            count++; checks += check.checks;
            printf("CASE_PASS %s %s checks=%u\n", groups[g], cases[i].name, check.checks);
        }
        if (!count || !checks) return 7;
        printf("GROUP_PASS %s cases=%u checks=%u\n", groups[g], count, checks);
        total_cases += count; total_checks += checks;
    }
    printf("PORTAL_TARGET_COMPLETE groups=3 cases=%u checks=%u\n", total_cases, total_checks);
    return 0;
}
