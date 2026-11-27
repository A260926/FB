#ifndef L3LOOP_H
#define L3LOOP_H

#include "types.h"

void shine_loop_initialise(shine_global_config *config);

void shine_iteration_loop(shine_global_config *config);

/* Outer loop driven by caller supplied masking ratios (see
 * shine_encode_buffer_masked). */
void shine_masked_iteration_loop(shine_global_config *config);

#endif
