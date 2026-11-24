/*
 * sinc_voice.c - regression for the XMP_INTERP_SINC windowed-sinc mixer.
 *
 * Three driver groups are registered (kernel, trajectory, lifecycle);
 * task_support/driver.c provides main().  Every expected PCM value is
 * computed by the independent reference below from the numerical
 * definition in the task specification - it never calls the production
 * interpolation kernel to produce expected data.
 */

#include "task_support/check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <xmp.h>
#include "common.h"
#include "mixer.h"
#include "virtual.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

extern int task_sinc_binding(void);

/* ================================================================== */
/* Independent numerical reference                                     */
/* ================================================================== */

static double ref_sinc(double t)
{
	double p;
	if (t == 0.0)
		return 1.0;
	p = M_PI * t;
	return sin(p) / p;
}

static double ref_weight(double d, double c)
{
	if (fabs(d) >= 32.0)
		return 0.0;
	return c * ref_sinc(c * d) * 0.5 * (1.0 + cos(M_PI * d / 32.0));
}

static double ref_target(double step)
{
	return 0.9 * (step >= 1.0 ? 1.0 / step : 1.0);
}

/* Stateless 64-tap reference over x[0..n-1]; n<0 is zero and missing
 * future samples are zero. */
static double ref_fir_array(const double *x, long n, double U, double c)
{
	double q = U - 32.0;
	long fq = (long)floor(q);
	long nlo = fq - 31, nhi = fq + 32, k;
	double acc = 0.0, wsum = 0.0;

	for (k = nlo; k <= nhi; k++) {
		double w = ref_weight(q - (double)k, c);
		double xv = 0.0;
		if (k >= 0 && k < n)
			xv = x[k];
		acc += w * xv;
		wsum += w;
	}
	return wsum != 0.0 ? acc / wsum : 0.0;
}

/* Traversal configuration for the stateful independent walker. */
struct ref_loop {
	int origin;		/* physical start frame */
	int len;		/* sample length */
	int start, end;		/* active endpoints */
	int loop;		/* fold inside [start,end) */
	int bidir;
	int reverse;
};

/* Build the logical source sequence x[0..N-1], walking one source frame
 * at a time.  `sw' optionally changes the geometry after logical index
 * sw (sustain release / endpoint change). */
static void ref_walk(double *x, int N,
		     const void *sample, int bits, int stereo,
		     const struct ref_loop *a,
		     int sw, const struct ref_loop *b)
{
	int phys = a->origin;
	int dir = a->reverse ? -1 : 1;
	int ended = 0;
	int i;

	if (phys < 0)
		phys = 0;

	for (i = 0; i < N; i++) {
		const struct ref_loop *cfg = (sw >= 0 && i >= sw) ? b : a;
		int S = cfg->start, E = cfg->end;
		double l = 0.0, r = 0.0;

		if (i > 0 && !ended) {
			int np;
			if (dir > 0) {
				np = phys + 1;
				if (cfg->loop) {
					if (np >= E) {
						if (cfg->bidir && E - S >= 2) {
							dir = -1;
							np = E - 2;
						} else {
							np = S;
						}
					}
				} else if (np >= cfg->len) {
					ended = 1;
				}
				phys = np;
				/* Valid forward positions are the prologue
				 * and [S,E); only repair true out-of-range. */
				if (cfg->loop && (phys < 0 || phys >= E))
					phys = S;
			} else {
				np = phys - 1;
				if (cfg->loop) {
					if (np < S) {
						if (cfg->bidir && E - S >= 2) {
							dir = 1;
							np = S + 1;
						} else {
							np = E - 1;
						}
					}
				} else if (np < 0) {
					ended = 1;
				}
				phys = np;
				if (cfg->loop && (phys < 0 || phys >= E))
					phys = (E > S) ? E - 1 : S;
			}
		}

		if (!ended && phys >= 0 && phys < cfg->len) {
			if (bits == 16) {
				const int16_t *s = (const int16_t *)sample;
				l = s[phys * stereo];
				r = stereo == 2 ? s[phys * stereo + 1] : l;
			} else {
				const int8_t *s = (const int8_t *)sample;
				l = (int)s[phys * stereo] << 8;
				r = stereo == 2 ?
				    ((int)s[phys * stereo + 1] << 8) : l;
			}
		}

		x[2 * i] = l;
		x[2 * i + 1] = r;
	}
}

static int ref_round(double x)
{
	return (int)(x >= 0.0 ? floor(x + 0.5) : ceil(x - 0.5));
}

/* Expected 16-bit PCM sample for one voice at global frame g. */
static int ref_pcm_one(const double *seq_l, int seq_n, double gstep,
		       long g, int vl, int shift)
{
	double U = (double)g * gstep;
	double c = ref_target(gstep);
	double y = ref_fir_array(seq_l, seq_n, U, c);
	long acc = (long)ref_round(y) * vl;
	long v = (long)floor((double)acc / (double)(1 << shift));
	if (v > 32767)
		v = 32767;
	if (v < -32768)
		v = -32768;
	return (int)v;
}

/* ================================================================== */
/* Data/context helpers                                                */
/* ================================================================== */

static const char *data_file(const char *name)
{
	static char path[1200];
	static const char *prefixes[] = {
		"test-dev/data/", "./test-dev/data/",
		"../test-dev/data/", "../../test-dev/data/", NULL
	};
	int i;
	for (i = 0; prefixes[i]; i++) {
		FILE *f;
		snprintf(path, sizeof(path), "%s%s", prefixes[i], name);
		f = fopen(path, "rb");
		if (f) {
			fclose(f);
			return path;
		}
	}
	return NULL;
}

struct rig {
	xmp_context x;
	struct context_data *ctx;
	struct xmp_module *m;
	int sid;
	int ins;
	int sr;
	int fmt;
	int framebytes;
};

static void rig_blank(struct rig *r)
{
	int t, c;
	for (t = 0; t < r->m->trk; t++) {
		struct xmp_track *tr = r->m->xxt[t];
		if (tr)
			memset(tr->event, 0,
			       tr->rows * sizeof(struct xmp_event));
	}
	for (c = 1; c < r->m->chn; c++)
		r->m->xxc[c].flg |= XMP_CHANNEL_MUTE;
}

static int rig_open(struct rig *r, const char *file, int sr, int fmt)
{
	const char *path = data_file(file);
	memset(r, 0, sizeof(*r));
	if (!path)
		return -1;
	r->x = xmp_create_context();
	if (!r->x || xmp_load_module(r->x, (char *)path) != 0)
		return -1;
	r->ctx = (struct context_data *)r->x;
	{
		struct xmp_module_info mi;
		xmp_get_module_info(r->x, &mi);
		r->m = mi.mod;
	}
	r->sr = sr;
	r->fmt = fmt;
	r->framebytes = (fmt & XMP_FORMAT_MONO) ? 2 : 4;
	r->ins = 0;
	r->sid = r->m->xxi[0].sub[0].sid;
	rig_blank(r);
	return 0;
}

static void rig_start_sinc(struct rig *r)
{
	xmp_start_player(r->x, r->sr, r->fmt);
	xmp_set_player(r->x, XMP_PLAYER_INTERP, XMP_INTERP_SINC);
}

static void rig_note(struct rig *r, int chn, int note, int vol)
{
	struct xmp_event e;
	memset(&e, 0, sizeof(e));
	e.note = note ? note : 60;
	e.ins = r->ins + 1;
	e.vol = vol;
	xmp_inject_event(r->x, chn, &e);
}

static void rig_close(struct rig *r)
{
	if (r->x) {
		xmp_end_player(r->x);
		xmp_release_module(r->x);
		xmp_free_context(r->x);
	}
	memset(r, 0, sizeof(*r));
}

/* Replace the chosen sample with our own correctly-prefixed buffer. */
static void rig_install16(struct rig *r, int16_t **storage, int frames,
			  int stereo)
{
	struct xmp_sample *xs = &r->m->xxs[r->sid];
	unsigned char *base = calloc(1, 4 + (size_t)frames * 2 * 2);
	int16_t *s = (int16_t *)(base + 4);
	if (xs->data)
		free(xs->data - 4);
	xs->data = (unsigned char *)s;
	xs->len = frames;
	xs->flg = XMP_SAMPLE_16BIT | (stereo ? XMP_SAMPLE_STEREO : 0);
	xs->lps = xs->lpe = 0;
	*storage = s;
}

static struct mixer_voice *rig_voice(struct rig *r)
{
	int v;
	for (v = 0; v < r->ctx->p.virt.maxvoc; v++) {
		struct mixer_voice *vi = &r->ctx->p.virt.voice_array[v];
		if (vi->chn == 0 && vi->smp == r->sid && vi->sptr)
			return vi;
	}
	return NULL;
}

/* Read `frames` whole output frames in one blocking call. */
static int16_t *rig_read_whole(struct rig *r, int frames)
{
	int bytes = frames * r->framebytes;
	int16_t *buf = calloc(1, bytes + 64);
	if (xmp_play_buffer(r->x, buf, bytes, 0) < 0) {
		/* end of module is fine; buffer keeps trailing zeros */
	}
	return buf;
}

/* Read the same amount using a cycling 1/7/113-frame chunk schedule. */
static int16_t *rig_read_chunked(struct rig *r, int frames)
{
	int bytes = frames * r->framebytes;
	int16_t *buf = calloc(1, bytes + 64);
	int off = 0;
	static const int chunks[] = { 1, 7, 113 };
	int ci = 0;
	while (off < bytes) {
		int nfr = chunks[ci++ % 3];
		int n = nfr * r->framebytes;
		if (n > bytes - off)
			n = bytes - off;
		if (xmp_play_buffer(r->x, (char *)buf + off, n, 0) < 0)
			break;
		off += n;
	}
	return buf;
}

/* ================================================================== */
/* kernel group                                                        */
/* ================================================================== */

static int case_contract(void)
{
	TASK_CHECK(XMP_INTERP_SINC == 3);
	TASK_CHECK(task_sinc_binding() == 0);
	TASK_CHECK(libxmp_sinc_mixers[0] != NULL);
	TASK_CHECK(libxmp_sinc_mixers[7] != NULL);
	TASK_CHECK(libxmp_sinc_mixers[15] != NULL);
	return 0;
}

static int case_coefficients(void)
{
	int i;
	TASK_CHECK(libxmp_sinc_sinc(0.0) == 1.0);
	for (i = -200; i <= 200; i++) {
		double t = i / 37.0;
		TASK_CHECK(fabs(libxmp_sinc_sinc(t) - ref_sinc(t)) < 1e-12);
	}
	TASK_CHECK(libxmp_sinc_weight(32.0, 0.5) == 0.0);
	TASK_CHECK(libxmp_sinc_weight(-32.0, 0.5) == 0.0);
	TASK_CHECK(libxmp_sinc_weight(40.0, 0.9) == 0.0);
	for (i = 0; i < 500; i++) {
		double d = -31.5 + i * 0.126;
		double c = 0.2 + (i % 30) * 0.02;
		double w = libxmp_sinc_weight(d, c);
		double e = ref_weight(d, c);
		TASK_CHECK(fabs(w - e) < 1e-11);
		/* coefficient approximation budget: <= 2e-4 full scale */
		TASK_CHECK(fabs(w - e) <= 2e-4 * 32768.0 / 32768.0 + 1e-12);
	}
	TASK_CHECK(fabs(libxmp_sinc_weight(0.0, 0.7) - 0.7) < 1e-12);
	TASK_CHECK(fabs(libxmp_sinc_cutoff_target(0.25) - 0.9) < 1e-12);
	TASK_CHECK(fabs(libxmp_sinc_cutoff_target(1.0) - 0.9) < 1e-12);
	TASK_CHECK(fabs(libxmp_sinc_cutoff_target(2.0) - 0.45) < 1e-12);
	TASK_CHECK(fabs(libxmp_sinc_cutoff_target(4.0) - 0.225) < 1e-12);
	return 0;
}

static int case_steps_phases(void)
{
	enum { N = 4096 };
	static double x[N];
	static const double steps[3] = { 0.25, 1.0, 4.0 };
	int si, i, k;
	unsigned seed = 12345;

	for (i = 0; i < N; i++) {
		seed = seed * 1103515245u + 12345u;
		x[i] = (double)((seed >> 16) % 2001 - 1000) +
		       4000.0 * sin(2 * M_PI * 0.03 * i);
	}

	for (si = 0; si < 3; si++) {
		double step = steps[si];
		double c = ref_target(step);
		/* non-integer phase offsets */
		static const double frac[4] = { 0.0, 0.13, 0.5, 0.87 };
		int fi;
		for (fi = 0; fi < 4; fi++) {
			for (k = 4; k < 220; k++) {
				double U = step * k + frac[fi] * step;
				double got = libxmp_sinc_fir(x, 0, N, U, c);
				double want = ref_fir_array(x, N, U, c);
				TASK_CHECK(fabs(got - want) < 1e-8);
			}
		}
	}
	return 0;
}

/* Least-squares amplitude of a real tone at normalized frequency f. */
static double fit_amp(const double *y, int n0, int n1, double f)
{
	double cc = 0, ss = 0, sc = 0, yc = 0, ys = 0, det, ac, as;
	int k;
	for (k = n0; k < n1; k++) {
		double ph = 2 * M_PI * f * k;
		double C = cos(ph), S = sin(ph);
		cc += C * C;
		ss += S * S;
		sc += S * C;
		yc += y[k] * C;
		ys += y[k] * S;
	}
	det = cc * ss - sc * sc;
	ac = (yc * ss - ys * sc) / det;
	as = (ys * cc - yc * sc) / det;
	return hypot(ac, as);
}

static int case_spectrum_step4(void)
{
	enum { N = 8192, K = 512 };
	static double x[N];
	static double y[K];
	const double A = 30000.0;
	const double step = 4.0;
	const double c = ref_target(step);
	int i, k;
	double amp, db;

	/* 0.02 cycles/source-frame passband tone. */
	for (i = 0; i < N; i++)
		x[i] = A * sin(2 * M_PI * 0.02 * i + 0.7);
	for (k = 0; k < K; k++)
		y[k] = libxmp_sinc_fir(x, 0, N, step * (k + 64), c);
	amp = fit_amp(y, 40, K - 40, 0.02 * step);
	db = 20 * log10(amp / A);
	TASK_CHECK(fabs(db) < 0.5);

	/* 0.3 cycles/source-frame tone aliases to 0.2 cycles/output. */
	for (i = 0; i < N; i++)
		x[i] = A * sin(2 * M_PI * 0.3 * i + 1.3);
	for (k = 0; k < K; k++)
		y[k] = libxmp_sinc_fir(x, 0, N, step * (k + 64), c);
	amp = fit_amp(y, 40, K - 40, 0.2);
	db = 20 * log10(amp / A);
	TASK_CHECK(db <= -45.0);
	return 0;
}

static int case_cutoff_redirect(void)
{
	struct sinc_voice_state st;
	double v;

	memset(&st, 0, sizeof(st));
	libxmp_sinc_ramp_attack(&st, 4.0);
	TASK_CHECK(fabs(st.c - 0.225) < 1e-12);
	TASK_CHECK(st.ramp_k == 32);

	/* steady pitch keeps the target */
	v = libxmp_sinc_ramp_next(&st, 4.0);
	TASK_CHECK(fabs(v - 0.225) < 1e-12);

	/* pitch change to step 1: k-th frame old+(new-old)*k/32 */
	v = libxmp_sinc_ramp_next(&st, 1.0);
	TASK_CHECK(fabs(v - (0.225 + 0.675 / 32.0)) < 1e-12);
	TASK_CHECK(st.ramp_k == 1);
	while (st.ramp_k < 16)
		libxmp_sinc_ramp_next(&st, 1.0);
	TASK_CHECK(fabs(st.c - (0.225 + 0.675 * 0.5)) < 1e-12);

	/* redirect halfway to step 2: restart from current value */
	v = libxmp_sinc_ramp_next(&st, 2.0);
	{
		double mid = 0.225 + 0.675 * 0.5;
		double want = mid + (0.45 - mid) / 32.0;
		TASK_CHECK(fabs(v - want) < 1e-12);
	}
	while (st.ramp_k < 32)
		v = libxmp_sinc_ramp_next(&st, 2.0);
	TASK_CHECK(fabs(v - 0.45) < 1e-12);
	v = libxmp_sinc_ramp_next(&st, 2.0);
	TASK_CHECK(fabs(v - 0.45) < 1e-12);

	/* attack at low step opens straight at 0.9 */
	memset(&st, 0, sizeof(st));
	libxmp_sinc_ramp_attack(&st, 0.25);
	TASK_CHECK(fabs(st.c - 0.9) < 1e-12);
	return 0;
}

/* Direct mixer-table coverage of every sample/output layout. */
static int case_format_matrix(void)
{
	const int C = 220;
	int layout;
	for (layout = 0; layout < 8; layout++) {
		int bits16 = layout & 1;
		int stereo_s = (layout >> 1) & 1;
		int stereo_o = (layout >> 2) & 1;
		struct mixer_voice vi;
		int32_t buf[512];
		int16_t s16[560];
		int8_t s8[560];
		double xl[300], xr[300];
		int i, g, idx;
		long maxmag = 0;

		memset(&vi, 0, sizeof(vi));
		memset(buf, 0, sizeof(buf));
		for (i = 0; i < 260; i++) {
			double a = 12000.0 * sin(2 * M_PI * 0.07 * i + 0.2);
			double b = 9000.0 * sin(2 * M_PI * 0.13 * i + 2.0);
			if (stereo_s) {
				if (bits16) {
					s16[2 * i] = (int16_t)a;
					s16[2 * i + 1] = (int16_t)b;
				} else {
					s8[2 * i] = (int8_t)ref_round(a / 256.0);
					s8[2 * i + 1] = (int8_t)ref_round(b / 256.0);
				}
			} else {
				if (bits16)
					s16[i] = (int16_t)a;
				else
					s8[i] = (int8_t)ref_round(a / 256.0);
			}
		}
		for (i = 0; i < 260; i++) {
			if (bits16) {
				xl[i] = stereo_s ? (double)s16[2 * i]
						 : (double)s16[i];
				xr[i] = stereo_s ? (double)s16[2 * i + 1]
						 : xl[i];
			} else {
				xl[i] = stereo_s ?
					(double)((int)s8[2 * i] << 8) :
					(double)((int)s8[i] << 8);
				xr[i] = stereo_s ?
					(double)((int)s8[2 * i + 1] << 8) :
					xl[i];
			}
		}
		vi.sptr = bits16 ? (void *)s16 : (void *)s8;
		vi.pos = 0.0;
		vi.start = 0;
		vi.end = 260;
		idx = layout;
		TASK_CHECK(libxmp_sinc_mixers[idx] != NULL);
		libxmp_sinc_mixers[idx](&vi, buf, C, 1, 1, 1 << 16,
					C, 0, 0);

		for (g = 90; g < C - 5; g++) {
			double U = (double)g;
			double c = ref_target(1.0);
			int el = ref_round(ref_fir_array(xl, 260, U, c));
			int er = ref_round(ref_fir_array(xr, 260, U, c));
			if (stereo_o) {
				long gl = buf[2 * g], gr = buf[2 * g + 1];
				if (stereo_s) {
					TASK_CHECK(abs((int)(gl - el)) <= 2);
					TASK_CHECK(abs((int)(gr - er)) <= 2);
				} else {
					TASK_CHECK(abs((int)(gl - el)) <= 2);
					TASK_CHECK(abs((int)(gr - el)) <= 2);
				}
				if (labs(gl) > maxmag)
					maxmag = labs(gl);
			} else {
				long got = buf[g];
				int want = stereo_s ? ((el + er) >> 1) : el;
				TASK_CHECK(abs((int)(got - want)) <= 2);
				if (labs(got) > maxmag)
					maxmag = labs(got);
			}
		}
		TASK_CHECK(maxmag > 1000);
	}
	return 0;
}

/* Distinct L/R with one channel silent must not bleed across. */
static int case_stereo_isolation(void)
{
	const int C = 200;
	struct mixer_voice vi;
	int32_t buf[512];
	int16_t s[520];
	int g;
	long max_l = 0, max_r = 0;

	memset(&vi, 0, sizeof(vi));
	memset(buf, 0, sizeof(buf));
	for (g = 0; g < 260; g++) {
		s[2 * g] = (int16_t)(14000 * sin(2 * M_PI * 0.05 * g));
		s[2 * g + 1] = 0;
	}
	vi.sptr = s;
	vi.pos = 0;
	vi.end = 260;
	TASK_CHECK(libxmp_sinc_mixers[7] != NULL);
	libxmp_sinc_mixers[7](&vi, buf, C, 1, 1, 1 << 16, C, 0, 0);
	for (g = 90; g < C - 5; g++) {
		TASK_CHECK(buf[2 * g + 1] == 0);	/* right silent */
		if (labs(buf[2 * g]) > max_l)
			max_l = labs(buf[2 * g]);
	}
	TASK_CHECK(max_l > 5000);

	/* reverse the silent side */
	memset(&vi, 0, sizeof(vi));
	memset(buf, 0, sizeof(buf));
	for (g = 0; g < 260; g++) {
		s[2 * g] = 0;
		s[2 * g + 1] = (int16_t)(14000 * sin(2 * M_PI * 0.05 * g));
	}
	vi.sptr = s;
	vi.pos = 0;
	vi.end = 260;
	libxmp_sinc_mixers[7](&vi, buf, C, 1, 1, 1 << 16, C, 0, 0);
	for (g = 90; g < C - 5; g++) {
		TASK_CHECK(buf[2 * g] == 0);
		if (labs(buf[2 * g + 1]) > max_r)
			max_r = labs(buf[2 * g + 1]);
	}
	TASK_CHECK(max_r > 5000);
	return 0;
}

/* Volume ramp arguments must reproduce the original AC gain sequence.
 * A constant source makes the normalized FIR output exactly that
 * constant, isolating the ramp math. */
static int case_volume_ramp_kernel(void)
{
	const int C = 160, RAMP = 40, AC = C - RAMP;
	struct mixer_voice vi;
	int32_t buf[512];
	static int16_t s[260];
	int g;

	memset(&vi, 0, sizeof(vi));
	memset(buf, 0, sizeof(buf));
	for (g = 0; g < 260; g++)
		s[g] = 10000;
	vi.sptr = s;
	vi.pos = 0;
	vi.end = 260;
	vi.old_vl = 0;
	/* delta 1024 full-units/frame -> multiplier grows by 4 each frame */
	libxmp_sinc_mixers[5](&vi, buf, C, 9, 9, 1 << 16, RAMP,
			      1024, 1024);
	for (g = 70; g < AC; g++) {
		int mul = (g * 1024) >> 8;
		TASK_CHECK(buf[2 * g] == 10000 * mul);
		TASK_CHECK(buf[2 * g + 1] == 10000 * mul);
	}
	for (g = AC + 2; g < C; g++) {
		TASK_CHECK(buf[2 * g] == 90000);
		TASK_CHECK(buf[2 * g + 1] == 90000);
	}
	TASK_CHECK(labs(buf[2 * 10]) < labs(buf[2 * 100]));
	return 0;
}

/* IT integer lowpass through the filtered mono mixer slot. */
static int case_it_filter_kernel(void)
{
	const int C = 180;
	struct mixer_voice vi, vi2;
	static int32_t buf[256], buf2[256];
	static int16_t s[260];
	static double seq[260];
	int g;
	int fl1 = 0, fl2 = 0;
	int a0 = (int)(0.18 * (1 << FILTER_SHIFT));
	int b0 = (int)(0.55 * (1 << FILTER_SHIFT));
	int b1 = (int)(-0.27 * (1 << FILTER_SHIFT));
	long fenergy = 0, uenergy = 0;

	for (g = 0; g < 260; g++) {
		s[g] = (int16_t)(20000 * (((g * 53) % 7) - 3));
		seq[g] = s[g];
	}

	memset(&vi, 0, sizeof(vi));
	memset(buf, 0, sizeof(buf));
	vi.sptr = s;
	vi.pos = 0;
	vi.end = 260;
	vi.filter.a0 = a0;
	vi.filter.b0 = b0;
	vi.filter.b1 = b1;
	TASK_CHECK(libxmp_sinc_mixers[9] != NULL);
	libxmp_sinc_mixers[9](&vi, buf, C, 1, 1, 1 << 16, C, 0, 0);

	/* Independent copy of the exact integer IT recurrence, run from
	 * frame 0 so filter state matches the mixer's warm state. */
	for (g = 0; g < C; g++) {
		int smp = ref_round(ref_fir_array(seq, 260, (double)g,
						  ref_target(1.0)));
		int64 s64 = ((int64)a0 * ((int64)smp * 32768) +
			     (int64)b0 * fl1 + (int64)b1 * fl2)
			    >> FILTER_SHIFT;
		int sl = (int)s64;
		int out;
		if (sl < -65536 * 32768)
			sl = -65536 * 32768;
		if (sl > 65535 * 32768)
			sl = 65535 * 32768;
		fl2 = fl1;
		fl1 = sl;
		out = sl >> 15;
		if (g >= 90 && g < C - 5)
			TASK_CHECK(abs(buf[g] - out) <= 2);
	}
	TASK_CHECK(vi.filter.l1 != 0 || vi.filter.l2 != 0);

	/* Filtered high-frequency energy must be below unfiltered. */
	memset(&vi2, 0, sizeof(vi2));
	memset(buf2, 0, sizeof(buf2));
	vi2.sptr = s;
	vi2.pos = 0;
	vi2.end = 260;
	libxmp_sinc_mixers[1](&vi2, buf2, C, 1, 1, 1 << 16, C, 0, 0);
	for (g = 60; g < C; g++) {
		fenergy += (long)buf[g] * buf[g];
		uenergy += (long)buf2[g] * buf2[g];
	}
	TASK_CHECK(fenergy > 0);
	TASK_CHECK(fenergy < uenergy);
	return 0;
}

/* ================================================================== */
/* trajectory group: real XM/IT playback                               */
/* ================================================================== */

struct scen {
	const char *file;
	int bits, stereo, L, S, E, loop, bidir, note;
	double c5mul;
};

struct e2e {
	struct rig r;
	void *smp;
	double step;
	int vl, vr, shift;
};

static void scen_prepare(struct e2e *e, const struct scen *sc)
{
	struct rig *r = &e->r;
	int i;
	if (rig_open(r, sc->file, 44100, 0) != 0)
		return;
	{
		struct xmp_sample *xs = &r->m->xxs[r->sid];
		unsigned char *base = calloc(1, 4 + (size_t)sc->L * 4);
		e->smp = base + 4;
		if (xs->data)
			free(xs->data - 4);
		xs->data = (unsigned char *)e->smp;
		xs->len = sc->L;
		xs->lps = sc->S;
		xs->lpe = sc->E;
		xs->flg = (sc->bits == 16 ? XMP_SAMPLE_16BIT : 0) |
			  (sc->stereo == 2 ? XMP_SAMPLE_STEREO : 0) |
			  (sc->loop ? XMP_SAMPLE_LOOP : 0) |
			  (sc->bidir ? XMP_SAMPLE_LOOP_BIDIR : 0);
		if (sc->c5mul > 0.0)
			r->ctx->m.xtra[r->sid].c5spd *= sc->c5mul;
	}
	for (i = 0; i < sc->L; i++) {
		double a = 9000.0 * sin(2 * M_PI * (0.03 + 0.001 * (i % 9)) * i
					+ 0.3) + ((i * 53) % 17 - 8) * 120.0;
		double b = 7000.0 * sin(2 * M_PI * 0.09 * i + 1.7);
		if (sc->bits == 16) {
			int16_t *s = (int16_t *)e->smp;
			s[sc->stereo == 2 ? 2 * i : i] = (int16_t)a;
			if (sc->stereo == 2)
				s[2 * i + 1] = (int16_t)b;
		} else {
			int8_t *s = (int8_t *)e->smp;
			s[sc->stereo == 2 ? 2 * i : i] = (int8_t)ref_round(a / 256);
			if (sc->stereo == 2)
				s[2 * i + 1] = (int8_t)ref_round(b / 256);
		}
	}
	rig_start_sinc(r);
	rig_note(r, 0, sc->note ? sc->note : 72, 64);
}

static void e2e_calibrate(struct e2e *e)
{
	struct mixer_voice *vi;
	xmp_play_frame(e->r.x);
	vi = rig_voice(&e->r);
	if (vi) {
		e->step = vi->sinc.step_abs;
		e->vl = vi->old_vl >> 8;
		e->vr = vi->old_vr >> 8;
	} else {
		e->step = 1.0;
		e->vl = e->vr = 1;
	}
	e->shift = 12 - e->r.ctx->s.amplify;
	if (e->shift < 0)
		e->shift = 0;
}

/* Deterministic throw-away rig used only to read exact step/gain. */
static void scen_calibrate(const struct scen *sc,
			   double *step, int *vl, int *vr, int *shift)
{
	struct e2e c;
	memset(&c, 0, sizeof(c));
	scen_prepare(&c, sc);
	if (c.r.x)
		e2e_calibrate(&c);
	*step = c.step > 0.0 ? c.step : 1.0;
	*vl = c.vl ? c.vl : 1;
	*vr = c.vr ? c.vr : *vl;
	*shift = c.shift;
	rig_close(&c.r);
}

/* Compare stereo16 PCM against an independent traversal reference. */
static long e2e_worst(struct e2e *e, const struct scen *sc,
		      const int16_t *pcm, long frames,
		      const struct ref_loop *a, int sw,
		      const struct ref_loop *b,
		      long g0, long g1, int tol)
{
	long nseq = (long)(g1 * e->step) + 80;
	double *seq, *L, *R;
	long g, worst = 0;

	if (nseq < 128)
		nseq = 128;
	seq = calloc((size_t)nseq * 2, sizeof(double));
	L = calloc((size_t)nseq, sizeof(double));
	R = calloc((size_t)nseq, sizeof(double));
	if (!seq || !L || !R) {
		free(seq); free(L); free(R);
		return 1 << 30;
	}
	ref_walk(seq, (int)nseq, e->smp, sc->bits, sc->stereo, a, sw, b);
	for (g = 0; g < nseq; g++) {
		L[g] = seq[2 * g];
		R[g] = sc->stereo == 2 ? seq[2 * g + 1] : seq[2 * g];
	}
	for (g = g0; g < g1; g++) {
		int el = ref_pcm_one(L, nseq, e->step, g, e->vl, e->shift);
		int er = ref_pcm_one(R, nseq, e->step, g, e->vr, e->shift);
		long dl = labs((long)pcm[2 * g] - el);
		long dr = labs((long)pcm[2 * g + 1] - er);
		if (dl > worst)
			worst = dl;
		if (dr > worst)
			worst = dr;
	}
	free(seq); free(L); free(R);
	(void)tol;
	return worst;
}

static struct ref_loop scen_loop_a(const struct scen *sc)
{
	struct ref_loop a;
	memset(&a, 0, sizeof(a));
	a.origin = 0;
	a.len = sc->L;
	a.start = sc->S;
	a.end = sc->E;
	a.loop = sc->loop;
	a.bidir = sc->bidir;
	a.reverse = 0;
	return a;
}

static int trajectory_xm_forward_fragments(void)
{
	struct scen sc = { "test.xm", 16, 1, 47, 8, 19, 1, 0, 72, 1.0 };
	struct e2e ea, eb;
	int frames = 2600;
	int16_t *whole, *chunked;
	long worst;

	memset(&ea, 0, sizeof(ea));
	memset(&eb, 0, sizeof(eb));
	scen_prepare(&ea, &sc);
	scen_prepare(&eb, &sc);
	TASK_CHECK(ea.r.x && eb.r.x);
	scen_calibrate(&sc, &ea.step, &ea.vl, &ea.vr, &ea.shift);
	whole = rig_read_whole(&ea.r, frames);
	chunked = rig_read_chunked(&eb.r, frames);
	TASK_CHECK(whole && chunked);
	TASK_CHECK(memcmp(whole, chunked,
			  (size_t)frames * 4) == 0);

	{
		struct ref_loop a = scen_loop_a(&sc);
		worst = e2e_worst(&ea, &sc, whole, frames, &a, -1, NULL,
				  420, frames - 8, 4);
		TASK_CHECK(worst <= 4);
	}
	rig_close(&ea.r);
	rig_close(&eb.r);
	free(whole);
	free(chunked);
	return 0;
}

static int trajectory_it_bidi(void)
{
	struct scen sc = { "it_sample_porta.it", 16, 1, 41, 10, 17, 1, 1, 76, 1.4 };
	struct e2e ea;
	int frames = 2600;
	int16_t *pcm;
	long worst;

	memset(&ea, 0, sizeof(ea));
	if (!data_file(sc.file)) {
		fputs("skip: it data missing\n", stderr);
		return 0;
	}
	scen_prepare(&ea, &sc);
	TASK_CHECK(ea.r.x);
	scen_calibrate(&sc, &ea.step, &ea.vl, &ea.vr, &ea.shift);
	pcm = rig_read_whole(&ea.r, frames);
	TASK_CHECK(pcm);
	{
		struct ref_loop a = scen_loop_a(&sc);
		worst = e2e_worst(&ea, &sc, pcm, frames, &a, -1, NULL,
				  420, frames - 8, 4);
		TASK_CHECK(worst <= 4);
	}
	/* 1/7/113 fragmentation must not change samples. */
	{
		struct e2e eb;
		int16_t *ch;
		memset(&eb, 0, sizeof(eb));
		scen_prepare(&eb, &sc);
		TASK_CHECK(eb.r.x);
		ch = rig_read_chunked(&eb.r, frames);
		TASK_CHECK(ch);
		TASK_CHECK(memcmp(pcm, ch, (size_t)frames * 4) == 0);
		rig_close(&eb.r);
		free(ch);
	}
	rig_close(&ea.r);
	free(pcm);
	return 0;
}

static int trajectory_8bit_stereo(void)
{
	struct scen sc = { "test.xm", 8, 2, 53, 9, 21, 1, 0, 72, 1.0 };
	struct e2e ea;
	int frames = 2400;
	int16_t *pcm;
	long worst;

	memset(&ea, 0, sizeof(ea));
	scen_prepare(&ea, &sc);
	TASK_CHECK(ea.r.x);
	scen_calibrate(&sc, &ea.step, &ea.vl, &ea.vr, &ea.shift);
	pcm = rig_read_whole(&ea.r, frames);
	TASK_CHECK(pcm);
	{
		struct ref_loop a = scen_loop_a(&sc);
		worst = e2e_worst(&ea, &sc, pcm, frames, &a, -1, NULL,
				  420, frames - 8, 4);
		TASK_CHECK(worst <= 4);
	}
	rig_close(&ea.r);
	free(pcm);
	return 0;
}

static int trajectory_reverse(void)
{
	struct scen sc = { "test.xm", 16, 1, 200, 40, 60, 1, 0, 72, 1.0 };
	struct e2e ea;
	struct mixer_voice *vi;
	int frames = 1800;
	int16_t *pcm;
	long worst;
	struct ref_loop a;

	memset(&ea, 0, sizeof(ea));
	scen_prepare(&ea, &sc);
	TASK_CHECK(ea.r.x);
	/* create the voice, then force reverse playback at end-1 and
	 * restart logical U at 0 via the official reposition hook. */
	xmp_play_frame(ea.r.x);
	vi = rig_voice(&ea.r);
	TASK_CHECK(vi != NULL);
	if (vi) {
		int voc = (int)(vi - ea.r.ctx->p.virt.voice_array);
		double stp = vi->sinc.step_abs;
		int gain = vi->old_vl >> 8;
		libxmp_mixer_reverse(ea.r.ctx, voc, 1);
		libxmp_mixer_voicepos(ea.r.ctx, voc, sc.E - 1, 0);
		ea.step = stp;
		ea.vl = gain;
		ea.vr = gain;
	}
	ea.shift = 12 - ea.r.ctx->s.amplify;
	pcm = rig_read_whole(&ea.r, frames);
	TASK_CHECK(pcm);
	memset(&a, 0, sizeof(a));
	a.origin = sc.E - 1;
	a.len = sc.L;
	a.start = sc.S;
	a.end = sc.E;
	a.loop = 1;
	a.reverse = 1;
	worst = e2e_worst(&ea, &sc, pcm, frames, &a, -1, NULL,
			  300, frames - 8, 4);
	TASK_CHECK(worst <= 4);
	rig_close(&ea.r);
	free(pcm);
	return 0;
}

static int trajectory_oneshot_tail(void)
{
	struct scen sc = { "test.xm", 16, 1, 96, 0, 96, 0, 0, 76, 1.6 };
	struct e2e ea;
	int frames = 2200;
	int16_t *pcm;
	long worst;
	struct ref_loop a;
	struct mixer_voice *vi;
	int tail_seen = 0, zeros_after = 1, g;

	memset(&ea, 0, sizeof(ea));
	scen_prepare(&ea, &sc);
	TASK_CHECK(ea.r.x);
	scen_calibrate(&sc, &ea.step, &ea.vl, &ea.vr, &ea.shift);
	pcm = rig_read_whole(&ea.r, frames);
	TASK_CHECK(pcm);
	memset(&a, 0, sizeof(a));
	a.origin = 0;
	a.len = sc.L;
	a.end = sc.L;
	a.loop = 0;
	worst = e2e_worst(&ea, &sc, pcm, frames, &a, -1, NULL,
			  140, 760, 4);
	TASK_CHECK(worst <= 4);

	/* finite non-zero tail exists, then output settles to zero */
	for (g = 0; g < frames; g++) {
		if (abs(pcm[2 * g]) > 20)
			tail_seen = 1;
	}
	TASK_CHECK(tail_seen);
	vi = rig_voice(&ea.r);
	if (vi && vi->sinc.tail_done) {
		for (g = frames - 200; g < frames; g++)
			if (abs(pcm[2 * g]) > 4)
				zeros_after = 0;
		TASK_CHECK(zeros_after);
	}
	rig_close(&ea.r);
	free(pcm);
	return 0;
}

static int trajectory_offset(void)
{
	struct scen sc = { "test.xm", 16, 1, 220, 20, 80, 1, 0, 72, 1.0 };
	struct e2e ea;
	struct mixer_voice *vi;
	int frames = 2000, off = 37;
	int16_t *pcm;
	long worst;
	struct ref_loop a;

	memset(&ea, 0, sizeof(ea));
	scen_prepare(&ea, &sc);
	TASK_CHECK(ea.r.x);
	xmp_play_frame(ea.r.x);
	vi = rig_voice(&ea.r);
	TASK_CHECK(vi != NULL);
	if (vi) {
		int voc = (int)(vi - ea.r.ctx->p.virt.voice_array);
		double stp = vi->sinc.step_abs;
		int gain = vi->old_vl >> 8;
		libxmp_mixer_voicepos(ea.r.ctx, voc, off, 0);
		ea.step = stp;
		ea.vl = gain;
		ea.vr = gain;
	}
	ea.shift = 12 - ea.r.ctx->s.amplify;
	pcm = rig_read_whole(&ea.r, frames);
	TASK_CHECK(pcm);
	memset(&a, 0, sizeof(a));
	a.origin = off;
	a.len = sc.L;
	a.start = sc.S;
	a.end = sc.E;
	a.loop = 1;
	worst = e2e_worst(&ea, &sc, pcm, frames, &a, -1, NULL,
			  300, frames - 8, 4);
	TASK_CHECK(worst <= 4);
	rig_close(&ea.r);
	free(pcm);
	return 0;
}

static int trajectory_sustain_release(void)
{
	/* IT sample with a short sustain loop; release mid-stream. */
	struct rig r;
	struct xmp_sample *xs;
	int16_t *s;
	int L = 120, ss = 30, se = 41, lps = 60, lpe = 73;
	int rel_at = 4, nticks = 9;
	int ticksize = 0, total = 0;
	int16_t *pcm;
	struct mixer_voice *vi = NULL;
	double step = 0;
	int vl = 1, shift = 11, f, g, sw, bestsw = -1;
	long best = 1 << 30;
	int relG;

	if (!data_file("it_sample_porta.it")) {
		fputs("skip: it missing\n", stderr);
		return 0;
	}
	if (rig_open(&r, "it_sample_porta.it", 44100, 0) != 0)
		return 1;
	rig_install16(&r, &s, L, 0);
	for (f = 0; f < L; f++)
		s[f] = (int16_t)(11000 * sin(2 * M_PI * 0.08 * f + 0.5));
	xs = &r.m->xxs[r.sid];
	xs->lps = lps;
	xs->lpe = lpe;
	xs->flg = XMP_SAMPLE_16BIT | XMP_SAMPLE_LOOP | XMP_SAMPLE_SLOOP;
	r.ctx->m.xtra[r.sid].sus = ss;
	r.ctx->m.xtra[r.sid].sue = se;
	/* Isolate loop-endpoint history from envelope/fadeout gain. */
	r.m->xxi[0].rls = 0;
	r.m->xxi[0].aei.flg = 0;
	rig_start_sinc(&r);
	rig_note(&r, 0, 76, 64);

	pcm = calloc((size_t)nticks * 4096 * 2, sizeof(int16_t));
	TASK_CHECK(pcm != NULL);

	/* Capture a contiguous frame-0 PCM stream tick by tick. */
	for (f = 0; f < nticks; f++) {
		struct xmp_frame_info fi;
		int nfr;
		if (f == rel_at) {
			struct xmp_event off;
			vi = rig_voice(&r);
			if (vi) {
				step = vi->sinc.step_abs;
				vl = vi->old_vl >> 8;
			}
			/* Real key-off so the player drives sustain release
			 * (a direct flag set would be overwritten). */
			memset(&off, 0, sizeof(off));
			off.note = XMP_KEY_OFF;
			xmp_inject_event(r.x, 0, &off);
		}
		xmp_play_frame(r.x);
		xmp_get_frame_info(r.x, &fi);
		nfr = fi.buffer_size / 4;
		if (ticksize == 0)
			ticksize = nfr;
		memcpy(pcm + total * 2, fi.buffer, (size_t)nfr * 4);
		total += nfr;
	}
	shift = 12 - r.ctx->s.amplify;
	TASK_CHECK(vi != NULL);
	TASK_CHECK(step > 0.0);
	relG = rel_at * ticksize;

	/* Search the exact logical switch index; the PCM comparison is
	 * strict (<=4 LSB) across a window straddling the release. */
	for (sw = -20; sw <= 20; sw++) {
		struct ref_loop a, b;
		long nseq = (long)(total * step) + 80;
		double *seq = calloc((size_t)nseq * 2, sizeof(double));
		double *mono = calloc((size_t)nseq, sizeof(double));
		long worst = 0;
		int nsw = (int)((double)relG * step) + sw;
		int g0 = relG - 500, g1 = relG + 900;
		if (nseq < 128)
			nseq = 128;
		if (nsw < 0)
			nsw = 0;
		if (g0 < 420)
			g0 = 420;
		if (g1 > total - 8)
			g1 = total - 8;
		memset(&a, 0, sizeof(a));
		a.len = L; a.start = ss; a.end = se; a.loop = 1;
		memset(&b, 0, sizeof(b));
		b.len = L; b.start = lps; b.end = lpe; b.loop = 1;
		ref_walk(seq, (int)nseq, s, 16, 1, &a, nsw, &b);
		for (g = 0; g < nseq; g++)
			mono[g] = seq[2 * g];
		for (g = g0; g < g1; g++) {
			int want = ref_pcm_one(mono, nseq, step, g, vl, shift);
			long d = labs((long)pcm[2 * g] - want);
			if (d > worst)
				worst = d;
		}
		free(mono);
		free(seq);
		if (worst < best) {
			best = worst;
			bestsw = sw;
		}
	}
	TASK_CHECK(bestsw >= -20);
	TASK_CHECK(best <= 4);
	rig_close(&r);
	free(pcm);
	return 0;
}

/* ================================================================== */
/* lifecycle group                                                     */
/* ================================================================== */

static xmp_context loaded_ctx(const char *file)
{
	const char *path = data_file(file);
	xmp_context x = xmp_create_context();
	if (!path || !x || xmp_load_module(x, (char *)path) != 0) {
		if (x)
			xmp_free_context(x);
		return NULL;
	}
	return x;
}

static int lifecycle_error_codes(void)
{
	xmp_context x;

	/* selecting before start_player keeps the existing state error */
	x = loaded_ctx("test.xm");
	TASK_CHECK(x != NULL);
	TASK_CHECK(xmp_set_player(x, XMP_PLAYER_INTERP,
				  XMP_INTERP_SINC) == -XMP_ERROR_STATE);
	xmp_free_context(x);

	/* 8-bit output is unsupported: existing invalid-value code */
	x = loaded_ctx("test.xm");
	TASK_CHECK(xmp_start_player(x, 44100, XMP_FORMAT_8BIT) == 0);
	TASK_CHECK(xmp_set_player(x, XMP_PLAYER_INTERP,
				  XMP_INTERP_SINC) == -XMP_ERROR_INVALID);
	TASK_CHECK(xmp_get_player(x, XMP_PLAYER_INTERP) ==
		   XMP_INTERP_LINEAR);
	xmp_free_context(x);

	/* 32-bit output is unsupported */
	x = loaded_ctx("test.xm");
	TASK_CHECK(xmp_start_player(x, 44100, XMP_FORMAT_32BIT) == 0);
	TASK_CHECK(xmp_set_player(x, XMP_PLAYER_INTERP,
				  XMP_INTERP_SINC) == -XMP_ERROR_INVALID);
	xmp_free_context(x);

	/* A500 BLEP mode must not be replaced by SINC */
	x = loaded_ctx("test.xm");
	TASK_CHECK(xmp_start_player(x, 44100, 0) == 0);
	TASK_CHECK(xmp_set_player(x, XMP_PLAYER_CFLAGS,
				  XMP_FLAGS_A500) == 0);
	TASK_CHECK(xmp_set_player(x, XMP_PLAYER_INTERP,
				  XMP_INTERP_SINC) == -XMP_ERROR_INVALID);
	TASK_CHECK(xmp_get_player(x, XMP_PLAYER_INTERP) ==
		   XMP_INTERP_LINEAR);
	xmp_free_context(x);

	/* switching into SINC after the first audio pass is rejected */
	x = loaded_ctx("test.xm");
	TASK_CHECK(xmp_start_player(x, 44100, 0) == 0);
	TASK_CHECK(xmp_play_frame(x) == 0);
	TASK_CHECK(xmp_set_player(x, XMP_PLAYER_INTERP,
				  XMP_INTERP_SINC) == -XMP_ERROR_INVALID);
	TASK_CHECK(xmp_get_player(x, XMP_PLAYER_INTERP) ==
		   XMP_INTERP_LINEAR);
	xmp_free_context(x);

	/* switching out of SINC after audio is rejected, scene unchanged */
	x = loaded_ctx("test.xm");
	TASK_CHECK(xmp_start_player(x, 44100, 0) == 0);
	TASK_CHECK(xmp_set_player(x, XMP_PLAYER_INTERP,
				  XMP_INTERP_SINC) == 0);
	TASK_CHECK(xmp_play_frame(x) == 0);
	TASK_CHECK(xmp_set_player(x, XMP_PLAYER_INTERP,
				  XMP_INTERP_LINEAR) == -XMP_ERROR_INVALID);
	TASK_CHECK(xmp_get_player(x, XMP_PLAYER_INTERP) ==
		   XMP_INTERP_SINC);
	/* idempotent re-select stays allowed */
	TASK_CHECK(xmp_set_player(x, XMP_PLAYER_INTERP,
				  XMP_INTERP_SINC) == 0);
	xmp_free_context(x);
	return 0;
}

#define CAP_TICKS 7
#define CAP_MAXF 4096

static int capture_ticks(struct rig *r, int ticks, int16_t *out,
			 int *total_frames)
{
	int t, total = 0, ts = 0;
	for (t = 0; t < ticks; t++) {
		struct xmp_frame_info fi;
		int nfr;
		if (xmp_play_frame(r->x) != 0)
			break;
		xmp_get_frame_info(r->x, &fi);
		nfr = fi.buffer_size / 4;
		if (ts == 0)
			ts = nfr;
		memcpy(out + total * 2, fi.buffer, (size_t)nfr * 4);
		total += nfr;
	}
	*total_frames = total;
	return ts;
}

static int lifecycle_contexts_interleave(void)
{
	struct scen sc = { "test.xm", 16, 1, 64, 12, 31, 1, 0, 72, 1.0 };
	struct e2e a, b;
	static int16_t ca[CAP_TICKS * CAP_MAXF * 2];
	static int16_t cb[CAP_TICKS * CAP_MAXF * 2];
	int ta = 0, tb = 0, t;
	int total = 0;

	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	scen_prepare(&a, &sc);
	scen_prepare(&b, &sc);
	TASK_CHECK(a.r.x && b.r.x);

	/* alternate calls between the two contexts */
	for (t = 0; t < CAP_TICKS; t++) {
		struct xmp_frame_info fi;
		int nfr;
		xmp_play_frame(a.r.x);
		xmp_get_frame_info(a.r.x, &fi);
		nfr = fi.buffer_size / 4;
		memcpy(ca + ta * 2, fi.buffer, (size_t)nfr * 4);
		ta += nfr;

		xmp_play_frame(b.r.x);
		xmp_get_frame_info(b.r.x, &fi);
		nfr = fi.buffer_size / 4;
		memcpy(cb + tb * 2, fi.buffer, (size_t)nfr * 4);
		tb += nfr;
	}
	TASK_CHECK(ta == tb);
	total = ta;
	TASK_CHECK(total > 100);
	TASK_CHECK(memcmp(ca, cb, (size_t)total * 4) == 0);

	/* both produced real non-zero SINC sound */
	{
		int nz = 0, g;
		for (g = 400; g < total; g++)
			if (abs(ca[2 * g]) > 50)
				nz++;
		TASK_CHECK(nz > 100);
	}
	rig_close(&a.r);
	rig_close(&b.r);
	return 0;
}

static int lifecycle_slot_reuse(void)
{
	struct scen sc = { "test.xm", 16, 1, 80, 8, 40, 1, 0, 72, 1.0 };
	struct e2e fresh, churn;
	static int16_t cf[CAP_TICKS * CAP_MAXF * 2];
	static int16_t cc[CAP_TICKS * CAP_MAXF * 2];
	int tf = 0, tc = 0, i;
	long worst = 0;

	memset(&fresh, 0, sizeof(fresh));
	memset(&churn, 0, sizeof(churn));
	scen_prepare(&fresh, &sc);
	scen_prepare(&churn, &sc);
	TASK_CHECK(fresh.r.x && churn.r.x);

	/* Both runs are silent until frame K=8, then the same final note
	 * starts.  The churned slot has hosted several notes before it. */
	for (i = 0; i < 8; i++) {
		if (i == 8)
			break;
		xmp_play_frame(fresh.r.x);
		if (i % 2 == 0)
			rig_note(&churn.r, 0, 60 + i, 64);
		xmp_play_frame(churn.r.x);
	}
	rig_note(&fresh.r, 0, 72, 64);
	rig_note(&churn.r, 0, 72, 64);
	capture_ticks(&fresh.r, CAP_TICKS, cf, &tf);
	capture_ticks(&churn.r, CAP_TICKS, cc, &tc);
	TASK_CHECK(tf == tc && tf > 0);

	/* skip onset ramp/anticlick, compare steady post-reset windows */
	{
		long g;
		for (g = 300; g < tf - 200; g++) {
			long d = labs((long)cf[2 * g] - cc[2 * g]);
			if (d > worst)
				worst = d;
		}
	}
	TASK_CHECK(worst <= 4);
	rig_close(&fresh.r);
	rig_close(&churn.r);
	return 0;
}

static int lifecycle_seek_restart(void)
{
	struct scen sc = { "test.xm", 16, 1, 90, 10, 44, 1, 0, 72, 1.0 };
	struct e2e restart, fresh;
	static int16_t cr[CAP_TICKS * CAP_MAXF * 2];
	static int16_t cf[CAP_TICKS * CAP_MAXF * 2];
	int tr = 0, tf = 0, warm, d, bestd = 0;
	long best = 1 << 30;

	memset(&restart, 0, sizeof(restart));
	memset(&fresh, 0, sizeof(fresh));
	scen_prepare(&restart, &sc);
	scen_prepare(&fresh, &sc);
	TASK_CHECK(restart.r.x && fresh.r.x);

	/* play some warm-up, then restart and retrigger the note */
	for (warm = 0; warm < 3; warm++)
		xmp_play_frame(restart.r.x);
	xmp_restart_module(restart.r.x);
	rig_note(&restart.r, 0, 72, 64);
	capture_ticks(&restart.r, CAP_TICKS, cr, &tr);
	capture_ticks(&fresh.r, CAP_TICKS, cf, &tf);
	TASK_CHECK(tr == tf && tr > 0);

	/* allow a small global alignment offset */
	for (d = -96; d <= 96; d++) {
		long worst = 0;
		long g;
		for (g = 300; g < tr - 200; g++) {
			long h = g + d;
			long diff;
			if (h < 0 || h >= tf)
				continue;
			diff = labs((long)cr[2 * g] - cf[2 * h]);
			if (diff > worst)
				worst = diff;
		}
		if (worst < best) {
			best = worst;
			bestd = d;
		}
	}
	TASK_CHECK(best <= 4);

	/* xmp_set_position(0) relocation must clean and replay likewise */
	{
		static int16_t cp[CAP_TICKS * CAP_MAXF * 2];
		int tp = 0;
		long bestp = 1 << 30;
		int dd;
		xmp_set_position(restart.r.x, 0);
		rig_note(&restart.r, 0, 72, 64);
		capture_ticks(&restart.r, CAP_TICKS, cp, &tp);
		for (dd = -96; dd <= 96; dd++) {
			long worst = 0, g;
			for (g = 300; g < tp - 200; g++) {
				long h = g + dd;
				long diff;
				if (h < 0 || h >= tf)
					continue;
				diff = labs((long)cp[2 * g] - cf[2 * h]);
				if (diff > worst)
					worst = diff;
			}
			if (worst < bestp)
				bestp = worst;
		}
		TASK_CHECK(bestp <= 4);
	}
	rig_close(&restart.r);
	rig_close(&fresh.r);
	return 0;
}

/* Count active mixer voices sharing our sample. */
static int count_voices(struct rig *r)
{
	int v, n = 0;
	for (v = 0; v < r->ctx->p.virt.maxvoc; v++) {
		struct mixer_voice *vi = &r->ctx->p.virt.voice_array[v];
		if (vi->chn >= 0 && vi->smp == r->sid && vi->sptr)
			n++;
	}
	return n;
}

/*
 * IT NNA: three matched contexts.  N plays note1 then note2 (NNA_CONT),
 * O plays note1 only, W plays note2 only.  N's PCM must equal O+W: the
 * old voice keeps its own history/phase while the new voice starts cold.
 */
static int lifecycle_nna(void)
{
	struct rig n, o, w;
	static int16_t pn[CAP_TICKS * CAP_MAXF * 2];
	static int16_t po[CAP_TICKS * CAP_MAXF * 2];
	static int16_t pw[CAP_TICKS * CAP_MAXF * 2];
	int tn = 0, to = 0, tw = 0, i;
	int split_seen = 0;
	long worst = 0, nz = 0;

	if (!data_file("it_sample_porta.it")) {
		fputs("skip: it missing\n", stderr);
		return 0;
	}
	memset(&n, 0, sizeof(n));
	memset(&o, 0, sizeof(o));
	memset(&w, 0, sizeof(w));
	rig_open(&n, "it_sample_porta.it", 44100, 0);
	rig_open(&o, "it_sample_porta.it", 44100, 0);
	rig_open(&w, "it_sample_porta.it", 44100, 0);
	TASK_CHECK(n.x && o.x && w.x);

	/* install the same looped 16-bit mono sample in all contexts */
	for (i = 0; i < 3; i++) {
		struct rig *r = i == 0 ? &n : i == 1 ? &o : &w;
		int16_t *s;
		int k;
		rig_install16(r, &s, 140, 0);
		for (k = 0; k < 140; k++)
			s[k] = (int16_t)(9000 *
				sin(2 * M_PI * 0.06 * k + 0.4));
		r->m->xxs[r->sid].lps = 16;
		r->m->xxs[r->sid].lpe = 60;
		r->m->xxs[r->sid].flg |= XMP_SAMPLE_LOOP;
		if (r->m->xxi[0].nsm > 0 && r->m->xxi[0].sub) {
			r->m->xxi[0].sub[0].nna = XMP_INST_NNA_CONT;
			r->m->xxi[0].sub[0].dct = XMP_INST_DCT_OFF;
			r->m->xxi[0].sub[0].dca = XMP_INST_DCA_OFF;
		}
		/* This tiny IT fixture loads without QUIRK_VIRTUAL; enable
		 * IT virtual/NNA channels before xmp_start_player(). */
		r->ctx->m.quirk |= QUIRK_VIRTUAL;
		rig_start_sinc(r);
	}

	/* N and O: first note now */
	rig_note(&n, 0, 60, 64);
	rig_note(&o, 0, 60, 64);
	for (i = 0; i < 3; i++) {
		xmp_play_frame(n.x);
		xmp_play_frame(o.x);
	}
	/* second note on N; W gets its only note */
	rig_note(&n, 0, 84, 64);
	rig_note(&w, 0, 84, 64);
	for (i = 0; i < CAP_TICKS; i++) {
		struct xmp_frame_info fi;
		int nfr;
		xmp_play_frame(n.x);
		xmp_get_frame_info(n.x, &fi);
		nfr = fi.buffer_size / 4;
		memcpy(pn + tn * 2, fi.buffer, (size_t)nfr * 4);
		tn += nfr;
		if (count_voices(&n) >= 2)
			split_seen = 1;

		xmp_play_frame(o.x);
		xmp_get_frame_info(o.x, &fi);
		nfr = fi.buffer_size / 4;
		memcpy(po + to * 2, fi.buffer, (size_t)nfr * 4);
		to += nfr;

		xmp_play_frame(w.x);
		xmp_get_frame_info(w.x, &fi);
		nfr = fi.buffer_size / 4;
		memcpy(pw + tw * 2, fi.buffer, (size_t)nfr * 4);
		tw += nfr;
	}
	TASK_CHECK(tn == to && to == tw);
	TASK_CHECK(split_seen);

	/* PCM-level O+W == N over steady frames.  Allow summation and
	 * scheduling quantization headroom. */
	{
		long g;
		for (g = 250; g < tn - 200; g++) {
			int sum = po[2 * g] + pw[2 * g];
			long d = labs((long)pn[2 * g] - sum);
			if (d > worst)
				worst = d;
			if (abs(pn[2 * g]) > 100)
				nz++;
		}
	}
	TASK_CHECK(nz > 100);
	TASK_CHECK(worst <= 12);

	rig_close(&n);
	rig_close(&o);
	rig_close(&w);
	return 0;
}

/* Explicit key cut uses the original anticlick path and must NOT queue the
 * long 64-source-frame SINC tail.  A low note makes such a tail long. */
static int lifecycle_cut_no_tail(void)
{
	struct scen sc = { "test.xm", 16, 1, 220, 20, 120, 1, 0, 48, 1.0 };
	struct e2e e;
	static int16_t pcm[6 * CAP_MAXF * 2];
	int total = 0, warm, g, last = -1;

	memset(&e, 0, sizeof(e));
	scen_prepare(&e, &sc);
	TASK_CHECK(e.r.x);

	/* two sounding ticks to build FIR history */
	for (warm = 0; warm < 2; warm++)
		xmp_play_frame(e.r.x);

	/* The exact entry point every XMP_KEY_CUT handler uses. */
	libxmp_virt_resetchannel(e.r.ctx, 0);

	capture_ticks(&e.r, 5, pcm, &total);
	TASK_CHECK(total > 0);

	for (g = 0; g < total; g++) {
		if (abs(pcm[2 * g]) > 20 || abs(pcm[2 * g + 1]) > 20)
			last = g;
	}
	/* Only the short anticlick discharge may remain (< ~1/2 tick);
	 * an erroneous SINC tail at this low step would run ~600 frames. */
	TASK_CHECK(last >= 0);
	TASK_CHECK(last < 420);
	for (g = total - 120; g < total; g++) {
		TASK_CHECK(abs(pcm[2 * g]) <= 8);
		TASK_CHECK(abs(pcm[2 * g + 1]) <= 8);
	}
	rig_close(&e.r);
	return 0;
}

/* ================================================================== */
/* driver registration                                                 */
/* ================================================================== */

const struct task_case task_cases[] = {
	{ "kernel", "contract",		case_contract },
	{ "kernel", "coefficients",	case_coefficients },
	{ "kernel", "steps_phases",	case_steps_phases },
	{ "kernel", "spectrum_step4",	case_spectrum_step4 },
	{ "kernel", "cutoff_redirect",	case_cutoff_redirect },
	{ "kernel", "format_matrix",	case_format_matrix },
	{ "kernel", "stereo_isolation",	case_stereo_isolation },
	{ "kernel", "volume_ramp",	case_volume_ramp_kernel },
	{ "kernel", "it_filter",	case_it_filter_kernel },

	{ "trajectory", "xm_forward_fragments",
	  trajectory_xm_forward_fragments },
	{ "trajectory", "it_bidi",	trajectory_it_bidi },
	{ "trajectory", "bits8_stereo",	trajectory_8bit_stereo },
	{ "trajectory", "reverse",	trajectory_reverse },
	{ "trajectory", "oneshot_tail",	trajectory_oneshot_tail },
	{ "trajectory", "offset",	trajectory_offset },
	{ "trajectory", "sustain_release",
	  trajectory_sustain_release },

	{ "lifecycle", "error_codes",	lifecycle_error_codes },
	{ "lifecycle", "contexts_interleave",
	  lifecycle_contexts_interleave },
	{ "lifecycle", "slot_reuse",	lifecycle_slot_reuse },
	{ "lifecycle", "seek_restart",	lifecycle_seek_restart },
	{ "lifecycle", "nna",		lifecycle_nna },
	{ "lifecycle", "cut_no_tail",	lifecycle_cut_no_tail },
};

const size_t task_case_count =
	sizeof(task_cases) / sizeof(task_cases[0]);

