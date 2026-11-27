#ifndef shine_MDCT_H
#define shine_MDCT_H

#include "types.h"

void shine_mdct_initialise(shine_global_config *config);
void shine_mdct_sub(shine_global_config *config, int stride);

/* Read-only observation points used by the test-suite. Only built when
 * SHINE_TEST_HOOKS is defined; the vectors are otherwise absent. */
#ifdef SHINE_TEST_HOOKS
#include "test_hooks.h"
#endif

#endif
