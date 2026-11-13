#include "test_support.h"
unsigned checks;
void run_stream_encoder_tests(void);
int main(void){run_stream_encoder_tests();if(!checks){fputs("zero assertions\n",stderr);return 3;}printf("stream-encoder assertions=%u\n",checks);return 0;}
