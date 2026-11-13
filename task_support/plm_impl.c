#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>

/*
 * Translation unit that instantiates the pl_mpeg implementation for the
 * regression binary. A deliberately small default ring buffer forces the high
 * level video buffer to compact (discard + memmove) and reallocate constantly
 * while the tests run, which stresses the absolute byte-offset bookkeeping of
 * the PES PTS markers. 2048 still holds the largest PES payload of the fixture
 * (~2042 bytes); the buffer grows on demand for anything larger, so decoding
 * itself is unaffected.
 */
#ifndef PLM_BUFFER_DEFAULT_SIZE
#define PLM_BUFFER_DEFAULT_SIZE 2048
#endif

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"
