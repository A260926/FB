#ifndef L3LOOP_H
#define L3LOOP_H

#include "types.h"

void shine_loop_initialise(shine_global_config *config);

void shine_iteration_loop(shine_global_config *config);

/* True for a pure short block (window-switched, type 2, not mixed). */
int shine_l3_is_short(gr_info *gi);

#endif
