/* Original C contract runner, MIT licensed. A nonzero result is always failure. */
#include <stdio.h>
int test_flow(void);
int test_delayed_voice(void);
int test_row_delay(void);
int test_export_boundary(void);
int main(void)
{
    int i;
    int (*const cases[])(void) = {
        test_flow, test_delayed_voice, test_row_delay, test_export_boundary
    };
    const char *const names[] = {
        "flow", "delayed-voice", "row-delay", "export-boundary"
    };
    for (i = 0; i < 4; ++i) {
        int result = cases[i]();
        if (result != 0) {
            fprintf(stderr, "CASE %s FAIL result=%d\n", names[i], result);
            return 1;
        }
        printf("CASE %s PASS\n", names[i]);
    }
    puts("POCKETMOD_ROW_SCHEDULER_OK cases=4");
    return 0;
}
