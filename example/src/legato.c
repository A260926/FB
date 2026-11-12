/*
 * Legato example for FluidLite.
 *
 * Builds a small RAM SoundFont with one looping sine sample, enables
 * monophonic legato on channel 0 with a 0.2 s glide and plays an
 * overlapping note sequence: the second and third note-ons reuse the
 * voices of the first attack, only the pitch glides.  The rendered
 * stereo float PCM is written to the given file (or standard output).
 *
 * Build (from the repository root):
 *   cc -std=c99 -Iinclude example/src/legato.c build/libfluidlite.a -lm \
 *      -o legato_example
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "fluidlite.h"

#define SAMPLE_RATE 44100
#define SAMPLE_LEN  256
#define GLIDE_SEC   0.2

static void render(fluid_synth_t *synth, int frames, float *buf)
{
  /* One call writes interleaved stereo: offsets 0/1, increments 2. */
  fluid_synth_write_float(synth, frames, buf, 0, 2, buf, 1, 2);
}

int main(int argc, char *argv[])
{
  fluid_settings_t *settings;
  fluid_synth_t *synth;
  fluid_sfont_t *sfont;
  fluid_sample_t *sample;
  short data[SAMPLE_LEN];
  float *buffer;
  FILE *out;
  int i, sfont_id;
  const int frames = SAMPLE_RATE / 2;       /* 0.5 s per segment */

  out = (argc > 1) ? fopen(argv[1], "wb") : stdout;
  if (out == NULL) {
    fprintf(stderr, "cannot open output %s\n", argv[1]);
    return 1;
  }

  buffer = (float *) calloc((size_t) frames * 2, sizeof(float));
  if (buffer == NULL) {
    return 1;
  }

  for (i = 0; i < SAMPLE_LEN; i++) {
    data[i] = (short) (12000.0 * sin(2.0 * M_PI * i / SAMPLE_LEN));
  }

  settings = new_fluid_settings();
  fluid_settings_setnum(settings, "synth.sample-rate", SAMPLE_RATE);
  synth = new_fluid_synth(settings);
  fluid_synth_set_reverb_on(synth, 0);
  fluid_synth_set_chorus_on(synth, 0);

  /* RAM SoundFont: one looping sine covering the whole key range. */
  sfont = fluid_ramsfont_create_sfont();
  sample = new_fluid_ramsample();
  fluid_sample_set_sound_data(sample, data, SAMPLE_LEN, 1, 60);
  fluid_ramsfont_add_izone(sfont->data, 0, 0, sample, 0, 127);
  fluid_ramsfont_izone_set_loop(sfont->data, 0, 0, sample, 1, 0, 0);
  sfont_id = fluid_synth_add_sfont(synth, sfont);
  fluid_synth_program_select(synth, 0, sfont_id, 0, 0);

  /* Enable monophonic legato with a 0.2 s cents-linear glide. */
  if (fluid_synth_set_legato(synth, 0, 1, GLIDE_SEC) != 0) {
    fprintf(stderr, "failed to enable legato\n");
    return 1;
  }

  /* First note: normal attack, creates the legato group. */
  fluid_synth_noteon(synth, 0, 60, 110);
  render(synth, frames, buffer);
  fwrite(buffer, sizeof(float), (size_t) frames * 2, out);

  /* Overlapping note-ons: no new attack, the pitch glides 60 -> 67. */
  fluid_synth_noteon(synth, 0, 67, 90);
  render(synth, frames, buffer);
  fwrite(buffer, sizeof(float), (size_t) frames * 2, out);

  /* 67 -> 72, still the same voices, envelopes never restarted. */
  fluid_synth_noteon(synth, 0, 72, 80);
  render(synth, frames, buffer);
  fwrite(buffer, sizeof(float), (size_t) frames * 2, out);

  /* Releasing the last physical key runs the ordinary note-off. */
  fluid_synth_noteoff(synth, 0, 72);
  fluid_synth_noteoff(synth, 0, 67);
  fluid_synth_noteoff(synth, 0, 60);
  render(synth, frames, buffer);
  fwrite(buffer, sizeof(float), (size_t) frames * 2, out);

  if (out != stdout) {
    fclose(out);
  }
  free(buffer);
  delete_fluid_synth(synth);
  delete_fluid_settings(settings);
  return 0;
}
