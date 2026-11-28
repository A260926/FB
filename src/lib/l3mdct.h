#ifndef shine_MDCT_H
#define shine_MDCT_H

#include "types.h"

void shine_mdct_initialise(shine_global_config *config);
void shine_mdct_sub(shine_global_config *config, int stride);

/* Per-granule window information. */
void shine_mdct_set_block_type(shine_global_config *config, int gr, int ch,
                               int block_type);
void shine_mdct_clear_block_types(shine_global_config *config);

#endif
