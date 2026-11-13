#include "fft_probe.h"
static size_t forward_count, inverse_count;
void task_fft_record(int inverse) { if(inverse) ++inverse_count; else ++forward_count; }
void task_fft_reset(void) { forward_count = inverse_count = 0; }
size_t task_fft_forward(void) { return forward_count; }
size_t task_fft_inverse(void) { return inverse_count; }
