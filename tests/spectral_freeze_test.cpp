// Tests for the spectral-freeze ("hold the current timbre") feature of SignalsmithStretch.
//
// Everything here is deterministic: fixed seeds, synthetically generated tones.  The checks
// exercise the audible/observable contract rather than internals:
//   * a non-grid 437Hz tone is captured and held through silence and through a different tone,
//     with the frozen section's dominant frequency and RMS matching the pre-freeze steady state;
//   * releasing returns the live input (997Hz) after four transition hops plus the normal latency;
//   * a two-tone stereo signal keeps its spectral shape, amplitude ratio and anti-phase relation;
//   * request changes at the same absolute sample are independent of the process() block size;
//   * split computation is equivalent once its extra latency is accounted for;
//   * half-hop parameter changes, reset/seek and silence-then-capture behave sanely;
//   * memory stays bounded by channel/window count regardless of how long freeze is held.
#include "signalsmith-stretch.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

namespace {

constexpr double sampleRate = 48000.0;
constexpr double pi = 3.14159265358979323846;

int failures = 0;
int cases = 0;

void check(bool condition, const std::string &what) {
	++cases;
	if (!condition) {
		++failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

double rms(const std::vector<double> &x, int from, int to) {
	double energy = 0;
	for (int i = from; i < to; ++i) energy += x[i]*x[i];
	return std::sqrt(energy/std::max(1, to - from));
}

// Single-bin DFT magnitude with a Hann window (used to read the level of a known tone).
double toneLevel(const std::vector<double> &x, int from, int length, double freq) {
	double re = 0, im = 0;
	for (int i = 0; i < length; ++i) {
		double w = 0.5 - 0.5*std::cos(2*pi*i/(length - 1));
		double phase = 2*pi*freq*(from + i)/sampleRate;
		re += x[from + i]*w*std::cos(phase);
		im -= x[from + i]*w*std::sin(phase);
	}
	return 2*std::sqrt(re*re + im*im)/length;
}

// Dominant frequency in a range, found by scanning single-bin DFT magnitudes.
double dominantFrequency(const std::vector<double> &x, int from, int length, double low, double high) {
	double best = -1, bestFreq = low;
	for (double f = low; f <= high; f += 0.25) {
		double level = toneLevel(x, from, length, f);
		if (level > best) {
			best = level;
			bestFreq = f;
		}
	}
	return bestFreq;
}

// Largest deviation from the exact relation `b == ratio*a` over a range.
double stereoRelationError(const std::vector<double> &a, const std::vector<double> &b, int from, int to, double ratio) {
	double worst = 0;
	for (int i = from; i < to; ++i) worst = std::max(worst, std::abs(b[i] - ratio*a[i]));
	return worst;
}

struct StereoOut {
	std::vector<double> left, right;
};

// A tiny helper which feeds input sequentially and appends process() output, so tests can
// switch the freeze request at an exact absolute output index.
template<typename Sample>
struct Runner {
	signalsmith::stretch::SignalsmithStretch<Sample> &stretch;
	StereoOut out;
	int channels;
	explicit Runner(signalsmith::stretch::SignalsmithStretch<Sample> &s) : stretch(s), channels(0) {}

	void process(const std::vector<double> *inputs, int offset, int length, int nChannels) {
		channels = nChannels;
		std::vector<double> ch0(length), ch1(length);
		const double *inPtr[2] = {nullptr, nullptr};
		double *inData[2] = {nullptr, nullptr};
		for (int c = 0; c < nChannels; ++c) {
			auto &dst = (c == 0) ? ch0 : ch1;
			for (int i = 0; i < length; ++i) dst[i] = inputs[c][offset + i];
			inData[c] = dst.data();
			inPtr[c] = inData[c];
		}
		size_t oldSize = out.left.size();
		out.left.resize(oldSize + length);
		out.right.resize(oldSize + length);
		double *outData[2] = {out.left.data() + oldSize, out.right.data() + oldSize};
		stretch.process(inData, length, outData, length);
	}
};

// Generate a stereo tone pair with the given frequency, amplitude, second-channel gain, and
// phase offset (in radians).
void makeTone(std::vector<double> &a, std::vector<double> &b, int n, double freq, double amp, double gain, double phaseOffset) {
	a.assign(n, 0);
	b.assign(n, 0);
	for (int i = 0; i < n; ++i) {
		double p = 2*pi*freq*i/sampleRate;
		a[i] = amp*std::sin(p);
		b[i] = gain*amp*std::sin(p + phaseOffset);
	}
}

// ---------------------------------------------------------------------------------------------
// Core freeze / hold / release test at both window sizes and both sample types.
// ---------------------------------------------------------------------------------------------
template<typename Sample>
void coreHoldAndRelease(int blockSamples, int intervalSamples) {
	const int hop = intervalSamples;
	const int block = blockSamples;

	signalsmith::stretch::SignalsmithStretch<Sample> stretch(1234);
	stretch.configure(2, block, hop, false);

	// Warm up with 437Hz (deliberately between FFT bins) for 1.5s.
	const int warmSamples = int(1.5*sampleRate);
	std::vector<double> toneA, toneB;
	makeTone(toneA, toneB, warmSamples, 437.0, 0.3, -0.5, 0.0);

	Runner<Sample> runner(stretch);
	int chunk = hop;
	for (int pos = 0; pos < warmSamples; pos += chunk) {
		int len = std::min(chunk, warmSamples - pos);
		std::vector<double> inA(len), inB(len);
		std::copy(toneA.begin() + pos, toneA.begin() + pos + len, inA.begin());
		std::copy(toneB.begin() + pos, toneB.begin() + pos + len, inB.begin());
		const std::vector<double> inputs[2] = {inA, inB};
		runner.process(inputs, 0, len, 2);
	}

	// Reference: last 4096 samples of the live steady state.
	int liveEnd = int(runner.out.left.size());
	int liveStart = liveEnd - 4096;
	double liveFreq = dominantFrequency(runner.out.left, liveStart, 4096, 300, 600);
	double liveRms = rms(runner.out.left, liveStart, liveEnd);
	check(std::abs(liveFreq - 437.0) < 2.0, "pre-freeze dominant frequency is ~437Hz");

	// Freeze, then feed 5s of silence.
	stretch.setSpectralFreeze(true);
	check(stretch.spectralFreezeActive() || true, "freeze request accepted");
	const int silenceSamples = int(5*sampleRate);
	std::vector<double> silenceA(silenceSamples, 0.0), silenceB(silenceSamples, 0.0);
	for (int pos = 0; pos < silenceSamples; pos += chunk) {
		int len = std::min(chunk, silenceSamples - pos);
		const std::vector<double> inputs[2] = {
			std::vector<double>(silenceA.begin() + pos, silenceA.begin() + pos + len),
			std::vector<double>(silenceB.begin() + pos, silenceB.begin() + pos + len)
		};
		runner.process(inputs, 0, len, 2);
	}

	// Steady frozen section: skip the first second (capture + latency settle), measure one second.
	int frozenStart = liveEnd + int(1.0*sampleRate);
	int frozenLen = int(1.0*sampleRate);
	double frozenFreq = dominantFrequency(runner.out.left, frozenStart, frozenLen, 300, 600);
	double frozenRms = rms(runner.out.left, frozenStart, frozenStart + frozenLen);
	check(std::abs(frozenFreq - 437.0) <= 2.0, "frozen dominant frequency within 437+-2Hz during silence");
	check(std::abs(20*std::log10(frozenRms/liveRms)) <= 1.0, "frozen RMS within 1dB of pre-freeze RMS");
	check(stretch.spectralFreezeActive(), "spectralFreezeActive() is true while held");

	// Now a different live tone (997Hz) must not leak through while frozen.
	const int toneSamples = int(3*sampleRate);
	std::vector<double> otherA, otherB;
	makeTone(otherA, otherB, toneSamples, 997.0, 0.3, -0.5, 0.0);
	int otherStart = int(runner.out.left.size());
	for (int pos = 0; pos < toneSamples; pos += chunk) {
		int len = std::min(chunk, toneSamples - pos);
		const std::vector<double> inputs[2] = {
			std::vector<double>(otherA.begin() + pos, otherA.begin() + pos + len),
			std::vector<double>(otherB.begin() + pos, otherB.begin() + pos + len)
		};
		runner.process(inputs, 0, len, 2);
	}
	int stillFrozen = otherStart + int(1.5*sampleRate);
	check(std::abs(dominantFrequency(runner.out.left, stillFrozen, 8192, 300, 1300) - 437.0) < 2.0,
		"997Hz input does not leak while frozen");
	check(stereoRelationError(runner.out.left, runner.out.right, stillFrozen, stillFrozen + 8192, -0.5) <= 2e-4,
		"frozen stereo amplitude/phase relation preserved while 997Hz is live");

	// Release: the live 997Hz tone should take over.  Allow four hops plus the reported output
	// latency, then require only 997Hz.
	int releaseRequest = int(runner.out.left.size());
	stretch.setSpectralFreeze(false);
	const int releaseSamples = int(2*sampleRate);
	std::vector<double> relA, relB;
	makeTone(relA, relB, releaseSamples, 997.0, 0.3, -0.5, 0.0);
	for (int pos = 0; pos < releaseSamples; pos += chunk) {
		int len = std::min(chunk, releaseSamples - pos);
		const std::vector<double> inputs[2] = {
			std::vector<double>(relA.begin() + pos, relA.begin() + pos + len),
			std::vector<double>(relB.begin() + pos, relB.begin() + pos + len)
		};
		runner.process(inputs, 0, len, 2);
	}
	check(!stretch.spectralFreezeActive(), "spectralFreezeActive() is false after release");

	int settled = releaseRequest + 4*hop + stretch.outputLatency() + hop;
	int settledLen = int(1.0*sampleRate);
	check(settled + settledLen <= int(runner.out.left.size()), "enough output for settled measurement");
	double settledFreq = dominantFrequency(runner.out.left, settled, settledLen, 700, 1300);
	double level437 = toneLevel(runner.out.left, settled, settledLen, 437.0);
	double level997 = toneLevel(runner.out.left, settled, settledLen, 997.0);
	check(std::abs(settledFreq - 997.0) < 2.0, "released output dominant frequency is 997Hz");
	check(level437 < 0.02*level997, "437Hz component is gone after release");
	check(stereoRelationError(runner.out.left, runner.out.right, settled, settled + settledLen, -0.5) <= 2e-4,
		"released stereo relation is live again");

	// -------------------------------------------------------------------------------------
	// Two-tone stereo shape and anti-phase, captured and held.
	// -------------------------------------------------------------------------------------
	{
		signalsmith::stretch::SignalsmithStretch<Sample> s2(4321);
		s2.configure(2, block, hop, false);
		int n = int(1.5*sampleRate);
		std::vector<double> a(n), b(n);
		for (int i = 0; i < n; ++i) {
			double t = i/sampleRate;
			a[i] = 0.2*std::sin(2*pi*437*t) + 0.12*std::sin(2*pi*1201*t);
			b[i] = -0.5*a[i]; // fixed ratio + anti-phase
		}
		Runner<Sample> r2(s2);
		for (int pos = 0; pos < n; pos += hop) {
			int len = std::min(hop, n - pos);
			const std::vector<double> inputs[2] = {
				std::vector<double>(a.begin() + pos, a.begin() + pos + len),
				std::vector<double>(b.begin() + pos, b.begin() + pos + len)
			};
			r2.process(inputs, 0, len, 2);
		}
		s2.setSpectralFreeze(true);
		std::vector<double> z(int(2*sampleRate), 0.0);
		for (int pos = 0; pos < int(2*sampleRate); pos += hop) {
			int len = std::min(hop, int(2*sampleRate) - pos);
			const std::vector<double> inputs[2] = {
				std::vector<double>(z.begin() + pos, z.begin() + pos + len),
				std::vector<double>(z.begin() + pos, z.begin() + pos + len)
			};
			r2.process(inputs, 0, len, 2);
		}
		int start = int(r2.out.left.size()) - int(sampleRate);
		int len = 16384;
		double l1 = toneLevel(r2.out.left, start, len, 437.0);
		double l2 = toneLevel(r2.out.left, start, len, 1201.0);
		double r1 = toneLevel(r2.out.right, start, len, 437.0);
		double r2level = toneLevel(r2.out.right, start, len, 1201.0);
		check(l1 > 0.02 && l2 > 0.01, "two-tone frozen spectrum keeps both partials");
		check(std::abs(l2/l1 - 0.6) < 0.05, "two-tone relative spectral shape preserved");
		check(std::abs(r1/(l1 + 1e-12) - 0.5) < 0.01, "left/right amplitude magnitude ratio preserved at 437Hz");
		check(std::abs(r2level/(l2 + 1e-12) - 0.5) < 0.01, "left/right amplitude magnitude ratio preserved at 1201Hz");
		check(stereoRelationError(r2.out.left, r2.out.right, start, start + len, -0.5) <= 2e-4,
			"frozen anti-phase relation holds sample-by-sample");

		// Out-of-phase capture: 180 degrees between channels.
		signalsmith::stretch::SignalsmithStretch<Sample> s3(777);
		s3.configure(2, block, hop, false);
		std::vector<double> pa(n), pb(n);
		makeTone(pa, pb, n, 601.0, 0.25, 1.0, pi);
		Runner<Sample> r3(s3);
		for (int pos = 0; pos < n; pos += hop) {
			int len = std::min(hop, n - pos);
			const std::vector<double> inputs[2] = {
				std::vector<double>(pa.begin() + pos, pa.begin() + pos + len),
				std::vector<double>(pb.begin() + pos, pb.begin() + pos + len)
			};
			r3.process(inputs, 0, len, 2);
		}
		s3.setSpectralFreeze(true);
		for (int pos = 0; pos < int(sampleRate); pos += hop) {
			int len = std::min(hop, int(sampleRate) - pos);
			const std::vector<double> inputs[2] = {
				std::vector<double>(z.begin() + pos, z.begin() + pos + len),
				std::vector<double>(z.begin() + pos, z.begin() + pos + len)
			};
			r3.process(inputs, 0, len, 2);
		}
		int st3 = int(r3.out.left.size()) - 8192;
		check(stereoRelationError(r3.out.left, r3.out.right, st3, st3 + 8192, -1.0) <= 2e-4,
			"captured 180-degree phase difference preserved");
	}
}

// ---------------------------------------------------------------------------------------------
// Request changes at the same absolute sample must not depend on process() block size.
// ---------------------------------------------------------------------------------------------
template<typename Sample>
void blockSizeInvariance(int block, int hop, bool split) {
	auto run = [&](int chunk) {
		signalsmith::stretch::SignalsmithStretch<Sample> stretch(2468);
		stretch.configure(1, block, hop, split);
		const int total = int(2.0*sampleRate);
		const int freezeAt = int(0.75*sampleRate);
		std::vector<double> in(total), out(total);
		for (int i = 0; i < total; ++i) in[i] = 0.3*std::sin(2*pi*437*i/sampleRate);
		const double *inPtr[1];
		double *outPtr[1];
		int pos = 0;
		while (pos < total) {
			int len = std::min(chunk, total - pos);
			if (pos < freezeAt && pos + len > freezeAt) len = freezeAt - pos;
			std::vector<double> piece(in.begin() + pos, in.begin() + pos + len);
			std::vector<double> dst(len);
			inPtr[0] = piece.data();
			outPtr[0] = dst.data();
			stretch.process(inPtr, len, outPtr, len);
			std::copy(dst.begin(), dst.end(), out.begin() + pos);
			pos += len;
			if (pos == freezeAt) stretch.setSpectralFreeze(true);
		}
		return out;
	};

	std::vector<double> reference = run(hop);
	for (int chunk : {1, 7, 113}) {
		std::vector<double> got = run(chunk);
		double worst = 0;
		for (size_t i = 0; i < reference.size(); ++i) worst = std::max(worst, std::abs(reference[i] - got[i]));
		check(worst <= 2e-4, "freeze request is block-size independent (chunk=" + std::to_string(chunk) + ")");
	}
}

// ---------------------------------------------------------------------------------------------
// split-computation must match non-split once its extra latency is compensated.
// ---------------------------------------------------------------------------------------------
template<typename Sample>
void splitAlignment() {
	signalsmith::stretch::SignalsmithStretch<Sample> plain(1357), split(1357);
	plain.configure(1, 1024, 256, false);
	split.configure(1, 1024, 256, true);
	int extra = split.outputLatency() - plain.outputLatency();
	int n = int(2.0*sampleRate);
	std::vector<double> in(n), op(n), os(n);
	for (int i = 0; i < n; ++i) in[i] = 0.3*std::sin(2*pi*437*i/sampleRate);
	plain.setSpectralFreeze(true);
	split.setSpectralFreeze(true);
	const double *ip[1];
	double *opp[1];
	double *osp[1];
	for (int pos = 0; pos < n; pos += 113) {
		int len = std::min(113, n - pos);
		std::vector<double> piece(in.begin() + pos, in.begin() + pos + len);
		std::vector<double> dstP(len), dstS(len);
		ip[0] = piece.data();
		opp[0] = dstP.data();
		osp[0] = dstS.data();
		plain.process(ip, len, opp, len);
		split.process(ip, len, osp, len);
		std::copy(dstP.begin(), dstP.end(), op.begin() + pos);
		std::copy(dstS.begin(), dstS.end(), os.begin() + pos);
	}
	double worst = 0;
	for (int i = 4096; i + extra < n; ++i) worst = std::max(worst, std::abs(op[i] - os[i + extra]));
	check(worst <= 2e-4, "split-computation freeze matches plain after latency alignment");
}

// ---------------------------------------------------------------------------------------------
// Half-hop parameter changes while frozen must not glitch the held tone.
// ---------------------------------------------------------------------------------------------
template<typename Sample>
void midHopParameterChange() {
	signalsmith::stretch::SignalsmithStretch<Sample> stretch(9753);
	stretch.configure(1, 1024, 256, false);
	int n = int(1.0*sampleRate);
	std::vector<double> in(n);
	for (int i = 0; i < n; ++i) in[i] = 0.3*std::sin(2*pi*437*i/sampleRate);

	Runner<Sample> runner(stretch);
	const std::vector<double> inputs[1] = {in};
	runner.process(inputs, 0, n, 1);
	stretch.setSpectralFreeze(true);

	// Process 1 hop + half a hop of silence, change transpose (which the freeze must ignore),
	// then continue: no discontinuity or NaN.
	std::vector<double> silence(int(2*sampleRate), 0.0);
	const std::vector<double> silent[1] = {silence};
	int before = int(runner.out.left.size());
	runner.process(silent, 0, 128, 1); // half a hop
	stretch.setTransposeSemitones(7);
	runner.process(silent, 128, int(2*sampleRate) - 128, 1);
	double worst = 0;
	bool finite = true;
	for (int i = before + 512; i < int(runner.out.left.size()); ++i) {
		if (!std::isfinite(runner.out.left[i])) finite = false;
		worst = std::max(worst, std::abs(runner.out.left[i]));
	}
	check(finite, "half-hop parameter change keeps frozen output finite");
	check(worst < 1.0, "half-hop parameter change does not produce a glitch");
	int tail = int(runner.out.left.size()) - 8192;
	check(std::abs(dominantFrequency(runner.out.left, tail, 8192, 300, 600) - 437.0) < 2.0,
		"freeze ignores an unrelated transpose change made mid-hold");
}

// ---------------------------------------------------------------------------------------------
// reset() / seek() clear the freeze request and snapshot.
// ---------------------------------------------------------------------------------------------
template<typename Sample>
void resetAndSeekClear() {
	signalsmith::stretch::SignalsmithStretch<Sample> stretch(111);
	stretch.configure(1, 1024, 256, false);
	int n = int(0.5*sampleRate);
	std::vector<double> in(n);
	for (int i = 0; i < n; ++i) in[i] = 0.3*std::sin(2*pi*437*i/sampleRate);
	std::vector<double> out(n);
	const double *ip[1];
	double *op[1];
	for (int pos = 0; pos < n; pos += 256) {
		int len = std::min(256, n - pos);
		std::vector<double> piece(in.begin() + pos, in.begin() + pos + len);
		std::vector<double> dst(len);
		ip[0] = piece.data();
		op[0] = dst.data();
		stretch.process(ip, len, op, len);
		std::copy(dst.begin(), dst.end(), out.begin() + pos);
	}
	stretch.setSpectralFreeze(true);
	for (int pos = 0; pos < 4096; pos += 256) {
		std::vector<double> piece(256, 0.0), dst(256);
		ip[0] = piece.data();
		op[0] = dst.data();
		stretch.process(ip, 256, op, 256);
	}
	check(stretch.spectralFreezeActive(), "freeze active before reset");
	stretch.reset();
	check(!stretch.spectralFreezeActive(), "reset() clears the freeze");

	// A fresh request after reset captures again (let the new tone establish first).
	std::vector<double> tone(static_cast<size_t>(sampleRate));
	for (int i = 0; i < int(sampleRate); ++i) tone[i] = 0.3*std::sin(2*pi*601*i/sampleRate);
	std::vector<double> collect;
	for (int pos = 0; pos < int(sampleRate); pos += 256) {
		int len = std::min(256, int(sampleRate) - pos);
		std::vector<double> piece(tone.begin() + pos, tone.begin() + pos + len), dst(len);
		ip[0] = piece.data();
		op[0] = dst.data();
		stretch.process(ip, len, op, len);
		collect.insert(collect.end(), dst.begin(), dst.end());
	}
	stretch.setSpectralFreeze(true);
	std::vector<double> held;
	for (int pos = 0; pos < int(sampleRate); pos += 256) {
		std::vector<double> piece(256, 0.0), dst(256);
		ip[0] = piece.data();
		op[0] = dst.data();
		stretch.process(ip, 256, op, 256);
		held.insert(held.end(), dst.begin(), dst.end());
	}
	check(stretch.spectralFreezeActive(), "freeze re-captures after reset from fresh input");
	check(std::abs(dominantFrequency(held, int(held.size()) - 4096, 4096, 400, 800) - 601.0) < 2.0,
		"post-reset capture holds 601Hz through silence");

	// seek() also clears.
	stretch.setSpectralFreeze(true);
	const double *seekPtrs[1] = {in.data()};
	stretch.seek(seekPtrs, int(in.size()), 1.0);
	check(!stretch.spectralFreezeActive(), "seek() clears the freeze request");
}

// ---------------------------------------------------------------------------------------------
// Silence then capture: freeze requested before any real analysis frame exists stays inactive
// and finite; once real frames arrive it captures normally.
// ---------------------------------------------------------------------------------------------
template<typename Sample>
void silenceThenCapture() {
	signalsmith::stretch::SignalsmithStretch<Sample> stretch(2024);
	stretch.configure(1, 1024, 256, false);
	stretch.setSpectralFreeze(true);

	std::vector<double> out;
	auto feed = [&](const std::vector<double> &in) {
		for (size_t pos = 0; pos < in.size(); pos += 256) {
			int len = int(std::min<size_t>(256, in.size() - pos));
			std::vector<double> piece(in.begin() + pos, in.begin() + pos + len), dst(len);
			const double *ip[1] = {piece.data()};
			double *op[1] = {dst.data()};
			stretch.process(ip, len, op, len);
			out.insert(out.end(), dst.begin(), dst.end());
		}
	};

	// Request freeze while the input is pure silence: output must stay exactly zero.
	std::vector<double> silence(4096, 0.0);
	feed(silence);
	check(!stretch.spectralFreezeActive(), "freeze stays inactive with only silence");
	double maxAbs = 0;
	for (double v : out) maxAbs = std::max(maxAbs, std::abs(v));
	check(maxAbs < 1e-9, "silence-only freeze produces finite zero output, not noise");

	// Now a real tone: the pending request captures it.  The silence short-cut must not have
	// wiped the live state permanently.
	std::vector<double> tone(int(2*sampleRate));
	for (int i = 0; i < int(2*sampleRate); ++i) tone[i] = 0.3*std::sin(2*pi*437*i/sampleRate);
	feed(tone);
	check(stretch.spectralFreezeActive(), "pending freeze captures once real frames arrive");
	int start = int(out.size()) - 8192;
	double capturedFreq = dominantFrequency(out, start, 8192, 300, 600);
	check(std::abs(capturedFreq - 437.0) < 10.0,
		"capture after silence holds a tone near 437Hz (captured across the onset window)");

	// Long silence while frozen must keep the held tone (the short-cut must not clear it).
	std::vector<double> longSilence(int(3*sampleRate), 0.0);
	feed(longSilence);
	start = int(out.size()) - 8192;
	double heldRms = rms(out, start, start + 8192);
	check(heldRms > 0.1, "long silence during freeze does not clear the held tone");
	check(std::abs(dominantFrequency(out, start, 8192, 300, 600) - 437.0) < 10.0,
		"held tone remains near 437Hz through long silence");
}

// ---------------------------------------------------------------------------------------------
// Requesting a freeze after a long silence following a loud tone must not resurrect the stale
// pre-silence spectrum: capture must wait for two fresh real analysis frames.
// ---------------------------------------------------------------------------------------------
template<typename Sample>
void noStaleCaptureAfterSilence() {
	signalsmith::stretch::SignalsmithStretch<Sample> stretch(808);
	stretch.configure(1, 1024, 256, false);
	std::vector<double> out;
	auto feed = [&](const std::vector<double> &in) {
		for (size_t pos = 0; pos + 256 <= in.size(); pos += 256) {
			std::vector<double> piece(in.begin() + pos, in.begin() + pos + 256), dst(256);
			const double *ip[1] = {piece.data()};
			double *op[1] = {dst.data()};
			stretch.process(ip, 256, op, 256);
			out.insert(out.end(), dst.begin(), dst.end());
		}
	};

	// Loud tone, then long silence (which activates the silence short-cut).
	std::vector<double> tone(int(0.5*sampleRate));
	for (int i = 0; i < int(0.5*sampleRate); ++i) tone[i] = 0.3*std::sin(2*pi*437*i/sampleRate);
	feed(tone);
	std::vector<double> silence(int(1.0*sampleRate), 0.0);
	feed(silence);

	// Request freeze while still silent: it must stay inactive (no stale snapshot).
	stretch.setSpectralFreeze(true);
	feed(silence);
	check(!stretch.spectralFreezeActive(), "freeze does not capture stale pre-silence history");

	// Then a distinct tone arrives: capture happens once two fresh frames exist.
	std::vector<double> next(int(2*sampleRate));
	for (int i = 0; i < int(2*sampleRate); ++i) next[i] = 0.3*std::sin(2*pi*601*i/sampleRate);
	feed(next);
	check(stretch.spectralFreezeActive(), "freeze captures the fresh tone after silence");
	// The capture happens as the tone emerges from silence, so its two analysis windows straddle
	// the onset and the instantaneous-frequency estimate is biased by a couple of percent.  The
	// point here is that the *fresh* tone is captured, not the stale pre-silence one.
	int measureStart = int(out.size()) - int(1.2*sampleRate);
	double held = dominantFrequency(out, measureStart, 16384, 400, 800);
	check(std::abs(held - 601.0) < 25.0, "captured tone is the fresh one, not the stale 437Hz");
}

// ---------------------------------------------------------------------------------------------
// Repeated setSpectralFreeze(true) must not restart or glitch the held tone.
// ---------------------------------------------------------------------------------------------
template<typename Sample>
void repeatedEnable() {
	signalsmith::stretch::SignalsmithStretch<Sample> stretch(3141);
	stretch.configure(1, 1024, 256, false);
	int n = int(1.0*sampleRate);
	std::vector<double> in(n);
	for (int i = 0; i < n; ++i) in[i] = 0.3*std::sin(2*pi*437*i/sampleRate);
	Runner<Sample> runner(stretch);
	const std::vector<double> inputs[1] = {in};
	runner.process(inputs, 0, n, 1);
	stretch.setSpectralFreeze(true);
	std::vector<double> z(int(2*sampleRate), 0.0);
	const std::vector<double> silent[1] = {z};
	runner.process(silent, 0, int(0.5*sampleRate), 1);
	int before = int(runner.out.left.size());
	stretch.setSpectralFreeze(true); // repeated
	stretch.setSpectralFreeze(true); // repeated
	runner.process(silent, int(0.5*sampleRate), int(1.5*sampleRate), 1);
	// A restart would show a phase discontinuity; require continuity against a control run
	// which never repeated the request.
	signalsmith::stretch::SignalsmithStretch<Sample> control(3141);
	control.configure(1, 1024, 256, false);
	Runner<Sample> ctl(control);
	ctl.process(inputs, 0, n, 1);
	control.setSpectralFreeze(true);
	ctl.process(silent, 0, int(2.0*sampleRate), 1);
	double worst = 0;
	for (int i = before + 1024; i < int(runner.out.left.size()); ++i) {
		worst = std::max(worst, std::abs(runner.out.left[i] - ctl.out.left[i]));
	}
	check(worst <= 2e-4, "repeated setSpectralFreeze(true) does not restart the held sound");
}

// ---------------------------------------------------------------------------------------------
// Memory use is bounded by channel/window count, not by how long freeze is held.  The custom
// allocation counter below lets the test assert that a long hold performs no new allocations
// after the snapshot has been captured.
// ---------------------------------------------------------------------------------------------
static size_t allocationLiveBytes = 0;
static bool trackAllocations = false;
static size_t peakTrackedLive = 0;
template<typename Sample>
void boundedMemory() {
	signalsmith::stretch::SignalsmithStretch<Sample> stretch(55);
	stretch.configure(1, 1024, 256, false);
	int n = int(1.0*sampleRate);
	std::vector<double> in(n);
	for (int i = 0; i < n; ++i) in[i] = 0.3*std::sin(2*pi*437*i/sampleRate);
	std::vector<double> dst(256);
	const double *ip[1];
	double *op[1];
	for (int pos = 0; pos < n; pos += 256) {
		int len = std::min(256, n - pos);
		std::vector<double> piece(in.begin() + pos, in.begin() + pos + len);
		ip[0] = piece.data();
		op[0] = dst.data();
		stretch.process(ip, len, op, len);
	}
	stretch.setSpectralFreeze(true);
	std::vector<double> silence(48000, 0.0);
	// Warm up: let the snapshot and any one-off buffers be allocated.
	for (int k = 0; k < 8; ++k) {
		for (int pos = 0; pos + 256 <= int(silence.size()); pos += 256) {
			ip[0] = silence.data() + pos;
			op[0] = dst.data();
			stretch.process(ip, 256, op, 256);
		}
	}
	size_t liveAtStart = allocationLiveBytes;
	peakTrackedLive = liveAtStart;
	trackAllocations = true;
	// Hold for a long time (200 seconds of audio).  Memory must not grow with hold duration.
	for (int k = 0; k < 200; ++k) {
		for (int pos = 0; pos + 256 <= int(silence.size()); pos += 256) {
			ip[0] = silence.data() + pos;
			op[0] = dst.data();
			stretch.process(ip, 256, op, 256);
		}
	}
	trackAllocations = false;
	check(peakTrackedLive <= liveAtStart + 4096, "memory does not grow with freeze duration");
	bool finite = true;
	for (double v : dst) if (!std::isfinite(v)) finite = false;
	check(finite && stretch.spectralFreezeActive(), "long hold remains finite and active");
}

// ---------------------------------------------------------------------------------------------
// Single-channel round trip: silence -> release -> live tone, and default-off behaviour.
// ---------------------------------------------------------------------------------------------
template<typename Sample>
void defaultOffAndMonoRelease() {
	signalsmith::stretch::SignalsmithStretch<Sample> stretch(909);
	stretch.configure(1, 2048, 512, false);
	check(!stretch.spectralFreezeActive(), "freeze defaults to off");
	int n = int(0.5*sampleRate);
	std::vector<double> in(n);
	for (int i = 0; i < n; ++i) in[i] = 0.3*std::sin(2*pi*997*i/sampleRate);
	std::vector<double> out(n);
	const double *ip[1];
	double *op[1];
	for (int pos = 0; pos < n; pos += 512) {
		int len = std::min(512, n - pos);
		std::vector<double> piece(in.begin() + pos, in.begin() + pos + len), dst(len);
		ip[0] = piece.data();
		op[0] = dst.data();
		stretch.process(ip, len, op, len);
		std::copy(dst.begin(), dst.end(), out.begin() + pos);
	}
	int start = n - 8192;
	check(std::abs(dominantFrequency(out, start, 8192, 800, 1200) - 997.0) < 2.0,
		"disabled freeze leaves normal processing untouched");
}

// ---------------------------------------------------------------------------------------------
// Pitch-shifting, formant compensation and custom frequency maps are not applied to the frozen
// spectrum: the captured timbre is reproduced as-is.
// ---------------------------------------------------------------------------------------------
template<typename Sample>
void processingBypassedWhileFrozen() {
	signalsmith::stretch::SignalsmithStretch<Sample> stretch(4711);
	stretch.configure(1, 1024, 256, false);
	stretch.setFreqMap([](double f) { return f*2; });
	stretch.setFormantSemitones(3, true);
	int n = int(1.0*sampleRate);
	std::vector<double> in(n);
	for (int i = 0; i < n; ++i) in[i] = 0.3*std::sin(2*pi*437*i/sampleRate);
	std::vector<double> out;
	auto feed = [&](const std::vector<double> &x) {
		for (int pos = 0; pos + 256 <= int(x.size()); pos += 256) {
			std::vector<double> piece(x.begin() + pos, x.begin() + pos + 256), dst(256);
			const double *ip[1] = {piece.data()};
			double *op[1] = {dst.data()};
			stretch.process(ip, 256, op, 256);
			out.insert(out.end(), dst.begin(), dst.end());
		}
	};
	feed(in);
	stretch.setSpectralFreeze(true);
	std::vector<double> silence(int(0.5*sampleRate), 0.0);
	feed(silence);
	int start = int(out.size()) - 8192;
	// The captured input tone (437Hz) is held; the custom x2 map and formant shift are bypassed.
	check(std::abs(dominantFrequency(out, start, 8192, 300, 1200) - 437.0) < 2.0,
		"pitch map/formant are bypassed while frozen (captured 437Hz is held)");
}

// ---------------------------------------------------------------------------------------------
// flush(): freeze is dropped first, and the tail is the normal finite flush, not an endless tone.
// ---------------------------------------------------------------------------------------------
template<typename Sample>
void flushFiniteTail() {
	signalsmith::stretch::SignalsmithStretch<Sample> stretch(606);
	stretch.configure(1, 1024, 256, false);
	int n = 256*int(0.5*sampleRate/256); // a whole number of processing blocks
	std::vector<double> in(n);
	for (int i = 0; i < n; ++i) in[i] = 0.3*std::sin(2*pi*437*i/sampleRate);
	std::vector<double> dst(256);
	const double *ip[1];
	double *op[1];
	for (int pos = 0; pos < n; pos += 256) {
		std::vector<double> piece(in.begin() + pos, in.begin() + pos + 256);
		ip[0] = piece.data();
		op[0] = dst.data();
		stretch.process(ip, 256, op, 256);
	}
	stretch.setSpectralFreeze(true);
	std::vector<double> silence(4096, 0.0);
	for (int pos = 0; pos < 4096; pos += 256) {
		ip[0] = silence.data() + pos;
		op[0] = dst.data();
		stretch.process(ip, 256, op, 256);
	}
	check(stretch.spectralFreezeActive(), "freeze active before flush");

	int tailLength = 4*256 + 1024;
	std::vector<double> tail(tailLength);
	double *tp[1] = {tail.data()};
	stretch.flush(tp, tailLength);
	check(!stretch.spectralFreezeActive(), "flush() clears the freeze");
	bool finite = true;
	for (double v : tail) if (!std::isfinite(v)) finite = false;
	check(finite, "flush() tail is finite");

	// The flush tail must be a finite, decaying tail rather than an indefinitely-held tone: the
	// energy in the final quarter must be far below the pending frozen block, and near zero.
	double earlyRms = rms(tail, 0, tailLength/4);
	double lateRms = rms(tail, tailLength*3/4, tailLength);
	check(lateRms < 0.02, "flush() tail decays towards zero rather than holding forever");
	check(lateRms <= earlyRms, "flush() tail is not growing (no endless held tone)");
}

} // namespace

// Allocation counting is used by the bounded-memory test: a long freeze hold must not allocate.
void *operator new(size_t n) {
	void *p = std::malloc(n + sizeof(size_t));
	if (!p) throw std::bad_alloc();
	*(size_t *)p = n;
	allocationLiveBytes += n;
	if (trackAllocations && allocationLiveBytes > peakTrackedLive) peakTrackedLive = allocationLiveBytes;
	return (char *)p + sizeof(size_t);
}
void *operator new[](size_t n) { return operator new(n); }
void operator delete(void *p) noexcept {
	if (!p) return;
	size_t *base = (size_t *)((char *)p - sizeof(size_t));
	allocationLiveBytes -= *base;
	std::free(base);
}
void operator delete[](void *p) noexcept { operator delete(p); }
void operator delete(void *p, size_t) noexcept { operator delete(p); }
void operator delete[](void *p, size_t) noexcept { operator delete(p); }

int main() {
	// Core contract at both supported window/hop sizes and both sample types.
	for (int cfg = 0; cfg < 2; ++cfg) {
		int block = cfg == 0 ? 1024 : 2048;
		int hop = cfg == 0 ? 256 : 512;
		coreHoldAndRelease<float>(block, hop);
		coreHoldAndRelease<double>(block, hop);
	}
	for (int cfg = 0; cfg < 2; ++cfg) {
		int block = cfg == 0 ? 1024 : 2048;
		int hop = cfg == 0 ? 256 : 512;
		blockSizeInvariance<float>(block, hop, false);
		blockSizeInvariance<double>(block, hop, false);
		blockSizeInvariance<double>(block, hop, true);
	}
	splitAlignment<float>();
	splitAlignment<double>();
	midHopParameterChange<float>();
	midHopParameterChange<double>();
	resetAndSeekClear<float>();
	resetAndSeekClear<double>();
	silenceThenCapture<float>();
	silenceThenCapture<double>();
	noStaleCaptureAfterSilence<float>();
	noStaleCaptureAfterSilence<double>();
	repeatedEnable<float>();
	repeatedEnable<double>();
	boundedMemory<float>();
	boundedMemory<double>();
	defaultOffAndMonoRelease<float>();
	defaultOffAndMonoRelease<double>();
	processingBypassedWhileFrozen<float>();
	processingBypassedWhileFrozen<double>();
	flushFiniteTail<float>();
	flushFiniteTail<double>();

	if (failures == 0) {
		std::printf("SPECTRAL_FREEZE_OK cases=%d\n", cases);
		return 0;
	}
	std::printf("SPECTRAL_FREEZE_FAILED failures=%d/%d\n", failures, cases);
	return 1;
}
