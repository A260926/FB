#ifndef TASK_FFT_PROBE_H
#define TASK_FFT_PROBE_H
#include <stddef.h>
void task_fft_record(int inverse);
void task_fft_reset(void);
size_t task_fft_forward(void);
size_t task_fft_inverse(void);
#endif
