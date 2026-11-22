#include <stdint.h>
uint64_t task_synth_work;
#define MINIMP3_SYNTH_WORK(n) (task_synth_work += (uint64_t)(n))
#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"
