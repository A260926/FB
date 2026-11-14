/*
 * Independent PNS / M/S reference model for the FAAD2 regression tests.
 *
 * This header is test-only. Nothing here is part of the public decoder API.
 * The model deliberately does not call ne_rng() and does not use the
 * production Parity lookup table: the two 32-bit shift registers are advanced
 * with explicit bit-by-bit parity, as described in ISO/IEC 14496-3.
 */
#ifndef PNS_REFERENCE_H
#define PNS_REFERENCE_H

#include <stdint.h>

#define PNS_FRAME_LEN 1024u
#define PNS_NSHORT    128u

/* Initial global RNG state, identical to NeAACDecOpen() (decoder.c). */
#define PNS_R1_INIT 0x2bb431eau
#define PNS_R2_INIT 0x206155b7u

/* SWB tables for the 44.1 kHz configuration used by the generated streams
 * (sf_index 4, frame length 1024). Values are the standardised band tables;
 * they are duplicated here so the reference does not read production objects.
 */
extern const uint16_t pns_long_offsets[48];  /* 47 bands + terminator */
extern const uint16_t pns_short_offsets[15]; /* 14 bands + terminator */

typedef struct
{
    uint32_t r1;
    uint32_t r2;
} pns_rng_t;

void pns_rng_init(pns_rng_t *rng);

/* One step of the two-register generator; returns the XORed 32-bit word. */
uint32_t pns_rng_next(pns_rng_t *rng);

/* Same word reinterpreted as a signed 32-bit integer, matching the
 * (int32_t) cast in gen_rand_vector(). */
int32_t pns_rng_signed(pns_rng_t *rng);

/* PNS vector: signed random draws, normalised independently by the sum of
 * squares of the signed vector, then scaled by 2^(sf/4). The energy sum is
 * kept in single precision to match the production float accumulation. */
void pns_gen_vector(double *out, int scale_factor, unsigned size,
                    pns_rng_t *rng);

/* Plain AAC inverse quantisation: sign(q)*|q|^(4/3) * 2^((sf-100)/4). */
double pns_iquant(int q, int scale_factor);

/* Noise energy accumulation. The first noise band carries a 9-bit PCM word
 * (transmitted value, 0..511); every later noise band carries a HCB_SF delta.
 * Bands are interpreted strictly in transmission order. */
int pns_noise_energy(int global_gain, int first9,
                     const int *deltas, unsigned num_deltas);

/* Reference M/S rotation on one band: l' = l+r, r' = l-r. */
void pns_apply_ms(double *l, double *r, unsigned begin, unsigned end);

#endif
