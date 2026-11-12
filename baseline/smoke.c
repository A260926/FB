#include "libinjection.h"
#include "libinjection_sqli.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    char fp[8];
    struct libinjection_sqli_state s;
    assert(libinjection_sqli("hello world", 11, fp) == 0 && fp[0] == 0);
    assert(libinjection_sqli("1 OR 1=1", 8, fp) == 1);
    assert(strcmp(fp, "1&1") == 0);
    const char *q = "SELECT /* outer /* inner */ tail */ 7";
    libinjection_sqli_init(&s, q, strlen(q), FLAG_SQL_ANSI|FLAG_QUOTE_NONE);
    assert(libinjection_sqli_tokenize(&s) == 1);
    assert(s.current->type == 'E');
    assert(libinjection_sqli_tokenize(&s) == 1);
    assert(s.current->type == 'X');
    printf("BASELINE_SQLI_OK fp=%s nested=legacy-X\n", fp);
    return 0;
}
