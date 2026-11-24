#ifndef TASK_GRANULE_WORK_H
#define TASK_GRANULE_WORK_H
#include <stdint.h>
extern uint64_t task_spectral_passes, task_synth_passes;
void task_granule_work(unsigned stage);
#define MAD_GRANULE_SPECTRAL_BEGIN() task_granule_work(0)
#define MAD_GRANULE_SYNTH_BEGIN() task_granule_work(1)
#endif
