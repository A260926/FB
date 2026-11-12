/*
 * RNN regression suite for rnnoise.
 *
 * Groups
 *   model      - a C++-generated, legal version-1 text model is loaded
 *                through the real rnnoise_model_from_file; widths, activation
 *                codes and every weight are checked against the generator.
 *   recurrence - controlled feature sequences compared frame by frame with an
 *                independent double-precision reference, a hand-computed
 *                two-neuron anchor, two-instance interleaving, and the two
 *                deliberately broken private copies of rnn.c built by
 *                checks/target.sh and run by checks/mutants.cpp.
 *   stream     - real rnnoise_create / rnnoise_process_frame calls on
 *                deterministic 48 kHz PCM.  The test-only, read-only hook in
 *                src/denoise.c records the actual 42-dim feature vector and
 *                the hidden states before/after the network; the reference is
 *                advanced only on frames where compute_rnn really ran.
 *
 * Production code paths are not modified for this suite.  The only source
 * change is the observation hook in src/denoise.c, guarded by
 * RNNOISE_TEST_OBSERVE, which is not defined in normal builds.
 */
#include "suite.h"
#include "rnn-ref.h"
#include "rnn-observe.h"
#include "rnnoise.h"

extern "C" {
#include "rnn.h"
#include "rnn_data.h"
}

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

const double kTol = 5e-4;
const double kLinearBound = 2.0;

const char *kLayerNames[6] = {"input_dense", "vad_gru", "vad_output",
                              "noise_gru", "denoise_gru", "denoise_output"};

std::string temp_path(const char *tag) {
  const char *dir = getenv("TMPDIR");
  std::string base = dir && *dir ? dir : ".";
  char buf[512];
  snprintf(buf, sizeof(buf), "%s/rnnoise-%s-XXXXXX", base.c_str(), tag);
  int fd = mkstemp(buf);
  if (fd < 0) throw std::runtime_error("mkstemp failed");
  close(fd);
  return std::string(buf);
}

/* --------------------------------------------------------- error report */

struct Worst {
  double diff = 0.0;
  int layer = -1, frame = -1, neuron = -1;
  std::string what;
  void note(double d, int l, int f, int n, const char *w) {
    if (d > diff) { diff = d; layer = l; frame = f; neuron = n; what = w; }
  }
  void report(const char *tag) const {
    if (layer < 0) return;
    std::printf("WORST %s layer=%s frame=%d neuron=%d diff=%.9g\n", tag,
                kLayerNames[layer], frame, neuron, diff);
  }
};

void require_close(double got, double want, Worst &w, int layer, int frame,
                   int neuron, const char *what) {
  double d = std::fabs(got - want);
  w.note(d, layer, frame, neuron, what);
  UNIT_CHECK(d <= kTol);
  if (d > kTol) {
    throw std::runtime_error(
        std::string("first deviation in ") + what +
        " layer=" + kLayerNames[layer] + " frame=" + std::to_string(frame) +
        " neuron=" + std::to_string(neuron) + " got=" + std::to_string(got) +
        " want=" + std::to_string(want) + " diff=" + std::to_string(d));
  }
}

void require_layout(int got, int want, const char *what) {
  UNIT_CHECK(got == want);
  if (got != want)
    throw std::runtime_error(std::string("layout mismatch in ") + what +
                             " got=" + std::to_string(got) +
                             " want=" + std::to_string(want));
}

/* ----------------------------------------------- model file / reader */

struct LoadedModel {
  std::string path;
  RNNModel *model = nullptr;
  rnref::Model ref;
  explicit LoadedModel(const rnref::Model &m) : ref(m) {
    path = temp_path("model");
    m.write_file(path);
    FILE *f = fopen(path.c_str(), "r");
    if (!f) throw std::runtime_error("cannot reopen generated model");
    model = rnnoise_model_from_file(f);
    fclose(f);
    if (!model)
      throw std::runtime_error("rnnoise_model_from_file rejected a legal v1 model");
  }
  ~LoadedModel() {
    if (model) rnnoise_model_free(model);
    if (!path.empty()) remove(path.c_str());
  }
};

/* Test-owned RNNState over the loaded model.  A private copy of the state
 * buffers keeps the controlled comparison independent of DenoiseState. */
struct TestRnn {
  RNNState rnn;
  std::vector<float> vad, noise, denoise;
  TestRnn(const RNNModel *model, const std::vector<double> &initial) {
    rnn.model = model;
    vad.assign(model->vad_gru_size, 0.f);
    noise.assign(model->noise_gru_size, 0.f);
    denoise.assign(model->denoise_gru_size, 0.f);
    int off = 0;
    for (size_t i = 0; i < vad.size(); i++) vad[i] = (float)initial[off++];
    for (size_t i = 0; i < noise.size(); i++) noise[i] = (float)initial[off++];
    for (size_t i = 0; i < denoise.size(); i++) denoise[i] = (float)initial[off++];
    rnn.vad_gru_state = vad.data();
    rnn.noise_gru_state = noise.data();
    rnn.denoise_gru_state = denoise.data();
  }
};

/* One frame of the production compute_rnn compared against the reference.
 * Reports the three hidden states, the VAD and all 22 gains. */
void compare_one(const rnref::RefOutputs &ref, const TestRnn &t, float vad,
                 const float *gains, int frame, Worst &w) {
  const RNNModel *m = t.rnn.model;
  for (int i = 0; i < m->vad_gru_size; i++)
    require_close(t.vad[i], ref.vad[i], w, 1, frame, i, "vad_gru state");
  require_close(vad, ref.vad_out, w, 2, frame, 0, "vad output");
  for (int i = 0; i < m->noise_gru_size; i++)
    require_close(t.noise[i], ref.noise[i], w, 3, frame, i, "noise_gru state");
  for (int i = 0; i < m->denoise_gru_size; i++)
    require_close(t.denoise[i], ref.denoise[i], w, 4, frame, i, "denoise_gru state");
  for (int i = 0; i < 22; i++)
    require_close(gains[i], ref.gains[i], w, 5, frame, i, "denoise_output gain");
}

/* Feature vectors for the controlled runs: bounded by 1 and alternating in
 * sign so every neuron sees both polarities. */
std::vector<double> controlled_features(int frame, int variant) {
  std::vector<double> f(42);
  for (int i = 0; i < 42; i++) {
    double v;
    if (variant == 1) {
      v = ((i & 1) ? 1.0 : -1.0) * (0.35 + 0.05 * (i % 5));
    } else {
      v = std::sin((frame + 1) * (i + 3) * 0.11) * (0.4 + 0.2 * (((frame + i) % 3) - 1));
    }
    f[i] = v;
  }
  return f;
}

std::vector<double> nonzero_initial(const rnref::Model &m) {
  std::vector<double> s;
  for (int i = 0; i < m.vad_gru.nb_neurons; i++) s.push_back(i == 0 ? 0.5 : -0.3);
  for (int i = 0; i < m.noise_gru.nb_neurons; i++) s.push_back(i == 0 ? 0.25 : (i == 1 ? -0.5 : 0.1));
  for (int i = 0; i < m.denoise_gru.nb_neurons; i++) s.push_back(i == 0 ? -0.4 : 0.2 + 0.05 * i);
  return s;
}

}  // namespace

/* ======================================================== model group */

UNIT_TEST(model, model_loads_and_layout) {
  rnref::Model gen = rnref::build_model(0x5eed1234u, 2, 4, 4, 4);
  gen.validate();
  gen.check_structure();
  LoadedModel lm(gen);

  const RNNModel *m = lm.model;
  require_layout(m->input_dense_size, 3, "input_dense_size");
  require_layout(m->vad_gru_size, 2, "vad_gru_size");
  require_layout(m->noise_gru_size, 4, "noise_gru_size");
  require_layout(m->denoise_gru_size, 5, "denoise_gru_size");
  require_layout(m->vad_output_size, 1, "vad_output_size");
  require_layout(m->denoise_output_size, 22, "denoise_output_size");

  require_layout(m->input_dense->nb_inputs, 42, "input_dense.nb_inputs");
  require_layout(m->input_dense->nb_neurons, 3, "input_dense.nb_neurons");
  require_layout(m->input_dense->activation, rnref::ACT_TANH, "input_dense.activation");
  require_layout(m->vad_gru->nb_inputs, 3, "vad_gru.nb_inputs");
  require_layout(m->vad_gru->nb_neurons, 2, "vad_gru.nb_neurons");
  require_layout(m->noise_gru->nb_inputs, 47, "noise_gru.nb_inputs");
  require_layout(m->denoise_gru->nb_inputs, 48, "denoise_gru.nb_inputs");
  require_layout(m->denoise_output->nb_neurons, 22, "denoise_output.nb_neurons");
  require_layout(m->denoise_output->activation, rnref::ACT_SIGMOID,
                 "denoise_output.activation");
  require_layout(m->vad_output->nb_neurons, 1, "vad_output.nb_neurons");

  /* Byte-for-byte weight order proves the reader copied the matrices in the
   * order the generator wrote, including the GRU stride blocks. */
  for (int i = 0; i < 42 * 3; i++)
    require_layout(m->input_dense->input_weights[i], gen.input_dense.input_weights[i],
                   "input_dense.input_weights order");
  for (int i = 0; i < 3; i++)
    require_layout(m->input_dense->bias[i], gen.input_dense.bias[i],
                   "input_dense.bias order");
  for (int i = 0; i < 47 * 4 * 3; i++)
    require_layout(m->noise_gru->input_weights[i], gen.noise_gru.input_weights[i],
                   "noise_gru.input_weights order");
  for (int i = 0; i < 4 * 4 * 3; i++)
    require_layout(m->noise_gru->recurrent_weights[i],
                   gen.noise_gru.recurrent_weights[i],
                   "noise_gru.recurrent_weights order");
  for (int i = 0; i < 4 * 3; i++)
    require_layout(m->noise_gru->bias[i], gen.noise_gru.bias[i],
                   "noise_gru.bias order");
  for (int i = 0; i < 5 * 5 * 3; i++)
    require_layout(m->denoise_gru->recurrent_weights[i],
                   gen.denoise_gru.recurrent_weights[i],
                   "denoise_gru.recurrent_weights order");
  for (int i = 0; i < 2 * 1; i++)
    require_layout(m->vad_output->input_weights[i], gen.vad_output.input_weights[i],
                   "vad_output.input_weights order");

  /* The generator really did produce a non-trivial model: signs both ways,
   * no zero weights, asymmetric recurrent blocks, distinct widths. */
  gen.check_structure();
  UNIT_CHECK(gen.input_dense.nb_neurons != gen.vad_gru.nb_neurons);
  UNIT_CHECK(gen.vad_gru.nb_neurons != gen.noise_gru.nb_neurons);
  UNIT_CHECK(gen.noise_gru.nb_neurons != gen.denoise_gru.nb_neurons);

  /* A wrong version must be rejected, proving the header was parsed. */
  {
    std::string bad = temp_path("badver");
    FILE *f = fopen(bad.c_str(), "w");
    fprintf(f, "rnnoise-nu model file version 2\n");
    fclose(f);
    f = fopen(bad.c_str(), "r");
    RNNModel *rejected = rnnoise_model_from_file(f);
    fclose(f);
    remove(bad.c_str());
    UNIT_CHECK(rejected == nullptr);
    if (rejected) {
      rnnoise_model_free(rejected);
      throw std::runtime_error("loader accepted version 2");
    }
  }
}

UNIT_TEST(model, second_seed_keeps_layout) {
  /* A different seed must load with the same shapes but different weights,
   * so the suite is not accidentally depending on one lucky matrix. */
  rnref::Model a = rnref::build_model(0x00000001u, 2, 4, 4, 4);
  rnref::Model b = rnref::build_model(0x00000002u, 2, 4, 4, 4);
  LoadedModel la(a), lb(b);
  int different = 0;
  for (int i = 0; i < 42 * 3; i++)
    if (la.model->input_dense->input_weights[i] != lb.model->input_dense->input_weights[i])
      different++;
  UNIT_CHECK(different > 42 * 3 / 2);
  require_layout(lb.model->input_dense_size, 3, "second seed input_dense_size");
  require_layout(lb.model->denoise_output_size, 22, "second seed denoise_output_size");
}

/* =================================================== recurrence group */

UNIT_TEST(recurrence, reference_hand_anchor_two_neurons) {
  /* Hand-computed anchor, integer arithmetic scaled by 1/256.
   *
   * Dense (tanh), input_weights[j*N+i]:
   *   j0 -> (4,-6), j1 -> (9,5), bias (7,-13), x = (2,-3)
   *   sum0 = 7 + 4*2 + 9*(-3)      = -12  -> tanh(-12/256)
   *   sum1 = -13 + (-6)*2 + 5*(-3) = -40  -> tanh(-40/256)
   *
   * GRU (2 neurons, stride 6, tanh candidate), old state (0.25,-0.5),
   * x = (2,-3); the candidate terms below are written out by hand.
   */
  rnref::Dense d;
  d.nb_inputs = 2; d.nb_neurons = 2; d.activation = rnref::ACT_TANH;
  int Wd[4] = {4, -6, 9, 5};
  int Bd[2] = {7, -13};
  d.input_weights.assign(Wd, Wd + 4);
  d.bias.assign(Bd, Bd + 2);
  std::vector<double> x(2);
  x[0] = 2.0; x[1] = -3.0;
  std::vector<double> out = rnref::dense(d, x);
  UNIT_CHECK(std::fabs(out[0] - std::tanh(-12.0 / 256.0)) < 1e-12);
  UNIT_CHECK(std::fabs(out[1] - std::tanh(-40.0 / 256.0)) < 1e-12);

  rnref::Gru g;
  g.nb_inputs = 2; g.nb_neurons = 2; g.activation = rnref::ACT_TANH;
  int Wi[12] = {12, -5, 7, 9, -11, 4,
                 -3, 8, -6, 2, 10, -9};
  int Wr[12] = {5, -7, 3, -4, 8, -2,
                 -6, 9, 1, -8, -3, 11};
  int Bg[6] = {10, -20, 5, -10, 30, -15};
  g.input_weights.assign(Wi, Wi + 12);
  g.recurrent_weights.assign(Wr, Wr + 12);
  g.bias.assign(Bg, Bg + 6);
  std::vector<double> s(2);
  s[0] = 0.25; s[1] = -0.5;

  double o0 = s[0], o1 = s[1];
  double z0 = 1.0 / (1.0 + std::exp(-(10.0 + 12 * 2 + (-3) * -3 + 5 * o0 + (-6) * o1) / 256.0));
  double z1 = 1.0 / (1.0 + std::exp(-(-20.0 + (-5) * 2 + 8 * -3 + (-7) * o0 + 9 * o1) / 256.0));
  double r0 = 1.0 / (1.0 + std::exp(-(5.0 + 7 * 2 + (-6) * -3 + 3 * o0 + 1 * o1) / 256.0));
  double r1 = 1.0 / (1.0 + std::exp(-(-10.0 + 9 * 2 + 2 * -3 + (-4) * o0 + (-8) * o1) / 256.0));
  double c0 = std::tanh((30.0 + (-11) * 2 + 10 * -3 + 8 * (o0 * r0) + (-3) * (o1 * r1)) / 256.0);
  double c1 = std::tanh((-15.0 + 4 * 2 + (-9) * -3 + (-2) * (o0 * r0) + 11 * (o1 * r1)) / 256.0);
  double h0 = z0 * o0 + (1.0 - z0) * c0;
  double h1 = z1 * o1 + (1.0 - z1) * c1;

  std::vector<double> h = rnref::gru(g, s, x);
  UNIT_CHECK(std::fabs(h[0] - h0) < 1e-12);
  UNIT_CHECK(std::fabs(h[1] - h1) < 1e-12);
  UNIT_CHECK(std::fabs(s[0] - h0) < 1e-12);   /* whole layer committed */
  UNIT_CHECK(std::fabs(s[1] - h1) < 1e-12);

  /* Near-miss variants must be far from the anchor, otherwise the anchor
   * could not distinguish them. */
  double z0_after_commit =
      1.0 / (1.0 + std::exp(-(10.0 + 12 * 2 + (-3) * -3 + 5 * h0 + (-6) * h1) / 256.0));
  UNIT_CHECK(std::fabs(z0 - z0_after_commit) > 1e-5);
  double c0_r_i = std::tanh((30.0 - 11 * 2 + 10 * -3 + 8 * (o0 * r0) + (-3) * (o1 * r0)) / 256.0);
  UNIT_CHECK(std::fabs(c0 - c0_r_i) > 1e-5);
}

UNIT_TEST(recurrence, controlled_sequence_matches_reference) {
  rnref::Model gen = rnref::build_mild_model();
  gen.validate();
  LoadedModel lm(gen);
  std::vector<double> init = nonzero_initial(gen);

  std::vector<std::vector<double> > feats;
  for (int f = 0; f < 7; f++) feats.push_back(controlled_features(f, (f & 1) ? 1 : 0));

  TestRnn t(lm.model, init);
  rnref::RefState rs;
  rnref::ref_init(rs, gen, &init);
  Worst w;
  float gains[22] = {0};
  float vad = 0;

  for (size_t f = 0; f < feats.size(); f++) {
    std::vector<float> in(42);
    for (int i = 0; i < 42; i++) in[i] = (float)feats[f][i];
    rnref::RefOutputs ref = rnref::ref_frame(gen, rs, feats[f]);
    compute_rnn(&t.rnn, gains, &vad, in.data());
    compare_one(ref, t, vad, gains, (int)f, w);
    /* The mild model must keep every layer's linear sum under 2 in
     * absolute value after the 1/256 scaling. */
    UNIT_CHECK(ref.linear_max <= kLinearBound);
  }
  w.report("controlled");
}

UNIT_TEST(recurrence, two_instances_do_not_share_history) {
  /* Two independent RNNStates with different nonzero initial states and
   * different feature streams, advanced alternately. */
  rnref::Model gen = rnref::build_mild_model(0x1234abcdu);
  LoadedModel lm(gen);
  std::vector<double> init_a = nonzero_initial(gen);
  std::vector<double> init_b = nonzero_initial(gen);
  for (size_t i = 0; i < init_b.size(); i++) init_b[i] = -0.6 * init_b[i];

  TestRnn ta(lm.model, init_a), tb(lm.model, init_b);
  rnref::RefState ra, rb;
  rnref::ref_init(ra, gen, &init_a);
  rnref::ref_init(rb, gen, &init_b);
  Worst wa, wb;
  float ga[22] = {0}, gb[22] = {0}, va = 0, vb = 0;

  for (int f = 0; f < 6; f++) {
    for (int inst = 0; inst < 2; inst++) {
      std::vector<std::vector<double> > feats;
      std::vector<double> fa = controlled_features(f, inst == 0 ? 0 : 1);
      std::vector<double> fb = controlled_features(f + 3, inst == 0 ? 1 : 0);
      std::vector<float> ina(42), inb(42);
      for (int i = 0; i < 42; i++) { ina[i] = (float)fa[i]; inb[i] = (float)fb[i]; }
      rnref::RefOutputs ra_out = rnref::ref_frame(gen, ra, fa);
      rnref::RefOutputs rb_out = rnref::ref_frame(gen, rb, fb);
      compute_rnn(&ta.rnn, ga, &va, ina.data());
      compute_rnn(&tb.rnn, gb, &vb, inb.data());
      compare_one(ra_out, ta, va, ga, f, wa);
      compare_one(rb_out, tb, vb, gb, f, wb);
    }
  }
  wa.report("instance_a");
  wb.report("instance_b");
  /* Different histories and inputs must leave different final states. */
  double sep = 0.0;
  for (size_t i = 0; i < ta.vad.size(); i++) sep += std::fabs(ta.vad[i] - tb.vad[i]);
  UNIT_CHECK(sep > 1e-3);
}

/* ======================================================== stream group */

namespace {

struct Recorder {
  std::vector<RNNOISE_TEST_OBS> frames;
  bool record = true;
  void push(const RNNOISE_TEST_OBS *r) {
    if (record) frames.push_back(*r);
  }
};

Recorder *g_recorder = nullptr;

}  // namespace

extern "C" void rnnoise_test_observe(const RNNOISE_TEST_OBS *rec) {
  if (g_recorder && g_recorder->record) g_recorder->frames.push_back(*rec);
}

namespace {

struct RecorderGuard {
  Recorder r;
  RecorderGuard() { g_recorder = &r; }
  ~RecorderGuard() { g_recorder = nullptr; }
};

/* Deterministic 48 kHz material: two-tone, noise, silence and re-attack. */
struct PcmScript {
  enum Kind { TONE, NOISE, SILENCE } kind;
  int frames;
};

std::vector<float> make_frame(const PcmScript &s, int frame_index, uint32_t &lcg) {
  std::vector<float> x(480);
  for (int i = 0; i < 480; i++) {
    double t = (double)(frame_index * 480 + i) / 48000.0;
    double v = 0.0;
    switch (s.kind) {
      case PcmScript::TONE:
        v = 0.35 * std::sin(2 * M_PI * 440.0 * t) +
            0.22 * std::sin(2 * M_PI * 1370.0 * t);
        break;
      case PcmScript::NOISE:
        lcg = 1664525u * lcg + 1013904223u;
        v = ((double)((lcg >> 9) & 0x7fffff) / 4194304.0 - 1.0) * 0.30;
        break;
      default:
        v = 0.0;
        break;
    }
    x[i] = (float)v;
  }
  return x;
}

void run_stream(const rnref::Model &gen, RNNModel *model, Recorder &rec,
                const std::vector<float> &pcm, Worst &w, double *energy) {
  DenoiseState *st = rnnoise_create(model);
  if (!st) throw std::runtime_error("rnnoise_create failed");
  rnref::RefState rs;
  rnref::ref_init(rs, gen);
  std::vector<float> out(480);
  double e = 0.0;
  size_t frame_index = 0;
  const size_t nframes = pcm.size() / 480;
  for (size_t f = 0; f < nframes; f++) {
    const float *in = &pcm[f * 480];
    float vad = rnnoise_process_frame(st, out.data(), in);
    UNIT_CHECK(std::isfinite(vad));
    for (int i = 0; i < 480; i++) {
      UNIT_CHECK(std::isfinite(out[i]));
      e += (double)out[i] * out[i];
    }
    (void)vad;
    (void)frame_index;
  }
  *energy = e;
  rnnoise_destroy(st);
  if (rec.frames.size() != nframes)
    throw std::runtime_error("observation hook did not record every frame");
}
}  // namespace
