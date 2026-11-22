#include "src/mix_sinc.h"
#include "src/mix_all.h"
/* The new mixer table must be provided by the built production archive. */
extern const MIXER_FP libxmp_sinc_mixers[];
int task_sinc_binding(void) { return libxmp_sinc_mixers[0] == 0; }
