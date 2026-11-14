/*
 * Independent PNS / M/S reference model.
 *
 * The RNG is re-derived from the register description in libfaad/common.c
 * but implemented with explicit parity loops: there is no call to ne_rng()
 * and no use of the production Parity table. Expected spectra come from this
 * model and from the generator's own band plan; observed decoder values are
 * never fed back into the expectations.
 */
#include "pns_reference.h"

#include <math.h>

/* 44.1 kHz, 1024-sample long windows: swb_offset_1024_48 (47 bands). */
const uint16_t pns_long_offsets[48] = {
    0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72,
    80, 88, 96, 108, 120, 132, 144, 160, 176, 196, 216, 240, 264, 292,
    320, 352, 384, 416, 448, 480, 512, 544, 576, 608, 640, 672, 704, 736,
    768, 800, 832, 864, 1024
};

/* 44.1 kHz, 128-sample short windows: swb_offset_128_48 (14 bands). */
const uint16_t pns_short_offsets[15] = {
    0, 4, 8, 12, 16, 20, 28, 36, 44, 56, 68, 80, 96, 112, 128
};

void pns_rng_init(pns_rng_t *rng)
{
    rng->r1 = PNS_R1_INIT;
    rng->r2 = PNS_R2_INIT;
}

static unsigned bit_parity(uint32_t x)
{
    unsigned p = 0;
    while (x)
    {
        p ^= (unsigned)(x & 1u);
        x >>= 1;
    }
    return p;
}

uint32_t pns_rng_next(pns_rng_t *rng)
{
    uint32_t t3 = rng->r1;
    uint32_t t4 = rng->r2;
    /* Register 1 rotates right; the new MSB is parity of the masked bits
     * (bits 0,2,4,5,7 of the old word, i.e. mask 0xF5). */
    uint32_t t1 = bit_parity(t3 & 0xF5u) << 31;
    /* Register 2 rotates left; the new LSB is parity of the old top bits
     * shifted into positions 0..6 and masked with 0x63. */
    uint32_t t2 = bit_parity((t4 >> 25) & 0x63u);

    rng->r1 = (t3 >> 1) | t1;
    rng->r2 = (t4 + t4) | t2;
    return rng->r1 ^ rng->r2;
}

int32_t pns_rng_signed(pns_rng_t *rng)
{
    return (int32_t)pns_rng_next(rng);
}

void pns_gen_vector(double *out, int scale_factor, unsigned size,
                    pns_rng_t *rng)
{
    float energy = 0.0f;
    float scale;
    unsigned i;

    if (scale_factor < -120) scale_factor = -120;
    if (scale_factor > 120) scale_factor = 120;

    for (i = 0; i < size; i++)
    {
        float tmp = (float)pns_rng_signed(rng);
        out[i] = (double)tmp;
        energy += tmp * tmp;
    }

    if (energy > 0.0f)
    {
        /* Match the production float arithmetic: float inverse sqrt, then
         * the double-precision pow() folded back into a float scale. */
        scale = 1.0f / sqrtf(energy);
        scale *= (float)pow(2.0, 0.25 * (double)scale_factor);
        for (i = 0; i < size; i++)
            out[i] = (double)((float)out[i] * scale);
    }
}

double pns_iquant(int q, int scale_factor)
{
    double mag = pow((double)(q < 0 ? -q : q), 4.0 / 3.0);
    double scf = pow(2.0, ((double)scale_factor - 100.0) / 4.0);
    return (q < 0 ? -mag : mag) * scf;
}

int pns_noise_energy(int global_gain, int first9,
                     const int *deltas, unsigned num_deltas)
{
    int energy = global_gain - 90 + (first9 - 256);
    unsigned i;
    for (i = 0; i < num_deltas; i++)
        energy += deltas[i];
    return energy;
}

void pns_apply_ms(double *l, double *r, unsigned begin, unsigned end)
{
    unsigned i;
    for (i = begin; i < end; i++)
    {
        double ll = l[i];
        double rr = r[i];
        l[i] = ll + rr;
        r[i] = ll - rr;
    }
}
