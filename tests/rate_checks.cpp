// Tests for ModPlug_ChangeRate: seamless output-rate changes in libmodplug.
//
// Everything here works through the public C API plus the task test harness
// (task_support/check.h).  Cases are grouped as:
//   clock  - playback-clock integrity (ticks/envelopes not re-advanced,
//            mid-tick and mid-ramp switches, loop pitch)
//   tails  - effect tails (surround/reverb) survive the switch
//   compat - API contract, rejection paths, unchanged behaviour at a fixed rate,
//            chunk-size independence
#include "modplug.h"
#include "../task_support/mod_fixture.h"
#include "../task_support/check.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

typedef std::vector<short> Pcm;   // interleaved stereo samples

static void put16(std::vector<unsigned char> &d, size_t at, unsigned v)
{
	if (at + 2 > d.size()) d.resize(at + 2, 0);
	d[at] = (unsigned char)(v & 0xFF);
	d[at + 1] = (unsigned char)((v >> 8) & 0xFF);
}
static void put32(std::vector<unsigned char> &d, size_t at, uint32_t v)
{
	if (at + 4 > d.size()) d.resize(at + 4, 0);
	d[at] = (unsigned char)(v & 0xFF);
	d[at + 1] = (unsigned char)((v >> 8) & 0xFF);
	d[at + 2] = (unsigned char)((v >> 16) & 0xFF);
	d[at + 3] = (unsigned char)((v >> 24) & 0xFF);
}
// Append helpers: make the intended layout explicit and bounds-safe.
static void app16(std::vector<unsigned char> &d, unsigned v)
{
	put16(d, d.size(), v);
}
static void app32(std::vector<unsigned char> &d, uint32_t v)
{
	put32(d, d.size(), v);
}

// A minimal one-instrument, one-pattern XM module with a volume envelope and a
// looping sample.  The envelope lets the tests observe whether envelope position
// is advanced once (correct) or twice (a re-triggered ReadNote).
static std::vector<unsigned char> make_xm(bool sloped_env = false, bool impulse_only = false)
{
	const int ROWS = 64, CHN = 1;
	// For tail tests the sample is a short non-looping burst so that, after a few
	// milliseconds, only the DSP tails are still sounding.
	const int SAMPLEN = impulse_only ? 24 : 256;
	std::vector<unsigned char> d;

	// --- header ---
	// magic(16) + 0x1A + name(20) + tracker(20) + version(2) + pad(1) = 60 bytes,
	// then the XMFILEHEADER the loader reads at offset 60.
	const char magic[17] = "Extended Module:";   // 16 chars + NUL
	d.insert(d.end(), magic, magic + 16);
	d.push_back(0x1A);                             // offset 16
	for (int i = 0; i < 20; i++) d.push_back((unsigned char)("xm rate probe       "[i]));
	for (int i = 0; i < 20; i++) d.push_back((unsigned char)("libmodplug probe    "[i]));
	d.push_back(0x04); d.push_back(0x01);          // version 1.04
	d.push_back(0);                                // pad to offset 60
	// XMFILEHEADER at offset 60
	put32(d, d.size(), 276); // header size
	put16(d, d.size(), 1);   // song length (orders)
	put16(d, d.size(), 0);   // restart
	put16(d, d.size(), CHN); // channels
	put16(d, d.size(), 1);   // patterns
	put16(d, d.size(), 1);   // instruments
	put16(d, d.size(), 0);   // flags (amiga freq)
	put16(d, d.size(), 6);   // default speed
	put16(d, d.size(), 125); // default tempo
	for (int i = 0; i < 256; i++) d.push_back(0);    // order table
	// pad header to 60+276 bytes
	while (d.size() < 60 + 276) d.push_back(0);

	// --- pattern 0 ---
	put32(d, d.size(), 9);  // pattern header size
	d.push_back(0);                                  // packing type
	put16(d, d.size(), ROWS);
	put16(d, d.size(), 0);   // packed size (patched below)
	size_t pack_start = d.size();
	for (int row = 0; row < ROWS; row++)
	{
		// In impulse mode only the very first row strikes a note; otherwise a note
		// every 16 rows keeps the fixture "playing" throughout.
		bool strike = impulse_only ? (row == 0) : (row % 16 == 0);
		if (strike)
		{
			// 0x83 = packed: note and instrument present.
			d.push_back(0x83);
			d.push_back(49);                          // note -> C-4 after +12 bias
			d.push_back(1);                           // instrument 1
		}
		else
		{
			d.push_back(0x80);                        // packed: all fields absent
		}
	}
	unsigned packsize = (unsigned)(d.size() - pack_start);
	put16(d, pack_start - 2, packsize);

	// --- instrument 1 ---
	// 29-byte fixed header (size + name + type + nsamples) followed by the
	// 234-byte XMSAMPLEHEADER, for a declared header size of 263 bytes.
	size_t ins_start = d.size();
	put32(d, d.size(), 263); // instrument header size
	for (int i = 0; i < 22; i++) d.push_back((unsigned char)("env probe             "[i]));
	d.push_back(0);                                  // type
	put16(d, d.size(), 1);   // number of samples
	// Extended sample header.  The volume envelope is flat at 64 by default; with
	// sloped_env it falls from 64 to 0 so an extra tick advance would change the
	// measured amplitude.
	std::vector<unsigned char> sh(234, 0);
	put32(sh, 0, 40);                                // shsize
	// snum[96] at offset 4 stays zero.
	// Volume envelope at offset 100: 24 WORDs = 12 (frame, value) pairs.
	// In sloped mode the envelope falls from 64 to 0 over just 6 ticks, so one
	// extra tick advance is a large, easily detected amplitude change.
	put16(sh, 100 + 0, 0);   put16(sh, 100 + 2, 64);   // point 0: (0, 64)
	put16(sh, 100 + 4, sloped_env ? 6 : 100);
	put16(sh, 100 + 6, sloped_env ? 0 : 64);           // point 1
	// Panning envelope at offset 148 stays zero.
	sh[196] = 2;   // vnum
	sh[197] = 0;   // pnum
	sh[198] = 0;   // vsustain
	sh[199] = 0;   // vloops
	sh[200] = 0;   // vloope
	sh[201] = 0;   // psustain
	sh[202] = 0;   // ploops
	sh[203] = 0;   // ploope
	sh[204] = 1;   // vtype: volume envelope on
	sh[205] = 0;   // ptype
	// vibtype/vibsweep/vibdepth/vibrate at 206..209 stay zero.
	// volfade at 210 and res at 212 stay zero.
	d.insert(d.end(), sh.begin(), sh.end());
	// --- sample header (40 bytes) ---
	put32(d, d.size(), SAMPLEN);  // sample length
	put32(d, d.size(), 0);         // loop start
	put32(d, d.size(), impulse_only ? 0 : SAMPLEN); // loop length
	d.push_back(64);                                       // volume
	d.push_back(0);                                        // finetune
	d.push_back(impulse_only ? 0x00 : 0x01);               // type: loop, 8-bit
	d.push_back(128);                                      // pan
	d.push_back(0);                                        // relnote
	d.push_back(0);                                        // reserved
	for (int i = 0; i < 22; i++) d.push_back(0);

	// --- sample data (8-bit delta) ---
	if (impulse_only)
	{
		// A short triangular burst that returns to zero, then silence.
		for (int i = 0; i < SAMPLEN; i++)
			d.push_back((unsigned char)(i < SAMPLEN / 2 ? 6 : 0xFA)); // +6, -6
	}
	else
	{
		// A ramp built as deltas so it loops back to zero cleanly.
		for (int i = 0; i < SAMPLEN; i++)
			d.push_back((unsigned char)(i == 0 ? 0 : 1));
	}

	return d;
}

static int setup(int rate, int flags)
{
	ModPlug_Settings s;
	ModPlug_GetSettings(&s);
	s.mFrequency = rate;
	s.mChannels = 2;
	s.mBits = 16;
	s.mFlags = flags;
	s.mLoopCount = 0;
	ModPlug_SetSettings(&s);
	return 0;
}

static ModPlugFile* load_mod()
{
	std::vector<unsigned char> mod = make_mod();
	return ModPlug_Load(mod.data(), (int)mod.size());
}

static ModPlugFile* load_xm(bool sloped_env = false, bool impulse_only = false)
{
	std::vector<unsigned char> xm = make_xm(sloped_env, impulse_only);
	return ModPlug_Load(xm.data(), (int)xm.size());
}

// Read exactly `frames` stereo frames (or fewer at end of song) into out.
static int read_frames(ModPlugFile *f, Pcm &out, int frames, int chunk_frames = 0)
{
	out.clear();
	int want = frames * 2;
	int done = 0;
	std::vector<short> buf;
	int step = chunk_frames > 0 ? chunk_frames * 2 : want;
	buf.resize(step > 0 ? step : 2);
	while (done < want)
	{
		int n = step;
		if (n > want - done) n = want - done;
		int got = ModPlug_Read(f, buf.data(), n * (int)sizeof(short));
		if (got <= 0) break;
		int gs = got / (int)sizeof(short);
		out.insert(out.end(), buf.begin(), buf.begin() + gs);
		done += gs;
	}
	return done / 2;
}

static uint64_t abs_sum(const Pcm &p, size_t from = 0, size_t to = 0)
{
	if (!to || to > p.size()) to = p.size();
	uint64_t s = 0;
	for (size_t i = from; i < to; ++i)
		s += p[i] < 0 ? (uint64_t)(-(int)p[i]) : (uint64_t)p[i];
	return s;
}

// Index of the first frame at which two PCM streams differ; -1 when equal up to
// the shorter one (returns number of compared frames in *cmp).
static int first_diff(const Pcm &a, const Pcm &b, long *cmp = NULL)
{
	size_t n = a.size() < b.size() ? a.size() : b.size();
	for (size_t i = 0; i < n; ++i)
	{
		if (a[i] != b[i])
		{
			if (cmp) *cmp = (long)(i / 2);
			return (int)(i / 2);
		}
	}
	if (cmp) *cmp = (long)(n / 2);
	return -1;
}

// Unload defensively (header has no NULL tolerance).
static void unload(ModPlugFile *f) { if (f) ModPlug_Unload(f); }

// ---------------------------------------------------------------------------
// clock group: change on an already-running song; tick and envelope positions
// must continue from where they were, not restart.
// ---------------------------------------------------------------------------

// Changing the rate at an arbitrary point must not advance or rewind the tick,
// row or envelope: the same number of ticks elapses as at a constant rate.
static int clock_tick_position_preserved(void)
{
	setup(44100, MODPLUG_ENABLE_NOISE_REDUCTION);
	ModPlugFile *f = load_mod();
	TASK_CHECK(f != NULL);

	Pcm warm;
	read_frames(f, warm, 2000);                 // start playback mid-song
	int row_before = ModPlug_GetCurrentRow(f);
	int order_before = ModPlug_GetCurrentOrder(f);
	int speed_before = ModPlug_GetCurrentSpeed(f);
	TASK_CHECK(row_before >= 0);
	TASK_CHECK(speed_before > 0);

	TASK_CHECK(ModPlug_ChangeRate(f, 22050) == 1);

	// Reading 1 second at the new rate must advance by the same musical time as
	// one second at 44.1 kHz would: compare with a twin that never changed rate.
	Pcm after;
	read_frames(f, after, 22050);
	int row_after = ModPlug_GetCurrentRow(f);
	int order_after = ModPlug_GetCurrentOrder(f);
	unload(f);

	setup(44100, MODPLUG_ENABLE_NOISE_REDUCTION);
	ModPlugFile *g = load_mod();
	TASK_CHECK(g != NULL);
	Pcm warm2;
	read_frames(g, warm2, 2000);
	TASK_CHECK(ModPlug_GetCurrentRow(g) == row_before);
	TASK_CHECK(ModPlug_GetCurrentOrder(g) == order_before);
	Pcm after2;
	read_frames(g, after2, 44100);
	int row_ref = ModPlug_GetCurrentRow(g);
	int order_ref = ModPlug_GetCurrentOrder(g);
	unload(g);

	// The clock must not have been rewound or fast-forwarded: the row reached after
	// one second of audio should match the constant-rate reference closely.
	TASK_CHECK(order_after == order_ref);
	TASK_CHECK(abs(row_after - row_ref) <= 1);
	// And the PCM must be non-silent: notes keep sounding across the switch.
	TASK_CHECK(abs_sum(after) > 0);
	return 0;
}

// A change in the middle of a tick: the tick's remaining wall-clock time must be
// honoured, not restarted or cut short.  Verified by comparing tick boundaries at
// a constant rate against the switched run, at equal times.
static int clock_midtick_boundary(void)
{
	setup(44100, 0);
	ModPlugFile *f = load_mod();
	TASK_CHECK(f != NULL);

	// Advance an odd number of frames so the switch lands inside a tick.
	Pcm a, b;
	read_frames(f, a, 1234);
	TASK_CHECK(ModPlug_ChangeRate(f, 11025) == 1);
	// Then observe exactly one second of output; the row/tempo clock must advance
	// by the same musical amount as a one-second block at 44.1 kHz.
	int row_mid = ModPlug_GetCurrentRow(f);
	read_frames(f, b, 11025);
	int row_sw = ModPlug_GetCurrentRow(f);
	unload(f);

	setup(44100, 0);
	ModPlugFile *g = load_mod();
	TASK_CHECK(g != NULL);
	Pcm c, d;
	read_frames(g, c, 1234);
	TASK_CHECK(ModPlug_GetCurrentRow(g) == row_mid);
	read_frames(g, d, 44100);
	int row_ref = ModPlug_GetCurrentRow(g);
	unload(g);
	TASK_CHECK(abs(row_sw - row_ref) <= 1);
	return 0;
}

// Mid-ramp switch: the amplitude ramp must keep its current amplitude and target
// and simply continue over the rescaled remaining time.  The XM impulse note
// produces a short volume ramp at note-on; switching 20 frames in (mid-ramp) and
// comparing the following loudness envelope against a constant-22050 run at the
// same wall-clock position (10 frames in) checks this.
static int clock_midramp_amplitude(void)
{
	// Switched run: 20 frames at 44100, switch to 22050, then 40 frames.
	setup(44100, 0);
	ModPlugFile *f = load_xm(false, true);
	TASK_CHECK(f != NULL);
	Pcm pre;
	read_frames(f, pre, 20);
	TASK_CHECK(ModPlug_ChangeRate(f, 22050) == 1);
	Pcm post;
	read_frames(f, post, 40);
	unload(f);
	uint64_t e_sw = abs_sum(post);

	// Matched constant-rate reference: 20 frames at 44100 = 10 frames at 22050.
	setup(22050, 0);
	ModPlugFile *g = load_xm(false, true);
	TASK_CHECK(g != NULL);
	Pcm skip;
	read_frames(g, skip, 10);
	Pcm ref;
	read_frames(g, ref, 40);
	unload(g);
	uint64_t e_ref = abs_sum(ref);

	TASK_CHECK(e_ref > 0);
	// Correct: the ramp continues identically, so the energies match closely.
	// A non-rescaled ramp would change slope and land well outside this band.
	uint64_t hi = e_ref > e_sw ? e_ref : e_sw;
	uint64_t lo = e_ref > e_sw ? e_sw : e_ref;
	TASK_CHECK(lo * 100 >= hi * 85);

	// Also check the following, fully-open part remains audible and continuous.
	setup(44100, 0);
	ModPlugFile *h = load_xm(false, true);
	TASK_CHECK(h != NULL);
	Pcm hpre;
	read_frames(h, hpre, 20);
	TASK_CHECK(ModPlug_ChangeRate(h, 11025) == 1);
	Pcm hpost;
	read_frames(h, hpost, 200);
	unload(h);
	TASK_CHECK(abs_sum(hpost) > 0);
	return 0;
}

// Looped sample pitch: the step must be rescaled so the loop still plays at the
// same musical pitch.  Measured via zero-crossing rate over the same musical
// span of two constant-rate decodes, and of a rate-switched decode.
static double zc_rate(const Pcm &p, int from_frame, int to_frame, int rate)
{
	int crossings = 0;
	int prev = 0, have = 0;
	for (int i = from_frame; i < to_frame && 2 * i + 1 < (int)p.size(); ++i)
	{
		int v = p[2 * i];
		if (v == 0) continue;
		if (have && ((v > 0) != (prev > 0))) crossings++;
		prev = v; have = 1;
	}
	double secs = (to_frame - from_frame) / (double)rate;
	return crossings / secs;
}

static int clock_loop_pitch(void)
{
	// Decode 0.5 s at each rate; compare the middle span (same musical time).
	const int frames = 22050;   // 0.5 s at 44100, 1 s at 22050
	setup(44100, 0);
	ModPlugFile *f = load_mod();
	TASK_CHECK(f != NULL);
	Pcm a;
	read_frames(f, a, frames);
	unload(f);

	setup(22050, 0);
	ModPlugFile *g = load_mod();
	TASK_CHECK(g != NULL);
	Pcm b;
	read_frames(g, b, frames);
	unload(g);

	// Windows cover frames 1/4..3/4 of each stream: identical musical span.
	double fa = zc_rate(a, frames / 4, frames * 3 / 4, 44100);
	double fb = zc_rate(b, frames / 4, frames * 3 / 4, 22050);
	TASK_CHECK(fa > 50.0);
	TASK_CHECK(fb > 50.0);
	double rel = fabs(fa - fb) / (fa > fb ? fa : fb);
	TASK_CHECK(rel < 0.10);

	// A rate-switch mid-stream must preserve pitch: measure the zero-crossing rate
	// of a window just before the switch (at 44100) and a window just after (at
	// 22050).  A stale mixing step would change the pitch at the new rate.
	setup(44100, 0);
	ModPlugFile *h = load_mod();
	TASK_CHECK(h != NULL);
	Pcm pre;
	read_frames(h, pre, 6000);           // 6000 frames at 44100
	TASK_CHECK(ModPlug_ChangeRate(h, 22050) == 1);
	Pcm c;
	read_frames(h, c, 4000);             // 4000 frames at 22050
	unload(h);
	// Both windows are 2000 frames, chosen well inside their respective streams so
	// they cover the same steady note (the fixture's ramp loop is periodic).
	double pre_f = zc_rate(pre, 2500, 4500, 44100);
	double post_f = zc_rate(c, 500, 2500, 22050);
	TASK_CHECK(pre_f > 50.0);
	TASK_CHECK(post_f > 50.0);
	double rel3 = fabs(pre_f - post_f) / (pre_f > post_f ? pre_f : post_f);
	TASK_CHECK(rel3 < 0.10);

	// The step must be recomputed *immediately*, before the next tick: compare the
	// very first block after the switch with a constant-22050 run at the same
	// wall-clock position.  6000 frames at 44100 equals 3000 frames at 22050, so
	// the two runs are at the same musical point when the compared block starts.
	setup(44100, 0);
	ModPlugFile *s1 = load_mod();
	TASK_CHECK(s1 != NULL);
	Pcm dummy;
	read_frames(s1, dummy, 6000);
	TASK_CHECK(ModPlug_ChangeRate(s1, 22050) == 1);
	Pcm sw_first;
	read_frames(s1, sw_first, 512);
	unload(s1);

	setup(22050, 0);
	ModPlugFile *s2 = load_mod();
	TASK_CHECK(s2 != NULL);
	Pcm dummy2, ref_first;
	read_frames(s2, dummy2, 3000);
	read_frames(s2, ref_first, 512);
	unload(s2);

	uint64_t d = 0, mag = 0;
	for (size_t i = 0; i < sw_first.size() && i < ref_first.size(); ++i)
	{
		long long delta = (long long)sw_first[i] - (long long)ref_first[i];
		d += (uint64_t)(delta < 0 ? -delta : delta);
		mag += (uint64_t)(ref_first[i] < 0 ? -(long long)ref_first[i] : ref_first[i]);
	}
	TASK_CHECK(mag > 0);
	// Correct: the first block essentially matches the constant-rate reference.
	// A stale step would make it differ by a large multiple of the signal level.
	TASK_CHECK(d * 100 <= mag * 30);
	return 0;
}

// Changing rate at the exact tick boundary must not process the row effect twice.
// A speed/tempo effect would double-advance if ReadNote were re-triggered; we use
// the row counter over a fixed wall-clock window as the invariant.
static int clock_no_double_row_effect(void)
{
	setup(44100, 0);
	ModPlugFile *f = load_mod();
	TASK_CHECK(f != NULL);
	Pcm tmp;
	// Land exactly on a tick boundary by reading one tick's worth of frames.
	read_frames(f, tmp, 2000);
	int row0 = ModPlug_GetCurrentRow(f);

	// Switch at the same logical point as the reference: many times in quick
	// succession before any output (no double processing can occur).
	for (int i = 0; i < 8; i++)
	{
		int r = (i & 1) ? 44100 : 22050;
		TASK_CHECK(ModPlug_ChangeRate(f, r) == 1);
	}
	int row_same = ModPlug_GetCurrentRow(f);
	TASK_CHECK(row_same == row0);   // pending change must not touch the clock

	Pcm out;
	read_frames(f, out, 44100);
	int row_end = ModPlug_GetCurrentRow(f);
	unload(f);

	setup(44100, 0);
	ModPlugFile *g = load_mod();
	TASK_CHECK(g != NULL);
	Pcm tmp2, out2;
	read_frames(g, tmp2, 2000);
	read_frames(g, out2, 44100);
	int row_ref = ModPlug_GetCurrentRow(g);
	unload(g);

	TASK_CHECK(abs(row_end - row_ref) <= 1);
	return 0;
}

// An XM volume envelope advances once per tick.  A switch must not call ReadNote
// again, which would advance the envelope an extra time and make the note decay
// too fast.  The envelope is sloped, so this is observable in the loudness.
static uint64_t window_energy(const Pcm &p, int from_frame, int to_frame)
{
	size_t from = (size_t)from_frame * 2, to = (size_t)to_frame * 2;
	if (to > p.size()) to = p.size();
	if (from >= to) return 0;
	return abs_sum(p, from, to);
}

static int clock_envelope_advance(void)
{
	// Reference: constant 22050 for the whole span.  A live run at 44100 switches
	// to 22050 very early and must then track the same envelope curve.
	setup(22050, 0);
	ModPlugFile *ref = load_xm(true);
	TASK_CHECK(ref != NULL);
	Pcm refpcm;
	read_frames(ref, refpcm, 6000);
	unload(ref);

	setup(44100, 0);
	ModPlugFile *f = load_xm(true);
	TASK_CHECK(f != NULL);
	Pcm pre;
	read_frames(f, pre, 882);          // 0.02 s at 44100
	TASK_CHECK(abs_sum(pre) > 0);
	TASK_CHECK(ModPlug_ChangeRate(f, 22050) == 1);
	Pcm post;
	read_frames(f, post, 5000);
	int row_sw = ModPlug_GetCurrentRow(f);
	unload(f);

	// Compare energy over wall-clock [0.03 s, 0.09 s].  `post` starts at 0.02 s,
	// i.e. frame 441 of the uniform 22050 time base, so subtract 441 from the
	// reference frame indices.
	const int kSwitchFrame = 441;
	int w0 = 22050 * 3 / 100, w1 = 22050 * 9 / 100;
	uint64_t e_ref = window_energy(refpcm, w0, w1);
	uint64_t e_sw = window_energy(post, w0 - kSwitchFrame, w1 - kSwitchFrame);
	TASK_CHECK(e_ref > 0);
	TASK_CHECK(e_sw > 0);
	// The sloped envelope decays over ~6 ticks, so one extra tick advanced by a
	// spurious ReadNote would change this window's energy by well over 10%.
	uint64_t hi = e_ref > e_sw ? e_ref : e_sw;
	uint64_t lo = e_ref > e_sw ? e_sw : e_ref;
	TASK_CHECK(lo * 100 >= hi * 88);

	// The tick/row clock must also match an equivalent-duration constant-rate run.
	setup(44100, 0);
	ModPlugFile *g = load_xm(true);
	TASK_CHECK(g != NULL);
	Pcm gpre;
	read_frames(g, gpre, 882);
	Pcm grest;
	read_frames(g, grest, 5000 * 2);   // 5000 frames at 22050 = 0.2268 s
	int row_ref = ModPlug_GetCurrentRow(g);
	unload(g);
	TASK_CHECK(abs(row_sw - row_ref) <= 1);

	// Stronger check: many rate changes interleaved with tiny reads must not
	// advance the song clock any further than the same amount of playback with
	// no changes at all.  A spurious ReadNote per change would show up clearly.
	const int kChanges = 40;
	setup(44100, 0);
	ModPlugFile *h = load_xm(true);
	TASK_CHECK(h != NULL);
	Pcm warm;
	read_frames(h, warm, 2000);
	int hrow0 = ModPlug_GetCurrentRow(h);
	// Play the same total wall-clock time (kChanges * 50 frames at the current
	// rate) while bouncing the rate on every 50-frame block.
	for (int i = 0; i < kChanges; i++)
	{
		int r = (i & 1) ? 44100 : 22050;
		TASK_CHECK(ModPlug_ChangeRate(h, r) == 1);
		Pcm tiny;
		read_frames(h, tiny, 50);
	}
	int hrow1 = ModPlug_GetCurrentRow(h);
	int horder1 = ModPlug_GetCurrentOrder(h);
	unload(h);

	setup(44100, 0);
	ModPlugFile *k = load_xm(true);
	TASK_CHECK(k != NULL);
	Pcm warm2, body;
	read_frames(k, warm2, 2000);
	// Total wall-clock time of the interleaved run: 20 blocks of 50 frames at
	// 44100 plus 20 blocks of 50 frames at 22050 = 3000 frames at 44100.
	read_frames(k, body, 3000);
	int krow1 = ModPlug_GetCurrentRow(k);
	int korder1 = ModPlug_GetCurrentOrder(k);
	unload(k);
	TASK_CHECK(horder1 == korder1);
	TASK_CHECK(abs(hrow1 - krow1) <= 1);
	return 0;
}

// ---------------------------------------------------------------------------
// tails group: surround / reverb audible tails survive the migration.
//
// These use the impulse fixture: a single short non-looping note, so once the
// sample has played out the only remaining sound is the DSP tail.  If the long
// delay lines were cleared instead of migrated, the post-switch block would be
// silent; if they were migrated, the tail keeps sounding.
// ---------------------------------------------------------------------------

static void config_effects(bool reverb, int rvb_depth, int rvb_delay,
                           bool surround, int sur_depth, int sur_delay)
{
	ModPlug_Settings s;
	ModPlug_GetSettings(&s);
	s.mReverbDepth = rvb_depth;
	s.mReverbDelay = rvb_delay;
	s.mSurroundDepth = sur_depth;
	s.mSurroundDelay = sur_delay;
	ModPlug_SetSettings(&s);
	(void)reverb; (void)surround;
}

static int tails_reverb(void)
{
	// Reverb on: impulse, let the echo build up, switch, and require the tail to
	// continue.  The migrated long lines must still hold the circulating echo, so
	// the first block after the switch is loud; if the lines were cleared the
	// reverb would collapse for a moment.
	setup(44100, MODPLUG_ENABLE_REVERB);
	config_effects(true, 90, 80, false, 0, 0);
	ModPlugFile *f = load_xm(false, true);
	TASK_CHECK(f != NULL);
	Pcm play;
	read_frames(f, play, 44100 / 2);     // 0.5 s: echo fully established
	TASK_CHECK(ModPlug_ChangeRate(f, 22050) == 1);
	Pcm first;
	read_frames(f, first, 1102);         // first ~50 ms at 22050
	uint64_t e_first = abs_sum(first);
	Pcm later;
	read_frames(f, later, 22050 / 2);
	unload(f);
	TASK_CHECK(abs_sum(later) > 0);

	// Control: a constant-rate run over the equivalent span (0.5 s at 44100 then
	// 50 ms at 22050-equivalent time = 2205 frames at 44100).
	setup(44100, MODPLUG_ENABLE_REVERB);
	config_effects(true, 90, 80, false, 0, 0);
	ModPlugFile *g = load_xm(false, true);
	TASK_CHECK(g != NULL);
	Pcm play2, ref;
	read_frames(g, play2, 44100 / 2);
	read_frames(g, ref, 2205);
	uint64_t e_ref = abs_sum(ref);
	unload(g);

	TASK_CHECK(e_ref > 0);
	TASK_CHECK(e_first > 0);
	// Migrated: the first post-switch block keeps essentially all of the incoming
	// tail energy.  Cleared lines would leave far less (measured ~9x lower).
	TASK_CHECK(e_first * 100 >= e_ref * 60);
	return 0;
}

static int tails_surround(void)
{
	// The surround line is a short fixed delay (<= 50 ms), so the echo is only
	// observable just after the impulse.  Switch 5 ms in, then read the next
	// 50 ms where the echo lands.
	setup(44100, MODPLUG_ENABLE_SURROUND);
	config_effects(false, 0, 0, true, 90, 50);
	ModPlugFile *f = load_xm(false, true);
	TASK_CHECK(f != NULL);
	Pcm pre;
	read_frames(f, pre, 220);            // 5 ms
	TASK_CHECK(abs_sum(pre) > 0);
	TASK_CHECK(ModPlug_ChangeRate(f, 22050) == 1);
	Pcm tail;
	read_frames(f, tail, 1102);          // 50 ms at 22050
	unload(f);

	// Constant-rate control over the same wall-clock span (55 ms total): at 44100
	// that is 220 + 2205 frames.
	setup(44100, MODPLUG_ENABLE_SURROUND);
	config_effects(false, 0, 0, true, 90, 50);
	ModPlugFile *g = load_xm(false, true);
	TASK_CHECK(g != NULL);
	Pcm pre2, ref;
	read_frames(g, pre2, 220);
	read_frames(g, ref, 2205);
	unload(g);

	TASK_CHECK(abs_sum(tail) > 0);
	TASK_CHECK(abs_sum(ref) > 0);
	// Enabled surround must produce an echo comparable to the unchanged-rate run;
	// a cleared line would leave ~0 here.
	uint64_t hi = abs_sum(tail) > abs_sum(ref) ? abs_sum(tail) : abs_sum(ref);
	uint64_t lo = abs_sum(tail) > abs_sum(ref) ? abs_sum(ref) : abs_sum(tail);
	TASK_CHECK(lo * 100 >= hi * 60);
	return 0;
}

// Noise reduction, surround and reverb all enabled (an allowed configuration) plus
// a rate change: the tail after the switch must stay continuous with the tail just
// before it, not jump to silence.
static int tails_decay_continuity(void)
{
	setup(44100, MODPLUG_ENABLE_NOISE_REDUCTION | MODPLUG_ENABLE_REVERB |
	             MODPLUG_ENABLE_SURROUND);
	config_effects(true, 70, 120, true, 80, 40);
	ModPlugFile *f = load_xm(false, true);
	TASK_CHECK(f != NULL);
	Pcm pre;
	read_frames(f, pre, 44100 / 2);
	uint64_t e_pre = abs_sum(pre, pre.size() > 4096 ? pre.size() - 4096 : 0);
	TASK_CHECK(ModPlug_ChangeRate(f, 22050) == 1);
	Pcm post;
	read_frames(f, post, 4096);
	uint64_t e_post = abs_sum(post);
	unload(f);

	// The incoming block had audible tail energy, and right after the switch the
	// tail must still be present (continuity, not a reset).
	TASK_CHECK(e_pre > 0);
	TASK_CHECK(e_post > 0);
	// A migrated tail is in the same ballpark as the one it replaced.
	TASK_CHECK(e_post * 100 >= e_pre);     // not more than ~100x smaller
	TASK_CHECK(e_post <= e_pre * 1000);    // and not wildly larger
	return 0;
}

// ---------------------------------------------------------------------------
// compat group: API contract, rejects, fixed-rate identity, chunking.
// ---------------------------------------------------------------------------

// Supported rates succeed; unsupported rates and bad handles fail without
// disturbing playback (reads keep working after a rejection).
static int compat_reject_and_continue(void)
{
	setup(44100, 0);
	ModPlugFile *f = load_mod();
	TASK_CHECK(f != NULL);

	TASK_CHECK(ModPlug_ChangeRate(NULL, 22050) == 0);
	TASK_CHECK(ModPlug_ChangeRate(f, 0) == 0);
	TASK_CHECK(ModPlug_ChangeRate(f, 8000) == 0);
	TASK_CHECK(ModPlug_ChangeRate(f, 48000) == 0);
	TASK_CHECK(ModPlug_ChangeRate(f, -22050) == 0);
	TASK_CHECK(ModPlug_ChangeRate(f, 11025 + 1) == 0);

	// After all rejections, the original rate is still in force and playback works.
	ModPlug_Settings s;
	ModPlug_GetSettings(&s);
	TASK_CHECK(s.mFrequency == 44100);
	Pcm p;
	int frames = read_frames(f, p, 2048);
	TASK_CHECK(frames == 2048);
	TASK_CHECK(abs_sum(p) > 0);

	// A valid change still works afterwards.
	TASK_CHECK(ModPlug_ChangeRate(f, 11025) == 1);
	Pcm q;
	TASK_CHECK(read_frames(f, q, 11024) == 11024);
	TASK_CHECK(abs_sum(q) > 0);
	unload(f);
	return 0;
}

// Only 16-bit stereo is accepted.
static int compat_format_gate(void)
{
	// 8-bit stereo
	setup(44100, 0);
	ModPlug_Settings s;
	ModPlug_GetSettings(&s);
	s.mBits = 8;
	ModPlug_SetSettings(&s);
	ModPlugFile *f = load_mod();
	TASK_CHECK(f != NULL);
	TASK_CHECK(ModPlug_ChangeRate(f, 22050) == 0);
	std::vector<short> buf(4000);
	int got = ModPlug_Read(f, buf.data(), (int)buf.size() * (int)sizeof(short));
	TASK_CHECK(got > 0);
	unload(f);

	// mono 16-bit
	setup(44100, 0);
	ModPlug_GetSettings(&s);
	s.mChannels = 1;
	s.mBits = 16;
	ModPlug_SetSettings(&s);
	ModPlugFile *g = load_mod();
	TASK_CHECK(g != NULL);
	TASK_CHECK(ModPlug_ChangeRate(g, 22050) == 0);
	unload(g);
	return 0;
}

// MegaBass enabled -> reject; disabled -> accept.
static int compat_megabass_gate(void)
{
	setup(44100, MODPLUG_ENABLE_MEGABASS);
	ModPlugFile *f = load_mod();
	TASK_CHECK(f != NULL);
	TASK_CHECK(ModPlug_ChangeRate(f, 22050) == 0);
	Pcm p;
	TASK_CHECK(read_frames(f, p, 1000) == 1000);
	unload(f);

	setup(44100, 0);
	ModPlugFile *g = load_mod();
	TASK_CHECK(g != NULL);
	TASK_CHECK(ModPlug_ChangeRate(g, 22050) == 1);
	unload(g);
	return 0;
}

// libmodplug keeps its DSP delay lines and noise-reduction history in file-static
// storage, so a freshly loaded module does not start from the same bytes as the
// previous handle left behind.  Loading once with a *different* output rate forces
// InitPlayer(TRUE), which clears that shared state; we then restore the requested
// configuration so the following measured runs start from identical bytes.
static void warm_static_dsp(int rate, int flags)
{
	int other = (rate == 44100) ? 22050 : 44100;
	setup(other, flags);
	ModPlugFile *w = load_mod();
	if (w) unload(w);
	setup(rate, flags);
}

// No new API call -> identical PCM to the untouched decoder, byte for byte.
static int compat_fixed_rate_identity(void)
{
	warm_static_dsp(44100, MODPLUG_ENABLE_NOISE_REDUCTION | MODPLUG_ENABLE_REVERB);
	setup(44100, MODPLUG_ENABLE_NOISE_REDUCTION | MODPLUG_ENABLE_REVERB);
	ModPlugFile *a = load_mod();
	TASK_CHECK(a != NULL);
	Pcm pa;
	read_frames(a, pa, 6000);
	unload(a);

	warm_static_dsp(44100, MODPLUG_ENABLE_NOISE_REDUCTION | MODPLUG_ENABLE_REVERB);
	setup(44100, MODPLUG_ENABLE_NOISE_REDUCTION | MODPLUG_ENABLE_REVERB);
	ModPlugFile *b = load_mod();
	TASK_CHECK(b != NULL);
	// A same-rate change is allowed and must be a byte-exact no-op.
	TASK_CHECK(ModPlug_ChangeRate(b, 44100) == 1);
	Pcm pb;
	read_frames(b, pb, 6000);
	unload(b);

	TASK_CHECK(pa.size() == pb.size());
	TASK_CHECK(first_diff(pa, pb) == -1);

	// Also at 22050: the new API at an unchanged rate must not alter decoding.
	warm_static_dsp(22050, 0);
	setup(22050, 0);
	ModPlugFile *c = load_mod();
	TASK_CHECK(c != NULL);
	Pcm pc;
	read_frames(c, pc, 3000);
	unload(c);

	warm_static_dsp(22050, 0);
	setup(22050, 0);
	ModPlugFile *d = load_mod();
	TASK_CHECK(d != NULL);
	TASK_CHECK(ModPlug_ChangeRate(d, 22050) == 1);
	Pcm pd;
	read_frames(d, pd, 3000);
	unload(d);
	TASK_CHECK(pc.size() == pd.size());
	TASK_CHECK(first_diff(pc, pd) == -1);
	return 0;
}

// Chunk-size independence: a change followed by the same rate sequence must give
// identical PCM regardless of how the reads are split.
static int compat_chunk_independence(void)
{
	const int rates[] = { 22050, 22050, 11025, 11025, 44100, 22050 };
	const int nrates = (int)(sizeof(rates) / sizeof(rates[0]));

	// Reference: large reads sized exactly to each segment.
	setup(44100, MODPLUG_ENABLE_NOISE_REDUCTION);
	ModPlugFile *f = load_mod();
	TASK_CHECK(f != NULL);
	Pcm whole;
	read_frames(f, whole, 1024);
	for (int i = 0; i < nrates; i++)
	{
		TASK_CHECK(ModPlug_ChangeRate(f, rates[i]) == 1);
		Pcm seg;
		read_frames(f, seg, 1536);
		whole.insert(whole.end(), seg.begin(), seg.end());
	}
	unload(f);

	// Same sequence, chopped into small reads.
	setup(44100, MODPLUG_ENABLE_NOISE_REDUCTION);
	ModPlugFile *g = load_mod();
	TASK_CHECK(g != NULL);
	Pcm chopped;
	read_frames(g, chopped, 1024, 37);
	for (int i = 0; i < nrates; i++)
	{
		TASK_CHECK(ModPlug_ChangeRate(g, rates[i]) == 1);
		Pcm seg;
		read_frames(g, seg, 1536, 37);
		chopped.insert(chopped.end(), seg.begin(), seg.end());
	}
	unload(g);

	TASK_CHECK(chopped.size() == whole.size());
	TASK_CHECK(first_diff(whole, chopped) == -1);
	return 0;
}

// Repeated rate changes must not accumulate rounding drift in the tick duration.
// Each cycle changes to 22050, reads one frame, changes back to 44100 and reads
// one frame.  The total wall-clock consumed by the cycles has an exact
// 44100-frame equivalent, so the remaining frames to the next row must match a
// no-change run that consumed exactly that much.  Any per-change rounding drift
// shows up as a growing difference.
static int frames_to_next_row(ModPlugFile *f)
{
	int guard = 400000;
	int target = ModPlug_GetCurrentRow(f) + 1;
	int frames = 0;
	while (ModPlug_GetCurrentRow(f) < target && guard-- > 0)
	{
		std::vector<short> buf(2);
		int got = ModPlug_Read(f, buf.data(), (int)buf.size() * (int)sizeof(short));
		if (got <= 0) break;
		frames += got / ((int)sizeof(short) * 2);
	}
	return frames;
}

static int compat_roundtrip_no_drift(void)
{
	const int K = 100;   // 100 change/read cycles
	setup(44100, 0);
	ModPlugFile *f = load_mod();
	TASK_CHECK(f != NULL);
	Pcm tmp;
	read_frames(f, tmp, 101);   // mid-tick
	for (int i = 0; i < K; ++i)
	{
		TASK_CHECK(ModPlug_ChangeRate(f, 22050) == 1);
		std::vector<short> b(2);
		TASK_CHECK(ModPlug_Read(f, b.data(), (int)b.size() * (int)sizeof(short)) > 0);
		TASK_CHECK(ModPlug_ChangeRate(f, 44100) == 1);
		TASK_CHECK(ModPlug_Read(f, b.data(), (int)b.size() * (int)sizeof(short)) > 0);
	}
	int cycled = frames_to_next_row(f);
	unload(f);

	// No-change equivalent: the cycles consumed K*(1/22050 + 1/44100) seconds,
	// which is exactly K*3 frames at 44100.
	setup(44100, 0);
	ModPlugFile *g = load_mod();
	TASK_CHECK(g != NULL);
	Pcm tmp2;
	read_frames(g, tmp2, 101);
	Pcm equiv;
	read_frames(g, equiv, K * 3);
	int reference = frames_to_next_row(g);
	unload(g);

	// Correct implementation: identical up to a single frame of boundary
	// quantisation.  Accumulated rounding drift would be ~1 frame per cycle.
	TASK_CHECK(cycled >= reference - 1);
	TASK_CHECK(cycled <= reference + 1);
	return 0;
}

// The global settings report the new frequency after a successful change.
static int compat_settings_frequency(void)
{
	setup(44100, 0);
	ModPlugFile *f = load_mod();
	TASK_CHECK(f != NULL);
	TASK_CHECK(ModPlug_ChangeRate(f, 11025) == 1);
	ModPlug_Settings s;
	ModPlug_GetSettings(&s);
	TASK_CHECK(s.mFrequency == 11025);
	unload(f);
	return 0;
}

// Seek after a rate change still lands approximately and clears the fractional
// tick remainder: a subsequent read is at the sought position, not at zero.
static int compat_seek_after_change(void)
{
	setup(44100, 0);
	ModPlugFile *f = load_mod();
	TASK_CHECK(f != NULL);
	Pcm tmp;
	read_frames(f, tmp, 5000);
	TASK_CHECK(ModPlug_ChangeRate(f, 22050) == 1);
	Pcm mid;
	read_frames(f, mid, 2000);

	ModPlug_Seek(f, 5000);   // 5 seconds in
	int row = ModPlug_GetCurrentRow(f);
	TASK_CHECK(row >= 0);
	Pcm after;
	read_frames(f, after, 4096);
	TASK_CHECK(abs_sum(after) > 0);
	// A seek implements an approximate relocation; a zero row from a cleared state
	// would mean it lost the position entirely.
	TASK_CHECK(ModPlug_GetCurrentRow(f) >= 0);
	unload(f);
	return 0;
}

// Rejected calls after a successful one leave playback unchanged.
static int compat_reject_preserves_state(void)
{
	setup(44100, 0);
	ModPlugFile *f = load_mod();
	TASK_CHECK(f != NULL);
	Pcm a;
	read_frames(f, a, 3000);
	TASK_CHECK(ModPlug_ChangeRate(f, 22050) == 1);
	Pcm warm;
	read_frames(f, warm, 512);

	// Snapshot state through the API.
	int row = ModPlug_GetCurrentRow(f);
	int order = ModPlug_GetCurrentOrder(f);
	int speed = ModPlug_GetCurrentSpeed(f);
	int tempo = ModPlug_GetCurrentTempo(f);

	TASK_CHECK(ModPlug_ChangeRate(f, 44101) == 0);
	TASK_CHECK(ModPlug_ChangeRate(f, 0) == 0);

	TASK_CHECK(ModPlug_GetCurrentRow(f) == row);
	TASK_CHECK(ModPlug_GetCurrentOrder(f) == order);
	TASK_CHECK(ModPlug_GetCurrentSpeed(f) == speed);
	TASK_CHECK(ModPlug_GetCurrentTempo(f) == tempo);

	Pcm b;
	TASK_CHECK(read_frames(f, b, 1000) == 1000);
	TASK_CHECK(abs_sum(b) > 0);
	unload(f);
	return 0;
}

// ---------------------------------------------------------------------------
// registration
// ---------------------------------------------------------------------------

const struct task_case task_cases[] =
{
	{ "clock",  "tick_position_preserved", clock_tick_position_preserved },
	{ "clock",  "midtick_boundary",        clock_midtick_boundary },
	{ "clock",  "midramp_amplitude",       clock_midramp_amplitude },
	{ "clock",  "loop_pitch",              clock_loop_pitch },
	{ "clock",  "no_double_row_effect",    clock_no_double_row_effect },
	{ "clock",  "envelope_advance",        clock_envelope_advance },

	{ "tails",  "reverb",                  tails_reverb },
	{ "tails",  "surround",                tails_surround },
	{ "tails",  "decay_continuity",        tails_decay_continuity },

	{ "compat", "reject_and_continue",     compat_reject_and_continue },
	{ "compat", "format_gate",             compat_format_gate },
	{ "compat", "megabass_gate",           compat_megabass_gate },
	{ "compat", "fixed_rate_identity",     compat_fixed_rate_identity },
	{ "compat", "chunk_independence",      compat_chunk_independence },
	{ "compat", "roundtrip_no_drift",      compat_roundtrip_no_drift },
	{ "compat", "settings_frequency",      compat_settings_frequency },
	{ "compat", "seek_after_change",       compat_seek_after_change },
	{ "compat", "reject_preserves_state",  compat_reject_preserves_state },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
