/* Original task support, CC0-1.0. This file contains no decoder assertions. */
#include "check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
void task_check_impl(int ok,const char *expression,const char *file,int line) {
    checks++;
    if(!ok) {fprintf(stderr,"CHECK_FAILED %s:%d %s\n",file,line,expression);exit(41);}
}
int main(int argc, char **argv) {
    const char *group = (argc > 1) ? argv[1] : "all";
    static const char *all_groups[] = {
        "lists", "prediction", "subpart", "filter", "reject", "lifecycle"
    };
    if (strcmp(group, "all") == 0) {
        for (unsigned i = 0; i < sizeof(all_groups)/sizeof(all_groups[0]); i++) {
            checks = 0;
            int completed = task_cases(all_groups[i]);
            if (completed != 1 || checks == 0) {
                fprintf(stderr, "INCOMPLETE %s checks=%u return=%d\n",
                    all_groups[i], checks, completed);
                return 43;
            }
            printf("GROUP %s checks=%u COMPLETE\n", all_groups[i], checks);
            fflush(stdout);
        }
    } else {
        checks = 0;
        int completed = task_cases(group);
        if (completed != 1 || checks == 0) {
            fprintf(stderr, "INCOMPLETE %s checks=%u return=%d\n",
                group, checks, completed);
            return 43;
        }
        printf("GROUP %s checks=%u COMPLETE\n", group, checks);
    }
    puts("H264BSD_B_EXPLICIT_OK");
    return 0;
}
