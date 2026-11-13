/* Normal-input compile probe only. This is not a graph implementation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "regint.h"

static const char *patterns[] = {
    "ab[cd].",
    "(?:ab|c[de])f",
    "(?:ab){2,5}?",
    "(?:a|bc){3,12}",
    "(?<piece>ab){0}\\g<piece>\\g<piece>",
    "(?<node>a(?:\\g<node>)?b)",
    "(?<left>a\\g<right>?b){0}(?<right>c\\g<left>?d){0}\\g<left>"
};

int main(void) {
    OnigEncoding enc = ONIG_ENCODING_ASCII;
    size_t k;
    unsigned long total_ops = 0;
    int rc = onig_initialize(&enc, 1);
    if (rc != ONIG_NORMAL) {
        fprintf(stderr, "onig_initialize failed: %d\n", rc);
        return 1;
    }
    for (k = 0; k < sizeof(patterns) / sizeof(patterns[0]); ++k) {
        OnigRegex re = NULL;
        OnigErrorInfo error;
        const UChar *begin = (const UChar *)patterns[k];
        unsigned i, calls = 0, returns = 0, repeats = 0;
        rc = onig_new(&re, begin, begin + strlen(patterns[k]),
                      ONIG_OPTION_NONE, enc, ONIG_SYNTAX_RUBY, &error);
        if (rc != ONIG_NORMAL) {
            UChar message[ONIG_MAX_ERROR_MESSAGE_LEN];
            onig_error_code_to_str(message, rc, &error);
            fprintf(stderr, "normal pattern %lu compile failed: %s\n",
                    (unsigned long)k, message);
            onig_end();
            return 2;
        }
        if (re->ops_used == 0 || re->ops == NULL) {
            fprintf(stderr, "empty compiled result: %lu\n", (unsigned long)k);
            onig_free(re); onig_end(); return 3;
        }
#ifdef USE_DIRECT_THREADED_CODE
        if (re->ocs == NULL) {
            fprintf(stderr, "missing ocs\n");
            onig_free(re); onig_end(); return 4;
        }
#endif
        for (i = 0; i < re->ops_used; ++i) {
#ifdef USE_DIRECT_THREADED_CODE
            enum OpCode code = re->ocs[i];
#else
            enum OpCode code = re->ops[i].opcode;
#endif
            if (code < OP_FINISH || code > OP_CALLOUT_NAME) {
                fprintf(stderr, "unexpected opcode at %u\n", i);
                onig_free(re); onig_end(); return 5;
            }
            calls += code == OP_CALL;
            returns += code == OP_RETURN;
            repeats += code == OP_REPEAT || code == OP_REPEAT_NG;
        }
        if (k >= 4 && (calls == 0 || returns == 0)) {
            fprintf(stderr, "subcall pattern lost call/return: %lu\n", (unsigned long)k);
            onig_free(re); onig_end(); return 6;
        }
        total_ops += re->ops_used;
        printf("COMPILE case=%lu ops=%u calls=%u returns=%u repeats=%u\n",
               (unsigned long)k, re->ops_used, calls, returns, repeats);
        onig_free(re);
    }
    onig_end();
#ifdef USE_DIRECT_THREADED_CODE
    printf("BASELINE_OK mode=direct patterns=7 total_ops=%lu\n", total_ops);
#else
    printf("BASELINE_OK mode=switch patterns=7 total_ops=%lu\n", total_ops);
#endif
    return 0;
}
