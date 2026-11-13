#ifndef L3OBS_H
#define L3OBS_H

/*
 * Internal, test-only switch for the read-only observation points in
 * shine_global_config. This header is part of the in-tree build and is
 * pulled in by types.h, so every library translation unit agrees on the
 * struct layout. It is never installed: downstream code only ever sees the
 * public layer3.h, where the observation fields and macro do not exist.
 */

#define SHINE_TASK_OBSERVATIONS 1

#include "layer3.h"

/* Read-only view of the encoder internals for the in-tree test driver.
 * Only the incomplete struct tag is needed here; the full layout comes from
 * types.h in the translation units that actually read the fields. */
const struct shine_global_flags *shine_test_observations(shine_t s);

#endif
