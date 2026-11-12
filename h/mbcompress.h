/*
 * Mbcompress
 *
 * Stereo three-band compressor for a mastering chain.
 *
 * The two fixed crossover frequencies f1 and f2 split the signal into
 * low, mid and high bands with 4th-order Linkwitz-Riley filters. Every
 * branch of every channel is an independent two-stage cascade built from
 * the same bilinear-transform 2nd-order Butterworth lowpass/highpass
 * formulas as sp_butlp/sp_buthp. The filter delay state is kept in
 * double precision internally so that the six-stage-deep crossover stays
 * within the float accuracy target; sp_butlp/sp_buthp themselves are not
 * modified.
 * The summed bands have the unit-gain all-pass response
 * A_f1(z) * A_f2(z), where A_f(z) = L_f(z)^2 + H_f(z)^2 is the
 * 4th-order lowpass/highpass sum at one crossover. The dry path runs
 * through the same all-pass response, so dry/wet mixing never introduces
 * a comb notch at the crossover points.
 *
 * Each band has its own compressor. The detector uses the larger absolute
 * value of the left and right band signals, so one gain (in dB, computed
 * in linear gain afterwards) is applied to both channels and constant
 * level or phase stereo relationships are preserved.
 *
 * Units:
 *  - f1, f2: frequency in Hz, 0 < f1 < f2 < sr/2, fixed at init time
 *  - thresholdDb: compressor threshold in decibels
 *  - ratio: compression ratio, ratio >= 1 (1 = no compression)
 *  - attackSeconds, releaseSeconds: time constants in seconds, >= 0;
 *    a time of 0 reaches the target gain immediately
 *  - wet: linear dry/wet balance, 0 = fully phase-aligned dry,
 *    1 = fully compressed, values in between are a linear blend
 *
 * The module works with both the default float build and the USE_DOUBLE
 * build. No memory is allocated or freed inside sp_mbcompress_compute.
 * In-place operation (input and output pointers aliasing per channel)
 * is supported.
 */

typedef struct {
    SPFLOAT thresholdDb;
    SPFLOAT ratio;
    SPFLOAT attackSeconds;
    SPFLOAT releaseSeconds;
} sp_mbcompress_band;

typedef struct {
    int sr;
    SPFLOAT f1;
    SPFLOAT f2;
    SPFLOAT wet;

    /* per-band compressor parameters, indexed 0 = low, 1 = mid, 2 = high */
    sp_mbcompress_band band[3];
    /* smoothed per-band gain in dB, one gain shared by both channels */
    SPFLOAT gainDb[3];

    /* independent 2nd-order Butterworth stage state per channel, branch
     * and cascade stage; opaque storage owned by the module */
    struct mb_stage *lp;
    struct mb_stage *hp;

    int initok;
} sp_mbcompress;

int sp_mbcompress_create(sp_mbcompress **p);
int sp_mbcompress_destroy(sp_mbcompress **p);
/* f1, f2 are fixed crossover frequencies in Hz: 0 < f1 < f2 < sr/2 */
int sp_mbcompress_init(sp_data *sp, sp_mbcompress *p,
                       SPFLOAT f1, SPFLOAT f2);
int sp_mbcompress_compute(sp_data *sp, sp_mbcompress *p,
                          SPFLOAT *inL, SPFLOAT *inR,
                          SPFLOAT *outL, SPFLOAT *outR);

/* Parameter setters. Legal values take effect at the next sample and do
 * not clear filter or gain history. Illegal values are rejected and the
 * previous configuration stays in effect. Crossover frequencies cannot
 * be changed after init. band selects 0 (low), 1 (mid) or 2 (high). */
int sp_mbcompress_set_threshold(sp_mbcompress *p, int band,
                                SPFLOAT thresholdDb);
int sp_mbcompress_set_ratio(sp_mbcompress *p, int band, SPFLOAT ratio);
int sp_mbcompress_set_attack(sp_mbcompress *p, int band,
                             SPFLOAT attackSeconds);
int sp_mbcompress_set_release(sp_mbcompress *p, int band,
                              SPFLOAT releaseSeconds);
int sp_mbcompress_set_wet(sp_mbcompress *p, SPFLOAT wet);

/* Clears all filter history and restores the three band gains to unity.
 * The configuration (crossover frequencies and band parameters) is kept. */
int sp_mbcompress_reset(sp_data *sp, sp_mbcompress *p);
