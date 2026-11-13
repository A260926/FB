/* Original task support, CC0-1.0. Creates public input, not a decoder. */
#ifndef TASK_FIXTURE_H
#define TASK_FIXTURE_H
#include <stddef.h>
#include <stdint.h>
size_t task_fixture(uint8_t *dst, size_t capacity, int include_b);
unsigned task_pixel(unsigned ref, unsigned plane, unsigned x, unsigned y);
#endif
