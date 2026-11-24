#ifndef TASK_GRANULE_FIXTURE_H
#define TASK_GRANULE_FIXTURE_H
#include <stddef.h>
#include <stdint.h>
#include "fixed.h"
#include "synth.h"
#define TASK_FRAMES 8
#define TASK_MAX_BYTES (TASK_FRAMES*417+MAD_BUFFER_GUARD)
#define TASK_MAX_SAMPLES (TASK_FRAMES*1152)
size_t task_fixture(unsigned char *, int lsf, int bad_second);
int task_reference(const unsigned char *, size_t, int options,
                   mad_fixed_t out[2][TASK_MAX_SAMPLES], unsigned *samples,
                   uint64_t *hash);
#endif
