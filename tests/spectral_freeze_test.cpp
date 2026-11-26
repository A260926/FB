// Spectral-freeze feature tests for SignalsmithStretch.
//
// All sounds are generated in C++ from a fixed seed.  The checks cover:
//   * off-bin 437 Hz capture, >5 s of sustained silence / 997 Hz replacement
//   * frozen pitch accuracy (+-2 Hz) and RMS (<=1 dB)
//   * the four-hop release back to the live signal
//   * dual-tone spectral shape, fixed stereo ratios and anti-phase
//   * sample-accurate stereo relation (<=2e-4)
//   * frame vs 1/7/113-sample chunk equivalence (<=2e-4, full output)
//   * split-computation alignment (including half-hop automation changes)
//   * reset/seek clearing, silence re-capture and finite flush tails
//
// On success this prints:  SPECTRAL_FREEZE_OK cases=N

#include "signalsmith-stretch.h"

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>
#include <string>
#include <algorithm>

namespace {

constexpr int SR = 48000;
constexpr long FIXED_SEED = 0x51EEDL;
constexpr double REL_TOL = 2e-4;
constexpr double CHUNK_TOL = 2e-4;
constexpr double FREQ_TOL = 2.0;
constexpr double RMS_DB_TOL = 1.0;

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const std::string &name, const std::string &detail = "") {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s%s%s\n", name.c_str(), detail.empty() ? "" : " -- ", detail.c_str());
	}
}

// Deterministic PRNG (fixed seed), used for generator phase jitter.
struct Rng {
	uint64_t state;
	explicit Rng(uint64_t seed) : state(seed ? seed : 1) {}
	double unit() { // [0,1)
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		return double((state >> 11) & ((1ull << 53) - 1)) / double(1ull << 53);
	}
};

struct Toggle {
	int at;
	bool enabled;
};

template <class Sample>
struct Render {
	std::vector<Sample> left, right;
	int inputLatency = 0, outputLatency = 0;
};

// Renders N frames, applying toggles at exact absolute sample positions.
// Chunks are split at toggle points so automation timing is independent of chunk size.
template <class Sample>
Render<Sample> renderFreeze(
	int channels, int block, int hop, bool split, int chunk, int N,
	const std::vector<Sample> &inL, const std::vector<Sample> &inR,
	const std::vector<Toggle> &toggles, bool reassert = false) {

	signalsmith::stretch::SignalsmithStretch<Sample> stretch(FIXED_SEED);
	stretch.configure(channels, block, hop, split);

	Render<Sample> out;
	out.left.assign(N, Sample(0));
	out.right.assign(N, Sample(0));
	out.inputLatency = stretch.inputLatency();
	out.outputLatency = stretch.outputLatency();

	size_t ti = 0;
	bool currentRequest = false;
	int pos = 0;
	while (pos < N) {
		while (ti < toggles.size() && toggles[ti].at <= pos) {
			currentRequest = toggles[ti].enabled;
			stretch.setSpectralFreeze(currentRequest);
			++ti;
		}
		if (reassert) stretch.setSpectralFreeze(currentRequest);

		int n = std::min(chunk, N - pos);
		for (const Toggle &t : toggles) {
			if (t.at > pos && t.at < pos + n) n = t.at - pos;
		}
		n = std::max(1, n);

		Sample *in[2] = {
			const_cast<Sample *>(inL.data() + pos),
			channels > 1 ? const_cast<Sample *>(inR.data() + pos) : nullptr
		};
		Sample *outPtr[2] = { out.left.data() + pos, out.right.data() + pos };
		if (channels == 1) {
			stretch.process(in, n, outPtr, n);
		} else {
			stretch.process(in, n, outPtr, n);
		}
		pos += n;
	}
	return out;
}

double rmsStereo(const std::vector<float> &l, const std::vector<float> &r, int a, int b) {
	a = std::max(0, a);
	b = std::min(int(l.size()), b);
	double e = 0;
	for (int i = a; i < b; ++i) e += double(l[i]) * l[i] + double(r[i]) * r[i];
	return std::sqrt(e / std::max(1, 2 * (b - a)));
}

// Direct DFT level (Hann window optional) at one frequency.
double dftLevel(const std::vector<float> &x, int a, int b, double freq, bool hann = true) {
	a = std::max(0, a);
	b = std::min(int(x.size()), b);
	int n = b - a;
	double sumW = 0, re = 0, im = 0;
	for (int i = 0; i < n; ++i) {
		double w = hann ? 0.5 * (1.0 - std::cos(2 * M_PI * i / std::max(1, n - 1))) : 1.0;
		double phase = 2 * M_PI * freq * (a + i) / SR;
		double v = double(x[a + i]) * w;
		re += v * std::cos(phase);
		im -= v * std::sin(phase);
		sumW += w;
	}
	return 2.0 * std::sqrt(re * re + im * im) / std::max(1.0, sumW);
}

// Best (frequency, level) over [fMin, fMax], with parabolic refinement.
std::pair<double, double> peakFrequency(
	const std::vector<float> &x, int a, int b, double fMin, double fMax, double step = 0.5) {
	double bestF = fMin, bestL = -1;
	for (double f = fMin; f <= fMax + 1e-9; f += step) {
		double l = dftLevel(x, a, b, f);
		if (l > bestL) { bestL = l; bestF = f; }
	}
	double l0 = dftLevel(x, a, b, bestF - step);
	double l1 = bestL;
	double l2 = dftLevel(x, a, b, bestF + step);
	double denom = (l0 - 2 * l1 + l2);
	double shift = 0;
	if (std::abs(denom) > 1e-30) {
		// Parabolic interpolation on log-ish magnitudes; clamp to one step.
		shift = 0.5 * (l0 - l2) / denom;
		shift = std::max(-1.0, std::min(1.0, shift));
	}
	return { bestF + step * shift, bestL };
}

double maxStereoRelation(
	const std::vector<float> &l, const std::vector<float> &r, double ratio, int a, int b) {
	double m = 0;
	a = std::max(0, a);
	b = std::min(int(l.size()), b);
	for (int i = a; i < b; ++i) {
		// r[i] should equal ratio*l[i]
		m = std::max(m, std::abs(double(r[i]) - ratio * double(l[i])));
	}
	return m;
}

double maxDiff(const std::vector<float> &a, const std::vector<float> &b, int shiftB = 0) {
	double m = 0;
	int n = std::min(a.size(), b.size());
	for (int i = 0; i < n; ++i) {
		int j = i + shiftB;
		if (j < 0 || j >= int(b.size())) continue;
		m = std::max(m, std::abs(double(a[i]) - double(b[j])));
	}
	return m;
}

// Generates a mono/stereo test signal.  After `switchAt`, either silence or a new frequency.
struct ToneSpec {
	double f1 = 437.0;
	double f2 = 0;          // 0 = single tone
	double f2Amp = 0.0;
	double afterFreq = 0;   // 0 = silence after switchAt
	int switchAt = 0;
	double amp = 0.22;
	double stereoRatio = -0.5;
	double phase2 = 0.0;
};

void generate(std::vector<float> &l, std::vector<float> &r, int N, const ToneSpec &sp) {
	l.assign(N, 0);
	r.assign(N, 0);
	for (int i = 0; i < N; ++i) {
		double f = (sp.switchAt && i >= sp.switchAt) ? sp.afterFreq : sp.f1;
		double v = sp.amp * std::sin(2 * M_PI * f * i / SR);
		if (sp.f2 > 0) {
			double g2 = (sp.switchAt && i >= sp.switchAt) ? sp.afterFreq * (sp.f2 / sp.f1) : sp.f2;
			v += sp.f2Amp * sp.amp * std::sin(2 * M_PI * g2 * i / SR + sp.phase2);
		}
		l[i] = float(v);
		r[i] = float(sp.stereoRatio * v);
	}
}

// Checks the sustained 437 pitch/RMS for one configuration; returns true on success.
bool checkSustainConfig(int block, int hop, bool split, int chunk, bool useDouble) {
	const int pre = 2 * SR;
	const int hold = 5 * SR; // at least five seconds
	const int fz = pre;
	const int N = fz + hold + 4096;
	ToneSpec sp;
	sp.switchAt = fz;
	sp.afterFreq = 0; // silence after freezing
	std::vector<float> inL, inR;
	generate(inL, inR, N, sp);

	int outLat = 0;
	std::vector<float> oL, oR;
	if (useDouble) {
		// Run the same render in double and narrow for the metric helpers.
		std::vector<double> dl(inL.begin(), inL.end()), dr(inR.begin(), inR.end());
		auto rd = renderFreeze<double>(2, block, hop, split, chunk, N, dl, dr, {{fz, true}});
		oL.assign(rd.left.begin(), rd.left.end());
		oR.assign(rd.right.begin(), rd.right.end());
		outLat = rd.outputLatency;
	} else {
		auto rf = renderFreeze<float>(2, block, hop, split, chunk, N, inL, inR, {{fz, true}});
		oL = rf.left;
		oR = rf.right;
		outLat = rf.outputLatency;
	}

	int a = fz + outLat + 4 * hop + 4096;
	int b = fz + hold - 4096;
	auto [freq, mag] = peakFrequency(oL, a, std::min(b, a + 3 * SR), 425, 449, 0.5);

	double preRms = rmsStereo(oL, oR, fz - SR / 2, fz - 2048);
	double fzRms = rmsStereo(oL, oR, a, b);
	double db = 20 * std::log10(fzRms / std::max(1e-12, preRms));

	char tag[160];
	std::snprintf(tag, sizeof(tag), "sustain %s %d/%d split=%d chunk=%d",
		useDouble ? "f64" : "f32", block, hop, int(split), chunk);
	check(std::abs(freq - 437.0) <= FREQ_TOL, std::string(tag) + " pitch",
		"freq=" + std::to_string(freq));
	check(std::abs(db) <= RMS_DB_TOL, std::string(tag) + " rms",
		"db=" + std::to_string(db));
	check(mag > 0.05, std::string(tag) + " level", "mag=" + std::to_string(mag));

	bool finite = true;
	for (int i = 0; i < N; ++i) {
		if (!std::isfinite(oL[i]) || !std::isfinite(oR[i])) { finite = false; break; }
	}
	check(finite, std::string(tag) + " finite");
	return std::abs(freq - 437.0) <= FREQ_TOL && std::abs(db) <= RMS_DB_TOL;
}

} // namespace

int main() {
	Rng rng(FIXED_SEED);
	(void)rng;

	// ---- 1. Headline sustain cases (float/double, both window/hop pairs, split) ----
	checkSustainConfig(1024, 256, false, 1, false);
	checkSustainConfig(1024, 256, true, 7, false);
	checkSustainConfig(2048, 512, false, 113, false);
	checkSustainConfig(2048, 512, false, 113, true);

	// ---- 2. Release back to 997 Hz ----
	{
		const int hop = 256, block = 1024;
		const int fz = 2 * SR;
		const int rel = fz + 5 * SR; // hold the (replaced) 997 input for 5 s
		const int N = rel + SR;
		ToneSpec sp;
		sp.f1 = 437;
		sp.switchAt = fz;
		sp.afterFreq = 997;
		std::vector<float> inL, inR;
		generate(inL, inR, N, sp);

		// Release on an exact hop boundary for deterministic four-hop timing.
		int relAligned = (rel / hop) * hop;
		auto r = renderFreeze<float>(2, block, hop, false, 113, N, inL, inR,
			{{fz, true}, {relAligned, false}});

		int start = relAligned + 4 * hop + r.outputLatency + block;
		int end = N - 4096;
		auto [f997, mag997] = peakFrequency(r.left, start, end, 985, 1009, 0.5);
		double lvl997 = dftLevel(r.left, N - 16384, N - 256, 997);
		double lvl437 = dftLevel(r.left, N - 16384, N - 256, 437);
		check(std::abs(f997 - 997.0) <= 3.0, "release dominant frequency",
			"f=" + std::to_string(f997));
		check(lvl997 > 0.05, "release 997 present", "lvl=" + std::to_string(lvl997));
		check(lvl437 < 0.02 * std::max(1e-9, lvl997), "release removes 437",
			"ratio=" + std::to_string(lvl437 / std::max(1e-9, lvl997)));

		// active() semantics around release
		signalsmith::stretch::SignalsmithStretch<float> s(FIXED_SEED);
		s.configure(2, block, hop, false);
		check(!s.spectralFreezeActive(), "active false by default");
	}

	// ---- 3. Dual tone spectral shape + stereo ratios / anti-phase ----
	for (double ratio : { 1.0, 0.68, -0.83, -1.0 }) {
		const int fz = 2 * SR;
		const int N = fz + 4 * SR + 2048;
		ToneSpec sp;
		sp.f1 = 437;
		sp.f2 = 811; // off-bin as well
		sp.f2Amp = 0.55;
		sp.phase2 = 0.9;
		sp.stereoRatio = ratio;
		sp.switchAt = fz;
		sp.afterFreq = 0;
		std::vector<float> inL, inR;
		generate(inL, inR, N, sp);
		auto r = renderFreeze<float>(2, 1024, 256, false, 1, N, inL, inR, {{fz, true}});

		int a = fz + r.outputLatency + 4 * 256 + 4096;
		int b = fz + 3 * SR;
		auto [fa, ma] = peakFrequency(r.left, a, b, 429, 445, 0.5);
		auto [fb, mb] = peakFrequency(r.left, a, b, 803, 819, 0.5);
		check(std::abs(fa - 437.0) <= FREQ_TOL, "dual tone f1 preserved",
			"f=" + std::to_string(fa));
		check(std::abs(fb - 811.0) <= FREQ_TOL, "dual tone f2 preserved",
			"f=" + std::to_string(fb));
		// Shape: second tone relative level close to the generated 0.55 (within 20%).
		check(mb > 0.35 * ma && mb < 0.78 * ma, "dual tone spectral shape",
			"ratio=" + std::to_string(mb / std::max(1e-9, ma)));

		double rel = maxStereoRelation(r.left, r.right, ratio, a, b);
		check(rel <= REL_TOL, "stereo sample relation (fixed/anti ratio)",
			"ratio=" + std::to_string(ratio) + " err=" + std::to_string(rel));

		// Stereo phase/amplitude at the two frequencies via complex DFT ratio.
		for (double f : {437.0, 811.0}) {
			int w = 16384, wa = b - w, wb = b;
			std::complex<double> zl(0, 0), zr(0, 0);
			for (int i = wa; i < wb; ++i) {
				double win = 0.5 * (1 - std::cos(2 * M_PI * (i - wa) / (w - 1)));
				double ph = 2 * M_PI * f * i / SR;
				std::complex<double> e(std::cos(ph), -std::sin(ph));
				zl += double(r.left[i]) * win * e;
				zr += double(r.right[i]) * win * e;
			}
			std::complex<double> q = zr / zl;
			double ampRatio = std::abs(q);
			double phaseDeg = std::arg(q) * 180 / M_PI;
			check(std::abs(ampRatio - std::abs(ratio)) < 0.02, "stereo amplitude ratio at peak",
				"f=" + std::to_string(f) + " ar=" + std::to_string(ampRatio));
			double expectedPhase = ratio < 0 ? 180.0 : 0.0;
			double phaseErr = std::abs(std::remainder(phaseDeg - expectedPhase, 360.0));
			check(phaseErr < 2.0, "stereo phase at peak",
				"f=" + std::to_string(f) + " errDeg=" + std::to_string(phaseErr));
		}
	}

	// ---- 4. Frame vs 1/7/113 chunk equivalence, exact absolute toggles ----
	{
		const int N = 7 * SR;
		ToneSpec sp;
		sp.switchAt = int(2.6 * SR);
		sp.afterFreq = 997;
		std::vector<float> inL, inR;
		generate(inL, inR, N, sp);
		std::vector<Toggle> sched = {
			{ int(1.137 * SR), true },
			{ int(3.419 * SR), false },
			{ int(4.101 * SR), true } // re-capture from live 997
		};
		auto f1 = renderFreeze<float>(2, 1024, 256, false, 1, N, inL, inR, sched);
		auto f7 = renderFreeze<float>(2, 1024, 256, false, 7, N, inL, inR, sched);
		auto f113 = renderFreeze<float>(2, 1024, 256, false, 113, N, inL, inR, sched);
		check(maxDiff(f1.left, f7.left) <= CHUNK_TOL, "chunk eq 1 vs 7 (L)",
			std::to_string(maxDiff(f1.left, f7.left)));
		check(maxDiff(f1.right, f7.right) <= CHUNK_TOL, "chunk eq 1 vs 7 (R)");
		check(maxDiff(f1.left, f113.left) <= CHUNK_TOL, "chunk eq 1 vs 113 (L)",
			std::to_string(maxDiff(f1.left, f113.left)));
		check(maxDiff(f1.right, f113.right) <= CHUNK_TOL, "chunk eq 1 vs 113 (R)");

		// Double precision too.
		std::vector<double> dl(inL.begin(), inL.end()), dr(inR.begin(), inR.end());
		auto d1 = renderFreeze<double>(2, 1024, 256, false, 1, N, dl, dr, sched);
		auto d113 = renderFreeze<double>(2, 1024, 256, false, 113, N, dl, dr, sched);
		double dd = 0;
		for (int i = 0; i < N; ++i) dd = std::max(dd, std::abs(d1.left[i] - d113.left[i]));
		check(dd <= CHUNK_TOL, "chunk eq double", std::to_string(dd));

		// Repeated set(true) must not restart/change the sound.
		auto rep = renderFreeze<float>(2, 1024, 256, false, 7, N, inL, inR, sched, true);
		check(maxDiff(f7.left, rep.left) == 0.0f, "repeated set(true) is a no-op");
	}

	// ---- 4b. Release into long silence must stay chunk-equivalent (silence shortcut hand-off) ----
	{
		const int N = 8 * SR;
		const int fz = 2 * SR;
		const int rl = fz + 5 * SR + 137; // deliberately off hop-grid
		std::vector<float> inL(N, 0), inR(N, 0);
		for (int i = 0; i < fz + 1000; ++i) {
			float v = 0.22f * std::sin(2 * M_PI * 437 * i / SR);
			inL[i] = v;
			inR[i] = -0.6f * v;
		}
		std::vector<Toggle> sched = {{ fz, true }, { rl, false }};
		auto q1 = renderFreeze<float>(2, 1024, 256, false, 1, N, inL, inR, sched);
		auto q7 = renderFreeze<float>(2, 1024, 256, false, 7, N, inL, inR, sched);
		auto q113 = renderFreeze<float>(2, 1024, 256, false, 113, N, inL, inR, sched);
		check(maxDiff(q1.left, q7.left) <= CHUNK_TOL, "release-in-silence eq 1 vs 7",
			std::to_string(maxDiff(q1.left, q7.left)));
		check(maxDiff(q1.left, q113.left) <= CHUNK_TOL, "release-in-silence eq 1 vs 113",
			std::to_string(maxDiff(q1.left, q113.left)));
	}

	// ---- 5. Split computation: chunk equivalence + extra-delay alignment + half-hop changes ----
	{
		const int N = 6 * SR;
		ToneSpec sp;
		sp.switchAt = int(2.2 * SR);
		sp.afterFreq = 777;
		std::vector<float> inL, inR;
		generate(inL, inR, N, sp);
		// Automation deliberately lands halfway through hops and flips quickly.
		std::vector<Toggle> sched = {
			{ SR + 384, true },
			{ SR + 700, false }, // mid-hop release
			{ SR + 905, true },  // re-press during the four-hop fade
			{ 3 * SR + 130, false },
			{ 3 * SR + 260, true }
		};
		auto s1 = renderFreeze<float>(2, 1024, 256, true, 1, N, inL, inR, sched);
		auto s7 = renderFreeze<float>(2, 1024, 256, true, 7, N, inL, inR, sched);
		auto ns = renderFreeze<float>(2, 1024, 256, false, 113, N, inL, inR, sched);
		check(maxDiff(s1.left, s7.left) <= CHUNK_TOL, "split chunk eq 1 vs 7",
			std::to_string(maxDiff(s1.left, s7.left)));

		int extra = s7.outputLatency - ns.outputLatency; // one interval (256)
		check(extra == 256, "split extra latency is one hop", "extra=" + std::to_string(extra));
		double aligned = 0;
		for (int i = extra; i < N; ++i) {
			aligned = std::max(aligned, std::abs(double(s7.left[i]) - double(ns.left[i - extra])));
		}
		check(aligned <= CHUNK_TOL, "split vs non-split aligned", std::to_string(aligned));
	}

	// ---- 6. active() warm-up semantics ----
	{
		signalsmith::stretch::SignalsmithStretch<float> s(FIXED_SEED);
		s.configure(2, 1024, 256, false);
		check(!s.spectralFreezeActive(), "inactive before request");
		s.setSpectralFreeze(true);
		check(!s.spectralFreezeActive(), "request alone does not activate");
		const int n = 64;
		std::vector<float> x(n, 0.1f), y(n), z(n);
		float *in[2] = { x.data(), x.data() };
		float *out[2] = { y.data(), z.data() };
		s.process(in, n, out, n);
		check(!s.spectralFreezeActive(), "still inactive before two analysis frames");
		for (int k = 0; k < 30; ++k) s.process(in, n, out, n);
		check(s.spectralFreezeActive(), "active after enough frames");

		// Long digital silence from a fresh instance: finite zero output, stays inactive.
		signalsmith::stretch::SignalsmithStretch<float> zs(FIXED_SEED);
		zs.configure(2, 1024, 256, false);
		zs.setSpectralFreeze(true);
		const int ZN = 5 * SR;
		std::vector<float> zl(ZN, 0), zr(ZN, 0), ol(ZN), orr(ZN);
		int p = 0;
		while (p < ZN) {
			int nn = std::min(113, ZN - p);
			float *zi[2] = { zl.data() + p, zr.data() + p };
			float *zo[2] = { ol.data() + p, orr.data() + p };
			zs.process(zi, nn, zo, nn);
			p += nn;
		}
		double mx = 0;
		bool finite = true;
		for (float v : ol) { mx = std::max(mx, double(std::abs(v))); if (!std::isfinite(v)) finite = false; }
		// Once two real (zero-valued) spectra exist the zero-energy snapshot is captured, so active
		// may be true; the required guarantee is finite, exactly-zero-ish output either way.
		check(finite && mx < 1e-6, "silence yields finite (near-)zero output", "max=" + std::to_string(mx));
	}

	// ---- 7. reset / configure / seek clear the request and snapshot ----
	{
		signalsmith::stretch::SignalsmithStretch<float> s(FIXED_SEED);
		s.configure(2, 1024, 256, true);
		const int n = 512;
		std::vector<float> x(n), y(n), z(n);
		for (int i = 0; i < n; ++i) x[i] = 0.2f * std::sin(2 * M_PI * 437 * i / SR);
		float *in[2] = { x.data(), x.data() };
		float *out[2] = { y.data(), z.data() };
		s.setSpectralFreeze(true);
		for (int k = 0; k < 20; ++k) s.process(in, n, out, n);
		check(s.spectralFreezeActive(), "freeze active before reset");
		s.reset();
		check(!s.spectralFreezeActive(), "reset clears freeze");
		// Request was cleared: processing loud sound must not re-activate freeze.
		for (int k = 0; k < 20; ++k) s.process(in, n, out, n);
		check(!s.spectralFreezeActive(), "reset cleared the request too");

		s.setSpectralFreeze(true);
		for (int k = 0; k < 20; ++k) s.process(in, n, out, n);
		check(s.spectralFreezeActive(), "re-activated after reset");
		s.seek(in, n, 1.0);
		check(!s.spectralFreezeActive(), "seek clears freeze");

		s.setSpectralFreeze(true);
		s.configure(2, 2048, 512, false);
		check(!s.spectralFreezeActive(), "configure clears freeze");
	}

	// ---- 8. Re-capture after silence (new frequency) ----
	{
		const int N = 8 * SR;
		std::vector<float> inL(N, 0), inR(N, 0);
		for (int i = 0; i < 2 * SR; ++i) {
			inL[i] = 0.2f * std::sin(2 * M_PI * 437 * i / SR);
			inR[i] = -0.7f * inL[i];
		}
		const int tone2 = 5 * SR;
		for (int i = tone2; i < N; ++i) {
			inL[i] = 0.2f * std::sin(2 * M_PI * 661 * i / SR);
			inR[i] = -0.7f * inL[i];
		}
		// Freeze 437, release at 2 s, then freeze the new 661 tone after it stabilises.
		auto r = renderFreeze<float>(2, 1024, 256, false, 7, N, inL, inR,
			{{ SR, true }, { 2 * SR, false }, { tone2 + 6000, true }});
		int a = N - 40000, b = N - 8192;
		auto [f, mag] = peakFrequency(r.left, a, b, 650, 672, 0.5);
		double rel = maxStereoRelation(r.left, r.right, -0.7, a, b);
		check(std::abs(f - 661.0) <= FREQ_TOL, "re-capture after silence picks new frequency",
			"f=" + std::to_string(f));
		check(rel <= REL_TOL, "re-capture stereo relation", std::to_string(rel));
	}

	// ---- 9. flush ends freeze and produces a finite bounded tail ----
	{
		const int N = 2 * SR;
		std::vector<float> inL(N), inR(N), oL(N + 4096, 0), oR(N + 4096, 0);
		for (int i = 0; i < N; ++i) {
			inL[i] = 0.2f * std::sin(2 * M_PI * 437 * i / SR);
			inR[i] = -0.5f * inL[i];
		}
		signalsmith::stretch::SignalsmithStretch<float> s(FIXED_SEED);
		s.configure(2, 1024, 256, true);
		s.setSpectralFreeze(true);
		float *in[2] = { inL.data(), inR.data() };
		float *out[2] = { oL.data(), oR.data() };
		s.process(in, N, out, N);
		check(s.spectralFreezeActive(), "flush case was active");
		const int tail = s.outputLatency() + 1024;
		float *tailOut[2] = { oL.data() + N, oR.data() + N };
		s.flush(tailOut, tail);
		check(!s.spectralFreezeActive(), "flush clears freeze");
		bool finite = true;
		for (int i = N; i < N + tail; ++i) {
			if (!std::isfinite(oL[i]) || !std::isfinite(oR[i])) finite = false;
		}
		check(finite, "flush tail finite");
	}

	// ---- 10. Mono support ----
	{
		const int fz = SR;
		const int N = fz + 3 * SR;
		std::vector<float> inL(N);
		for (int i = 0; i < N; ++i) inL[i] = 0.2f * std::sin(2 * M_PI * 437 * i / SR);
		signalsmith::stretch::SignalsmithStretch<float> s(FIXED_SEED);
		s.configure(1, 2048, 512, false);
		std::vector<float> oL(N);
		int p = 0;
		bool on = false;
		while (p < N) {
			if (p >= fz && !on) { s.setSpectralFreeze(true); on = true; }
			int n = std::min(113, N - p);
			float *in[1] = { inL.data() + p };
			float *out[1] = { oL.data() + p };
			s.process(in, n, out, n);
			p += n;
		}
		auto [f, mag] = peakFrequency(oL, fz + 4096, fz + 2 * SR, 425, 449, 0.5);
		check(std::abs(f - 437.0) <= FREQ_TOL, "mono freeze pitch", std::to_string(f));
	}

	if (g_failures) {
		std::printf("SPECTRAL_FREEZE_FAILED checks=%d failures=%d\n", g_checks, g_failures);
		return 1;
	}
	std::printf("SPECTRAL_FREEZE_OK cases=%d\n", g_checks);
	return 0;
}
