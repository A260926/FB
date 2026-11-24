#include "task_support/work.h"
uint64_t task_spectral_passes, task_synth_passes;
void task_granule_work(unsigned stage) {
  if (stage == 0) ++task_spectral_passes;
  else ++task_synth_passes;
}
