/*
 * tests/legato.c - monophonic legato test suite for FluidLite.
 *
 * The sounds always come out of the public rendering entry points
 * (fluid_synth_write_float); the internal headers are only used to
 * inspect voices, legato groups and glide state.  Seven named cases
 * are executed; run_legato.sh requires the exact success line.
 */

#include <fluidlite.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "fluid_synth.h"
#include "fluid_voice.h"
#include "fluid_chan.h"
#include "fluid_legato.h"
#include "fluid_conv.h"

#ifndef FLUID_FAILED
#define FLUID_FAILED (-1)
#endif

#define SR 44100.0
#define NFRAMES 8192

static int failures;
static int checks;

#define CHECK(cond, ...) do {                                       \
  checks++;                                                         \
  if (!(cond)) {                                                    \
    failures++;                                                     \
    printf("  FAIL: "); printf(__VA_ARGS__); printf("\n");          \
  }                                                                 \
} while (0)

/* ------------------------------------------------------------------ */
/* RAM SoundFont helpers                                              */
/* ------------------------------------------------------------------ */

static void make_sine(short *data, int n, int periods)
{
  int i;
  for (i = 0; i < n; i++) {
    data[i] = (short)(12000.0 * sin(2.0 * M_PI * periods * i / n));
  }
}

typedef struct
{
  fluid_settings_t *settings;
  fluid_synth_t *synth;
  fluid_sfont_t *sf;
  fluid_sample_t *sample[4];
  short data[4][256];
} rig_t;

static void rig_init(rig_t *rig, int interp, int nsamples, int glide_ms)
{
  int i;
  memset(rig, 0, sizeof(*rig));
  rig->settings = new_fluid_settings();
  fluid_settings_setnum(rig->settings, "synth.sample-rate", SR);
  /* Note-offs must take effect immediately so the tests can observe
   * sustain-pedal and release states without waiting out the default
   * 10 ms minimum note length. */
  fluid_settings_setint(rig->settings, "synth.min-note-length", 0);
  rig->synth = new_fluid_synth(rig->settings);
  fluid_synth_set_reverb_on(rig->synth, 0);
  fluid_synth_set_chorus_on(rig->synth, 0);
  fluid_synth_set_interp_method(rig->synth, 0, interp);
  if (rig->synth->midi_channels > 1) {
    fluid_synth_set_interp_method(rig->synth, 1, interp);
  }

  int sfont_id;
  rig->sf = fluid_ramsfont_create_sfont();
  for (i = 0; i < nsamples; i++) {
    make_sine(rig->data[i], 256, i + 1);
    rig->sample[i] = new_fluid_ramsample();
    fluid_sample_set_sound_data(rig->sample[i], rig->data[i], 256, 1, 60);
    /* All zones cover the whole key range so one note-on can create
     * one voice per zone. */
    fluid_ramsfont_add_izone(rig->sf->data, 0, 0, rig->sample[i], 0, 127);
    fluid_ramsfont_izone_set_loop(rig->sf->data, 0, 0, rig->sample[i],
                                  1, 0, 0);
  }
  sfont_id = fluid_synth_add_sfont(rig->synth, rig->sf);
  if (sfont_id < 0) {
    printf("add_sfont failed\n");
  }
  if (fluid_synth_program_select(rig->synth, 0, sfont_id, 0, 0) != 0) {
    printf("program_select ch0 failed\n");
  }
  if (rig->synth->midi_channels > 1) {
    if (fluid_synth_program_select(rig->synth, 1, sfont_id, 0, 0) != 0) {
      printf("program_select ch1 failed\n");
    }
  }
  fluid_synth_set_legato(rig->synth, 0, 1, glide_ms / 1000.0);
  if (rig->synth->midi_channels > 1) {
    fluid_synth_set_legato(rig->synth, 1, 1, glide_ms / 1000.0);
  }
}

static void rig_done(rig_t *rig)
{
  delete_fluid_synth(rig->synth);
  delete_fluid_settings(rig->settings);
}

static int count_voices(fluid_synth_t *synth, int chan)
{
  int i, n = 0;
  for (i = 0; i < synth->polyphony; i++) {
    if (_PLAYING(synth->voice[i]) && synth->voice[i]->chan == chan) {
      n++;
    }
  }
  return n;
}

static double energy(const float *buf, int n)
{
  double e = 0;
  int i;
  for (i = 0; i < n; i++) {
    e += (double)buf[i] * buf[i];
  }
  return e;
}

static int all_finite(const float *buf, int n)
{
  int i;
  for (i = 0; i < n; i++) {
    if (!isfinite(buf[i])) {
      return 0;
    }
  }
  return 1;
}

static fluid_legato_group_t *group0(rig_t *rig)
{
  return rig->synth->channel[0]->legato->group;
}

/* Independent re-implementation of the specified glide formula. */
static double expected_base(double c0, double ct, unsigned n, unsigned t)
{
  double f;
  if (n == 0) {
    return ct;
  }
  f = (double)t / (double)n;
  if (f > 1.0) {
    f = 1.0;
  }
  return c0 + (ct - c0) * f;
}

/* ------------------------------------------------------------------ */
/* Case 1: multi-zone attack, reuse without any restart               */
/* ------------------------------------------------------------------ */
static int case_multi_zone_attack(void)
{
  rig_t rig;
  static float left[NFRAMES], right[NFRAMES];
  fluid_legato_group_t *g;
  fluid_voice_t *v0, *v1;
  int section_before, section_after;

  rig_init(&rig, FLUID_INTERP_4THORDER, 2, 100);

  CHECK(fluid_synth_noteon(rig.synth, 0, 60, 100) == 0, "noteon 60");
  fluid_synth_write_float(rig.synth, 128, left, 0, 1, right, 0, 1);

  g = group0(&rig);
  CHECK(g->nvoice == 2, "expected 2 zones/voices, got %d", g->nvoice);
  CHECK(count_voices(rig.synth, 0) == 2, "2 playing voices");
  CHECK(g->nkeys == 1 && g->keys[0] == 60, "one physical key");
  CHECK(g->voice[0]->legato_group == g && g->voice[1]->legato_group == g,
        "both voices attached to group");
  v0 = g->voice[0];
  v1 = g->voice[1];
  CHECK(v0->sample != v1->sample, "zones keep their own samples");

  section_before = v0->volenv_section;
  CHECK(fluid_synth_noteon(rig.synth, 0, 72, 64) == 0, "legato noteon 72");
  /* The new velocity must not change the voices of the group. */
  CHECK(v0->vel == 100 && v1->vel == 100, "velocity modulation preserved");
  CHECK(g->voice[0] == v0 && g->voice[1] == v1, "voice slots reused");
  CHECK(g->nvoice == 2 && count_voices(rig.synth, 0) == 2,
        "no extra voice started");
  section_after = v0->volenv_section;
  CHECK(section_before == section_after, "envelope not restarted");
  CHECK(g->glide_ct == 7200.0, "glide target is 7200 cents");

  fluid_synth_write_float(rig.synth, NFRAMES - 128, left, 0, 1, right, 0, 1);
  CHECK(all_finite(left, NFRAMES), "finite PCM");
  CHECK(energy(left, NFRAMES) > 1.0, "audible PCM");
  CHECK(v0->sample == rig.sample[0] || v0->sample == rig.sample[1],
        "sample never swapped");

  rig_done(&rig);
  return failures == 0;
}

/* ------------------------------------------------------------------ */
/* Case 2: key priority fallback and repeated key note-on             */
/* ------------------------------------------------------------------ */
static int case_key_fallback_repeat(void)
{
  rig_t rig;
  static float left[256], right[256];
  fluid_legato_group_t *g;
  fluid_voice_t *v;

  rig_init(&rig, FLUID_INTERP_LINEAR, 1, 100);
  g = group0(&rig);

  fluid_synth_noteon(rig.synth, 0, 60, 100);
  fluid_synth_write_float(rig.synth, 64, left, 0, 1, right, 0, 1);
  v = g->voice[0];

  fluid_synth_noteon(rig.synth, 0, 64, 100);
  fluid_synth_noteon(rig.synth, 0, 67, 100);
  CHECK(g->nkeys == 3, "three held keys");
  CHECK(g->keys[0] == 60 && g->keys[1] == 64 && g->keys[2] == 67,
        "most recent key last");
  CHECK(g->glide_ct == 6700.0, "target follows newest key");

  /* Releasing the origin key while other keys are still held must not
   * release the group: its voices keep key 60's key number. */
  fluid_synth_noteoff(rig.synth, 0, 60);
  CHECK(g->nkeys == 2 && g->nvoice == 1, "origin key removed, group alive");
  CHECK(_ON(v), "group voice not released by origin-key note-off");
  CHECK(g->keys[0] == 64 && g->keys[1] == 67, "remaining keys ordered");
  /* Re-press 60 so the original key list is restored for the checks
   * below; it becomes the most recent key. */
  fluid_synth_noteon(rig.synth, 0, 60, 100);
  CHECK(g->nkeys == 3 && g->keys[2] == 60, "origin key re-added last");

  /* Repeated note-on of a held key moves it to the end only. */
  fluid_synth_noteon(rig.synth, 0, 60, 100);
  CHECK(g->nkeys == 3, "repeat does not stack key counts");
  CHECK(g->keys[0] == 64 && g->keys[1] == 67 && g->keys[2] == 60,
        "repeat moves key to priority end");
  CHECK(g->voice[0] == v && g->nvoice == 1, "no retrigger on repeat");
  CHECK(g->glide_ct == 6000.0, "repeat retargets glide");

  /* Release a non-current key: the sounding pitch must not change. */
  fluid_synth_noteoff(rig.synth, 0, 67);
  CHECK(g->nkeys == 2, "key removed");
  CHECK(g->keys[0] == 64 && g->keys[1] == 60, "order kept");
  CHECK(g->glide_ct == 6000.0, "non-current release leaves pitch");

  /* Release the current key: glide towards the newest remaining key. */
  fluid_synth_noteoff(rig.synth, 0, 60);
  CHECK(g->nkeys == 1 && g->keys[0] == 64, "fallback key remains");
  CHECK(g->glide_ct == 6400.0, "fallback retargets glide");
  CHECK(g->voice[0] == v, "same voice keeps sounding");

  fluid_synth_write_float(rig.synth, 64, left, 0, 1, right, 0, 1);
  CHECK(energy(left, 64) > 0.0, "still sounding after fallback");

  fluid_synth_noteoff(rig.synth, 0, 64);
  CHECK(g->nkeys == 0 && g->nvoice == 0, "last release ends group");

  rig_done(&rig);
  return failures == 0;
}

/* ------------------------------------------------------------------ */
/* Case 3: glide trajectory, mid-way retarget, phase integration       */
/* ------------------------------------------------------------------ */
static int case_glide_retarget_trajectory(void)
{
  rig_t rig;
  static float left[8192], right[8192];
  fluid_legato_group_t *g;
  fluid_voice_t *v;
  const unsigned N = 4410;          /* 0.1 s at 44100 Hz */
  unsigned block;
  double c0, ct;
  double expected_delta;
  int frames = 0;
  const int loop_size = 256;        /* RAM sample loop covers all data */

  memset(left, 0, sizeof(left));
  memset(right, 0, sizeof(right));
  rig_init(&rig, FLUID_INTERP_4THORDER, 1, 100);
  g = group0(&rig);

  fluid_synth_noteon(rig.synth, 0, 60, 110);
  fluid_synth_write_float(rig.synth, 64, left, 0, 1, right, 0, 1);
  frames = 64;
  v = g->voice[0];
  CHECK(fabs(v->legato_pitch_offset) < 1e-6, "no tuning offset on plain sample");

  /* Start a 6000 -> 7200 cents glide. */
  fluid_synth_noteon(rig.synth, 0, 72, 90);
  c0 = 6000.0;
  ct = 7200.0;
  CHECK(g->glide_n == N && g->gliding, "glide N=round(0.1*44100)=4410");

  /* Every render is exactly one 64-sample internal block, so events
   * always land on a block boundary and the glide clock advances by
   * exactly the samples generated. */
  for (block = 0; block < 70; block++) {
    int j;
    double pre_phase = fluid_phase_double(v->phase);
    int pre_env = v->volenv_section;

    /* Trajectory check against an independent formula, per sample.
     * The engine stores the trajectory in fluid_real_t (float), so a
     * millicent-scale tolerance is appropriate. */    for (j = 0; j < 64; j++) {
      unsigned t = block * 64 + j;
      double want = expected_base(c0, ct, N, t);
      double got = fluid_legato_base_pitch(g, j);
      CHECK(fabs(got - want) < 1e-3, "trajectory sample t=%u want=%f got=%f",
            t, want, got);
      if (failures) {
        goto done_traj;
      }
    }

    /* Phase integration from the independent pitch formula.  The
     * sample phase wraps at loop points, so allow a whole number of
     * loop lengths between observed and expected displacement. */
    expected_delta = 0.0;
    for (j = 0; j < 64; j++) {
      unsigned t = block * 64 + j;
      double pitch = expected_base(c0, ct, N, t);
      expected_delta += fluid_ct2hz_real((fluid_real_t)pitch)
                        / v->root_pitch;
    }
    {
      double phase_before = fluid_phase_double(v->phase);
      double phase_after, actual;
      fluid_synth_write_float(rig.synth, 64,
                              left + frames, 0, 1, right + frames, 0, 1);
      frames += 64;
      phase_after = fluid_phase_double(v->phase);
      actual = phase_after - phase_before;
      {
	int wraps = (int) floor((expected_delta - actual) / loop_size + 0.5);
	double unwrapped = actual + wraps * loop_size;
	CHECK(fabs(unwrapped - expected_delta) / expected_delta < 2e-4,
	      "phase integral block=%u actual=%f expected=%f",
	      block, unwrapped, expected_delta);
      }
    }

    /* The voice's envelope section and sample phase can only evolve
     * forward; a glide never restarts them (explicit retarget
     * preservation is checked in the section below). */
    CHECK(v->volenv_section >= pre_env, "envelope never rewinds");
    CHECK(fluid_phase_double(v->phase) >= pre_phase
          || v->has_looped, "phase only resets at a loop wrap");

    /* Per-sample increments really vary inside the block. */
    if (block == 0) {
      double p0 = fluid_legato_voice_phase_incr(v, 0);
      double p63 = fluid_legato_voice_phase_incr(v, 63);
      CHECK(fabs(p63 - p0) > 1e-6, "per-sample phase incr varies in block");
    }
  }

  /* 70 blocks (4480 samples) cover the 4410-sample segment entirely. */
  CHECK(!g->gliding && fabs(g->glide_c0 - 7200.0) < 1e-3
        && fabs(g->glide_ct - 7200.0) < 1e-3,
        "glide settles at target");

done_traj:
  /* Retarget mid-way through a fresh glide: it must continue from the
   * actual current pitch rather than restart from the previous key. */
  {
    unsigned k;
    double mid;
    double phase_at_retarget;
    int env_at_retarget;
    fluid_synth_noteon(rig.synth, 0, 48, 90);       /* 7200 -> 4800 */
    CHECK(fabs(g->glide_c0 - 7200.0) < 1e-3, "retarget starts at settled pitch");
    fluid_synth_write_float(rig.synth, 64,
                            left + frames, 0, 1, right + frames, 0, 1);
    frames += 64;
    fluid_synth_write_float(rig.synth, 64,
                            left + frames, 0, 1, right + frames, 0, 1);
    frames += 64;
    fluid_synth_write_float(rig.synth, 64,
                            left + frames, 0, 1, right + frames, 0, 1);
    frames += 64;
    phase_at_retarget = fluid_phase_double(v->phase);
    env_at_retarget = v->volenv_section;
    /* Direction reversal mid-glide: downwards 7200->4800 becomes
     * upwards ->8400, continuing from the actual current position. */
    fluid_synth_noteon(rig.synth, 0, 84, 90);       /* retarget at t=192 */
    CHECK(fabs(fluid_phase_double(v->phase) - phase_at_retarget) < 1e-12,
          "mid-glide retarget preserves sample phase");
    CHECK(v->volenv_section == env_at_retarget,
          "mid-glide retarget preserves envelope");
    mid = expected_base(7200.0, 4800.0, N, 192);
    CHECK(fabs(g->glide_c0 - mid) < 1e-3, "c0 is current position %f",
          g->glide_c0);
    CHECK(g->glide_ct == 8400.0 && g->glide_j == 0, "new segment from here");
    for (k = 0; k < 64; k++) {
      double want = expected_base(mid, 8400.0, N, k);
      double got = fluid_legato_base_pitch(g, k);
      CHECK(fabs(got - want) < 1e-3, "retargeted trajectory k=%u", k);
      if (failures) {
        break;
      }
    }
    fluid_synth_write_float(rig.synth, 64,
                            left + frames, 0, 1, right + frames, 0, 1);
    frames += 64;
  }

  /* Envelope continuity: no attack restart ever. */
  CHECK(v->volenv_section != FLUID_VOICE_ENVATTACK || v->ticks == 0,
        "no replayed attack");
  CHECK(all_finite(left, frames), "finite PCM");
  CHECK(energy(left, frames) > 1.0, "audible PCM");

  rig_done(&rig);
  return failures == 0;
}

/* ------------------------------------------------------------------ */
/* Case 4: pitch bend freeze and SoundFont tuning offset              */
/* ------------------------------------------------------------------ */
static int case_bend_and_tuning(void)
{
  rig_t ref, rig_bend;
  static float lref[4096], rref[4096], lb[4096], rb[4096];
  fluid_legato_group_t *g;
  int i, same;

  /* Reference: bend held at maximum for the whole run. */
  rig_init(&ref, FLUID_INTERP_LINEAR, 1, 100);
  fluid_synth_pitch_wheel_sens(ref.synth, 0, 2);
  fluid_synth_pitch_bend(ref.synth, 0, 16383);          /* +200 cents */
  fluid_synth_noteon(ref.synth, 0, 60, 100);
  g = group0(&ref);
  /* The wheel value is quantized to 14 bits, so allow a small tolerance. */
  CHECK(fabs(g->voice[0]->legato_pitch_offset - 200.0) < 0.1,
        "pitch bend frozen into offset, got %f",
        g->voice[0]->legato_pitch_offset);

  /* Second rig: same start, but bend moves and returns to center. */
  rig_init(&rig_bend, FLUID_INTERP_LINEAR, 1, 100);
  fluid_synth_pitch_wheel_sens(rig_bend.synth, 0, 2);
  fluid_synth_pitch_bend(rig_bend.synth, 0, 16383);
  fluid_synth_noteon(rig_bend.synth, 0, 60, 100);

  fluid_synth_write_float(ref.synth, 256, lref, 0, 1, rref, 0, 1);
  fluid_synth_write_float(rig_bend.synth, 256, lb, 0, 1, rb, 0, 1);

  /* Bend gymnastics, including a return to center. */
  fluid_synth_pitch_bend(rig_bend.synth, 0, 0);
  fluid_synth_pitch_bend(rig_bend.synth, 0, 8192);
  fluid_synth_noteon(rig_bend.synth, 0, 72, 100);
  fluid_synth_pitch_bend(rig_bend.synth, 0, 4096);
  fluid_synth_pitch_bend(rig_bend.synth, 0, 8192);

  /* Same events on the reference except the bend changes. */
  fluid_synth_noteon(ref.synth, 0, 72, 100);

  fluid_synth_write_float(ref.synth, 3840, lref + 256, 0, 1, rref + 256, 0, 1);
  fluid_synth_write_float(rig_bend.synth, 3840, lb + 256, 0, 1, rb + 256, 0, 1);

  same = 1;
  for (i = 0; i < 4096; i++) {
    if (lref[i] != lb[i] || rref[i] != rb[i]) {
      same = 0;
      break;
    }
  }
  CHECK(same, "bend changes (incl. center return) do not alter glide PCM");
  CHECK(all_finite(lb, 4096) && energy(lb, 4096) > 1.0, "bend PCM ok");

  rig_done(&ref);
  rig_done(&rig_bend);

  /* Fine/coarse tuning stays in the frozen offset. */
  {
    rig_t tune;
    rig_init(&tune, FLUID_INTERP_LINEAR, 1, 0);
    fluid_synth_set_gen(tune.synth, 0, GEN_FINETUNE, 50.0f);
    fluid_synth_set_gen(tune.synth, 0, GEN_COARSETUNE, 1.0f);
    fluid_synth_noteon(tune.synth, 0, 60, 100);
    fluid_synth_write_float(tune.synth, 64, lb, 0, 1, rb, 0, 1);
    g = group0(&tune);
    CHECK(fabs(g->voice[0]->legato_pitch_offset - 150.0) < 1e-6,
          "fine+coarse tuning offset=%f", g->voice[0]->legato_pitch_offset);
    /* N = 0: retargeting is instantaneous. */
    fluid_synth_noteon(tune.synth, 0, 72, 100);
    CHECK(g->glide_n == 0 && !g->gliding && g->glide_ct == 7200.0,
          "zero glide time jumps immediately");
    rig_done(&tune);
  }
  return failures == 0;
}

/* ------------------------------------------------------------------ */
/* Case 5: four interpolators across loop wraps + chunk equivalence   */
/* ------------------------------------------------------------------ */
static int case_interp_loop_chunking(void)
{
  static const int methods[4] = {
    FLUID_INTERP_NONE, FLUID_INTERP_LINEAR,
    FLUID_INTERP_4THORDER, FLUID_INTERP_7THORDER
  };
  static const char *names[4] = {"none", "linear", "4th", "7th"};
  int m;

  for (m = 0; m < 4; m++) {
    rig_t whole, chunked;
    static float lw[6000], rw[6000], lc[6000], rc[6000];
    int i, produced, step;
    fluid_legato_group_t *g;

    rig_init(&whole, methods[m], 1, 200);
    rig_init(&chunked, methods[m], 1, 200);

    fluid_synth_noteon(whole.synth, 0, 60, 100);
    fluid_synth_noteon(chunked.synth, 0, 60, 100);
    fluid_synth_write_float(whole.synth, 192, lw, 0, 1, rw, 0, 1);
    /* consume the same 192 frames in small pieces on the chunked rig */
    produced = 0;
    step = 1;
    while (produced < 192) {
      int take = step;
      if (produced + take > 192) take = 192 - produced;
      fluid_synth_write_float(chunked.synth, take,
                              lc + produced, 0, 1, rc + produced, 0, 1);
      produced += take;
      step = (step == 1) ? 17 : (step == 17 ? 65 : 1);
    }

    /* Event on an internal 64-sample boundary (t=192). */
    fluid_synth_noteon(whole.synth, 0, 84, 100);
    fluid_synth_noteon(chunked.synth, 0, 84, 100);

    fluid_synth_write_float(whole.synth, 5808, lw + 192, 0, 1, rw + 192, 0, 1);
    produced = 0;
    step = 1;
    while (produced < 5808) {
      int take = step;
      if (produced + take > 5808) take = 5808 - produced;
      fluid_synth_write_float(chunked.synth, take,
                              lc + 192 + produced, 0, 1,
                              rc + 192 + produced, 0, 1);
      produced += take;
      step = (step == 1) ? 17 : (step == 17 ? 65 : 1);
    }

    for (i = 0; i < 6000; i++) {
      CHECK(lw[i] == lc[i] && rw[i] == rc[i],
            "%s: chunk mismatch at frame %d (%g vs %g)",
            names[m], i, lw[i], lc[i]);
      if (failures) break;
    }
    CHECK(all_finite(lw, 6000), "%s: finite", names[m]);
    CHECK(energy(lw, 6000) > 1.0, "%s: audible", names[m]);
    g = group0(&whole);
    CHECK(g->voice[0]->has_looped == 1, "%s: crossed sample loop", names[m]);

    rig_done(&whole);
    rig_done(&chunked);
  }
  return failures == 0;
}

/* ------------------------------------------------------------------ */
/* Case 6: sustain pedal keeps the old group isolated from new notes   */
/* ------------------------------------------------------------------ */
static int case_sustain_pedal_isolation(void)
{
  rig_t rig;
  static float left[4096], right[4096];
  fluid_legato_group_t *g;
  fluid_voice_t *old[8];
  int nold, i, old_playing_after_new;

  memset(left, 0, sizeof(left));
  memset(right, 0, sizeof(right));
  rig_init(&rig, FLUID_INTERP_4THORDER, 2, 100);
  g = group0(&rig);

  fluid_synth_noteon(rig.synth, 0, 60, 100);
  fluid_synth_write_float(rig.synth, 128, left, 0, 1, right, 0, 1);
  nold = g->nvoice;
  CHECK(nold == 2, "two-zone group");
  for (i = 0; i < nold; i++) {
    old[i] = g->voice[i];
    old[i]->debug = 1;
  }

  fluid_synth_noteon(rig.synth, 0, 72, 100);
  /* Let the 6000 -> 7200 glide run 192 samples before the keys go up. */
  fluid_synth_write_float(rig.synth, 192, left + 128, 0, 1,
                          right + 128, 0, 1);
  {
    double mid_pitch = expected_base(6000.0, 7200.0, 4410, 192);
    fluid_synth_cc(rig.synth, 0, 64, 127);            /* sustain down */
    fluid_synth_noteoff(rig.synth, 0, 72);
    fluid_synth_noteoff(rig.synth, 0, 60);            /* last key up */
    /* Detaching mid-glide must bake the reached pitch into the
     * sustained voices, not snap them back to the first key. */
    for (i = 0; i < nold; i++) {
      CHECK(fabs(old[i]->pitch - mid_pitch) < 1.0,
            "detach keeps reached pitch %f (got %f)", mid_pitch,
            old[i]->pitch);
    }
  }

  CHECK(g->nkeys == 0 && g->nvoice == 0, "group finished while sustained");
  for (i = 0; i < nold; i++) {
    CHECK(old[i]->legato_group == NULL, "sustained voice detached");
    CHECK(_SUSTAINED(old[i]), "voice held by pedal");
  }
  {
    /* The detached pitch must hold while the pedal keeps the voices. */
    double held_pitch = old[0]->pitch;
    fluid_synth_write_float(rig.synth, 128, left, 0, 1, right, 0, 1);
    CHECK(fabs(old[0]->pitch - held_pitch) < 1e-6,
          "detached pitch holds without a glide group");
  }

  /* A new note-on is a brand new attack; the pedal-held old group must
   * not follow its glide. */
  fluid_synth_write_float(rig.synth, 64, left, 0, 1, right, 0, 1);
  fluid_synth_noteon(rig.synth, 0, 48, 100);
  fluid_synth_write_float(rig.synth, 128, left, 0, 1, right, 0, 1);
  CHECK(g->nvoice == 2, "new group created");
  for (i = 0; i < g->nvoice; i++) {
    CHECK(g->voice[i] != old[0] && g->voice[i] != old[1],
          "new attack uses fresh voice slots");
  }
  old_playing_after_new = 0;
  for (i = 0; i < nold; i++) {
    if (_PLAYING(old[i])) old_playing_after_new++;
  }
  CHECK(old_playing_after_new == nold, "old group still rings on pedal");
  {
    double old_pitch = old[0]->pitch;
    fluid_synth_noteon(rig.synth, 0, 96, 100);
    fluid_synth_write_float(rig.synth, 128, left, 0, 1, right, 0, 1);
    CHECK(old[0]->pitch == old_pitch, "old group ignores new key glide");
  }

  /* Pedal up releases the old voices while the new group continues. */
  fluid_synth_cc(rig.synth, 0, 64, 0);
  fluid_synth_write_float(rig.synth, 2048, left, 0, 1, right, 0, 1);
  for (i = 0; i < nold; i++) {
    CHECK(!_PLAYING(old[i]), "old voices finished after pedal up");
  }
  CHECK(g->nvoice == 2 && _PLAYING(g->voice[0]), "new group survives");
  CHECK(all_finite(left, 4096), "finite PCM");

  rig_done(&rig);
  return failures == 0;
}

/* ------------------------------------------------------------------ */
/* Case 7: channels, resets, API validation                           */
/* ------------------------------------------------------------------ */
static int case_channels_reset_validation(void)
{
  rig_t rig;
  static float left[2048], right[2048];
  fluid_legato_group_t *g0, *g1;
  fluid_voice_t *v;

  rig_init(&rig, FLUID_INTERP_LINEAR, 1, 100);
  g0 = rig.synth->channel[0]->legato->group;
  g1 = rig.synth->channel[1]->legato->group;

  /* Invalid glide arguments: failure without state change. */
  CHECK(fluid_synth_set_legato(rig.synth, 0, 1, NAN) == FLUID_FAILED,
        "NaN rejected");
  CHECK(fluid_synth_set_legato(rig.synth, 0, 1, -0.01) == FLUID_FAILED,
        "negative rejected");
  CHECK(fluid_synth_set_legato(rig.synth, 0, 1, 10.01) == FLUID_FAILED,
        ">10s rejected");
  CHECK(fluid_synth_set_legato(rig.synth, 0, 1, INFINITY) == FLUID_FAILED,
        "infinity rejected");
  CHECK(fluid_synth_set_legato(rig.synth, 99, 1, 0.1) == FLUID_FAILED,
        "bad channel rejected");
  CHECK(rig.synth->channel[0]->legato->enabled, "state unchanged on failure");
  CHECK(rig.synth->channel[0]->legato->glide_seconds == 0.1,
        "glide time unchanged on failure");

  /* Cannot toggle while keys are held. */
  fluid_synth_noteon(rig.synth, 0, 60, 100);
  CHECK(fluid_synth_set_legato(rig.synth, 0, 0, 0.1) == FLUID_FAILED,
        "disable while held rejected");
  CHECK(rig.synth->channel[0]->legato->enabled, "still enabled");

  /* Channel independence. */
  fluid_synth_noteon(rig.synth, 1, 60, 100);
  fluid_synth_noteon(rig.synth, 1, 72, 100);
  CHECK(g1->nkeys == 2 && g1->glide_ct == 7200.0, "ch1 glides");
  CHECK(g0->nkeys == 1 && g0->glide_ct == 6000.0, "ch0 untouched by ch1");
  fluid_synth_write_float(rig.synth, 128, left, 0, 1, right, 0, 1);

  /* all-notes-off clears keys/glide; next note-on is a new attack. */
  v = g0->voice[0];
  fluid_synth_all_notes_off(rig.synth, 0);
  CHECK(g0->nkeys == 0 && g0->nvoice == 0, "all notes off clears state");
  fluid_synth_write_float(rig.synth, 128, left, 0, 1, right, 0, 1);
  fluid_synth_noteon(rig.synth, 0, 60, 100);
  CHECK(g0->nvoice == 1 && g0->voice[0] != v, "fresh attack after all notes off");

  /* Program change clears the held list but keeps mode enabled. */
  fluid_synth_noteon(rig.synth, 0, 72, 100);
  CHECK(fluid_synth_program_change(rig.synth, 0, 0) == 0, "program change");
  CHECK(g0->nkeys == 0 && g0->nvoice == 0, "program change clears group");
  CHECK(rig.synth->channel[0]->legato->enabled, "legato stays enabled");
  fluid_synth_noteon(rig.synth, 0, 60, 100);
  CHECK(g0->nkeys == 1, "first key after program change re-arms");

  /* all-sounds-off stops voices immediately. */
  fluid_synth_all_sounds_off(rig.synth, 0);
  CHECK(g0->nkeys == 0 && g0->nvoice == 0, "all sounds off clears state");
  CHECK(count_voices(rig.synth, 0) == 0, "all sounds off stops voices");

  /* System reset preserves the enabled flag but clears everything. */
  fluid_synth_noteon(rig.synth, 0, 60, 100);
  fluid_synth_noteon(rig.synth, 1, 60, 100);
  fluid_synth_system_reset(rig.synth);
  CHECK(g0->nkeys == 0 && g0->nvoice == 0, "reset clears ch0");
  CHECK(g1->nkeys == 0 && g1->nvoice == 0, "reset clears ch1");
  CHECK(rig.synth->channel[0]->legato->enabled
        && rig.synth->channel[1]->legato->enabled, "reset keeps mode flag");
  fluid_synth_noteon(rig.synth, 0, 60, 100);
  CHECK(g0->nvoice == 1 && g0->nkeys == 1, "new attack after reset");
  fluid_synth_write_float(rig.synth, 128, left, 0, 1, right, 0, 1);

  /* Toggling is allowed once nothing is held; boundary values accepted. */
  fluid_synth_noteoff(rig.synth, 0, 60);
  fluid_synth_write_float(rig.synth, 128, left, 0, 1, right, 0, 1);
  CHECK(fluid_synth_set_legato(rig.synth, 0, 0, 0.0) == 0, "disable when idle");
  CHECK(fluid_synth_set_legato(rig.synth, 0, 1, 10.0) == 0,
        "10s boundary accepted");
  CHECK(fluid_synth_set_legato(rig.synth, 0, 1, 0.0) == 0,
        "0s boundary accepted");

  rig_done(&rig);
  return failures == 0;
}

/* ------------------------------------------------------------------ */

int main(void)
{
  struct {
    const char *name;
    int (*fn)(void);
  } cases[7] = {
    {"multi_zone_attack", case_multi_zone_attack},
    {"key_fallback_repeat", case_key_fallback_repeat},
    {"glide_retarget_trajectory", case_glide_retarget_trajectory},
    {"bend_center_and_tuning", case_bend_and_tuning},
    {"interp_loop_chunking", case_interp_loop_chunking},
    {"sustain_pedal_isolation", case_sustain_pedal_isolation},
    {"channels_reset_validation", case_channels_reset_validation},
  };
  int i, passed = 0;

  for (i = 0; i < 7; i++) {
    int before = failures;
    printf("case %d/7 %s\n", i + 1, cases[i].name);
    cases[i].fn();
    if (failures == before) {
      printf("  ok\n");
      passed++;
    } else {
      printf("  FAILED\n");
    }
  }

  printf("checks=%d failures=%d\n", checks, failures);
  if (passed == 7) {
    printf("FLUIDLITE_LEGATO_OK cases=7\n");
    return 0;
  }
  printf("FLUIDLITE_LEGATO_FAIL passed=%d/7\n", passed);
  return 1;
}
