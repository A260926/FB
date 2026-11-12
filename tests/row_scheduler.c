/* Row scheduler regression tests. MIT licensed.
 *
 * Every check renders through the public pocketmod_render() API. Status
 * fields may help locate a bug, but audio assertions decide pass/fail.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define POCKETMOD_IMPLEMENTATION
#include "pocketmod.h"
#include "fixture.h"

#define RATE 44100
#define TICK 882

static unsigned char module[MOD_BYTES];

static int fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    return 1;
}

/* ---------------------------------------------------------------- render */

/* Render exactly 'count' stereo frames into dst, following short returns. */
static int render_into(pocketmod_context *c, float *dst, int count)
{
    int done = 0;
    while (done < count) {
        int want = count - done;
        int got = pocketmod_render(c, dst + done * 2,
                                   want * (int) sizeof(float[2]));
        if (got <= 0) return 0;
        done += got / (int) sizeof(float[2]);
    }
    return 1;
}

static int init(pocketmod_context *c)
{
    return pocketmod_init(c, module, MOD_BYTES, RATE);
}

/* -------------------------------------------------------- shared samples */

/* Sample 1 is a positive ramp with a periodic sign flip; sample 2 is its
 * inverse. They are long enough that a few hundred frames stay inside the
 * single-cycle region and phase changes are visible. */
static void make_waves(unsigned char *m)
{
    static signed char wave1[MOD_SAMPLE_BYTES];
    static signed char wave2[MOD_SAMPLE_BYTES];
    int i;
    for (i = 0; i < MOD_SAMPLE_BYTES; ++i) {
        /* Period 256, never zero, opposite signs on the two samples. */
        int v = (i & 128) ? -((i & 127) + 64) : ((i & 127) + 64);
        wave1[i] = (signed char) v;
        wave2[i] = (signed char) -v;
    }
    mod_make(m);
    mod_set_sample(m, 1, 0, 64, wave1);
    mod_set_sample(m, 2, 0, 64, wave2);
}

/* Render one tick at a time and record the song position at each tick
 * boundary together with the first PCM frame it produces. */
typedef struct {
    int order[256];
    int line[256];
    int tick[256];
    float pcm[256];
} trace;

static int trace_song(const unsigned char *m, trace *t, int ticks)
{
    pocketmod_context c;
    float tickbuf[TICK][2];
    float frame[2];
    int i;
    if (!pocketmod_init(&c, m, MOD_BYTES, RATE)) return 0;
    memset(t, 0, sizeof(*t));
    for (i = 0; i < ticks; ++i) {
        t->order[i] = c.pattern;
        t->line[i] = c.line;
        t->tick[i] = c.tick;
        if (!render_into(&c, &frame[0], 1)) return 0;
        t->pcm[i] = frame[0];
        if (!render_into(&c, &tickbuf[0][0], TICK - 1)) return 0;
    }
    return 1;
}

/* ================================================================ flow */

int test_flow(void)
{
    pocketmod_context c;
    trace t;
    float *scratch = (float *) malloc(sizeof(float[2]) * TICK * 512);
    int bad = 0;

    if (!scratch) return 1;

    /* (1) B01 on one channel and D02 on another are one destination:
     * order 1, row 2. Swapping the channels must not matter. */
    {
        int swap;
        for (swap = 0; swap < 2 && !bad; ++swap) {
            int bch = swap ? 2 : 0;
            int dch = swap ? 0 : 2;
            mod_make(module);
            mod_event(module, 0, 0, bch, 1, 428, 0xb, 1);
            mod_event(module, 0, 0, dch, 0, 0, 0xd, 2);
            if (!trace_song(module, &t, 8)) return 2;
            /* Ticks 0..5 are the first row itself. */
            if (t.line[0] != 0 || t.order[0] != 0)
                bad = fail("first row cut short");
            if (t.pcm[0] <= 0.0f)
                bad = fail("first row audio missing");
            /* Tick 6 has crossed to order 1 row 2. */
            if (!bad && (t.order[6] != 1 || t.line[6] != 2))
                bad = fail("B+D destination is not order 1 row 2");
            /* The destination row carries the fixture's sample-2 note. */
            if (!bad && t.pcm[6] >= 0.0f)
                bad = fail("destination row audio missing");
        }
    }

    /* (2) Dxy alone enters the next order's named row. */
    if (!bad) {
        mod_make(module);
        mod_event(module, 0, 0, 0, 1, 428, 0xd, 5);
        if (!trace_song(module, &t, 8)) return 6;
        if (t.order[6] != 1 || t.line[6] != 5)
            bad = fail("Dxy next-order row");
        if (t.pcm[0] <= 0.0f)
            bad = fail("Dxy source row audio missing");
    }

    /* (3) Bxx alone starts the target at row 0. */
    if (!bad) {
        mod_make(module);
        mod_event(module, 0, 0, 0, 1, 428, 0xb, 2);
        if (!trace_song(module, &t, 8)) return 8;
        if (t.order[6] != 2 || t.line[6] != 0)
            bad = fail("Bxx destination row 0");
        if (t.pcm[6] <= 0.0f)
            bad = fail("Bxx destination audio missing");
    }

    /* (4) B/D beat an E6 repeat that shares the row. */
    if (!bad) {
        mod_make(module);
        mod_event(module, 0, 0, 1, 0, 0, 0xe, 0x61);
        mod_event(module, 0, 0, 2, 0, 0, 0xb, 1);
        if (!trace_song(module, &t, 8)) return 11;
        if (t.order[6] != 1 || t.line[6] != 0)
            bad = fail("B/D does not outrank E6");
    }

    /* (5) Out-of-range Bxx falls back to order 0. */
    if (!bad) {
        mod_make(module);
        mod_event(module, 0, 0, 0, 1, 428, 0xb, 9);
        if (!trace_song(module, &t, 8)) return 13;
        if (t.order[6] != 0 || t.line[6] != 0)
            bad = fail("out-of-range Bxx");
    }

    /* (6) Dxy outside 0..63 means row 0. */
    if (!bad) {
        mod_make(module);
        mod_event(module, 0, 0, 0, 1, 428, 0xd, 0x70);
        if (!trace_song(module, &t, 8)) return 15;
        if (t.order[6] != 1 || t.line[6] != 0)
            bad = fail("out-of-range Dxy");
    }

    /* (7) Local repeat. E60 at row 0, E61 at row 2 replays rows 0..2 one
     * extra time. It must not raise the song loop count, and the audio at
     * the repeated row 0 must match the first pass exactly. */
    if (!bad) {
        int i;
        mod_make(module);
        mod_event(module, 0, 0, 1, 0, 0, 0xe, 0x60);
        mod_event(module, 0, 2, 1, 0, 0, 0xe, 0x61);
        if (!trace_song(module, &t, 60)) return 17;
        /* Row sequence: 0,1,2, 0,1,2, then 3,4,... */
        for (i = 0; i < 18; ++i)
            if (t.line[i] != i / 6) bad = fail("local repeat row sequence");
        for (i = 18; i < 36; ++i)
            if (!bad && t.line[i] != (i - 18) / 6)
                bad = fail("local repeat second pass");
        for (i = 36; i < 48; ++i)
            if (!bad && t.line[i] != 3 + (i - 36) / 6)
                bad = fail("local repeat resumed after the loop");
        if (!bad && (t.order[18] != 0 || t.order[0] != 0))
            bad = fail("local repeat left its order");
        /* Same row, same phase: the repeated first frame is identical. */
        if (!bad && fabsf(t.pcm[18] - t.pcm[0]) > 1e-6f)
            bad = fail("repeated row audio differs");
        if (!bad && fabsf(t.pcm[0]) < 1e-6f)
            bad = fail("repeated row audio is silent");
    }

    /* (8) Per-channel E6 counters. Channel 0 sets its origin at row 0 and
     * channel 1 at row 1. On row 2 both ask for one extra play. Their
     * counters are independent; the higher channel's origin (row 1) wins
     * the destination. A second pass of row 2 consumes the last repeat in
     * both channels and falls through to row 3. */
    if (!bad) {
        mod_make(module);
        mod_event(module, 0, 0, 0, 0, 0, 0xe, 0x60);
        mod_event(module, 0, 1, 1, 0, 0, 0xe, 0x60);
        mod_event(module, 0, 2, 0, 0, 0, 0xe, 0x61);
        mod_event(module, 0, 2, 1, 0, 0, 0xe, 0x61);
        if (!pocketmod_init(&c, module, MOD_BYTES, RATE)) return 21;
        /* Reaching row 2: both counters hold one pending repeat. */
        if (!render_into(&c, scratch, TICK * 18)) return 22;
        if (c.channels[0].loop_count != 1 || c.channels[1].loop_count != 1)
            bad = fail("E6 counters not independent");
        if (c.line != 1)
            bad = fail("highest E6 channel origin did not win");
        /* Second pass of row 2 consumes both counters and falls through
         * to row 3: rows 1, 2 (12 ticks) plus row 3's tick 0. */
        if (!render_into(&c, scratch, TICK * 13)) return 23;
        if (c.channels[0].loop_count != 0 || c.channels[1].loop_count != 0)
            bad = fail("E6 counters not consumed once per pass");
        if (c.line != 3)
            bad = fail("E6 repeat did not resume after the counters ran out");
        if (pocketmod_loop_count(&c) != 0)
            bad = fail("E6 repeat counted as song loop");
    }

    /* (9) The row itself still plays fully before the jump: other
     * channels keep their note and effect for the whole line even though
     * the destination was chosen mid-decode. */
    if (!bad) {
        mod_make(module);
        /* Row 0 has a note on channel 0 and B01 on channel 1. The tone
         * must sound for all six ticks, not stop at the B command. */
        mod_event(module, 0, 0, 0, 1, 428, 0, 0);
        mod_event(module, 0, 0, 1, 0, 0, 0xb, 1);
        if (!pocketmod_init(&c, module, MOD_BYTES, RATE)) return 28;
        {
            int tick;
            /* Ticks 0..4: the row is still current and its tone plays. */
            for (tick = 0; tick < 5; ++tick) {
                if (!render_into(&c, scratch, TICK)) return 29;
                if (scratch[0] <= 0.0f) {
                    bad = fail("current row audio cut by the destination");
                    break;
                }
                if (c.pattern != 0 || c.line != 0) {
                    bad = fail("destination applied before the row finished");
                    break;
                }
            }
            /* The render that completes tick 5 plays its full tick of
             * audio, then performs the transition. */
            if (!bad) {
                if (!render_into(&c, scratch, TICK)) return 30;
                if (scratch[0] <= 0.0f)
                    bad = fail("last tick of the row was silent");
            }
        }
        /* Only after the sixth tick does the order transition happen. */
        if (!bad && (c.pattern != 1 || c.line != 0))
            bad = fail("Bxx destination not applied after the row");
    }

    /* (10) An order-level Bxx resets local playback state even when it
     * jumps back into the same order, and that transition counts as a
     * song loop. Channel 0 is mid-repeat when channel 1 issues B on row 5;
     * B outranks E6 and the reset clears channel 0's pending count. */
    if (!bad) {
        mod_make(module);
        mod_event(module, 0, 0, 0, 1, 428, 0xe, 0x60);  /* origin row 0 */
        mod_event(module, 0, 5, 0, 0, 0, 0xe, 0x61);    /* wants repeat  */
        mod_event(module, 0, 5, 1, 0, 0, 0xb, 0);       /* B beats E6    */
        if (!pocketmod_init(&c, module, MOD_BYTES, RATE)) return 24;
        /* Run up to the start of row 5 (the row with E6 and B). */
        if (!render_into(&c, scratch, TICK * 6 * 5)) return 25;
        if (!render_into(&c, scratch, TICK)) return 26;
        if (c.channels[0].loop_count != 1)
            bad = fail("E6 count not pending before jump");
        if (!render_into(&c, scratch, TICK * 5)) return 27;
        if (c.pattern != 0 || c.line != 0 || c.tick != 0)
            bad = fail("Bxx to same order did not land at row 0");
        if (c.channels[0].loop_count != 0)
            bad = fail("Bxx to same order did not reset local counts");
        if (pocketmod_loop_count(&c) != 1)
            bad = fail("Bxx back to visited order failed to bump loop");
    }

    free(scratch);
    return bad ? 1 : 0;
}

/* ======================================================= delayed_voice */

/* Fixture: sample 1 positive ramp, sample 2 negative inverse, both long. */
static void voice_fixture(void)
{
    make_waves(module);
}

int test_delayed_voice(void)
{
    pocketmod_context c;
    float *buf = (float *) malloc(sizeof(float[2]) * TICK * 64);
    int bad = 0;
    if (!buf) return 1;

    /* (1) ED2 defers the whole attack. The old sample, period and phase
     * keep playing until the tick-2 boundary, then the new sample starts
     * at phase 0 and the audio changes sign. */
    if (!bad) {
        int n = TICK * 6 + TICK * 2;   /* boundary into row 1 tick 2 */
        voice_fixture();
        mod_event(module, 0, 0, 0, 1, 428, 0, 0);
        mod_event(module, 0, 1, 0, 2, 360, 0xe, 0xd2);
        if (!init(&c)) return 2;
        /* One frame short of the trigger boundary: still the old voice. */
        if (!render_into(&c, buf, n - 1)) return 3;
        if (buf[(n - 2) * 2] == 0.0f) bad = fail("old voice silent before ED");
        if (c.tick != 1) bad = fail("ED triggered too early");
        if (c.channels[0].sample != 1) bad = fail("sample changed before ED");
        if (c.channels[0].period != 428) bad = fail("period changed before ED");
        if (c.channels[0].position < 500.0f)
            bad = fail("old voice phase reset early");
        /* Cross the boundary: the delayed attack fires now. The first
         * frame after the crossing is the first frame rendered under the
         * tick-2 state, at buffer index 1 (index 0 is tick 1's tail). */
        if (!render_into(&c, buf, 2)) return 4;
        if (c.tick != 2) bad = fail("ED did not trigger at tick 2");
        if (c.channels[0].sample != 2) bad = fail("delayed sample not installed");
        if (c.channels[0].period != 360) bad = fail("delayed period not installed");
        if (c.channels[0].position > 30.0f) bad = fail("delayed phase not reset");
        if (buf[1 * 2] >= 0.0f) bad = fail("delayed voice has the old sign");
    }

    /* (2) ED0 fires on tick 0 of its row, with the new sample. */
    if (!bad) {
        voice_fixture();
        mod_event(module, 0, 0, 0, 2, 428, 0, 0);       /* negative voice */
        mod_event(module, 0, 1, 0, 1, 360, 0xe, 0xd0);  /* positive, now  */
        if (!init(&c)) return 5;
        /* One frame before the row boundary: the old voice still plays. */
        if (!render_into(&c, buf, TICK * 6 - 1)) return 6;
        if (c.channels[0].sample != 2)
            bad = fail("ED0 switched before its row");
        /* Cross into row 1 tick 0: ED0 fires with the new sample. */
        if (!render_into(&c, buf, 2)) return 7;
        if (c.channels[0].sample != 1) bad = fail("ED0 sample not installed");
        if (c.channels[0].period != 360) bad = fail("ED0 period not installed");
        if (buf[1 * 2] <= 0.0f) bad = fail("ED0 did not sound at tick 0");
    }

    /* (3) An attack at or past the row's speed is cancelled outright. The
     * old voice keeps playing through the row and the next row starts
     * clean, with no late trigger. */
    if (!bad) {
        int tick;
        voice_fixture();
        mod_event(module, 0, 0, 0, 1, 428, 0, 0);
        mod_event(module, 0, 1, 0, 2, 360, 0xe, 0xd6);  /* param == speed */
        if (!init(&c)) return 8;
        for (tick = 0; tick < 6; ++tick) {
            if (!render_into(&c, buf, TICK)) return 9;
            if (c.channels[0].sample != 1 || c.channels[0].period != 428)
                bad = fail("cancelled ED altered the voice");
        }
        /* The next row (empty) must not inherit a pending attack. Phase
         * keeps advancing through both rows: no late reset, no late swap. */
        if (!render_into(&c, buf, TICK)) return 10;
        if (c.channels[0].sample != 1 || c.channels[0].period != 428)
            bad = fail("cancelled ED leaked into the next row");
    }

    /* (4) Fxx on the same row lowers the speed first. At speed 4, ED2 is
     * still valid and fires at tick 2; ED4 is at the new speed and is
     * cancelled. */
    if (!bad) {
        voice_fixture();
        mod_event(module, 0, 0, 0, 1, 428, 0, 0);
        mod_event(module, 0, 1, 0, 2, 360, 0xe, 0xd2);
        mod_event(module, 0, 1, 1, 0, 0, 0xf, 4);
        if (!init(&c)) return 11;
        /* Row 0 is six ticks: reach row 1 tick 0. */
        if (!render_into(&c, buf, TICK * 6)) return 12;
        if (c.ticks_per_line != 4) bad = fail("F4 not in force on row 1");
        /* Ticks 0 and 1 still the old voice; crossing into tick 2 fires. */
        if (!render_into(&c, buf, TICK * 2 - 1)) return 13;
        if (c.channels[0].sample != 1)
            bad = fail("ED2 fired before its tick under F4");
        if (!render_into(&c, buf, 1)) return 14;
        if (c.channels[0].sample != 2 || c.channels[0].period != 360)
            bad = fail("ED2 did not fire at tick 2 under F4");
    }
    if (!bad) {
        voice_fixture();
        mod_event(module, 0, 0, 0, 1, 428, 0, 0);
        mod_event(module, 0, 1, 0, 2, 360, 0xe, 0xd4);
        mod_event(module, 0, 1, 1, 0, 0, 0xf, 4);
        if (!init(&c)) return 15;
        if (!render_into(&c, buf, TICK * 6)) return 16;
        {
            int tick;
            for (tick = 0; tick < 4; ++tick) {
                if (!render_into(&c, buf, TICK)) return 17;
                if (c.channels[0].sample != 1 || c.channels[0].period != 428)
                    bad = fail("ED4 not cancelled at speed 4");
            }
        }
    }

    /* (5) A delayed note with no sample number delays the previous sample
     * selection: the pitch changes at the trigger, the sample does not. */
    if (!bad) {
        voice_fixture();
        mod_event(module, 0, 0, 0, 1, 428, 0, 0);
        mod_event(module, 0, 1, 0, 0, 360, 0xe, 0xd2);  /* no sample */
        if (!init(&c)) return 18;
        if (!render_into(&c, buf, TICK * 6 + TICK * 2 - 1)) return 19;
        if (c.channels[0].period != 428) bad = fail("ED re-pitched early");
        if (!render_into(&c, buf, 1)) return 20;
        if (c.channels[0].sample != 1)
            bad = fail("unpitched ED switched the sample");
        if (c.channels[0].period != 360)
            bad = fail("previous-sample ED did not apply the pitch");
        if (c.channels[0].position > 30.0f)
            bad = fail("previous-sample ED did not reset the phase");
    }

    free(buf);
    return bad ? 1 : 0;
}

/* =========================================================== row_delay */

int test_row_delay(void)
{
    pocketmod_context c;
    float *buf = (float *) malloc(sizeof(float[2]) * TICK * 512);
    int bad = 0, tick;
    if (!buf) return 1;

    /* Row 1 carries EE2, a fine volume slide (EA) and a sustained
     * portamento (1xx). With speed 6 the row runs 8 cycles; period and
     * volume must move one step per tick, not once per row, and the row
     * must not be re-decoded during the extra cycles. */
    mod_make(module);
    mod_set_volume(module, 1, 40);
    mod_event(module, 0, 0, 0, 1, 428, 0, 0);
    mod_event(module, 0, 0, 1, 1, 428, 0, 0);      /* second voice, vol 40 */
    mod_event(module, 0, 1, 0, 0, 0, 0x1, 0x02);   /* slide up 2 per tick */
    mod_event(module, 0, 1, 1, 0, 0, 0xe, 0xa1);   /* fine volume +1, tick 0 */
    mod_event(module, 0, 1, 2, 0, 0, 0xe, 0xe2);   /* EE2 */
    if (!init(&c)) return 2;
    {
        int period[16], volume[16];
        /* Row 0 has six ticks. Then row 1 has eight cycles. */
        for (tick = 0; tick < 6; ++tick) {
            if (!render_into(&c, buf, TICK)) return 3;
        }
        /* Row 1 tick 0 is current: the fine slide already ran at tick 0,
         * and the portamento starts on tick 1. */
        period[0] = c.channels[0].period;
        volume[0] = c.channels[1].volume;
        for (tick = 1; tick <= 8; ++tick) {
            if (!render_into(&c, buf, TICK)) return 4;
            if (tick < 8) {
                period[tick] = c.channels[0].period;
                volume[tick] = c.channels[1].volume;
            }
        }
        /* Sustained portamento: exactly 2 units each tick, tick 0 neutral. */
        if (period[0] != 428)
            bad = fail("portamento fired on tick 0");
        for (tick = 1; tick < 8; ++tick) {
            if (period[tick] != period[tick - 1] - 2)
                bad = fail("sustained portamento did not run each tick");
        }
        /* The fine slide runs once at tick 0 and never during extra cycles. */
        if (volume[0] != 41) bad = fail("EA1 fine slide did not run once");
        for (tick = 1; tick < 8; ++tick) {
            if (volume[tick] != 41)
                bad = fail("EA1 re-ran during the EE extension");
        }
        /* Eight cycles happened: row 2 must be current now. */
        if (c.line != 2 || c.tick != 0)
            bad = fail("EE2 did not extend the row to 8 cycles");
        /* The extra cycles produce audio; the row was not skipped. */
        if (buf[0] == 0.0f) bad = fail("EE extension produced silence");
    }

    /* A delayed attack inside an EE row: the trigger belongs to the real
     * speed cycles and must fire exactly once, not again in the extra
     * cycles. EE1 gives 7 cycles with the attack at tick 2. */
    if (!bad) {
        voice_fixture();
        mod_event(module, 0, 0, 0, 1, 428, 0, 0);
        mod_event(module, 0, 1, 0, 2, 360, 0xe, 0xd2);
        mod_event(module, 0, 1, 3, 0, 0, 0xe, 0xe1);
        if (!init(&c)) return 5;
        /* Before the trigger: tick 6 + 2 cycles. */
        if (!render_into(&c, buf, TICK * 8 - 1)) return 6;
        if (c.channels[0].sample != 1)
            bad = fail("EE row triggered ED early");
        /* Cross the trigger. */
        if (!render_into(&c, buf, 1)) return 7;
        if (c.channels[0].sample != 2 || c.channels[0].period != 360)
            bad = fail("EE row lost the delayed attack");
        /* Through the extra cycle and the next row, the voice stays put. */
        if (!render_into(&c, buf, TICK * 5)) return 8;
        if (c.channels[0].sample != 2 || c.channels[0].period != 360)
            bad = fail("delayed attack repeated during EE extension");
        if (buf[0] == 0.0f) bad = fail("EE row produced silence");
    }

    /* Fxx applies to the row immediately: speed 3 + EE1 = 4 cycles. */
    if (!bad) {
        mod_make(module);
        mod_event(module, 0, 0, 0, 1, 428, 0, 0);
        mod_event(module, 0, 1, 0, 0, 0, 0xf, 3);
        mod_event(module, 0, 1, 1, 0, 0, 0xe, 0xe1);
        if (!init(&c)) return 9;
        if (!render_into(&c, buf, TICK * 6)) return 10;
        if (c.line != 1 || c.tick != 0) bad = fail("row 1 did not start");
        if (c.ticks_per_line != 3) bad = fail("F3 speed not applied on row 1");
        /* Three more cycles complete the four-cycle row 1. */
        if (!render_into(&c, buf, TICK * 3)) return 11;
        if (c.line != 1) bad = fail("EE1 row ended too early");
        if (!render_into(&c, buf, TICK)) return 12;
        if (c.line != 2 || c.tick != 0)
            bad = fail("F3+EE1 row did not run four cycles");
        if (c.ticks_per_line != 3) bad = fail("F3 speed not retained");
    }

    /* Highest channel's EE wins, independent of decode order. */
    if (!bad) {
        mod_make(module);
        mod_event(module, 0, 0, 0, 1, 428, 0, 0);
        mod_event(module, 0, 1, 1, 0, 0, 0xe, 0xe1);
        mod_event(module, 0, 1, 2, 0, 0, 0xe, 0xe3);
        if (!init(&c)) return 13;
        if (!render_into(&c, buf, TICK * 6)) return 14;   /* row 0 */
        if (!render_into(&c, buf, TICK * 8)) return 15;   /* 6+3 > 8 */
        if (c.line != 1) bad = fail("EE3 did not win over EE1");
        if (!render_into(&c, buf, TICK)) return 16;
        if (c.line != 2 || c.tick != 0) bad = fail("EE3 row length wrong");
    }

    free(buf);
    return bad ? 1 : 0;
}

/* ====================================================== export_boundary */

/* The end-to-end fixture: order 0 repeats rows 0..1 once with E6, row 2
 * carries EE1, and row 3 jumps with B/D into order 1 row 2. Order 1 runs
 * to the end and flows into order 2, whose natural end wraps into order 0
 * again: that wrap is the first genuine song loop. */
#define EX_TICKS_ORDER0 37          /* 6+6 replay, 6+6 replay, 7 with EE1, 6 */
#define EX_TICKS_ORDER1 372         /* rows 2..63 at speed 6                */
#define EX_TICKS_ORDER2 384         /* rows 0..63 at speed 6                */
#define EX_TICKS (EX_TICKS_ORDER0 + EX_TICKS_ORDER1 + EX_TICKS_ORDER2)
#define EX_FRAMES (EX_TICKS * TICK)

#define EXPORT_PERIOD 856   /* slowest note: no sample wrap inside a tick */

static void export_song(unsigned char *m)
{
    make_waves(m);
    mod_event(m, 0, 0, 0, 1, EXPORT_PERIOD, 0, 0); /* positive tone, row 0  */
    mod_event(m, 0, 0, 1, 0, 0, 0xe, 0x60);         /* E60 origin row 0      */
    mod_event(m, 0, 1, 1, 0, 0, 0xe, 0x61);         /* one extra play        */
    mod_event(m, 0, 2, 2, 0, 0, 0xe, 0xe1);         /* EE1 delays row 2      */
    mod_event(m, 0, 3, 0, 0, 0, 0xb, 1);            /* B01                   */
    mod_event(m, 0, 3, 1, 0, 0, 0xd, 2);            /* D02 -> order 1 row 2  */
    mod_event(m, 1, 2, 0, 2, EXPORT_PERIOD, 0, 0);  /* negative tone         */
}

/* Render the fixture with a fixed request size, splicing every short
 * return (order boundaries) until the first genuine song loop. Rendering
 * straight into the destination keeps large requests possible. */
static int render_trace(const unsigned char *m, int request_frames,
                        float *dst, int max_frames, int *frames_out)
{
    pocketmod_context c;
    int total = 0;
    if (!pocketmod_init(&c, m, MOD_BYTES, RATE)) return 0;
    while (pocketmod_loop_count(&c) == 0 && total < max_frames) {
        int want = request_frames;
        int bytes, got;
        if (want > max_frames - total) want = max_frames - total;
        bytes = pocketmod_render(&c, dst + total * 2,
                                 want * (int) sizeof(float[2]));
        got = bytes / (int) sizeof(float[2]);
        if (got <= 0) break;
        total += got;
    }
    *frames_out = total;
    return 1;
}

/* Read a 16-bit little-endian sample from the converter's WAV data. */
static int wav_frame(const unsigned char *wav, int frame, int channel,
                     float *out)
{
    int offset = (frame * 2 + channel) * 2;
    short value = (short) (((unsigned short) wav[offset + 1] << 8) |
                           (unsigned short) wav[offset]);
    *out = value / 32767.0f;
    return 1;
}

int test_export_boundary(void)
{
    static unsigned char song[MOD_BYTES];
    float *big = (float *) malloc(sizeof(float[2]) * EX_FRAMES);
    float *tmp = (float *) malloc(sizeof(float[2]) * EX_FRAMES);
    int bad = 0;
    int frames_big = 0, frames_tmp = 0;
    unsigned char *wav = NULL;
    long wav_bytes = 0;
    FILE *file;

    if (!big || !tmp) return 1;

    /* (1) The same fragment rendered with a whole-song request and with
     * 1/17/257-frame requests must splice into identical PCM. The window
     * covers the first 40 ticks, where the 856-period notes traverse far
     * less than one 8192-byte sample, so no sample wrap can masquerade as
     * a scheduling difference. That window spans the row replay, the EE1
     * row and the B/D jump. */
    export_song(song);
    if (!render_trace(song, EX_FRAMES, big, EX_FRAMES, &frames_big)) return 2;
    if (frames_big != EX_FRAMES)
        bad = fail("E6/EE fixture length is not the analytic 793 ticks");
    {
        int sizes[3] = {1, 17, 257};
        int k;
        for (k = 0; k < 3 && !bad; ++k) {
            if (!render_trace(song, sizes[k], tmp, EX_FRAMES, &frames_tmp)) {
                bad = fail("chunked render did not reach the first loop");
                break;
            }
            if (frames_tmp != frames_big) {
                bad = fail("chunked render produced a different length");
                break;
            }
            if (memcmp(big, tmp, (size_t) 40 * TICK * sizeof(float[2])) != 0)
                bad = fail("chunked render differs at a sample position");
        }
    }

    /* (2) A large request must stop short exactly at an order boundary,
     * not continue through it: that is the short return callers rely on
     * to split exports. The first boundary is after order 0 (37 ticks). */
    if (!bad) {
        pocketmod_context c;
        float *one = (float *) malloc(sizeof(float[2]) * EX_FRAMES);
        int first = 0, second = 0;
        if (!one) { free(one); return 3; }
        if (!pocketmod_init(&c, song, MOD_BYTES, RATE)) return 4;
        first = pocketmod_render(&c, one, EX_FRAMES * (int) sizeof(float[2]));
        first /= (int) sizeof(float[2]);
        /* It stopped at the order-0 boundary after 37 ticks. */
        if (first != 37 * TICK)
            bad = fail("first render did not stop short at the order boundary");
        if (!bad && (c.pattern != 1 || c.line != 2))
            bad = fail("short return did not land on the B/D destination");
        /* The very next call resumes at the destination and keeps going. */
        if (!bad) {
            second = pocketmod_render(&c, one, 100 * (int) sizeof(float[2]));
            second /= (int) sizeof(float[2]);
            if (second != 100)
                bad = fail("render did not resume after a short return");
        }
        free(one);
    }

    /* (3) Run the real converter on the fixture and inspect the WAV. */
    if (!bad) {
        int wrote = mod_write(".build/row_scheduler_song.mod", song);
        if (!wrote) bad = fail("could not write the fixture MOD");
        if (!bad) {
            int rc = system("./.build/converter .build/row_scheduler_song.mod "
                            ".build/row_scheduler_song.wav");
            if (rc != 0) bad = fail("converter failed on the fixture");
        }
    }
    if (!bad) {
        file = fopen(".build/row_scheduler_song.wav", "rb");
        if (!file) { bad = fail("converter wrote no WAV"); }
        else {
            /* Header: RIFF/<size>/WAVE/fmt <16>/.../data/<size>. */
            unsigned char header[44];
            if (fread(header, 1, 44, file) != 44) bad = fail("short WAV header");
            else {
                unsigned long data_bytes = (unsigned long) header[40] |
                    ((unsigned long) header[41] << 8) |
                    ((unsigned long) header[42] << 16) |
                    ((unsigned long) header[43] << 24);
                if (data_bytes % 4 != 0) bad = fail("odd WAV data size");
                else {
                    wav_bytes = (long) data_bytes;
                    wav = (unsigned char *) malloc(data_bytes);
                    if (!wav) bad = fail("WAV allocation failed");
                    else if (fread(wav, 1, data_bytes, file) != data_bytes)
                        bad = fail("short WAV data");
                }
            }
            fclose(file);
        }
    }

    /* (3) The WAV length proves both features survived: the local replay
     * adds 12 ticks and the EE row adds one, so a premature stop at E6 or
     * a dropped extension would be short. The end must be the first real
     * song loop, not one tick more or less. */
    if (!bad) {
        if (wav_bytes != (long) EX_FRAMES * 4)
            bad = fail("WAV length does not match the first genuine song loop");
    }

    /* (4) WAV sample values: row 0's replay restarts the waveform at the
     * same phase, and the EE extension keeps the old voice through the
     * tick that row 3 would otherwise have started on. */
    if (!bad) {
        float first_l = 0, replay_l = 0, before_ee_l = 0, ee_l = 0, jump_l = 0;
        wav_frame(wav, 0, 0, &first_l);
        wav_frame(wav, 12 * TICK, 0, &replay_l);           /* replay of row 0 */
        if (!(first_l > 0.0f)) bad = fail("row 0 first sample is not audible");
        if (!bad && fabsf(first_l - replay_l) > 2e-4f)
            bad = fail("replayed row 0 did not restart at the same phase");
        /* Tick 36 is row 3 if EE1 ran, order 1 row 2 if it did not. Row 3
         * keeps sample 1 (positive at this phase); order 1 starts the
         * negative sample. */
        wav_frame(wav, 36 * TICK, 0, &ee_l);
        wav_frame(wav, 37 * TICK, 0, &jump_l);
        if (!bad && ee_l <= 0.0f)
            bad = fail("EE1 extension lost: row 3 was cut short");
        if (!bad && jump_l >= 0.0f)
            bad = fail("B/D destination did not start its negative voice");
        (void) before_ee_l;
    }

    /* (5) Cross-check WAV samples against the library's own PCM at the
     * same absolute positions (the converter writes 16-bit quantised
     * values of the same float stream). */
    if (!bad && wav && frames_big == EX_FRAMES) {
        int positions[5];
        int k;
        positions[0] = 0;
        positions[1] = 12 * TICK + 5;
        positions[2] = 30 * TICK + 7;
        positions[3] = 36 * TICK + 1;
        positions[4] = EX_FRAMES - 1;
        for (k = 0; k < 5 && !bad; ++k) {
            float w;
            int p = positions[k];
            wav_frame(wav, p, 0, &w);
            if (fabsf(w - big[p * 2]) > 3e-4f)
                bad = fail("WAV sample differs from the scheduled PCM");
        }
    }

    free(wav);
    free(tmp);
    free(big);
    return bad ? 1 : 0;
}
