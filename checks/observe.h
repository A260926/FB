/* Test-side implementation of the observation hook installed in
 * src/denoise.c (only under RNNOISE_TEST_OBSERVE). */
#ifndef RNNOISE_OBSERVE_IMPL_H
#define RNNOISE_OBSERVE_IMPL_H 1

#include <vector>

#include "rnn-observe.h"

/* Start/stop collecting; records are kept until the next begin(). */
void observe_begin();
void observe_end();
void observe_clear();
const std::vector<RNNOISE_TEST_OBS> &observe_frames();

#endif /* RNNOISE_OBSERVE_IMPL_H */
