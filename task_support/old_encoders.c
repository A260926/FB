#include <stdio.h>
extern int test_encoders(void);
int main(void) {
    if (!test_encoders()) return 1;
    puts("OLD_ENCODERS_OK");
    return 0;
}
