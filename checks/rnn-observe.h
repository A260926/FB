/*
 * Test-only observation ABI shared by src/denoise.c (compiled with
 * -DRNNOISE_TEST_OBSERVE) and checks/target.cpp.  This header is never
 * installed and never referenced by a normal build.
 */
#ifndef RNNOISE_OBSERVE_H
#define RNNOISE_OBSERVE_H 1

#define RNNOISE_TEST_OBS_FEATURES 42
#define RNNOISE_TEST_OBS_STATE_MAX 128

typedef struct {
  const void *instance;            /* DenoiseState that produced the record */
  int frame;                       /* call index on that instance */
  int executed;                    /* 1 if compute_rnn actually ran */
  float features[RNNOISE_TEST_OBS_FEATURES];
  float vad_state_before[RNNOISE_TEST_OBS_STATE_MAX];
  float noise_state_before[RNNOISE_TEST_OBS_STATE_MAX];
  float denoise_state_before[RNNOISE_TEST_OBS_STATE_MAX];
  float vad_state_after[RNNOISE_TEST_OBS_STATE_MAX];
  float noise_state_after[RNNOISE_TEST_OBS_STATE_MAX];
  float denoise_state_after[RNNOISE_TEST_OBS_STATE_MAX];
  float vad_out;
  float gains_out[22];
  int in_size, noise_size, denoise_size;   /* valid element counts */
} RNNOISE_TEST_OBS;

void rnnoise_test_observe(const RNNOISE_TEST_OBS *rec);

/* Per-instance frame counter, kept outside DenoiseState so a normal build's
 * object layout and ABI are untouched.  Implemented by the test binary. */
int rnnoise_test_next_frame(const void *instance);

/* Called by src/denoise.c when the object goes away; also keyed outside the
 * object so rnnoise_destroy needs no test hook. */
void rnnoise_test_forget_instance(const void *instance);

#endif /* RNNOISE_OBSERVE_H */
