/*
 * Test model generator + independent reference implementation for the
 * rnnoise RNN regression.
 *
 * The reference deliberately does not include src/rnn.c, does not call
 * compute_dense/compute_gru and does not use tansig_table.h.  It interprets
 * the on-disk matrix layout itself:
 *
 *   Dense:  input_weights[j*nb_neurons + i]   (j = input, i = neuron)
 *   GRU:    input_weights[j*3N + k*N + i]     (j = input, k = gate block,
 *                                             i = neuron; k=0 z, 1 r, 2 cand)
 *
 * Every layer's linear sum is scaled by 1/256 after the accumulation.
 *
 * GRU semantics implemented here:
 *   - z and r both read the same snapshot of the old state;
 *   - the candidate's reset gate multiplies the *source* neuron's old state
 *     (state[j]*r[j]), not r[i];
 *   - the whole candidate vector is computed before anything is committed,
 *     so every neuron of layer L reads the old state for all j.
 */
#ifndef RNNOISE_TEST_REF_H
#define RNNOISE_TEST_REF_H 1

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace rnref {

/* Activation codes are the file-format codes (0 tanh, 1 sigmoid, 2 relu),
 * interpreted here independently of src/rnn.h. */
enum { ACT_TANH = 0, ACT_SIGMOID = 1, ACT_RELU = 2 };

/* ---------------------------------------------------------------- model */

struct Dense {
  int nb_inputs = 0, nb_neurons = 0, activation = ACT_TANH;
  std::vector<int> input_weights, bias;
};

struct Gru {
  int nb_inputs = 0, nb_neurons = 0, activation = ACT_TANH;
  std::vector<int> input_weights, recurrent_weights, bias;
};

struct Model {
  Dense input_dense, denoise_output, vad_output;
  Gru vad_gru, noise_gru, denoise_gru;

  void validate() const {
    if (input_dense.nb_inputs != 42)
      throw std::runtime_error("input_dense must take the 42-dim feature vector");
    if (denoise_output.nb_neurons != 22)
      throw std::runtime_error("denoise_output must produce 22 gains");
    if (vad_output.nb_neurons != 1)
      throw std::runtime_error("vad_output must produce one VAD value");
    const int *dense_w = &input_dense.nb_neurons;
    (void)dense_w;
    if (input_dense.nb_neurons < 2 || input_dense.nb_neurons > 5)
      throw std::runtime_error("input_dense width must be 2..5");
    if (vad_gru.nb_neurons < 2 || vad_gru.nb_neurons > 5 ||
        noise_gru.nb_neurons < 2 || noise_gru.nb_neurons > 5 ||
        denoise_gru.nb_neurons < 2 || denoise_gru.nb_neurons > 5)
      throw std::runtime_error("GRU width must be 2..5");
    if (vad_output.nb_inputs != vad_gru.nb_neurons)
      throw std::runtime_error("vad_output input width must match vad_gru");
    if (noise_gru.nb_inputs != input_dense.nb_neurons + vad_gru.nb_neurons + 42)
      throw std::runtime_error("noise_gru input width mismatch");
    if (denoise_gru.nb_inputs != vad_gru.nb_neurons + noise_gru.nb_neurons + 42)
      throw std::runtime_error("denoise_gru input width mismatch");
    if (denoise_output.nb_inputs != denoise_gru.nb_neurons)
      throw std::runtime_error("denoise_output input width mismatch");
    /* Pairwise-distinct widths make a shifted/transposed layout visible. */
    int widths[6] = {input_dense.nb_neurons, vad_gru.nb_neurons,
                     noise_gru.nb_neurons, denoise_gru.nb_neurons,
                     denoise_output.nb_neurons, vad_output.nb_neurons};
    for (int a = 0; a < 6; a++)
      for (int b = a + 1; b < 6; b++)
        if (widths[a] == widths[b])
          throw std::runtime_error("layer widths must be distinct");
  }

  void check_structure() const {
    auto signs = [](const std::vector<int> &v, const char *what) {
      bool pos = false, neg = false;
      int zeros = 0;
      for (size_t i = 0; i < v.size(); i++) {
        if (v[i] > 0) pos = true;
        else if (v[i] < 0) neg = true;
        else zeros++;
      }
      if (!pos || !neg)
        throw std::runtime_error(std::string(what) + " lacks + and - values");
      if (zeros) throw std::runtime_error(std::string(what) + " has zero entries");
    };
    auto dense = [&](const Dense &d, const char *n) {
      signs(d.input_weights, n);
      signs(d.bias, n);
    };
    auto gru = [&](const Gru &g, const char *n) {
      signs(g.input_weights, n);
      signs(g.recurrent_weights, n);
      signs(g.bias, n);
      if (g.nb_neurons < 2) return;
      const int N = g.nb_neurons, stride = 3 * N;
      /* Square recurrent matrices must not be symmetric. */
      bool asym = false;
      for (int i = 0; i < N && !asym; i++)
        for (int j = 0; j < N && !asym; j++)
          if (g.recurrent_weights[j * stride + i] != g.recurrent_weights[i * stride + j])
            asym = true;
      if (!asym) throw std::runtime_error(std::string(n) + " recurrent matrix is symmetric");
    };
    dense(input_dense, "input_dense");
    dense(denoise_output, "denoise_output");
    dense(vad_output, "vad_output");
    gru(vad_gru, "vad_gru");
    gru(noise_gru, "noise_gru");
    gru(denoise_gru, "denoise_gru");
  }

  /* ------------------------------------------------------- file format */

  static void put(FILE *f, int v) { fprintf(f, "%d ", v); }
  static void arr(FILE *f, const std::vector<int> &v) {
    for (size_t i = 0; i < v.size(); i++) {
      put(f, v[i]);
      if (i % 16 == 15) fprintf(f, "\n");
    }
    fprintf(f, "\n");
  }
  static void layer(FILE *f, const Dense &d) {
    put(f, d.nb_inputs); put(f, d.nb_neurons); put(f, d.activation); fprintf(f, "\n");
    arr(f, d.input_weights); arr(f, d.bias);
  }
  static void layer(FILE *f, const Gru &g) {
    put(f, g.nb_inputs); put(f, g.nb_neurons); put(f, g.activation); fprintf(f, "\n");
    arr(f, g.input_weights); arr(f, g.recurrent_weights); arr(f, g.bias);
  }

  void write(FILE *f, int version = 1) const {
    fprintf(f, "rnnoise-nu model file version %d\n", version);
    layer(f, input_dense);
    layer(f, vad_gru);
    layer(f, noise_gru);
    layer(f, denoise_gru);
    layer(f, denoise_output);
    layer(f, vad_output);
  }

  void write_file(const std::string &path) const {
    FILE *f = fopen(path.c_str(), "w");
    if (!f) throw std::runtime_error("cannot write model " + path);
    write(f);
    fclose(f);
  }
};

/* -------------------------------------------------------------- builder */

struct Rng {
  uint32_t s;
  explicit Rng(uint32_t seed) : s(seed) {}
  uint32_t next() { s = 1664525u * s + 1013904223u; return s; }
  int range(int lo, int hi) { return lo + (int)((next() >> 8) % (uint32_t)(hi - lo + 1)); }
};

/* Magnitudes in [1,max], signs forced so both + and - occur and no entry
 * is zero.  Entry 0 is +, entry 1 -, entry 2 -, entry 3 +. */
inline std::vector<int> filled(Rng &rng, int n, int maxmag) {
  std::vector<int> v(n);
  for (int i = 0; i < n; i++) {
    int mag = rng.range(1, maxmag);
    bool pos = (rng.next() >> 7) & 1;
    if (i == 0) pos = true;
    if (i == 1 || i == 2) pos = false;
    if (i == 3) pos = true;
    v[i] = pos ? mag : -mag;
  }
  return v;
}

inline Model build_model(uint32_t seed, int dense_w, int gru_in_w,
                         int gru_rec_w, int out_w) {
  Rng rng(seed);
  Model m;
  m.input_dense.nb_inputs = 42; m.input_dense.nb_neurons = 3;
  m.input_dense.input_weights = filled(rng, 42 * 3, dense_w);
  m.input_dense.bias = filled(rng, 3, 6);

  m.vad_gru.nb_inputs = 3; m.vad_gru.nb_neurons = 2;
  m.vad_gru.input_weights = filled(rng, 3 * 2 * 3, gru_in_w);
  m.vad_gru.recurrent_weights = filled(rng, 2 * 2 * 3, gru_rec_w);
  m.vad_gru.bias = filled(rng, 2 * 3, 4);

  m.noise_gru.nb_inputs = 3 + 2 + 42; m.noise_gru.nb_neurons = 4;
  m.noise_gru.input_weights = filled(rng, 47 * 4 * 3, gru_in_w);
  m.noise_gru.recurrent_weights = filled(rng, 4 * 4 * 3, gru_rec_w);
  m.noise_gru.bias = filled(rng, 4 * 3, 4);

  m.denoise_gru.nb_inputs = 2 + 4 + 42; m.denoise_gru.nb_neurons = 5;
  m.denoise_gru.input_weights = filled(rng, 48 * 5 * 3, gru_in_w);
  m.denoise_gru.recurrent_weights = filled(rng, 5 * 5 * 3, gru_rec_w);
  m.denoise_gru.bias = filled(rng, 5 * 3, 3);

  m.denoise_output.nb_inputs = 5; m.denoise_output.nb_neurons = 22;
  m.denoise_output.activation = ACT_SIGMOID;
  m.denoise_output.input_weights = filled(rng, 5 * 22, out_w);
  m.denoise_output.bias = filled(rng, 22, 4);

  m.vad_output.nb_inputs = 2; m.vad_output.nb_neurons = 1;
  m.vad_output.activation = ACT_SIGMOID;
  m.vad_output.input_weights = filled(rng, 2 * 1, out_w);
  m.vad_output.bias = filled(rng, 1, 4);

  /* Force a visibly asymmetric recurrent matrix even if the sign pattern
   * happened to be symmetric. */
  if (m.vad_gru.recurrent_weights[0 * 6 + 0 * 2 + 1] ==
      m.vad_gru.recurrent_weights[1 * 6 + 0 * 2 + 0])
    m.vad_gru.recurrent_weights[1 * 6 + 0 * 2 + 0] =
        -m.vad_gru.recurrent_weights[1 * 6 + 0 * 2 + 0];

  m.validate();
  m.check_structure();
  return m;
}

/* "Mild" model for the controlled comparison: every linear sum stays far
 * below 2 after the 1/256 scaling for unit-scale features. */
inline Model build_mild_model(uint32_t seed = 0x9e3779b9u) {
  return build_model(seed, 2, 4, 4, 4);
}

/* Strong-recurrent model used only to make the deliberately broken copies
 * diverge far above the 5e-4 tolerance.  Still a legal model file. */
inline Model build_sensitive_model(uint32_t seed = 0xc001d00du) {
  return build_model(seed, 2, 8, 96, 4);
}

/* ------------------------------------------------------------ reference */

inline double act(int a, double x) {
  switch (a) {
    case ACTIVATION_SIGMOID: return 1.0 / (1.0 + std::exp(-x));
    case ACTIVATION_RELU: return x < 0 ? 0.0 : x;
    default: return std::tanh(x);
  }
}

inline std::vector<double> dense(const Dense &d, const std::vector<double> &in,
                                 double *linear_max = nullptr) {
  const int M = d.nb_inputs, N = d.nb_neurons, stride = N;
  std::vector<double> out(N);
  for (int i = 0; i < N; i++) {
    double sum = d.bias[i];
    for (int j = 0; j < M; j++) sum += d.input_weights[j * stride + i] * in[j];
    if (linear_max && std::fabs(sum) / 256.0 > *linear_max) *linear_max = std::fabs(sum) / 256.0;
    out[i] = act(d.activation, sum / 256.0);
  }
  return out;
}

/* Gate sums are returned as double so callers can validate magnitudes. */
inline std::vector<double> gru(const Gru &g, std::vector<double> &state,
                              const std::vector<double> &input,
                              double *linear_max = nullptr) {
  const int M = g.nb_inputs, N = g.nb_neurons, stride = 3 * N;
  std::vector<double> z(N), r(N), cand(N), h(N);
  for (int i = 0; i < N; i++) {
    double sum = g.bias[i];
    for (int j = 0; j < M; j++) sum += g.input_weights[j * stride + i] * input[j];
    for (int j = 0; j < N; j++) sum += g.recurrent_weights[j * stride + i] * state[j];
    if (linear_max && std::fabs(sum) / 256.0 > *linear_max) *linear_max = std::fabs(sum) / 256.0;
    z[i] = 1.0 / (1.0 + std::exp(-sum / 256.0));
  }
  for (int i = 0; i < N; i++) {
    double sum = g.bias[N + i];
    for (int j = 0; j < M; j++) sum += g.input_weights[N + j * stride + i] * input[j];
    for (int j = 0; j < N; j++) sum += g.recurrent_weights[N + j * stride + i] * state[j];
    if (linear_max && std::fabs(sum) / 256.0 > *linear_max) *linear_max = std::fabs(sum) / 256.0;
    r[i] = 1.0 / (1.0 + std::exp(-sum / 256.0));
  }
  for (int i = 0; i < N; i++) {
    double sum = g.bias[2 * N + i];
    for (int j = 0; j < M; j++) sum += g.input_weights[2 * N + j * stride + i] * input[j];
    /* reset gate of the *source* neuron j against its old state */
    for (int j = 0; j < N; j++)
      sum += g.recurrent_weights[2 * N + j * stride + i] * state[j] * r[j];
    if (linear_max && std::fabs(sum) / 256.0 > *linear_max) *linear_max = std::fabs(sum) / 256.0;
    cand[i] = act(g.activation, sum / 256.0);
    h[i] = z[i] * state[i] + (1.0 - z[i]) * cand[i];
  }
  state = h;   /* commit only after the whole layer */
  return h;
}

struct RefState {
  std::vector<double> vad, noise, denoise;
};

struct RefOutputs {
  std::vector<double> dense, vad, noise, denoise, gains;
  double vad_out = 0.0;
  double linear_max = 0.0;   /* max |linear sum|/256 over every layer */
};

inline void ref_init(RefState &s, const Model &m,
                     const std::vector<double> *initial = nullptr) {
  s.vad.assign(m.vad_gru.nb_neurons, 0.0);
  s.noise.assign(m.noise_gru.nb_neurons, 0.0);
  s.denoise.assign(m.denoise_gru.nb_neurons, 0.0);
  if (initial) {
    if (initial->size() < s.vad.size() + s.noise.size() + s.denoise.size())
      throw std::runtime_error("initial state too small");
    for (size_t i = 0; i < s.vad.size(); i++) s.vad[i] = (*initial)[i];
    for (size_t i = 0; i < s.noise.size(); i++)
      s.noise[i] = (*initial)[s.vad.size() + i];
    for (size_t i = 0; i < s.denoise.size(); i++)
      s.denoise[i] = (*initial)[s.vad.size() + s.noise.size() + i];
  }
}

/* Wiring matches compute_rnn in src/rnn.c exactly:
 *   dense_out -> vad_gru -> vad_output
 *   [dense_out, new vad state, features] -> noise_gru
 *   [new vad state, new noise state, features] -> denoise_gru
 *   denoise_gru state -> denoise_output (22 gains)
 * No layer consumes a stale copy of another layer's state. */
inline RefOutputs ref_frame(const Model &m, RefState &s,
                            const std::vector<double> &features) {
  if ((int)features.size() != 42) throw std::runtime_error("features must be 42-dim");
  RefOutputs o;
  o.dense = dense(m.input_dense, features, &o.linear_max);

  gru(m.vad_gru, s.vad, o.dense, &o.linear_max);
  o.vad = s.vad;
  {
    std::vector<double> v = dense(m.vad_output, s.vad);
    o.vad_out = v[0];
  }
  {
    std::vector<double> in = o.dense;
    in.insert(in.end(), s.vad.begin(), s.vad.end());
    in.insert(in.end(), features.begin(), features.end());
    gru(m.noise_gru, s.noise, in, &o.linear_max);
  }
  o.noise = s.noise;
  {
    std::vector<double> in = s.vad;
    in.insert(in.end(), s.noise.begin(), s.noise.end());
    in.insert(in.end(), features.begin(), features.end());
    gru(m.denoise_gru, s.denoise, in, &o.linear_max);
  }
  o.denoise = s.denoise;
  {
    std::vector<double> gy = dense(m.denoise_output, s.denoise);
    o.gains = gy;
  }
  return o;
}

}  // namespace rnref

#endif /* RNNOISE_TEST_REF_H */
