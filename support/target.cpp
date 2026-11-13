// Task tests for the ChorusEffectsSend routing and the built-in chorus effect.
// The file defines TSF_IMPLEMENTATION itself and has no main() (runner.cpp provides it).
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>
#include <string>

// Count every heap operation so the lifetime case can prove that rendering
// never allocates. The overrides are installed before the implementation is
// compiled and are used through TSF's own TSF_MALLOC/REALLOC/FREE hooks.
static long g_test_malloc = 0, g_test_realloc = 0, g_test_free = 0;
extern "C" {
void* t_malloc(size_t n) { ++g_test_malloc; return std::malloc(n); }
void* t_realloc(void* p, size_t n) { ++g_test_realloc; return std::realloc(p, n); }
void  t_free(void* p) { ++g_test_free; std::free(p); }
}
#define TSF_MALLOC  t_malloc
#define TSF_REALLOC t_realloc
#define TSF_FREE    t_free

#define TSF_IMPLEMENTATION
#include "tsf.h"
#include "check.h"

// -------------------------------------------------------------------------------------------------
// Minimal in-memory SoundFont2 builder.
// Only the hydra chunks and a raw 16-bit PCM smpl chunk are emitted, which is
// all tsf_load_memory needs for the tiny fonts used below.
// -------------------------------------------------------------------------------------------------
struct Sf2Writer
{
	std::vector<unsigned char> data;

	void u8(unsigned char v) { data.push_back(v); }
	void u16(unsigned short v) { data.push_back((unsigned char)(v & 0xFF)); data.push_back((unsigned char)(v >> 8)); }
	void s16(short v) { u16((unsigned short)v); }
	void u32(unsigned int v) { for (int i = 0; i < 4; i++) data.push_back((unsigned char)((v >> (8 * i)) & 0xFF)); }
	void four(const char* id) { for (int i = 0; i < 4; i++) data.push_back((unsigned char)id[i]); }
	void name20(const char* s)
	{
		int n = 0;
		for (; s[n] && n < 20; n++) data.push_back((unsigned char)s[n]);
		for (; n < 20; n++) data.push_back(0);
	}
	void raw(const void* p, size_t n)
	{
		const unsigned char* b = (const unsigned char*)p;
		data.insert(data.end(), b, b + n);
	}
	std::vector<unsigned char> chunk(const char* id, const std::vector<unsigned char>& body) const
	{
		std::vector<unsigned char> out;
		auto push = [&](unsigned char c) { out.push_back(c); };
		for (int i = 0; i < 4; i++) push((unsigned char)id[i]);
		unsigned int sz = (unsigned int)body.size();
		for (int i = 0; i < 4; i++) push((unsigned char)((sz >> (8 * i)) & 0xFF));
		out.insert(out.end(), body.begin(), body.end());
		if (sz & 1) out.push_back(0); // RIFF words are aligned to two bytes
		return out;
	}
	std::vector<unsigned char> list(const char* id, const std::vector<unsigned char>& body) const
	{
		std::vector<unsigned char> inner;
		for (int i = 0; i < 4; i++) inner.push_back((unsigned char)id[i]);
		inner.insert(inner.end(), body.begin(), body.end());
		return chunk("LIST", inner);
	}
	void finish()
	{
		std::vector<unsigned char> out;
		auto push = [&](unsigned char c) { out.push_back(c); };
		for (int i = 0; i < 4; i++) push((unsigned char)"RIFF"[i]);
		unsigned int sz = 4 + (unsigned int)data.size();
		for (int i = 0; i < 4; i++) push((unsigned char)((sz >> (8 * i)) & 0xFF));
		for (int i = 0; i < 4; i++) push((unsigned char)"sfbk"[i]);
		out.insert(out.end(), data.begin(), data.end());
		data.swap(out);
	}
};

// hydra record helpers (all little-endian, matches the structs tsf reads)
static void put_phdr(std::vector<unsigned char>& b, const char* name, unsigned short preset, unsigned short bank, unsigned short bagNdx)
{
	Sf2Writer w; w.name20(name); w.u16(preset); w.u16(bank); w.u16(bagNdx); w.u32(0); w.u32(0); w.u32(0);
	b.insert(b.end(), w.data.begin(), w.data.end());
}
static void put_bag(std::vector<unsigned char>& b, unsigned short genNdx, unsigned short modNdx)
{
	Sf2Writer w; w.u16(genNdx); w.u16(modNdx);
	b.insert(b.end(), w.data.begin(), w.data.end());
}
static void put_mod_term(std::vector<unsigned char>& b)
{
	Sf2Writer w; w.u16(0); w.u16(0); w.s16(0); w.u16(0); w.u16(0);
	b.insert(b.end(), w.data.begin(), w.data.end());
}
static void put_gen(std::vector<unsigned char>& b, unsigned short oper, short amount)
{
	Sf2Writer w; w.u16(oper); w.s16(amount);
	b.insert(b.end(), w.data.begin(), w.data.end());
}
static void put_gen_range(std::vector<unsigned char>& b, unsigned short oper, unsigned char lo, unsigned char hi)
{
	Sf2Writer w; w.u16(oper); w.u8(lo); w.u8(hi);
	b.insert(b.end(), w.data.begin(), w.data.end());
}
static void put_inst(std::vector<unsigned char>& b, const char* name, unsigned short bagNdx)
{
	Sf2Writer w; w.name20(name); w.u16(bagNdx);
	b.insert(b.end(), w.data.begin(), w.data.end());
}
static void put_shdr(std::vector<unsigned char>& b, const char* name, unsigned int start, unsigned int end,
	unsigned int loopStart, unsigned int loopEnd, unsigned int sampleRate, unsigned char rootPitch, unsigned short type)
{
	Sf2Writer w;
	w.name20(name); w.u32(start); w.u32(end); w.u32(loopStart); w.u32(loopEnd); w.u32(sampleRate);
	w.u8(rootPitch); w.u8(0); w.u16(0); w.u16(type);
	b.insert(b.end(), w.data.begin(), w.data.end());
}

// Build a font:
//  preset 0 -> preset global chorus 100, preset local chorus 250
//  preset 1 -> preset local chorus 800 (exercises the 0..1000 clamp)
//  instrument global chorus 100; three zones with key ranges 0-59 / 60 / 61-127
//    zone A local chorus 200 pan -300, zone B local chorus 500 pan +100,
//    zone C has no local chorus (inherits the instrument global) pan 0
//  constant or ramp PCM controlled by constantSample.
static std::vector<unsigned char> build_routing_font(int outRate, bool constantSample, int sampleCount)
{
	Sf2Writer w;
	std::vector<unsigned char> pdta;

	// phdr: two presets plus terminal record (pbag layout: g0, l0, l1, terminal)
	std::vector<unsigned char> phdr;
	put_phdr(phdr, "P0", 0, 0, 0);
	put_phdr(phdr, "P1", 1, 0, 2);
	put_phdr(phdr, "EOS", 0, 0, 3);
	// pbag + pgen. Generators are emitted in ascending genOper order so the zone
	// values are applied before the Instrument generator (41) finalizes the zone.
	std::vector<unsigned char> pbag, pgen;
	put_bag(pbag, 0, 0);             // P0 global zone
	put_bag(pbag, 1, 0);             // P0 local zone
	put_bag(pbag, 3, 0);             // P1 zone
	put_bag(pbag, 6, 0);             // terminal
	put_gen(pgen, 15, 100);          // P0 global ChorusEffectsSend
	put_gen(pgen, 15, 250);          // P0 local ChorusEffectsSend (before instrument!)
	put_gen(pgen, 41, 0);            // P0 local: instrument 0
	put_gen(pgen, 15, 800);          // P1 ChorusEffectsSend (before instrument!)
	put_gen(pgen, 41, 0);            // P1: instrument 0
	put_gen(pgen, 0, 0);             // terminal
	std::vector<unsigned char> pmod; put_mod_term(pmod);

	// inst: one instrument plus terminal (ibag layout: global, A, B, C, terminal)
	std::vector<unsigned char> inst;
	put_inst(inst, "I0", 0);
	put_inst(inst, "EOI", 4);
	std::vector<unsigned char> ibag;
	put_bag(ibag, 0, 0); put_bag(ibag, 1, 0); put_bag(ibag, 5, 0); put_bag(ibag, 9, 0); put_bag(ibag, 12, 0);
	std::vector<unsigned char> imod; put_mod_term(imod);
	std::vector<unsigned char> igen;
	put_gen(igen, 15, 100);                       // instrument global ChorusEffectsSend
	put_gen(igen, 15, 200);                       // A local chorus (overrides global)
	put_gen(igen, 17, -300);                      // A pan
	put_gen_range(igen, 43, 0, 59);               // A key range
	put_gen(igen, 53, 0);                         // A sample id (finalizes the zone)
	put_gen(igen, 15, 500);                       // B local chorus
	put_gen(igen, 17, 100);                       // B pan
	put_gen_range(igen, 43, 60, 60);              // B key range
	put_gen(igen, 53, 0);                         // B sample id
	put_gen_range(igen, 43, 61, 127);             // C key range
	put_gen(igen, 17, 0);                         // C pan (no chorus gen -> inherits global)
	put_gen(igen, 53, 0);                         // C sample id
	put_gen(igen, 0, 0);                          // terminal

	std::vector<unsigned char> shdr;
	put_shdr(shdr, "S", 0, sampleCount, 0, 0, 32u * (unsigned int)outRate, 60, 1);
	put_shdr(shdr, "EOS", 0, 0, 0, 0, 0, 0, 0);

	auto add = [&](const char* id, const std::vector<unsigned char>& b) {
		std::vector<unsigned char> c = w.chunk(id, b);
		pdta.insert(pdta.end(), c.begin(), c.end());
	};
	add("phdr", phdr); add("pbag", pbag); add("pmod", pmod); add("pgen", pgen);
	add("inst", inst); add("ibag", ibag); add("imod", imod); add("igen", igen); add("shdr", shdr);
	std::vector<unsigned char> pdtaList = w.list("pdta", pdta);

	// sdta smpl: 16-bit PCM
	std::vector<unsigned char> smpl;
	smpl.reserve(sampleCount * 2);
	for (int k = 0; k < sampleCount; k++)
	{
		short v;
		if (constantSample) v = 32767;
		else v = (short)((k - sampleCount / 2) * 32); // linear ramp, centered around zero
		Sf2Writer s; s.s16(v);
		smpl.insert(smpl.end(), s.data.begin(), s.data.end());
	}
	std::vector<unsigned char> smplChunk = w.chunk("smpl", smpl);
	std::vector<unsigned char> sdtaList = w.list("sdta", smplChunk);

	w.data.insert(w.data.end(), pdtaList.begin(), pdtaList.end());
	w.data.insert(w.data.end(), sdtaList.begin(), sdtaList.end());
	w.finish();
	return w.data;
}

// -------------------------------------------------------------------------------------------------
// Independent double-precision reference chorus using plain (growing) history
// arrays with absolute sample indices. No production delay helper is used.
// -------------------------------------------------------------------------------------------------
struct RefChorus
{
	int sampleRate;
	double D, A, rate, feedback, wetGain;
	std::vector<double> histL, histR;

	RefChorus(int sr, double dFrames, double aFrames, double rateHz, double fb, double wet)
		: sampleRate(sr), D(dFrames), A(aFrames), rate(rateHz), feedback(fb), wetGain(wet) {}

	static double readHist(const std::vector<double>& h, long index)
	{
		if (index < 0) return 0.0;
		return h[(size_t)index];
	}

	// Processes send[0..N) and returns wet L/R (history keeps growing across calls).
	void process(const std::vector<double>& send, std::vector<double>& wetL, std::vector<double>& wetR)
	{
		long base = (long)histL.size();
		size_t N = send.size();
		histL.resize(base + N, 0.0);
		histR.resize(base + N, 0.0);
		wetL.resize(N); wetR.resize(N);
		for (size_t n = 0; n < N; n++)
		{
			long absN = base + (long)n;
			double phase = 2.0 * TSF_PI * rate * (double)absN / (double)sampleRate;
			double mod = std::sin(phase) * A;
			double dL = D + mod, dR = D - mod;

			double iLd = (double)absN - dL, iRd = (double)absN - dR;
			long iL0 = (long)std::floor(iLd), iR0 = (long)std::floor(iRd);
			double fL = iLd - (double)iL0, fR = iRd - (double)iR0;
			double yL = readHist(histL, iL0) * (1.0 - fL) + readHist(histL, iL0 + 1) * fL;
			double yR = readHist(histR, iR0) * (1.0 - fR) + readHist(histR, iR0 + 1) * fR;

			histL[absN] = send[n] + feedback * yR; // cross feedback: L write uses R read
			histR[absN] = send[n] + feedback * yL;
			wetL[n] = wetGain * yL;
			wetR[n] = wetGain * yR;
		}
	}
};

static bool approx(double a, double b, double eps)
{
	double d = std::fabs(a - b);
	return d <= eps || d <= eps * (1.0 + std::fabs(a) + std::fabs(b));
}

static tsf* load_font(const std::vector<unsigned char>& font)
{
	tsf* f = tsf_load_memory(font.data(), (int)font.size());
	CHECK(f != TSF_NULL);
	return f;
}

// Render one voice of a preset and return the per-frame mono bus signal by
// rendering the same sequence on a chorus-disabled instance in MONO mode.
static std::vector<double> capture_mono_send(const std::vector<unsigned char>& font, int outRate,
	int presetIndex, int key, int frames)
{
	tsf* f = load_font(font);
	tsf_set_output(f, TSF_MONO, outRate, 0);
	CHECK(tsf_note_on(f, presetIndex, key, 1.0f) == 1);
	std::vector<float> out(frames, 0.0f);
	tsf_render_float(f, out.data(), frames, 0);
	std::vector<double> send(frames);
	for (int i = 0; i < frames; i++) send[i] = out[i];
	tsf_close(f);
	return send;
}

// -------------------------------------------------------------------------------------------------
// routing: generator inheritance/override/clamp, pre-pan mono send, single bus
// advancement, and layout equivalence, all through real tsf_load_memory/render.
// -------------------------------------------------------------------------------------------------
CASE(routing)
{
	const int outRate = 8000, frames = 64, delayFrames = 4;
	std::vector<unsigned char> font = build_routing_font(outRate, true, outRate);
	const float delayMs = (float)delayFrames / (float)outRate * 1000.0f;
	const float wet = 1.0f;

	// Expected merged factors: preset local overrides its global (P0: 250),
	// instrument local overrides its global; layers add and clamp to 1000.
	const struct { int preset, key; double factor; } expects[] =
	{
		{ 0, 48, 0.45 }, // P0 250 + inst-A local 200
		{ 0, 60, 0.75 }, // P0 250 + inst-B local 500
		{ 0, 72, 0.35 }, // P0 250 + inst-C inherits inst global 100
		{ 1, 48, 1.00 }, // P1 800 + 200 = 1000
		{ 1, 60, 1.00 }, // P1 800 + 500 = 1300 -> clamped 1000
		{ 1, 72, 0.90 }, // P1 800 + inst global 100
	};

	for (size_t t = 0; t < sizeof(expects) / sizeof(expects[0]); t++)
	{
		int preset = expects[t].preset, key = expects[t].key;
		double wantFactor = expects[t].factor;

		// Dry render (chorus disabled) in every layout.
		tsf* dry = load_font(font);
		tsf_set_output(dry, TSF_STEREO_INTERLEAVED, outRate, 0);
		CHECK(tsf_note_on(dry, preset, key, 1.0f) == 1);
		std::vector<float> dryIL(frames * 2, 0.0f);
		tsf_render_float(dry, dryIL.data(), frames, 0);
		tsf_close(dry);

		tsf* dryU = load_font(font);
		tsf_set_output(dryU, TSF_STEREO_UNWEAVED, outRate, 0);
		CHECK(tsf_note_on(dryU, preset, key, 1.0f) == 1);
		std::vector<float> dryUW(frames * 2, 0.0f);
		tsf_render_float(dryU, dryUW.data(), frames, 0);
		tsf_close(dryU);

		// Pre-pan mono signal of the same voice.
		std::vector<double> mono = capture_mono_send(font, outRate, preset, key, frames);

		// Chorus render: integer delay, no modulation/feedback makes the reference trivial.
		tsf* wetf = load_font(font);
		tsf_set_output(wetf, TSF_STEREO_INTERLEAVED, outRate, 0);
		CHECK(tsf_set_chorus(wetf, delayMs, 0.0f, 0.0f, 0.0f, wet) == 1);
		CHECK(tsf_note_on(wetf, preset, key, 1.0f) == 1);
		std::vector<float> wetIL(frames * 2, 0.0f);
		tsf_render_float(wetf, wetIL.data(), frames, 0);

		tsf* wetU = load_font(font);
		tsf_set_output(wetU, TSF_STEREO_UNWEAVED, outRate, 0);
		CHECK(tsf_set_chorus(wetU, delayMs, 0.0f, 0.0f, 0.0f, wet) == 1);
		CHECK(tsf_note_on(wetU, preset, key, 1.0f) == 1);
		std::vector<float> wetUW(frames * 2, 0.0f);
		tsf_render_float(wetU, wetUW.data(), frames, 0);

		tsf* wetM = load_font(font);
		tsf_set_output(wetM, TSF_MONO, outRate, 0);
		CHECK(tsf_set_chorus(wetM, delayMs, 0.0f, 0.0f, 0.0f, wet) == 1);
		CHECK(tsf_note_on(wetM, preset, key, 1.0f) == 1);
		std::vector<float> wetMono(frames, 0.0f);
		tsf_render_float(wetM, wetMono.data(), frames, 0);

		double observedSum = 0.0; int observedN = 0;
		for (int n = 0; n < frames; n++)
		{
			double refWet = (n >= delayFrames ? mono[n - delayFrames] * wantFactor : 0.0);

			// Wet is tapped before panning: both channels carry the same mono send.
			float gotWetL = wetIL[n * 2] - dryIL[n * 2];
			float gotWetR = wetIL[n * 2 + 1] - dryIL[n * 2 + 1];
			CHECK(approx(gotWetL, refWet, 1e-6));
			CHECK(approx(gotWetR, refWet, 1e-6));
			// Dry path is untouched by the send.
			CHECK(wetIL[n * 2] == dryIL[n * 2] + gotWetL);

			// Unweaved layout sounds identical to interleaved.
			CHECK(approx(wetUW[n], wetIL[n * 2], 0.0));
			CHECK(approx(wetUW[frames + n], wetIL[n * 2 + 1], 0.0));
			// Mono wet is the average of the two stereo wet channels (which are equal here).
			tsf* dryMf = TSF_NULL; (void)dryMf;
			CHECK(approx(wetMono[n], mono[n] + refWet, 1e-6));

			if (n >= delayFrames + 4 && mono[n - delayFrames] > 0.5) { observedSum += gotWetL / mono[n - delayFrames]; observedN++; }
		}
		CHECK(observedN > 0);
		CHECK(approx(observedSum / observedN, wantFactor, 1e-4));

		tsf_close(wetf); tsf_close(wetU); tsf_close(wetM);
	}

	// Two simultaneous voices sum into one bus and the instance chorus advances
	// exactly once (reference is a single delay over the summed send).
	{
		const int preset = 1, key = 60;
		std::vector<double> m1 = capture_mono_send(font, outRate, preset, key, frames);
		std::vector<double> m2 = capture_mono_send(font, outRate, preset, key, frames);

		tsf* f = load_font(font);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, outRate, 0);
		CHECK(tsf_set_chorus(f, delayMs, 0.0f, 0.0f, 0.3f, 0.8f) == 1);
		CHECK(tsf_note_on(f, preset, key, 1.0f) == 1);
		CHECK(tsf_note_on(f, preset, key, 1.0f) == 1);
		std::vector<float> out(frames * 2, 0.0f);
		tsf_render_float(f, out.data(), frames, 0);

		tsf* d = load_font(font);
		tsf_set_output(d, TSF_STEREO_INTERLEAVED, outRate, 0);
		CHECK(tsf_note_on(d, preset, key, 1.0f) == 1);
		CHECK(tsf_note_on(d, preset, key, 1.0f) == 1);
		std::vector<float> dry(frames * 2, 0.0f);
		tsf_render_float(d, dry.data(), frames, 0);

		RefChorus ref(outRate, (double)delayFrames, 0.0, 0.0, 0.3, 0.8);
		std::vector<double> send(frames), wL, wR;
		for (int n = 0; n < frames; n++) send[n] = (m1[n] + m2[n]) * 1.0; // factor 1.0 for P1/B
		ref.process(send, wL, wR);
		for (int n = 0; n < frames; n++)
		{
			CHECK(approx(out[n * 2] - dry[n * 2], wL[n], 2e-6));
			CHECK(approx(out[n * 2 + 1] - dry[n * 2 + 1], wR[n], 2e-6));
		}
		tsf_close(f); tsf_close(d);
	}
}

// Replicate the exact tsf_render_short quantizer/saturator for the expectation.
static short quantize_short(float v)
{
	int vi = (v < -1.00004566f ? (int)-32768 : (v > 1.00001514f ? (int)32767 : (int)(v * 32767.5f)));
	return (vi < -32768 ? (short)-32768 : (vi > 32767 ? (short)32767 : (short)vi));
}

static std::vector<double> capture_mono_gain(const std::vector<unsigned char>& font, int outRate,
	int presetIndex, int key, int frames, float gainDb)
{
	tsf* f = load_font(font);
	tsf_set_output(f, TSF_MONO, outRate, gainDb);
	CHECK(tsf_note_on(f, presetIndex, key, 1.0f) == 1);
	std::vector<float> out(frames, 0.0f);
	tsf_render_float(f, out.data(), frames, 0);
	std::vector<double> mono(frames);
	for (int i = 0; i < frames; i++) mono[i] = out[i];
	tsf_close(f);
	return mono;
}

// -------------------------------------------------------------------------------------------------
// delay: the discrete delay model against an independent double-precision
// history-array reference (fractional delays, cross feedback signs, L/R
// modulation), all three layouts, chunked bus clocking, short saturation and
// the flag_mixing path.
// -------------------------------------------------------------------------------------------------
CASE(delay)
{
	const int outRate = 8000, frames = 700;
	std::vector<unsigned char> font = build_routing_font(outRate, true, outRate);
	const int preset = 1, key = 60; // merged chorus factor 1.0

	const struct Cfg { double dFrames, aFrames, rate, fb, wet; } cfgs[] =
	{
		{ 4.5, 1.25, 2.0,  0.4, 0.7 }, // fractional delay + modulation + positive feedback
		{ 7.25, 2.5, 3.5, -0.5, 1.1 }, // negative cross feedback
		{ 3.0, 0.0,  0.0,  0.9, 1.0 }, // integer delay, no LFO, max feedback
	};

	for (int ci = 0; ci < 3; ci++)
	{
		double dF = cfgs[ci].dFrames, aF = cfgs[ci].aFrames, rate = cfgs[ci].rate;
		double fb = cfgs[ci].fb, wet = cfgs[ci].wet;
		float dMs = (float)(dF / outRate * 1000.0), aMs = (float)(aF / outRate * 1000.0);
		std::vector<double> mono = capture_mono_send(font, outRate, preset, key, frames);

		// Reference, fed in the same chunks the production render will use, so the
		// bus clock (write position and LFO phase) is checked across boundaries.
		const int chunks[] = { 300, 250, 150 };
		RefChorus refIL(outRate, dF, aF, rate, fb, wet);
		RefChorus refUW(outRate, dF, aF, rate, fb, wet);
		RefChorus refMono(outRate, dF, aF, rate, fb, wet);
		std::vector<double> wIL_L, wIL_R, wUW_L, wUW_R, wM_L, wM_R;
		int off = 0;
		for (int ch = 0; ch < 3; ch++)
		{
			std::vector<double> send(chunks[ch]);
			for (int k = 0; k < chunks[ch]; k++) send[k] = mono[off + k];
			std::vector<double> tL, tR;
			refIL.process(send, tL, tR); wIL_L.insert(wIL_L.end(), tL.begin(), tL.end()); wIL_R.insert(wIL_R.end(), tR.begin(), tR.end());
			refUW.process(send, tL, tR); wUW_L.insert(wUW_L.end(), tL.begin(), tL.end()); wUW_R.insert(wUW_R.end(), tR.begin(), tR.end());
			refMono.process(send, tL, tR); wM_L.insert(wM_L.end(), tL.begin(), tL.end()); wM_R.insert(wM_R.end(), tR.begin(), tR.end());
			off += chunks[ch];
		}

		// Production: whole renders per layout.
		tsf* fIL = load_font(font);
		tsf_set_output(fIL, TSF_STEREO_INTERLEAVED, outRate, 0);
		CHECK(tsf_set_chorus(fIL, dMs, aMs, (float)rate, (float)fb, (float)wet) == 1);
		CHECK(tsf_note_on(fIL, preset, key, 1.0f) == 1);
		std::vector<float> pIL(frames * 2, 0.0f);
		tsf_render_float(fIL, pIL.data(), frames, 0);

		tsf* fUW = load_font(font);
		tsf_set_output(fUW, TSF_STEREO_UNWEAVED, outRate, 0);
		CHECK(tsf_set_chorus(fUW, dMs, aMs, (float)rate, (float)fb, (float)wet) == 1);
		CHECK(tsf_note_on(fUW, preset, key, 1.0f) == 1);
		std::vector<float> pUW(frames * 2, 0.0f);
		tsf_render_float(fUW, pUW.data(), frames, 0);

		tsf* fM = load_font(font);
		tsf_set_output(fM, TSF_MONO, outRate, 0);
		CHECK(tsf_set_chorus(fM, dMs, aMs, (float)rate, (float)fb, (float)wet) == 1);
		CHECK(tsf_note_on(fM, preset, key, 1.0f) == 1);
		std::vector<float> pM(frames, 0.0f);
		tsf_render_float(fM, pM.data(), frames, 0);

		// Dry counterparts so the wet delta is measured against the real panned dry path.
		tsf* dIL = load_font(font);
		tsf_set_output(dIL, TSF_STEREO_INTERLEAVED, outRate, 0);
		CHECK(tsf_note_on(dIL, preset, key, 1.0f) == 1);
		std::vector<float> dryIL(frames * 2, 0.0f);
		tsf_render_float(dIL, dryIL.data(), frames, 0);
		tsf* dUW = load_font(font);
		tsf_set_output(dUW, TSF_STEREO_UNWEAVED, outRate, 0);
		CHECK(tsf_note_on(dUW, preset, key, 1.0f) == 1);
		std::vector<float> dryUW(frames * 2, 0.0f);
		tsf_render_float(dUW, dryUW.data(), frames, 0);
		tsf* dM = load_font(font);
		tsf_set_output(dM, TSF_MONO, outRate, 0);
		CHECK(tsf_note_on(dM, preset, key, 1.0f) == 1);
		std::vector<float> dryM(frames, 0.0f);
		tsf_render_float(dM, dryM.data(), frames, 0);

		// Chunked interleaved render crossing the internal 512-sample send block.
		tsf* fCh = load_font(font);
		tsf_set_output(fCh, TSF_STEREO_INTERLEAVED, outRate, 0);
		CHECK(tsf_set_chorus(fCh, dMs, aMs, (float)rate, (float)fb, (float)wet) == 1);
		CHECK(tsf_note_on(fCh, preset, key, 1.0f) == 1);
		std::vector<float> pChunk(frames * 2, 0.0f);
		off = 0;
		for (int ch = 0; ch < 3; ch++)
		{
			tsf_render_float(fCh, pChunk.data() + off * 2, chunks[ch], 0);
			off += chunks[ch];
		}

		int modulated = 0;
		for (int n = 0; n < frames; n++)
		{
			// Whole and chunked runs must be bit identical (same per-sample clock).
			CHECK(pChunk[n * 2] == pIL[n * 2]);
			CHECK(pChunk[n * 2 + 1] == pIL[n * 2 + 1]);

			// The reference keeps double history while the product uses float rings;
			// allow the small accumulation difference that feedback recirculation adds.
			CHECK(approx(pIL[n * 2] - dryIL[n * 2], wIL_L[n], 1e-5));
			CHECK(approx(pIL[n * 2 + 1] - dryIL[n * 2 + 1], wIL_R[n], 1e-5));
			CHECK(approx(pUW[n] - dryUW[n], wUW_L[n], 1e-5));
			CHECK(approx(pUW[frames + n] - dryUW[frames + n], wUW_R[n], 1e-5));
			CHECK(approx(pM[n] - dryM[n], 0.5 * (wM_L[n] + wM_R[n]), 1e-5));
			if (std::fabs(wIL_L[n] - wIL_R[n]) > 1e-4) modulated++;
		}
		// With a nonzero depth/rate the two modulated delay lines must actually diverge.
		if (aF > 0.0) CHECK(modulated > 10); else CHECK(modulated == 0);

		tsf_close(fIL); tsf_close(fUW); tsf_close(fM); tsf_close(fCh);
		tsf_close(dIL); tsf_close(dUW); tsf_close(dM);
	}

	// Cross feedback sign: direct delay terms are identical for +/- feedback, so
	// differencing the two wet outputs isolates the feedback contribution. It
	// first reaches the output after roughly two delay times (dR then dL).
	{
		const double g = 0.6, dF = 5.0;
		auto renderWet = [&](double fb) {
			tsf* f = load_font(font);
			tsf_set_output(f, TSF_STEREO_INTERLEAVED, outRate, 0);
			CHECK(tsf_set_chorus(f, (float)(dF / outRate * 1000.0), 0.0f, 0.0f, (float)fb, 1.0f) == 1);
			CHECK(tsf_note_on(f, preset, key, 1.0f) == 1);
			std::vector<float> wet(256 * 2, 0.0f);
			tsf_render_float(f, wet.data(), 256, 0);
			tsf* d = load_font(font);
			tsf_set_output(d, TSF_STEREO_INTERLEAVED, outRate, 0);
			CHECK(tsf_note_on(d, preset, key, 1.0f) == 1);
			std::vector<float> dry(256 * 2, 0.0f);
			tsf_render_float(d, dry.data(), 256, 0);
			std::vector<double> out(256);
			for (int n = 0; n < 256; n++) out[n] = wet[n * 2] - dry[n * 2];
			tsf_close(f); tsf_close(d);
			return out;
		};
		std::vector<double> wPos = renderWet(g), wNeg = renderWet(-g);
		int firstSign = 0, start = (int)std::ceil(2.0 * dF) - 1;
		for (int n = start; n < start + 12 && !firstSign; n++)
		{
			double diff = wPos[n] - wNeg[n];
			if (std::fabs(diff) > 1e-7) firstSign = (diff > 0 ? 1 : -1);
		}
		CHECK(firstSign == 1); // positive feedback yields a positive first difference
		// And the two full signals genuinely differ past the first feedback arrival.
		int differ = 0;
		for (int n = start; n < 256; n++) if (std::fabs(wPos[n] - wNeg[n]) > 1e-7) differ++;
		CHECK(differ > 50);
	}

	// Short path: quantization/saturation happens after dry+wet are summed; the
	// bus itself keeps floating point and only the final merge is clamped.
	{
		const float gainDb = 12.0f;
		std::vector<double> mono = capture_mono_gain(font, outRate, preset, key, frames, gainDb);
		double dF = 5.5, aF = 1.5, rate = 1.25, fb = 0.7, wet = 2.0;
		RefChorus ref(outRate, dF, aF, rate, fb, wet);
		std::vector<double> wL, wR;
		ref.process(mono, wL, wR);

		tsf* d = load_font(font);
		tsf_set_output(d, TSF_STEREO_INTERLEAVED, outRate, gainDb);
		CHECK(tsf_note_on(d, preset, key, 1.0f) == 1);
		std::vector<float> dryIL(frames * 2, 0.0f);
		tsf_render_float(d, dryIL.data(), frames, 0);

		tsf* f = load_font(font);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, outRate, gainDb);
		CHECK(tsf_set_chorus(f, (float)(dF / outRate * 1000.0), (float)(aF / outRate * 1000.0),
			(float)rate, (float)fb, (float)wet) == 1);
		CHECK(tsf_note_on(f, preset, key, 1.0f) == 1);
		std::vector<short> sh(frames * 2, 0);
		tsf_render_short(f, sh.data(), frames, 0);

		int saturated = 0;
		for (int n = 0; n < frames; n++)
		{
			float expL = dryIL[n * 2] + (float)wL[n], expR = dryIL[n * 2 + 1] + (float)wR[n];
			CHECK(sh[n * 2] == quantize_short(expL));
			CHECK(sh[n * 2 + 1] == quantize_short(expR));
			if (std::fabs(expL) > 1.00001514 || std::fabs(expR) > 1.00001514) saturated++;
		}
		CHECK(saturated > 50); // the boosted gain really does clip the short path
		tsf_close(f); tsf_close(d);
	}

	// flag_mixing goes through the same path: pre-existing content is preserved
	// and the wet bus is added on top; the send bus never sees the prefill.
	{
		double dF = 4.0, aF = 0.0, rate = 0.0, fb = 0.25, wet = 0.9;
		std::vector<double> mono = capture_mono_send(font, outRate, preset, key, 128);
		RefChorus ref(outRate, dF, aF, rate, fb, wet);
		std::vector<double> wL, wR;
		ref.process(mono, wL, wR);

		tsf* d = load_font(font);
		tsf_set_output(d, TSF_STEREO_INTERLEAVED, outRate, 0);
		CHECK(tsf_note_on(d, preset, key, 1.0f) == 1);
		std::vector<float> dryIL(128 * 2, 0.0f);
		tsf_render_float(d, dryIL.data(), 128, 0);

		tsf* f = load_font(font);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, outRate, 0);
		CHECK(tsf_set_chorus(f, (float)(dF / outRate * 1000.0), 0.0f, 0.0f, (float)fb, (float)wet) == 1);
		CHECK(tsf_note_on(f, preset, key, 1.0f) == 1);
		std::vector<float> mixed(128 * 2, 0.0f), fresh(128 * 2, 0.0f);
		for (int i = 0; i < 256; i++) mixed[i] = 0.25f;
		tsf_render_float(f, mixed.data(), 128, 1);
		tsf_render_float(f, fresh.data(), 128, 0); // second call: zero-input tail continues
		for (int n = 0; n < 128; n++)
		{
			CHECK(approx(mixed[n * 2], 0.25 + dryIL[n * 2] + wL[n], 2e-6));
			CHECK(approx(mixed[n * 2 + 1], 0.25 + dryIL[n * 2 + 1] + wR[n], 2e-6));
		}
		// The fresh render after it is all zero input: only decaying wet remains.
		int tailNonZero = 0;
		for (int n = 0; n < 128; n++) if (std::fabs(fresh[n * 2]) > 1e-7 || std::fabs(fresh[n * 2 + 1]) > 1e-7) tailNonZero++;
		CHECK(tailNonZero > 0);
		tsf_close(f); tsf_close(d);
	}
}

// -------------------------------------------------------------------------------------------------
// lifetime: zero-input tails, zero-frame rendering, reconfiguration and reset
// semantics, independent copies, shared-font close ordering, and zero
// allocations during rendering.
// -------------------------------------------------------------------------------------------------
CASE(lifetime)
{
	const int outRate = 8000;
	std::vector<unsigned char> font = build_routing_font(outRate, true, outRate);
	const int preset = 1, key = 60;
	const float dMs = 6.0f, aMs = 1.5f, rateHz = 2.0f, fb = 0.35f, wet = 0.8f;

	// Disabled by default: behavior is bit-identical without ever calling tsf_set_chorus.
	{
		tsf* a = load_font(font); tsf* b = load_font(font);
		tsf_set_output(a, TSF_STEREO_INTERLEAVED, outRate, 0);
		tsf_set_output(b, TSF_STEREO_INTERLEAVED, outRate, 0);
		tsf_note_on(a, preset, key, 1.0f); tsf_note_on(b, preset, key, 1.0f);
		std::vector<float> oa(512), ob(512);
		tsf_render_float(a, oa.data(), 256, 0); tsf_render_float(b, ob.data(), 256, 0);
		tsf_render_short(a, (short*)oa.data(), 128, 0); tsf_render_short(b, (short*)ob.data(), 128, 0);
		for (size_t i = 0; i < oa.size(); i++) CHECK(oa[i] == ob[i]);
		tsf_close(a); tsf_close(b);
	}

	// Parameter validation: failures return 0 and leave the old setup intact.
	{
		tsf* f = load_font(font);
		tsf_set_output(f, TSF_MONO, outRate, 0);
		CHECK(tsf_set_chorus(f, dMs, aMs, rateHz, fb, wet) == 1);
		float nanv = std::numeric_limits<float>::quiet_NaN();
		float infv = std::numeric_limits<float>::infinity();
		CHECK(tsf_set_chorus(f, nanv, aMs, rateHz, fb, wet) == 0);
		CHECK(tsf_set_chorus(f, dMs, infv, rateHz, fb, wet) == 0);
		CHECK(tsf_set_chorus(f, 0.05f, 0.0f, rateHz, fb, wet) == 0);   // D under one frame at 8kHz
		CHECK(tsf_set_chorus(f, 90.0f, 11.0f, rateHz, fb, wet) == 0);  // D+A over 100ms
		CHECK(tsf_set_chorus(f, dMs, aMs, 10.5f, fb, wet) == 0);       // rate out of range
		CHECK(tsf_set_chorus(f, dMs, aMs, rateHz, 0.95f, wet) == 0);   // feedback out of range
		CHECK(tsf_set_chorus(f, dMs, aMs, rateHz, fb, 2.5f) == 0);     // wet gain out of range

		// Old configuration still works after the rejected calls.
		tsf_note_on(f, preset, key, 1.0f);
		std::vector<float> out(128, 0.0f);
		tsf_render_float(f, out.data(), 128, 0);
		tsf* g = load_font(font);
		tsf_set_output(g, TSF_MONO, outRate, 0);
		CHECK(tsf_set_chorus(g, dMs, aMs, rateHz, fb, wet) == 1);
		tsf_note_on(g, preset, key, 1.0f);
		std::vector<float> ref(128, 0.0f);
		tsf_render_float(g, ref.data(), 128, 0);
		for (int i = 0; i < 128; i++) CHECK(out[i] == ref[i]);
		tsf_close(f); tsf_close(g);
	}

	// Sample rate is locked while enabled; disabling allows the change again.
	{
		tsf* f = load_font(font);
		tsf_set_output(f, TSF_MONO, outRate, 0);
		CHECK(tsf_set_chorus(f, dMs, aMs, rateHz, fb, wet) == 1);
		tsf_set_output(f, TSF_MONO, 16000, 0);
		// Rate stayed at 8000: a 1ms delay is still 8 frames.
		tsf_note_on(f, preset, key, 1.0f);
		std::vector<float> out(32, 0.0f);
		tsf_render_float(f, out.data(), 32, 0);
		CHECK(tsf_set_chorus(f, dMs, aMs, rateHz, fb, 0.0f) == 1); // disable + clear
		tsf_set_output(f, TSF_MONO, 16000, 0);
		// At 16kHz the same ms delay is now 16 frames: re-enable must succeed with new sizing.
		CHECK(tsf_set_chorus(f, dMs, aMs, rateHz, fb, wet) == 1);
		tsf_close(f);
	}

	// wet_gain 0 disables and clears: output is dry-only afterwards.
	{
		tsf* f = load_font(font);
		tsf_set_output(f, TSF_MONO, outRate, 0);
		CHECK(tsf_set_chorus(f, dMs, aMs, rateHz, fb, wet) == 1);
		tsf_note_on(f, preset, key, 1.0f);
		std::vector<float> wet1(256, 0.0f), dry(256, 0.0f);
		tsf_render_float(f, wet1.data(), 256, 0);
		CHECK(tsf_set_chorus(f, dMs, aMs, rateHz, fb, 0.0f) == 1);
		tsf_note_off(f, preset, key);
		tsf* d = load_font(font);
		tsf_set_output(d, TSF_MONO, outRate, 0);
		tsf_note_on(d, preset, key, 1.0f);
		tsf_render_float(d, dry.data(), 256, 0);
		tsf_note_off(d, preset, key);
		// Fresh render on both with no voices: disabled instance is fully silent.
		std::vector<float> tail(256, 0.0f), dtail(256, 0.0f);
		tsf_render_float(f, tail.data(), 256, 0);
		tsf_render_float(d, dtail.data(), 256, 0);
		for (int i = 0; i < 256; i++) { CHECK(tail[i] == 0.0f); CHECK(tail[i] == dtail[i]); }
		tsf_close(f); tsf_close(d);
	}

	// Zero-input tail keeps playing after every voice has ended.
	{
		tsf* f = load_font(font);
		tsf_set_output(f, TSF_MONO, outRate, 0);
		CHECK(tsf_set_chorus(f, dMs, aMs, rateHz, fb, wet) == 1);
		tsf_set_max_voices(f, 8);
		tsf_note_on(f, preset, key, 1.0f);
		std::vector<float> out(1024, 0.0f);
		tsf_render_float(f, out.data(), 256, 0);
		tsf_note_off(f, preset, key);
		tsf_render_float(f, out.data() + 256, 400, 0); // voices release and finish
		CHECK(tsf_active_voice_count(f) == 0);
		std::vector<float> tail(256, 0.0f);
		tsf_render_float(f, tail.data(), 256, 0);     // zero input, effect still advances
		int nonzero = 0;
		for (float v : tail) if (v != 0.0f) nonzero++;
		CHECK(nonzero > 0);
		// And the tail decays toward silence.
		double early = 0.0, late = 0.0;
		for (int i = 0; i < 32; i++) early += std::fabs(tail[i]);
		for (int i = 224; i < 256; i++) late += std::fabs(tail[i]);
		CHECK(late < early);
		tsf_close(f);
	}

	// A zero-frame render must not advance the LFO phase or delay history.
	{
		tsf* a = load_font(font); tsf* b = load_font(font);
		tsf_set_output(a, TSF_STEREO_INTERLEAVED, outRate, 0);
		tsf_set_output(b, TSF_STEREO_INTERLEAVED, outRate, 0);
		CHECK(tsf_set_chorus(a, dMs, aMs, rateHz, fb, wet) == 1);
		CHECK(tsf_set_chorus(b, dMs, aMs, rateHz, fb, wet) == 1);
		tsf_note_on(a, preset, key, 1.0f); tsf_note_on(b, preset, key, 1.0f);
		std::vector<float> oa(512, 0.0f), ob(512, 0.0f);
		tsf_render_float(a, oa.data(), 128, 0);
		tsf_render_float(b, ob.data(), 128, 0);
		tsf_render_float(b, TSF_NULL, 0, 0); // zero frames: no phase/history movement
		tsf_render_float(a, oa.data() + 256, 128, 0);
		tsf_render_float(b, ob.data() + 256, 128, 0);
		for (int i = 0; i < 256; i++) { CHECK(oa[i * 2] == ob[i * 2]); CHECK(oa[i * 2 + 1] == ob[i * 2 + 1]); }
		tsf_close(a); tsf_close(b);
	}

	// Valid reconfiguration clears history and restarts the phase at zero.
	{
		tsf* f = load_font(font);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, outRate, 0);
		CHECK(tsf_set_chorus(f, 10.0f, 3.0f, 4.0f, 0.6f, 1.0f) == 1);
		tsf_set_max_voices(f, 8);
		tsf_note_on(f, preset, key, 1.0f);
		std::vector<float> warm(1024, 0.0f); // interleaved: holds the later 400-frame render
		tsf_render_float(f, warm.data(), 128, 0);
		tsf_note_off(f, preset, key);
		tsf_render_float(f, warm.data(), 400, 0);
		CHECK(tsf_active_voice_count(f) == 0);
		// Reconfigure with new parameters; history must be gone.
		CHECK(tsf_set_chorus(f, dMs, aMs, rateHz, fb, wet) == 1);
		std::vector<float> silent(128, 0.0f);
		tsf_render_float(f, silent.data(), 64, 0);
		for (float v : silent) CHECK(v == 0.0f);
		// A new note now matches a freshly created instance configured the same way.
		// The reference advances the same number of zero-input frames so its LFO
		// phase age matches (zero input still advances the effect; only a
		// zero-frame render leaves the phase untouched).
		tsf* g = load_font(font);
		tsf_set_output(g, TSF_STEREO_INTERLEAVED, outRate, 0);
		CHECK(tsf_set_chorus(g, dMs, aMs, rateHz, fb, wet) == 1);
		std::vector<float> gSilent(128, 0.0f);
		tsf_render_float(g, gSilent.data(), 64, 0);
		for (float v : gSilent) CHECK(v == 0.0f);
		tsf_note_on(f, preset, key, 1.0f); tsf_note_on(g, preset, key, 1.0f);
		std::vector<float> of(256, 0.0f), og(256, 0.0f);
		tsf_render_float(f, of.data(), 128, 0);
		tsf_render_float(g, og.data(), 128, 0);
		for (int i = 0; i < 256; i++) CHECK(of[i] == og[i]);
		tsf_close(f); tsf_close(g);
	}

	// tsf_reset: quick release of voices, chorus history/phase cleared, config kept.
	{
		tsf* f = load_font(font);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, outRate, 0);
		CHECK(tsf_set_chorus(f, dMs, aMs, rateHz, fb, wet) == 1);
		tsf_set_max_voices(f, 8);
		tsf_note_on(f, preset, key, 1.0f);
		std::vector<float> warm(512, 0.0f); // interleaved: holds the later 200-frame render
		tsf_render_float(f, warm.data(), 128, 0);
		tsf_reset(f);
		CHECK(tsf_active_voice_count(f) == 1); // quick release, not an instant kill
		tsf_render_float(f, warm.data(), 200, 0);
		CHECK(tsf_active_voice_count(f) == 0); // 25ms exceeds the 10ms fast release
		// Keep advancing on zero input until the feedback tail has decayed below
		// float precision (0.35^25 ~ 1e-11), leaving a ring indistinguishable from
		// the freshly cleared one of a new instance.
		const int flushFrames = 1500;
		std::vector<float> flush(flushFrames * 2, 0.0f);
		tsf_render_float(f, flush.data(), flushFrames, 0);
		// Config retained and history/phase cleared: a new note matches a fresh
		// same-config instance that has aged by the same number of effect frames.
		tsf* g = load_font(font);
		tsf_set_output(g, TSF_STEREO_INTERLEAVED, outRate, 0);
		CHECK(tsf_set_chorus(g, dMs, aMs, rateHz, fb, wet) == 1);
		std::vector<float> gAge((200 + flushFrames) * 2, 0.0f);
		tsf_render_float(g, gAge.data(), 200 + flushFrames, 0);
		tsf_note_on(f, preset, key, 1.0f); tsf_note_on(g, preset, key, 1.0f);
		std::vector<float> of(256, 0.0f), og(256, 0.0f);
		tsf_render_float(f, of.data(), 128, 0);
		tsf_render_float(g, og.data(), 128, 0);
		for (int i = 0; i < 256; i++) CHECK(of[i] == og[i]);
		tsf_close(f); tsf_close(g);
	}

	// Copies keep the parameters but get independent, zeroed history and no voices.
	{
		tsf* f = load_font(font);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, outRate, 0);
		CHECK(tsf_set_chorus(f, dMs, aMs, rateHz, fb, wet) == 1);
		tsf_note_on(f, preset, key, 1.0f);
		std::vector<float> warm(256, 0.0f);
		tsf_render_float(f, warm.data(), 128, 0);

		tsf* g = tsf_copy(f);
		CHECK(g != TSF_NULL);
		CHECK(tsf_active_voice_count(g) == 0);

		// g behaves like a fresh instance with the same config and empty history.
		tsf* h = load_font(font);
		tsf_set_output(h, TSF_STEREO_INTERLEAVED, outRate, 0);
		CHECK(tsf_set_chorus(h, dMs, aMs, rateHz, fb, wet) == 1);
		tsf_note_on(g, preset, key, 1.0f); tsf_note_on(h, preset, key, 1.0f);
		std::vector<float> og(256, 0.0f), oh(256, 0.0f);
		tsf_render_float(g, og.data(), 128, 0);
		tsf_render_float(h, oh.data(), 128, 0);
		for (int i = 0; i < 256; i++) CHECK(og[i] == oh[i]);

		// The two histories are independent: f still carries its warm-up tail.
		std::vector<float> of(256, 0.0f);
		tsf_render_float(f, of.data(), 128, 0);
		int differ = 0;
		for (int i = 0; i < 256; i++) if (of[i] != og[i]) differ++;
		CHECK(differ > 0);

		// Close ordering: original first, copy keeps working (shared soundfont lifetime).
		tsf_close(f);
		tsf_render_float(g, og.data(), 64, 0);
		tsf_close(h);

		// Reverse ordering on another pair.
		tsf* p = load_font(font);
		tsf_set_output(p, TSF_MONO, outRate, 0);
		CHECK(tsf_set_chorus(p, dMs, aMs, rateHz, fb, wet) == 1);
		tsf* q = tsf_copy(p);
		CHECK(q != TSF_NULL);
		tsf_close(q);
		std::vector<float> op(64, 0.0f);
		tsf_render_float(p, op.data(), 32, 0);
		tsf_close(p);
		tsf_close(g);
	}

	// Copy of a disabled instance: chorus stays off and can be enabled independently.
	{
		tsf* f = load_font(font);
		tsf_set_output(f, TSF_MONO, outRate, 0);
		tsf* g = tsf_copy(f);
		CHECK(g != TSF_NULL);
		CHECK(tsf_set_chorus(g, dMs, aMs, rateHz, fb, wet) == 1);
		tsf_note_on(g, preset, key, 1.0f);
		std::vector<float> og(128, 0.0f);
		tsf_render_float(g, og.data(), 64, 0);
		tsf_close(g); tsf_close(f);
	}

	// Rendering (float/short, mixing, zero frames, tails) performs no allocation.
	{
		tsf* f = load_font(font);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, outRate, 0);
		CHECK(tsf_set_chorus(f, dMs, aMs, rateHz, fb, wet) == 1);
		CHECK(tsf_set_max_voices(f, 8) == 1);
		tsf_note_on(f, preset, key, 1.0f);
		std::vector<float> fl(2048, 0.0f);
		std::vector<short> sh(2048, 0);
		long m0 = g_test_malloc, r0 = g_test_realloc;
		tsf_render_float(f, fl.data(), 512, 0);
		tsf_render_float(f, fl.data(), 512, 1);
		tsf_render_float(f, TSF_NULL, 0, 0);
		tsf_render_short(f, sh.data(), 384, 0);
		tsf_render_short(f, sh.data(), 128, 1);
		tsf_note_off(f, preset, key);
		tsf_render_float(f, fl.data(), 512, 0); // tail with no active voices
		CHECK(g_test_malloc == m0);
		CHECK(g_test_realloc == r0);
		tsf_close(f);
	}
}
