#ifndef TASK_SUPPORT_RNG_H
#define TASK_SUPPORT_RNG_H
/* Deterministic, self-contained PRNG (splitmix64). The whole property suite
 * is driven from a 64 bit seed so that a fixed seed replays byte-for-byte. */
#include <stdint.h>

typedef struct {
    uint64_t state;
} TaskRng;

static inline void task_rng_init(TaskRng *r, uint64_t seed) {
    r->state = seed;
}

static inline uint64_t task_rng_next(TaskRng *r) {
    uint64_t z = (r->state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

/* Uniform value in [0, n); n must be positive and well below 2^63. */
static inline uint64_t task_rng_below(TaskRng *r, uint64_t n) {
    return task_rng_next(r) % n;
}

static inline int task_rng_chance(TaskRng *r, unsigned percent) {
    return task_rng_below(r, 100) < (uint64_t)percent;
}
#endif
