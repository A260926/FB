/*
 * Mbcompress
 *
 * Stereo three-band compressor built from fourth-order Linkwitz-Riley
 * crossovers. Each crossover splits a signal into a low branch (two
 * cascaded second-order Butterworth lowpass stages) and a high branch
 * (two cascaded second-order Butterworth highpass stages) tuned to the
 * same cutoff. A Linkwitz-Riley pair sums back to an all-pass response:
 *
 *     A_f(z) = LP4_f(z) + HP4_f(z)
 *
 * so the three-band recombination (two cascaded crossover all-passes,
 * A_f1(z) * A_f2(z)) is unit magnitude with no comb notches at the
 * crossover points. The dry path is filtered through that same combined
 * all-pass response, which keeps dry/wet mixes phase aligned.
 *
 * Per-band level detection uses the larger absolute value of the left
 * and right band signals (never their sum), so a single linear gain is
 * shared by both channels of a band. Each band has its own threshold,
 * ratio, attack and release.
 *
 * Units and ranges:
 *   - f1, f2 are fixed at init time in Hz, and must satisfy
 *     0 < f1 < f2 < sr/2. They cannot be changed afterwards.
 *   - thresholdDb is in decibels (any finite value, typically <= 0).
 *   - ratio is dimensionless and must be >= 1 (1 bypasses compression).
 *   - attackSeconds / releaseSeconds are in seconds and must be >= 0;
 *     a time of 0 reaches the target gain in one sample.
 *   - wet is a linear dry/wet mix in [0, 1].
 *   - audio samples are SPFLOAT, linear amplitude (not dB).
 *
 * Works with both the default float build and the USE_DOUBLE build.
 * The public sample type is SPFLOAT in either build; the crossover
 * histories and gain recursion are evaluated internally in double so
 * the float build keeps the unit-gain all-pass accurate.
 */
typedef struct {
    SPFLOAT f1;
    SPFLOAT f2;
    SPFLOAT sr;

    /* per-band compressor parameters, indexed 0 = low, 1 = mid, 2 = high */
    SPFLOAT thresholdDb[3];
    SPFLOAT ratio[3];
    SPFLOAT attackSeconds[3];
    SPFLOAT releaseSeconds[3];
    SPFLOAT wet;

    /* smoothed per-band gain in dB, shared by both channels */
    SPFLOAT gainDb[3];

    /* internal filter histories, allocated by sp_mbcompress_init */
    void *ud;
} sp_mbcompress;

int sp_mbcompress_create(sp_mbcompress **p);
int sp_mbcompress_destroy(sp_mbcompress **p);
/* f1, f2 in Hz: 0 < f1 < f2 < sr/2, fixed for the lifetime of the module */
int sp_mbcompress_init(sp_data *sp, sp_mbcompress *p, SPFLOAT f1, SPFLOAT f2);
/* one stereo sample in, one stereo sample out; in-place buffers are safe */
int sp_mbcompress_compute(sp_data *sp, sp_mbcompress *p,
                          SPFLOAT *inL, SPFLOAT *inR,
                          SPFLOAT *outL, SPFLOAT *outR);
/* band: 0 = low, 1 = mid, 2 = high. Invalid arguments are rejected. */
int sp_mbcompress_threshold(sp_mbcompress *p, int band, SPFLOAT thresholdDb);
int sp_mbcompress_ratio(sp_mbcompress *p, int band, SPFLOAT ratio);
int sp_mbcompress_attack(sp_mbcompress *p, int band, SPFLOAT attackSeconds);
int sp_mbcompress_release(sp_mbcompress *p, int band, SPFLOAT releaseSeconds);
/* linear dry/wet mix, must be within [0, 1] */
int sp_mbcompress_wet(sp_mbcompress *p, SPFLOAT wet);
/* clears every filter history and restores all band gains to 1 (0 dB) */
int sp_mbcompress_reset(sp_mbcompress *p);
