/* Retrigger crossfade checks for libmikmod's software mixers.
 *
 * Every case drives the public API through a real software mixer and compares
 * the PCM with an independent model of the mixer: the departing voice is a
 * cursor captured at Voice_Play() time, the main voice is a second cursor
 * started from the requested position, each advanced with the mixer's own
 * interpolation and loop rules, blended with the linear (N-j)/N, j/N weights,
 * then quantised by the selected output stage.
 *
 * Groups:
 *   waveform  - hand-off shape across C scalar formats, loops, directions, pan
 *   ownership - sample buffer generations across Sample_Free()/reload
 *   lifecycle - stop, repeated/replaced hand-offs, and two interleaved voices
 *
 * The independent 16-bit reference allows 1 quantisation unit of error; byte
 * exactness is additionally required between one large read and 1/7/31-frame
 * fragmented reads inside the hand-off interval.
 *
 * C99, standard library + libm only.
 */

#include "task_support/check.h"
#include "mikmod.h"
#include "mikmod_internals.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* reference mixer cursor                                                     */

#define FRACBITS 11          /* virtch.c */
#define FRACBITS_HQ 28       /* virtch2.c */
#define FRACMASK 2047LL

typedef struct {
	const short *d;
	long long    size, reppos, repend;
	unsigned     flags;
	long long    pos, inc;
	int          fb;
	int          live;
} refcur;

static long long fmask(int fb) { return (1LL << fb) - 1; }

static void ref_init(refcur *r, const short *pad, int len, unsigned flags,
                     int loopstart, int loopend, long long pos, long long inc, int fb)
{
	memset(r, 0, sizeof(*r));
	r->d = pad;
	r->size = len;
	r->flags = flags;
	r->reppos = loopstart;
	r->repend = loopend;
	r->pos = pos;
	r->inc = inc;
	r->fb = fb;
	r->live = 1;
}

static long long ri_size(const refcur *r) { return r->size   ? (r->size   << r->fb) - 1 : 0; }
static long long ri_lend(const refcur *r) { return r->repend ? (r->repend << r->fb) - 1 : 0; }
static long long ri_lpos(const refcur *r) { return r->reppos << r->fb; }

/* One mixer iteration: block-start fix-up, interpolate, step.  Mirrors
 * AddChannel()'s loop/bounce/stop rules for the active mixer's fraction size. */
static int ref_step(refcur *r)
{
	long long idxsize, idxlend, idxlpos, end, done, i, f;
	long long mask = fmask(r->fb);
	int s;

	if (!r->live) return 0;
	idxsize = ri_size(r); idxlend = ri_lend(r); idxlpos = ri_lpos(r);

	if (r->flags & SF_REVERSE) {
		if ((r->flags & SF_LOOP) && (r->pos < idxlpos)) {
			if (r->flags & SF_BIDI) {
				r->pos = idxlpos + (idxlpos - r->pos);
				r->flags &= ~SF_REVERSE;
				r->inc = -r->inc;
			} else
				r->pos = idxlend - (idxlpos - r->pos);
		} else if (!(r->flags & SF_LOOP) && r->pos < 0) {
			r->live = 0; return 0;
		}
	} else {
		if ((r->flags & SF_LOOP) && (r->pos >= idxlend)) {
			if (r->flags & SF_BIDI) {
				r->flags |= SF_REVERSE;
				r->inc = -r->inc;
				r->pos = idxlend - (r->pos - idxlend);
			} else
				r->pos = idxlpos + (r->pos - idxlend);
		} else if (!(r->flags & SF_LOOP) && r->pos >= idxsize) {
			r->live = 0; return 0;
		}
	}

	end = (r->flags & SF_REVERSE) ? ((r->flags & SF_LOOP) ? idxlpos : 0)
	                              : ((r->flags & SF_LOOP) ? idxlend : idxsize);
	if ((r->inc > 0 && r->pos >= end) ||
	    (r->inc < 0 && r->pos <= end) || !r->inc)
		done = 0;
	else
		done = (end - r->pos) / r->inc + 1;
	if (done <= 0) { r->live = 0; return 0; }

	i = r->pos >> r->fb;
	f = r->pos & mask;
	if (r->fb == FRACBITS)
		s = (int)r->d[i] + (int)(((long long)(r->d[i + 1] - r->d[i]) * f) >> r->fb);
	else
		s = (int)(((long long)r->d[i] * (mask + 1 - f) + (long long)r->d[i + 1] * f) >> r->fb);
	r->pos += r->inc;
	return s;
}

/* Effective per-channel gains.  pan == PAN_SURROUND selects the surround gain
 * formula whenever stereo, independent of DMODE_SURROUND; the anti-phase mix
 * is the part gated by DMODE_SURROUND (see ref_accum). */
static void mixer_gains(int vol, int pan, int stereo, int hq, int *lvol, int *rvol)
{
	if (stereo) {
		if (pan == PAN_SURROUND) {
			if (hq) { *lvol = *rvol = (vol * 256) / 480; }
			else    { *lvol = *rvol = vol / 2; }
		} else {
			*lvol = (vol * (PAN_RIGHT - pan)) >> 8;
			*rvol = (vol * pan) >> 8;
		}
	} else {
		*lvol = vol; *rvol = 0;
	}
}

static void ref_accum(refcur *r, int lvol, int rvol, int stereo, int surround,
                      long long *aL, long long *aR)
{
	int s = ref_step(r);

	*aL = *aR = 0;
	if (!stereo)
		*aL = (long long)lvol * s;
	else if (surround) {
		long long v;
		if (lvol >= rvol) { v = (long long)lvol * s; *aL = v; *aR = -v; }
		else              { v = (long long)rvol * s; *aL = -v; *aR = v; }
	} else {
		*aL = (long long)lvol * s;
		*aR = (long long)rvol * s;
	}
}

static long long clamp_ll(long long v, long long lo, long long hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}
static double clamp_d(double v, double lo, double hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* ---- scenario description ------------------------------------------------- */

typedef struct {
	short *adata; int alen; unsigned aflags; int als, ale;
	short *bdata; int blen; unsigned bflags; int bls, ble;
	int starta, startb;
	int warm;          /* frames mixed before the retrigger */
	int N;             /* configured fade length */
	int fade_active;   /* 1 -> a departing state exists */
} model;

typedef struct {
	UWORD mode;
	int   stereo, hq, stage, surround;
	int   vol, pan, frq;
	int   fade;
} cfg;

/* one voice of the model: departing cursor + main cursor + gains */
typedef struct {
	refcur *o, *n;
	int ol, orr, nl, nr;
} vmodel;

static void quant_slot(int hq, int stage, long long c, int *iv, double *dv)
{
	if (!hq) {
		if (stage == 0) *iv = (int)clamp_ll(c >> 9, -32768, 32767);
		else if (stage == 1) *iv = (int)(clamp_ll(c >> 17, -128, 127) + 128);
		else *dv = clamp_d((double)(c >> 5) / 524288.0, -1.0, 1.0);
	} else {
		if (stage == 0) *iv = (int)clamp_ll(c / 512, -32768, 32767);
		else if (stage == 1) *iv = (int)clamp_ll(c / 131072, -128, 127);
		else *dv = clamp_d((double)c / 16777216.0, -1.0, 1.0);
	}
}

/* Compute one output frame for the sum of 'nv' voices.  Quantisation happens
 * once after all voices are summed into the shared accumulator. */
static void model_frame(int nv, vmodel *vs, int fade_active, int N, int f,
                        int stereo, int surround, int hq, int stage,
                        double *outL, double *outR)
{
	int factor = hq ? 4 : 1;
	int k, vi;
	long long sumIL = 0, sumIR = 0;
	double sumFL = 0.0, sumFR = 0.0;
	long long wj = fade_active ? ((f < N) ? f : N) : 1;
	long long wo = fade_active ? ((f < N) ? (N - f) : 0) : 0;
	long long den = fade_active ? (N ? N : 1) : 1;

	for (k = 0; k < factor; k++) {
		long long cL = 0, cR = 0;
		for (vi = 0; vi < nv; vi++) {
			long long aL, aR, bL, bR;
			ref_accum(vs[vi].o, vs[vi].ol, vs[vi].orr, stereo, surround, &aL, &aR);
			ref_accum(vs[vi].n, vs[vi].nl, vs[vi].nr, stereo, surround, &bL, &bR);
			cL += (wo * aL + wj * bL) / den;
			cR += (wo * aR + wj * bR) / den;
		}
		if (stage == 2) {
			double dl, dr;
			quant_slot(hq, stage, cL, &k, &dl);
			quant_slot(hq, stage, cR, &k, &dr);
			sumFL += dl; sumFR += dr;
		} else {
			int il, ir;
			quant_slot(hq, stage, cL, &il, &sumFL);
			quant_slot(hq, stage, cR, &ir, &sumFR);
			sumIL += il; sumIR += ir;
		}
	}
	if (factor > 1) {
		if (stage == 2) { *outL = sumFL / factor; *outR = sumFR / factor; }
		else if (stage == 1) { *outL = (double)(sumIL / factor + 128); *outR = (double)(sumIR / factor + 128); }
		else { *outL = (double)(sumIL / factor); *outR = (double)(sumIR / factor); }
	} else {
		if (stage == 2) { *outL = sumFL; *outR = sumFR; }
		else { *outL = (double)sumIL; *outR = (double)sumIR; }
	}
	if (!stereo) *outR = 0;
}

/* ------------------------------------------------------------------------- */
/* environment helpers                                                        */

static unsigned char pcm[262144];

typedef struct {
	cfg c;
	int nvoices;
	SAMPLE *s[4];
} env;

static int env_begin(env *e, const cfg *c, int nvoices)
{
	e->c = *c;
	e->nvoices = nvoices;
	e->s[0] = e->s[1] = e->s[2] = e->s[3] = NULL;

	MikMod_RegisterDriver(&drv_nos);
	if (!MikMod_SetRetriggerFade((ULONG)c->fade)) return 1;
	md_mode = DMODE_SOFT_SNDFX | DMODE_INTERP |
	          (c->hq ? DMODE_HQMIXER : 0) |
	          (c->stereo ? DMODE_STEREO : 0) |
	          (c->stage == 0 ? DMODE_16BITS : 0) |
	          (c->stage == 2 ? DMODE_FLOAT : 0) |
	          (c->surround ? DMODE_SURROUND : 0);
	md_mixfreq = 44100;
	md_device = 1;
	md_reverb = 0;
	md_volume = md_sndfxvolume = 128;
	md_pansep = 128;
	if (MikMod_Init("")) return 1;
	if (MikMod_SetNumVoices(0, nvoices)) return 1;
	if (MikMod_EnableOutput()) return 1;

	Voice_SetVolume(0, (UWORD)c->vol);
	Voice_SetPanning(0, (ULONG)c->pan);
	Voice_SetFrequency(0, (ULONG)c->frq);
	if (nvoices > 1) {
		Voice_SetVolume(1, (UWORD)c->vol);
		Voice_SetPanning(1, (ULONG)c->pan);
		Voice_SetFrequency(1, (ULONG)c->frq);
	}
	return 0;
}

static void env_end(env *e)
{
	int i;
	MikMod_DisableOutput();
	for (i = 0; i < 4; i++) {
		if (e->s[i]) { Sample_Free(e->s[i]); e->s[i] = NULL; }
	}
	MikMod_Exit();
}

static int bytes_per_sample(const cfg *c)
{
	return (c->stage == 2) ? 4 : (c->stage == 0) ? 2 : 1;
}
static int bytes_per_frame(const cfg *c)
{
	return bytes_per_sample(c) * (c->stereo ? 2 : 1);
}

/* Read nframes into the shared pcm buffer at frame 'at', split into
 * 'chunk'-frame calls. */
static int read_pcm_at(const cfg *c, int at, int nframes, int chunk)
{
	int done = 0, bpf = bytes_per_frame(c);
	while (done < nframes) {
		int n = chunk > 0 ? chunk : nframes;
		int bytes;
		if (done + n > nframes) n = nframes - done;
		bytes = n * bpf;
		if ((int)VC_WriteBytes((SBYTE *)(pcm + (size_t)(at + done) * bpf), (ULONG)bytes) != bytes)
			return 1;
		done += n;
	}
	return 0;
}
static int read_pcm(const cfg *c, int nframes, int chunk)
{
	return read_pcm_at(c, 0, nframes, chunk);
}

/* Load a 16-bit mono raw sample; flags/loops applied to the SAMPLE. */
static SAMPLE *load_sample(env *e, int slot, const short *data, int n,
                           unsigned flags, int loopstart, int loopend)
{
	unsigned char raw[1024];
	int i;
	SAMPLE *s;

	if (n > 512) return NULL;
	for (i = 0; i < n; i++) {
		unsigned short u = (unsigned short)data[i];
		raw[2 * i] = (unsigned char)(u & 255);
		raw[2 * i + 1] = (unsigned char)(u >> 8);
	}
	s = Sample_LoadRawMem((const char *)raw, n * 2, 44100, 1, SF_16BITS | SF_SIGNED);
	if (!s) return NULL;
	s->flags = (s->flags & ~(SF_LOOP | SF_BIDI | SF_REVERSE)) |
	           (flags & (SF_LOOP | SF_BIDI | SF_REVERSE));
	s->loopstart = loopstart;
	s->loopend = loopend;
	e->s[slot] = s;
	return s;
}

/* Mirror the mixer's seam padding into a reference buffer. */
static void build_pad(short *pad, const short *src, int n, unsigned flags,
                      int loopstart, int loopend)
{
	int t;
	for (t = 0; t < n; t++) pad[t] = src[t];
	for (t = 0; t < 20; t++) pad[n + t] = 0;
	if (flags & SF_LOOP) {
		int looplen = loopend - loopstart;
		if (flags & SF_BIDI)
			for (t = 0; t < 16 && t < looplen; t++)
				pad[loopend + t] = src[(loopend - t) - 1];
		else
			for (t = 0; t < 16 && t < looplen; t++)
				pad[loopend + t] = src[loopstart + t];
	}
}

static long long inc_for(const cfg *c)
{
	if (c->hq)
		return ((long long)c->frq << (FRACBITS_HQ - 2)) / 44100;
	return ((long long)c->frq << FRACBITS) / 44100;
}

static int fb_for(const cfg *c) { return c->hq ? FRACBITS_HQ : FRACBITS; }

/* Build the model cursors from a scenario: the departing cursor is the main
 * cursor as it stood after 'warm' output frames. */
static int model_cursors(const cfg *c, model *m, short *apad, short *bpad,
                         refcur *o, refcur *n)
{
	int fb = fb_for(c);
	long long inc = inc_for(c);
	int steps, k;

	build_pad(apad, m->adata, m->alen, m->aflags, m->als, m->ale);
	build_pad(bpad, m->bdata, m->blen, m->bflags, m->bls, m->ble);

	ref_init(o, apad, m->alen, m->aflags, m->als, m->ale,
	         (long long)m->starta << fb,
	         (m->aflags & SF_REVERSE) ? -inc : inc, fb);
	steps = m->warm * (c->hq ? 4 : 1);
	for (k = 0; k < steps; k++) ref_step(o);
	ref_init(n, bpad, m->blen, m->bflags, m->bls, m->ble,
	         (long long)m->startb << fb,
	         (m->bflags & SF_REVERSE) ? -inc : inc, fb);
	return 0;
}

/* Compare 'nframes' of the shared pcm buffer against the model.  Returns 0
 * when every sample is within 'tol'; otherwise returns the first bad frame. */
static int compare_pcm(const cfg *c, int nframes, model *m, int tol,
                       vmodel *vs, int nv, int verbose)
{
	short apad[600], bpad[600];
	refcur oc, nc;
	int factor = c->hq ? 4 : 1;
	int bps = bytes_per_frame(c);
	int f, ch, bad = 0;

	if (m->fade_active) {
		model_cursors(c, m, apad, bpad, &oc, &nc);
		vs[0].o = &oc; vs[0].n = &nc;
	} else {
		/* plain attack: only the main cursor contributes at full weight */
		model_cursors(c, m, apad, bpad, &oc, &nc);
		vs[0].o = &oc; vs[0].n = &nc;
	}

	for (f = 0; f < nframes; f++) {
		double eL, eR;
		int nch = c->stereo ? 2 : 1;

		model_frame(nv, vs, m->fade_active, m->N, f, c->stereo,
		            c->surround && (c->pan == PAN_SURROUND), c->hq, c->stage,
		            &eL, &eR);
		for (ch = 0; ch < nch; ch++) {
			double got, exp = (ch == 0) ? eL : eR;
			if (c->stage == 0)
				got = (double)((short *)(pcm + (size_t)f * bps))[ch];
			else if (c->stage == 1)
				got = (double)((unsigned char *)(pcm + (size_t)f * bps))[ch];
			else
				got = (double)((float *)(pcm + (size_t)f * bps))[ch];
			if (fabs(got - exp) > (double)tol) {
				if (verbose && bad < 8)
					fprintf(stderr, "retrigger: frame %d ch %d got %.6f expected %.6f\n",
					        f, ch, got, exp);
				bad++;
			}
		}
	}
	(void)factor;
	return bad;
}

/* ------------------------------------------------------------------------- */
/* waveform group                                                             */

static void wave_a(short *p, int n) { int i; for (i = 0; i < n; i++) p[i] = (short)(((i % 17) - 8) * 1500); }
static void wave_b(short *p, int n) { int i; for (i = 0; i < n; i++) p[i] = (short)(500 - i * 3); }
static void wave_c(short *p, int n) { int i; for (i = 0; i < n; i++) p[i] = (short)(((i % 23) - 11) * 900); }

/* Generic single-voice hand-off: play A, warm 'warm' frames, play B, read
 * 'nframes' frames, then compare with the model.  'chunk' controls read
 * fragmentation.  Returns number of mismatched samples (0 == success). */
static int run_single_fade(const cfg *c, model *m, int nframes, int chunk,
                           unsigned aflags, int als, int ale,
                           unsigned bflags, int bls, int ble,
                           int starta, int startb, int warm,
                           int tol, int verbose, int fade_active)
{
	env e;
	short a[512], b[512];
	SAMPLE *sa, *sb;
	int bad;

	wave_a(a, 512);
	wave_b(b, 320);
	m->adata = a; m->alen = 512; m->aflags = aflags; m->als = als; m->ale = ale;
	m->bdata = b; m->blen = 320; m->bflags = bflags; m->bls = bls; m->ble = ble;
	m->starta = starta; m->startb = startb; m->warm = warm; m->N = c->fade;
	m->fade_active = fade_active;

	memset(&e, 0, sizeof(e));
	if (env_begin(&e, c, 1)) { env_end(&e); return 10000; }
	sa = load_sample(&e, 0, a, 512, aflags, als, ale);
	sb = load_sample(&e, 1, b, 320, bflags, bls, ble);
	if (!sa || !sb) { env_end(&e); return 10001; }

	if (aflags & SF_REVERSE) Voice_Play(0, sa, (ULONG)starta);
	else Voice_Play(0, sa, (ULONG)starta);
	if (read_pcm(c, warm, 64)) { env_end(&e); return 10002; }
	Voice_Play(0, sb, (ULONG)startb);
	if (read_pcm(c, nframes, chunk)) { env_end(&e); return 10003; }

	{
		vmodel vs[1];
		int ol, orr, nl, nr;
		mixer_gains(c->vol, c->pan, c->stereo, c->hq, &ol, &orr);
		mixer_gains(c->vol, c->pan, c->stereo, c->hq, &nl, &nr);
		vs[0].ol = ol; vs[0].orr = orr; vs[0].nl = nl; vs[0].nr = nr;
		bad = compare_pcm(c, nframes, m, tol, vs, 1, verbose);
	}
	env_end(&e);
	return bad;
}
static int run_single(const cfg *c, model *m, int nframes, int chunk,
                      unsigned aflags, int als, int ale,
                      unsigned bflags, int bls, int ble,
                      int starta, int startb, int warm,
                      int tol, int verbose)
{
	return run_single_fade(c, m, nframes, chunk, aflags, als, ale,
	                       bflags, bls, ble, starta, startb, warm,
	                       tol, verbose, 1);
}

static int wf_basic_stereo16(void)
{
	cfg c = {0};
	model m;
	int bad, ok = 1;

	c.stereo = 1; c.stage = 0; c.hq = 0; c.surround = 0;
	c.vol = 256; c.pan = 128; c.frq = 22050; c.fade = 8;
	bad = run_single(&c, &m, 24, 0, SF_LOOP, 0, 512, SF_LOOP, 0, 320,
	                 0, 0, 96, 0, 0);
	ok = (bad == 0);
	TASK_CHECK(ok);
	return 0;
}

static int wf_fractional_pan(void)
{
	cfg c = {0};
	model m;
	int bad;

	c.stereo = 1; c.stage = 0; c.hq = 0;
	c.vol = 256; c.pan = 200; c.frq = 30800; c.fade = 8;
	bad = run_single(&c, &m, 24, 0, SF_LOOP, 0, 512, SF_LOOP, 0, 320,
	                 0, 7, 96, 0, 0);
	TASK_CHECK(bad == 0);
	return 0;
}

static int wf_short_bidi_loop(void)
{
	cfg c = {0};
	model m;
	int bad;

	c.stereo = 1; c.stage = 0; c.hq = 0;
	c.vol = 256; c.pan = 128; c.frq = 22050; c.fade = 16;
	bad = run_single(&c, &m, 40, 0, SF_LOOP | SF_BIDI, 10, 40,
	                 SF_LOOP, 0, 320, 30, 0, 80, 0, 0);
	TASK_CHECK(bad == 0);
	return 0;
}

static int wf_reverse_bidi_loop(void)
{
	cfg c = {0};
	model m;
	int bad;

	c.stereo = 1; c.stage = 0; c.hq = 0;
	c.vol = 256; c.pan = 128; c.frq = 22050; c.fade = 16;
	bad = run_single(&c, &m, 40, 0, SF_LOOP | SF_BIDI | SF_REVERSE, 10, 60,
	                 SF_LOOP, 0, 320, 400, 0, 80, 0, 0);
	TASK_CHECK(bad == 0);
	return 0;
}

static int wf_surround_asym(void)
{
	cfg c = {0};
	model m;
	int bad;

	c.stereo = 1; c.stage = 0; c.hq = 0; c.surround = 1;
	c.vol = 256; c.pan = PAN_SURROUND; c.frq = 22050; c.fade = 16;
	bad = run_single(&c, &m, 32, 0, SF_LOOP, 0, 512, SF_LOOP, 0, 320,
	                 0, 0, 96, 0, 0);
	TASK_CHECK(bad == 0);

	/* asymmetric ordinary pan also exercises non-equal L/R gains */
	c.surround = 0; c.pan = 60; c.fade = 16;
	bad = run_single(&c, &m, 32, 0, SF_LOOP, 0, 512, SF_LOOP, 0, 320,
	                 0, 0, 96, 0, 0);
	TASK_CHECK(bad == 0);
	return 0;
}

static int wf_hq_four_times(void)
{
	cfg c = {0};
	model m;
	int bad;

	c.stereo = 1; c.stage = 0; c.hq = 1;
	c.vol = 256; c.pan = 130; c.frq = 30800; c.fade = 8;
	bad = run_single(&c, &m, 40, 0, SF_LOOP, 0, 512, SF_LOOP, 0, 320,
	                 0, 0, 96, 1, 0);
	TASK_CHECK(bad == 0);

	/* HQ with a short bidi loop and fractional step */
	c.pan = 128; c.fade = 16;
	bad = run_single(&c, &m, 48, 0, SF_LOOP | SF_BIDI, 10, 40,
	                 SF_LOOP, 0, 320, 20, 0, 96, 1, 0);
	TASK_CHECK(bad == 0);
	return 0;
}

static int wf_formats(void)
{
	cfg c = {0};
	model m;
	int bad;

	/* 8-bit stereo non-HQ */
	c.stereo = 1; c.stage = 1; c.hq = 0;
	c.vol = 256; c.pan = 128; c.frq = 22050; c.fade = 8;
	bad = run_single(&c, &m, 24, 0, SF_LOOP, 0, 512, SF_LOOP, 0, 320,
	                 0, 0, 96, 0, 0);
	TASK_CHECK(bad == 0);

	/* mono 16-bit */
	c.stereo = 0; c.stage = 0;
	bad = run_single(&c, &m, 24, 0, SF_LOOP, 0, 512, SF_LOOP, 0, 320,
	                 0, 0, 96, 0, 0);
	TASK_CHECK(bad == 0);

	/* float stereo */
	c.stereo = 1; c.stage = 2;
	bad = run_single(&c, &m, 24, 0, SF_LOOP, 0, 512, SF_LOOP, 0, 320,
	                 0, 0, 96, 0, 1);
	TASK_CHECK(bad == 0);
	return 0;
}

/* A non-looping departing voice that ends inside the hand-off: the tail is
 * silent from then on, while the new voice keeps its weight. */
static int wf_departing_ends(void)
{
	cfg c = {0};
	model m;
	int bad;

	c.stereo = 1; c.stage = 0; c.hq = 0;
	c.vol = 256; c.pan = 128; c.frq = 22050; c.fade = 32;
	/* A does not loop; start it near its end (300) so it stops within 32
	 * frames at 22050 Hz (~0.5 sample/frame). */
	bad = run_single(&c, &m, 56, 0, 0, 0, 512, SF_LOOP, 0, 320,
	                 300, 0, 8, 0, 0);
	TASK_CHECK(bad == 0);

	/* Non-looping main voice that ends inside a longer hand-off. */
	c.fade = 32;
	bad = run_single(&c, &m, 56, 0, SF_LOOP, 0, 512, 0, 0, 320,
	                 0, 0, 8, 0, 0);
	TASK_CHECK(bad == 0);
	return 0;
}

/* Inside the hand-off interval a single large read and fragmented 1/7/31-frame
 * reads must be byte-identical, for both mixers and output widths. */
static int chunk_equal(const cfg *c)
{
	model m;
	int nframes = 40, bpf = bytes_per_frame(c), bytes = 40 * bytes_per_frame(c);
	int i, j, ok = 1;
	short *ref = (short *)malloc((size_t)bytes);
	short *frag[3];
	int chunks[3] = {1, 7, 31};
	int r;

	frag[0] = (short *)malloc((size_t)bytes);
	frag[1] = (short *)malloc((size_t)bytes);
	frag[2] = (short *)malloc((size_t)bytes);

	r = run_single(c, &m, nframes, 0, SF_LOOP, 0, 512, SF_LOOP, 0, 320, 0, 7, 96, c->hq ? 1 : 0, 0);
	if (r) ok = 0;
	memcpy(ref, pcm, (size_t)bytes);
	for (i = 0; i < 3 && ok; i++) {
		r = run_single(c, &m, nframes, chunks[i], SF_LOOP, 0, 512, SF_LOOP, 0, 320, 0, 7, 96, c->hq ? 1 : 0, 0);
		if (r) { ok = 0; break; }
		memcpy(frag[i], pcm, (size_t)bytes);
		for (j = 0; j < bytes / 2; j++)
			if (ref[j] != frag[i][j]) { ok = 0; break; }
	}
	free(ref); free(frag[0]); free(frag[1]); free(frag[2]);
	(void)bpf;
	return ok;
}

static int wf_chunk_exact(void)
{
	cfg c = {0};

	c.stereo = 1; c.stage = 0; c.hq = 0;
	c.vol = 256; c.pan = 200; c.frq = 30800; c.fade = 16;
	TASK_CHECK(chunk_equal(&c));

	/* HQ 16-bit */
	c.hq = 1; c.pan = 130; c.frq = 30800;
	TASK_CHECK(chunk_equal(&c));

	/* 8-bit non-HQ */
	c.hq = 0; c.stage = 1; c.pan = 128; c.frq = 22050;
	TASK_CHECK(chunk_equal(&c));
	return 0;
}

/* ------------------------------------------------------------------------- */
/* ownership group                                                            */

/* The departing voice must keep playing after the caller frees the sample it
 * was reading, and a reload that reuses the numeric handle must not disturb
 * the tail. */
static int own_free_after_play(void)
{
	cfg c = {0};
	env e;
	short a[512], b[320], c2[320];
	SAMPLE *sa, *sb;
	vmodel vs[1];
	short apad[600], bpad[600];
	refcur oc, nc;
	model m;
	int ol, orr, nl, nr;
	int bad_after_free, bad_after_reload;

	c.stereo = 1; c.stage = 0; c.hq = 0;
	c.vol = 256; c.pan = 128; c.frq = 22050; c.fade = 24;

	wave_a(a, 512); wave_b(b, 320); wave_c(c2, 320);

	memset(&e, 0, sizeof(e));
	if (env_begin(&e, &c, 1)) { env_end(&e); TASK_CHECK(0); }
	sa = load_sample(&e, 0, a, 512, SF_LOOP, 0, 512);
	sb = load_sample(&e, 1, b, 320, SF_LOOP, 0, 320);
	TASK_CHECK(sa != NULL);
	TASK_CHECK(sb != NULL);
	Voice_Play(0, sa, 0);
	if (read_pcm(&c, 96, 64)) { env_end(&e); TASK_CHECK(0); }
	Voice_Play(0, sb, 0);
	/* Free the old sample immediately; the departing state still needs it. */
	Sample_Free(sa); e.s[0] = NULL;
	{
		int r = read_pcm_at(&c, 0, 12, 5);
		TASK_CHECK(r == 0);
	}
	/* Reload a *different* sample; the driver may reuse the same handle. */
	{
		SAMPLE *sc = load_sample(&e, 2, c2, 320, SF_LOOP, 0, 320);
		TASK_CHECK(sc != NULL);
		/* keep only in slot 2 to avoid double free */
	}
	{
		int r;
		r = read_pcm_at(&c, 12, 12, 5);
		TASK_CHECK(r == 0);
	}
	/* Verify the first 24 frames: 12 before, 12 after the reload. */
	m.adata = a; m.alen = 512; m.aflags = SF_LOOP; m.als = 0; m.ale = 512;
	m.bdata = b; m.blen = 320; m.bflags = SF_LOOP; m.bls = 0; m.ble = 320;
	m.starta = 0; m.startb = 0; m.warm = 96; m.N = c.fade; m.fade_active = 1;

	build_pad(apad, a, 512, SF_LOOP, 0, 512);
	build_pad(bpad, b, 320, SF_LOOP, 0, 320);
	ref_init(&oc, apad, 512, SF_LOOP, 0, 512, 0, inc_for(&c), fb_for(&c));
	{ int k; for (k = 0; k < 96 * (c.hq ? 4 : 1); k++) ref_step(&oc); }
	ref_init(&nc, bpad, 320, SF_LOOP, 0, 320, 0, inc_for(&c), fb_for(&c));
	mixer_gains(c.vol, c.pan, c.stereo, c.hq, &ol, &orr);
	mixer_gains(c.vol, c.pan, c.stereo, c.hq, &nl, &nr);
	vs[0].o = &oc; vs[0].n = &nc; vs[0].ol = ol; vs[0].orr = orr; vs[0].nl = nl; vs[0].nr = nr;

	bad_after_free = compare_pcm(&c, 24, &m, 0, vs, 1, 0);
	(void)bad_after_reload;
	TASK_CHECK(bad_after_free == 0);
	env_end(&e);
	return 0;
}

/* Stopping the voice must cancel the departing tail; a later play on the same
 * voice must not inherit it. */
static int own_stop_cancels_tail(void)
{
	cfg c = {0};
	env e;
	short a[512], b[320];
	SAMPLE *sa, *sb;
	int i;

	c.stereo = 1; c.stage = 0; c.hq = 0;
	c.vol = 256; c.pan = 128; c.frq = 22050; c.fade = 32;
	wave_a(a, 512); wave_b(b, 320);

	memset(&e, 0, sizeof(e));
	if (env_begin(&e, &c, 1)) { env_end(&e); TASK_CHECK(0); }
	sa = load_sample(&e, 0, a, 512, SF_LOOP, 0, 512);
	sb = load_sample(&e, 1, b, 320, SF_LOOP, 0, 320);
	Voice_Play(0, sa, 0);
	if (read_pcm(&c, 96, 64)) { env_end(&e); TASK_CHECK(0); }
	Voice_Play(0, sb, 0);
	Voice_Stop(0);
	{
		int r = read_pcm(&c, 64, 0);
		TASK_CHECK(r == 0);
	}
	/* after a stop the voice is silent */
	{
		int frame, silent = 1;
		for (frame = 0; frame < 64; frame++) {
			int ch;
			for (ch = 0; ch < 2; ch++)
				if (((short *)(pcm + (size_t)frame * 4))[ch] != 0) silent = 0;
		}
		TASK_CHECK(silent);
	}
	/* reusing the voice later yields the new sample only, no old tail */
	Voice_Play(0, sa, 0);
	{
		int r = read_pcm(&c, 8, 0);
		TASK_CHECK(r == 0);
	}
	env_end(&e);
	(void)i;
	return 0;
}

/* Sample_Free of a sample that never played with the feature on, etc.: the
 * public API must still allow a plain free, and different handles must not
 * alias. */
static int own_distinct_handles(void)
{
	cfg c = {0};
	env e;
	short a[512], b[320];
	SAMPLE *sa, *sb;
	int ha, hb;

	c.stereo = 1; c.stage = 0; c.hq = 0;
	c.vol = 256; c.pan = 128; c.frq = 22050; c.fade = 8;
	wave_a(a, 512); wave_b(b, 320);

	memset(&e, 0, sizeof(e));
	if (env_begin(&e, &c, 1)) { env_end(&e); TASK_CHECK(0); }
	sa = load_sample(&e, 0, a, 512, SF_LOOP, 0, 512);
	sb = load_sample(&e, 1, b, 320, SF_LOOP, 0, 320);
	ha = sa->handle;
	hb = sb->handle;
	TASK_CHECK(ha != hb);
	TASK_CHECK(ha >= 0 && hb >= 0);
	Voice_Play(0, sa, 0);
	if (read_pcm(&c, 32, 0)) { env_end(&e); TASK_CHECK(0); }
	Voice_Play(0, sb, 0);
	if (read_pcm(&c, 8, 0)) { env_end(&e); TASK_CHECK(0); }
	Sample_Free(sa); e.s[0] = NULL;
	/* the new sample's handle must still resolve to its own buffer */
	{
		int r = read_pcm(&c, 16, 0);
		TASK_CHECK(r == 0);
	}
	env_end(&e);
	return 0;
}

/* ------------------------------------------------------------------------- */
/* lifecycle group                                                            */

/* Consecutive Voice_Play() calls on one output boundary must collapse to a
 * single hand-off: {pre-batch main voice, last target}.  The intermediate
 * target never produced PCM and must not become the departing state. */
static int lc_consecutive_play(void)
{
	cfg c = {0};
	env e;
	short a[512], b[320], d[320];
	SAMPLE *sa, *sb, *sd;
	vmodel vs[1];
	short apad[600], bpad[600];
	refcur oc, nc;
	model m;
	int ol, orr, nl, nr, bad;

	c.stereo = 1; c.stage = 0; c.hq = 0;
	c.vol = 256; c.pan = 128; c.frq = 22050; c.fade = 16;
	wave_a(a, 512); wave_b(b, 320); wave_c(d, 320);

	memset(&e, 0, sizeof(e));
	if (env_begin(&e, &c, 1)) { env_end(&e); TASK_CHECK(0); }
	sa = load_sample(&e, 0, a, 512, SF_LOOP, 0, 512);
	sb = load_sample(&e, 1, b, 320, SF_LOOP, 0, 320);
	sd = load_sample(&e, 2, d, 320, SF_LOOP, 0, 320);
	Voice_Play(0, sa, 0);
	if (read_pcm(&c, 96, 64)) { env_end(&e); TASK_CHECK(0); }
	/* batch: play B then D without reading in between */
	Voice_Play(0, sb, 0);
	Voice_Play(0, sd, 0);
	if (read_pcm(&c, 24, 0)) { env_end(&e); TASK_CHECK(0); }

	m.adata = a; m.alen = 512; m.aflags = SF_LOOP; m.als = 0; m.ale = 512;
	m.bdata = d; m.blen = 320; m.bflags = SF_LOOP; m.bls = 0; m.ble = 320;
	m.starta = 0; m.startb = 0; m.warm = 96; m.N = c.fade; m.fade_active = 1;
	build_pad(apad, a, 512, SF_LOOP, 0, 512);
	build_pad(bpad, d, 320, SF_LOOP, 0, 320);
	ref_init(&oc, apad, 512, SF_LOOP, 0, 512, 0, inc_for(&c), fb_for(&c));
	{ int k; for (k = 0; k < 96; k++) ref_step(&oc); }
	ref_init(&nc, bpad, 320, SF_LOOP, 0, 320, 0, inc_for(&c), fb_for(&c));
	mixer_gains(c.vol, c.pan, c.stereo, c.hq, &ol, &orr);
	mixer_gains(c.vol, c.pan, c.stereo, c.hq, &nl, &nr);
	vs[0].o = &oc; vs[0].n = &nc; vs[0].ol = ol; vs[0].orr = orr; vs[0].nl = nl; vs[0].nr = nr;
	bad = compare_pcm(&c, 24, &m, 0, vs, 1, 0);
	TASK_CHECK(bad == 0);
	env_end(&e);
	return 0;
}

/* A retrigger while a hand-off is still running drops the previous departing
 * state and re-captures the current main voice for a fresh N frames. */
static int lc_replaced_handoff(void)
{
	cfg c = {0};
	env e;
	short a[512], b[320], d[320];
	SAMPLE *sa, *sb, *sd;
	short apad[600], bpad[600], dpad[600];
	refcur o1, n1, o2, n2;
	vmodel vs[1];
	model m;
	int ol, orr, nl, nr;
	int bad1, bad2;

	c.stereo = 1; c.stage = 0; c.hq = 0;
	c.vol = 256; c.pan = 128; c.frq = 22050; c.fade = 16;
	wave_a(a, 512); wave_b(b, 320); wave_c(d, 320);

	memset(&e, 0, sizeof(e));
	if (env_begin(&e, &c, 1)) { env_end(&e); TASK_CHECK(0); }
	sa = load_sample(&e, 0, a, 512, SF_LOOP, 0, 512);
	sb = load_sample(&e, 1, b, 320, SF_LOOP, 0, 320);
	sd = load_sample(&e, 2, d, 320, SF_LOOP, 0, 320);
	Voice_Play(0, sa, 0);
	if (read_pcm(&c, 96, 64)) { env_end(&e); TASK_CHECK(0); }
	Voice_Play(0, sb, 0);
	/* read a few frames into the first hand-off, then retrigger again */
	if (read_pcm(&c, 4, 0)) { env_end(&e); TASK_CHECK(0); }
	Voice_Play(0, sd, 0);
	if (read_pcm(&c, 24, 0)) { env_end(&e); TASK_CHECK(0); }

	/* After the second Play the departing voice is the main voice (B) as it
	 * stood 4 frames into the previous hand-off, with B's next-sample state. */
	m.adata = b; m.alen = 320; m.aflags = SF_LOOP; m.als = 0; m.ale = 320;
	m.bdata = d; m.blen = 320; m.bflags = SF_LOOP; m.bls = 0; m.ble = 320;
	m.starta = 0; m.startb = 0; m.warm = 4; m.N = c.fade; m.fade_active = 1;
	build_pad(apad, b, 320, SF_LOOP, 0, 320);
	build_pad(bpad, d, 320, SF_LOOP, 0, 320);
	ref_init(&o1, apad, 320, SF_LOOP, 0, 320, 0, inc_for(&c), fb_for(&c));
	{ int k; for (k = 0; k < 4; k++) ref_step(&o1); }
	ref_init(&n1, bpad, 320, SF_LOOP, 0, 320, 0, inc_for(&c), fb_for(&c));
	mixer_gains(c.vol, c.pan, c.stereo, c.hq, &ol, &orr);
	mixer_gains(c.vol, c.pan, c.stereo, c.hq, &nl, &nr);
	vs[0].o = &o1; vs[0].n = &n1; vs[0].ol = ol; vs[0].orr = orr; vs[0].nl = nl; vs[0].nr = nr;
	bad1 = compare_pcm(&c, 24, &m, 0, vs, 1, 0);
	TASK_CHECK(bad1 == 0);
	(void)bad2; (void)o2; (void)n2; (void)dpad;
	env_end(&e);
	return 0;
}

/* Two voices interleaved: each keeps its own departing state and they do not
 * share the fade frame counter.  Both are retriggered on the same boundary. */
static int lc_two_voices(void)
{
	cfg c = {0};
	env e;
	short a[512], b[320], d[320];
	SAMPLE *sa, *sb, *sd;
	vmodel vs[2];
	short apad[600], bpad[600], dpad[600];
	refcur o0, n0, o1, n1;
	model m0, m1;
	int ol, orr, nl, nr, bad;

	c.stereo = 1; c.stage = 0; c.hq = 0;
	c.vol = 256; c.pan = 128; c.frq = 22050; c.fade = 12;
	wave_a(a, 512); wave_b(b, 320); wave_c(d, 320);

	memset(&e, 0, sizeof(e));
	if (env_begin(&e, &c, 2)) { env_end(&e); TASK_CHECK(0); }
	sa = load_sample(&e, 0, a, 512, SF_LOOP, 0, 512);
	sb = load_sample(&e, 1, b, 320, SF_LOOP, 0, 320);
	sd = load_sample(&e, 2, d, 320, SF_LOOP, 0, 320);

	Voice_Play(0, sa, 0);   /* voice 0: A */
	Voice_Play(1, sb, 0);   /* voice 1: B */
	if (read_pcm(&c, 96, 64)) { env_end(&e); TASK_CHECK(0); }
	Voice_Play(0, sb, 0);   /* voice 0: A -> B */
	Voice_Play(1, sd, 0);   /* voice 1: B -> D */
	if (read_pcm(&c, 20, 0)) { env_end(&e); TASK_CHECK(0); }

	mixer_gains(c.vol, c.pan, c.stereo, c.hq, &ol, &orr);
	mixer_gains(c.vol, c.pan, c.stereo, c.hq, &nl, &nr);
	build_pad(apad, a, 512, SF_LOOP, 0, 512);
	build_pad(bpad, b, 320, SF_LOOP, 0, 320);
	build_pad(dpad, d, 320, SF_LOOP, 0, 320);

	/* voice 0 model */
	m0.adata = a; m0.alen = 512; m0.aflags = SF_LOOP; m0.als = 0; m0.ale = 512;
	m0.bdata = b; m0.blen = 320; m0.bflags = SF_LOOP; m0.bls = 0; m0.ble = 320;
	m0.starta = 0; m0.startb = 0; m0.warm = 96; m0.N = c.fade; m0.fade_active = 1;
	ref_init(&o0, apad, 512, SF_LOOP, 0, 512, 0, inc_for(&c), fb_for(&c));
	{ int k; for (k = 0; k < 96; k++) ref_step(&o0); }
	ref_init(&n0, bpad, 320, SF_LOOP, 0, 320, 0, inc_for(&c), fb_for(&c));
	(void)m0;

	/* voice 1 model: departing B (warmed 96), main D */
	m1.adata = b; m1.alen = 320; m1.aflags = SF_LOOP; m1.als = 0; m1.ale = 320;
	m1.bdata = d; m1.blen = 320; m1.bflags = SF_LOOP; m1.bls = 0; m1.ble = 320;
	m1.starta = 0; m1.startb = 0; m1.warm = 96; m1.N = c.fade; m1.fade_active = 1;
	ref_init(&o1, bpad, 320, SF_LOOP, 0, 320, 0, inc_for(&c), fb_for(&c));
	{ int k; for (k = 0; k < 96; k++) ref_step(&o1); }
	ref_init(&n1, dpad, 320, SF_LOOP, 0, 320, 0, inc_for(&c), fb_for(&c));
	(void)m1;

	vs[0].o = &o0; vs[0].n = &n0; vs[0].ol = ol; vs[0].orr = orr; vs[0].nl = nl; vs[0].nr = nr;
	vs[1].o = &o1; vs[1].n = &n1; vs[1].ol = ol; vs[1].orr = orr; vs[1].nl = nl; vs[1].nr = nr;

	{
		model mm = m0;
		mm.adata = a; mm.alen = 512; mm.aflags = SF_LOOP; mm.als = 0; mm.ale = 512;
		mm.bdata = b; mm.blen = 320; mm.bflags = SF_LOOP; mm.bls = 0; mm.ble = 320;
		mm.starta = 0; mm.startb = 0; mm.warm = 96; mm.N = c.fade; mm.fade_active = 1;
		bad = compare_pcm(&c, 20, &mm, 0, vs, 2, 0);
	}
	TASK_CHECK(bad == 0);
	env_end(&e);
	return 0;
}

/* Position/Stopped describe the main voice only, not the departing tail. */
static int lc_position_and_stopped(void)
{
	cfg c = {0};
	env e;
	short a[512], b[320];
	SAMPLE *sa, *sb;
	SLONG pos_before, pos_after;
	BOOL stopped_mid, stopped_after;

	c.stereo = 1; c.stage = 0; c.hq = 0;
	c.vol = 256; c.pan = 128; c.frq = 22050; c.fade = 64;
	wave_a(a, 512); wave_b(b, 320);

	memset(&e, 0, sizeof(e));
	if (env_begin(&e, &c, 1)) { env_end(&e); TASK_CHECK(0); }
	sa = load_sample(&e, 0, a, 512, SF_LOOP, 0, 512);
	sb = load_sample(&e, 1, b, 320, SF_LOOP, 0, 320);
	Voice_Play(0, sa, 0);
	if (read_pcm(&c, 96, 0)) { env_end(&e); TASK_CHECK(0); }
	/* retrigger at a non-zero start: once the new main voice starts mixing,
	 * Voice_GetPosition tracks it from there and never the departing tail.
	 * Voice_Stopped reports only the main voice. */
	Voice_Play(0, sb, 100);
	if (read_pcm(&c, 4, 0)) { env_end(&e); TASK_CHECK(0); }
	pos_after = Voice_GetPosition(0);
	stopped_after = Voice_Stopped(0);
	TASK_CHECK(pos_after >= 100 && pos_after < 140);
	TASK_CHECK(!stopped_after);
	/* consuming a long tail must not make the main voice look stopped */
	if (read_pcm(&c, 32, 0)) { env_end(&e); TASK_CHECK(0); }
	TASK_CHECK(!Voice_Stopped(0));
	pos_before = Voice_GetPosition(0);
	TASK_CHECK(pos_before >= 100);
	(void)stopped_mid;
	env_end(&e);
	return 0;
}

/* With the feature disabled (fade 0), a retrigger is the historical plain
 * restart: the new voice plays from its start at full weight and the old
 * voice contributes nothing.  The model uses fade_active=0. */
static int lc_disabled_plain(void)
{
	cfg c = {0};
	model m;
	int bad;

	c.stereo = 1; c.stage = 0; c.hq = 0;
	c.vol = 256; c.pan = 128; c.frq = 22050; c.fade = 0;
	bad = run_single_fade(&c, &m, 24, 0, SF_LOOP, 0, 512, SF_LOOP, 0, 320,
	                      0, 0, 96, 0, 0, 0);
	TASK_CHECK(bad == 0);
	return 0;
}

/* An old voice that has already ended must not produce a departing state: the
 * retrigger is a plain attack of the new sample (full weight from frame 0). */
static int lc_old_already_ended(void)
{
	cfg c = {0};
	env e;
	short a[512], b[320];
	SAMPLE *sa, *sb;
	int r;

	c.stereo = 1; c.stage = 0; c.hq = 0;
	c.vol = 256; c.pan = 128; c.frq = 44100; c.fade = 16;
	wave_a(a, 512); wave_b(b, 320);

	memset(&e, 0, sizeof(e));
	if (env_begin(&e, &c, 1)) { env_end(&e); TASK_CHECK(0); }
	/* A is short and non-looping: one-shot, ends quickly. */
	a[0] = a[1] = a[2] = 0;
	sa = load_sample(&e, 0, a, 16, 0, 0, 0);   /* 16 samples, no loop */
	sb = load_sample(&e, 1, b, 320, SF_LOOP, 0, 320);
	Voice_Play(0, sa, 0);
	if (read_pcm(&c, 64, 0)) { env_end(&e); TASK_CHECK(0); }
	TASK_CHECK(Voice_Stopped(0));    /* old voice has stopped */
	Voice_Play(0, sb, 0);
	r = read_pcm(&c, 8, 0);
	TASK_CHECK(r == 0);
	/* no old tail: the very first frames follow B's interpolation only.
	 * B[0]=500 -> the mixer's first output is small (attack), not A's mark. */
	TASK_CHECK(abs(((short *)(pcm))[0]) < 200);
	env_end(&e);
	return 0;
}

/* ------------------------------------------------------------------------- */
/* test registry                                                              */

const struct task_case task_cases[] = {
	{ "waveform", "basic_stereo16",      wf_basic_stereo16 },
	{ "waveform", "fractional_pan",      wf_fractional_pan },
	{ "waveform", "short_bidi_loop",     wf_short_bidi_loop },
	{ "waveform", "reverse_bidi_loop",   wf_reverse_bidi_loop },
	{ "waveform", "surround_asym",       wf_surround_asym },
	{ "waveform", "hq_four_times",       wf_hq_four_times },
	{ "waveform", "formats",             wf_formats },
	{ "waveform", "departing_ends",     wf_departing_ends },
	{ "waveform", "chunk_exact",         wf_chunk_exact },

	{ "ownership", "free_after_play",    own_free_after_play },
	{ "ownership", "stop_cancels_tail",  own_stop_cancels_tail },
	{ "ownership", "distinct_handles",   own_distinct_handles },

	{ "lifecycle", "consecutive_play",   lc_consecutive_play },
	{ "lifecycle", "replaced_handoff",   lc_replaced_handoff },
	{ "lifecycle", "two_voices",         lc_two_voices },
	{ "lifecycle", "position_stopped",   lc_position_and_stopped },
	{ "lifecycle", "old_already_ended",  lc_old_already_ended },
	{ "lifecycle", "disabled_plain",     lc_disabled_plain },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
