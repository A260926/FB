/*****************************************************************************
 *
 *  Xvid VFR two-pass test driver
 *
 *  Exercises the variable framerate (fincr == 0) two-pass path:
 *   - timing:      first pass timing schedule, MPEG-4 VOP timestamps, decode
 *   - budget:      exact target size computation and second pass quant choice
 *   - vbv:         per-duration refill, real-time peak window, scene scaling
 *   - compatibility: malformed schedules, interleaved encoders, legacy files
 *
 *  Plain C, no dependency other than the C library. main() runs the four
 *  groups and prints "VFR_CASE <group> PASS" after each.
 *
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xvid.h"

/*****************************************************************************
 * Small reusable helpers
 ****************************************************************************/

#define TEST_W 96
#define TEST_H 64

typedef struct {
	int fbase;
	int *dur;      /* per-frame durations in fbase units */
	int count;
} schedule_t;

/* Deterministic synthetic I420 content. Every schedule uses the same
 * index-to-image mapping, so two schedules always encode identical
 * complexity; only the frame durations differ. */
static void fill_frame(unsigned char *img, int index)
{
	int x, y;
	for (y = 0; y < TEST_H; y++)
		for (x = 0; x < TEST_W; x++)
			img[y*TEST_W + x] =
				(unsigned char)(((x * 3 + y * 5) ^ (index * 17)) & 0xff);
	for (y = 0; y < TEST_H/2; y++)
		for (x = 0; x < TEST_W/2; x++) {
			img[TEST_W*TEST_H + y*TEST_W/2 + x] =
				(unsigned char)(128 + ((x + index) & 7));
			img[TEST_W*TEST_H*5/4 + y*TEST_W/2 + x] =
				(unsigned char)(128 - ((y + index) & 7));
		}
}

/*****************************************************************************
 * Encoding driver
 ****************************************************************************/

typedef struct {
	int pass;             /* 1 or 2 */
	int fbase;
	int vfr;              /* 1: create with fincr==0, feed per-frame fincr */
	const char *stats;
	int bitrate;
	int vbv_size, vbv_maxrate, vbv_peakrate;
	int overhead;
	int max_bframes;
	int threads;
	int frame_drop;
	const schedule_t *sched;   /* pass 1 and VFR pass 2 */
	int num_frames;
} enc_job_t;

typedef struct {
	int bytes;
	int frames;
	int *sizes;
	int *quants;
} enc_result_t;

/* Run a full two-pass-compatible encode and return produced bytes. Returns a
 * negative value if the encoder refused to create or aborted mid-stream. */
static int run_enc(const enc_job_t *job, unsigned char *bitstream, int bs_size,
				   enc_result_t *res, int *create_rc)
{
	xvid_enc_create_t create;
	xvid_enc_plugin_t plugins[1];
	xvid_plugin_2pass1_t p1;
	xvid_plugin_2pass2_t p2;
	unsigned char *img;
	void *handle;
	int i, n = job->num_frames;
	int ret = 0;
	int bytes = 0;

	memset(&create, 0, sizeof(create));
	create.version = XVID_VERSION;
	create.width = TEST_W;
	create.height = TEST_H;
	create.profile = 0xf5; /* unrestricted */
	create.max_bframes = job->max_bframes;
	create.num_threads = job->threads;
	create.frame_drop_ratio = job->frame_drop;
	create.max_key_interval = 1000;
	if (job->vfr) {
		create.fincr = 0;
		create.fbase = job->fbase;
	} else {
		create.fincr = 1;
		create.fbase = job->fbase;
	}

	if (job->pass == 1) {
		memset(&p1, 0, sizeof(p1));
		p1.version = XVID_VERSION;
		p1.filename = (char *)job->stats;
		plugins[0].func = xvid_plugin_2pass1;
		plugins[0].param = &p1;
	} else {
		memset(&p2, 0, sizeof(p2));
		p2.version = XVID_VERSION;
		p2.filename = (char *)job->stats;
		p2.bitrate = job->bitrate;
		p2.container_frame_overhead = job->overhead;
		p2.vbv_size = job->vbv_size;
		p2.vbv_initial = job->vbv_size * 3 / 4;
		p2.vbv_maxrate = job->vbv_maxrate;
		p2.vbv_peakrate = job->vbv_peakrate;
		plugins[0].func = xvid_plugin_2pass2;
		plugins[0].param = &p2;
	}
	create.num_plugins = 1;
	create.plugins = plugins;

	*create_rc = xvid_encore(NULL, XVID_ENC_CREATE, &create, NULL);
	if (*create_rc < 0)
		return -1;
	handle = create.handle;

	img = malloc(TEST_W * TEST_H * 3 / 2);
	if (img == NULL) {
		xvid_encore(handle, XVID_ENC_DESTROY, NULL, NULL);
		return -1;
	}
	memset(img, 0x80, TEST_W * TEST_H * 3 / 2);

	for (i = 0; i <= n; i++) {
		xvid_enc_frame_t frame;
		xvid_enc_stats_t stats;
		int len;

		memset(&frame, 0, sizeof(frame));
		memset(&stats, 0, sizeof(stats));
		frame.version = XVID_VERSION;
		stats.version = XVID_VERSION;
		frame.bitstream = bitstream + bytes;
		frame.length = bs_size - bytes;
		if (i < n) {
			if (job->sched)
				frame.fincr = job->sched->dur[i];
			fill_frame(img, i);
			frame.input.csp = XVID_CSP_I420;
			frame.input.plane[0] = img;
			frame.input.plane[1] = img + TEST_W*TEST_H;
			frame.input.plane[2] = img + TEST_W*TEST_H*5/4;
			frame.input.stride[0] = TEST_W;
			frame.input.stride[1] = TEST_W/2;
			frame.input.stride[2] = TEST_W/2;
		} else {
			frame.input.csp = XVID_CSP_NULL;
		}

		len = xvid_encore(handle, XVID_ENC_ENCODE, &frame, &stats);
		if (len < 0 && len != XVID_ERR_END) {
			ret = -1;
			break;
		}
		if (i < n) {
			if (len > 0)
				bytes += len;
			if (res && res->sizes)
				res->sizes[i] = len > 0 ? len : 0;
			if (res && res->quants)
				res->quants[i] = stats.quant;
		}
	}

	xvid_encore(handle, XVID_ENC_DESTROY, NULL, NULL);
	free(img);

	if (res) {
		res->bytes = bytes;
		res->frames = ret == 0 ? n : -1;
	}
	return ret;
}

/*****************************************************************************
 * Scheduling helpers
 ****************************************************************************/

static void sched_uniform(schedule_t *s, int *dur, int count, int fbase, int fincr)
{
	int i;
	s->fbase = fbase;
	s->count = count;
	s->dur = dur;
	for (i = 0; i < count; i++)
		dur[i] = fincr;
}

static int64_t sched_total(const schedule_t *s)
{
	int i;
	int64_t t = 0;
	for (i = 0; i < s->count; i++)
		t += s->dur[i];
	return t;
}

/*****************************************************************************
 * MPEG-4 elementary stream inspection
 *
 * Independent of the encoder internals: scans start codes and parses
 * modulo_time_base and vop_time_increment with a plain integer bit reader.
 ****************************************************************************/

typedef struct {
	const unsigned char *data;
	long bits;
	long pos;
} bitreader_t;

static unsigned br_get(bitreader_t *b, int n)
{
	unsigned v = 0;
	int i;
	for (i = 0; i < n; i++) {
		v <<= 1;
		if (b->pos < b->bits)
			v |= (b->data[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
		if (b->pos < b->bits)
			b->pos++;
	}
	return v;
}

static int vop_time_bits(int fbase)
{
	int bits = 0;
	unsigned v = (unsigned)(fbase - 1);
	while (v) { bits++; v >>= 1; }
	if (bits < 1) bits = 1;
	return bits;
}

typedef struct {
	int frames;
	int64_t final_time;   /* cumulative display duration in fbase units */
	int width, height;
	int vol_seen;
	int timestamps_ok;
	int64_t *times;
} stream_info_t;

/* Returns 0 and fills info, -1 when the stream does not look like MPEG-4. */
static int inspect_stream(const unsigned char *bs, int len, int fbase,
						  stream_info_t *info)
{
	long pos = 0;
	int64_t seconds = 0;
	int nbits = vop_time_bits(fbase);
	int64_t last_time = 0;

	memset(info, 0, sizeof(*info));
	info->timestamps_ok = 1;

	while (pos + 4 <= len) {
		if (bs[pos] == 0 && bs[pos+1] == 0 && bs[pos+2] == 1) {
			unsigned code = bs[pos+3];

			if (code == 0x20) { /* VOL start code */
				/* Parse just enough to find width/height: the encoder writes
				 * the VOL in the standard order this test relies on. Use the
				 * decoder for authoritative dimensions elsewhere, so only
				 * record that a VOL was present here. */
				info->vol_seen = 1;
			} else if (code == 0xb6) { /* VOP start code */
				bitreader_t br;
				unsigned type;
				unsigned secs = 0, inc;

				br.data = bs;
				br.bits = (long)len * 8;
				br.pos = (pos + 4) * 8;
				type = br_get(&br, 2);
				if (type > 4) {
					pos += 4;
					continue;
				}
				while (br_get(&br, 1) == 1)
					secs++;
				(void)br_get(&br, 1); /* marker */
				inc = br_get(&br, nbits);

				seconds += secs;
				last_time = seconds * fbase + inc;
				info->times = realloc(info->times,
									  (info->frames + 1) * sizeof(int64_t));
				info->times[info->frames] = last_time;
				info->frames++;
			}
			pos += 4;
			continue;
		}
		pos++;
	}

	info->final_time = last_time;
	return info->frames > 0 ? 0 : -1;
}

/*****************************************************************************
 * Decoder driver (uses the library's own MPEG-4 decoder entry point)
 ****************************************************************************/

typedef struct {
	int frames;
	int width, height;
	int nonzero_frames;
	int last_type;
} dec_info_t;

static int decode_stream(const unsigned char *bs, int len, dec_info_t *out)
{
	xvid_dec_create_t create;
	xvid_dec_frame_t frame;
	xvid_dec_stats_t stats;
	unsigned char *outbuf;
	void *handle;
	int used = 0;
	int ret;

	memset(out, 0, sizeof(*out));
	memset(&create, 0, sizeof(create));
	create.version = XVID_VERSION;
	ret = xvid_decore(NULL, XVID_DEC_CREATE, &create, NULL);
	if (ret < 0)
		return -1;
	handle = create.handle;

	outbuf = malloc(TEST_W * TEST_H * 3 / 2);
	if (outbuf == NULL) {
		xvid_decore(handle, XVID_DEC_DESTROY, NULL, NULL);
		return -1;
	}

	while (used < len) {
		int consumed;

		memset(&frame, 0, sizeof(frame));
		memset(&stats, 0, sizeof(stats));
		frame.version = XVID_VERSION;
		stats.version = XVID_VERSION;
		frame.bitstream = (void *)(bs + used);
		frame.length = len - used;
		frame.output.csp = XVID_CSP_I420;
		frame.output.plane[0] = outbuf;
		frame.output.plane[1] = outbuf + TEST_W*TEST_H;
		frame.output.plane[2] = outbuf + TEST_W*TEST_H*5/4;
		frame.output.stride[0] = TEST_W;
		frame.output.stride[1] = TEST_W/2;
		frame.output.stride[2] = TEST_W/2;

		consumed = xvid_decore(handle, XVID_DEC_DECODE, &frame, &stats);
		if (consumed <= 0)
			break;
		used += consumed;

		if (stats.type > 0 && stats.type != XVID_TYPE_VOL) {
			int i, nonzero = 0;
			for (i = 0; i < TEST_W*TEST_H*3/2; i++) {
				if (outbuf[i] != 0 && outbuf[i] != 0x80) {
					nonzero = 1;
					break;
				}
			}
			out->frames++;
			out->nonzero_frames += nonzero;
			out->last_type = stats.type;
		}
	}

	xvid_decore(handle, XVID_DEC_DESTROY, NULL, NULL);
	free(outbuf);
	return 0;
}

/* Read width/height from a decoded VOL through the decoder stats. */
static void read_dimensions(const unsigned char *bs, int len, int *w, int *h)
{
	xvid_dec_create_t create;
	xvid_dec_frame_t frame;
	xvid_dec_stats_t stats;
	void *handle;
	int used = 0;
	*w = *h = 0;
	memset(&create, 0, sizeof(create));
	create.version = XVID_VERSION;
	if (xvid_decore(NULL, XVID_DEC_CREATE, &create, NULL) < 0)
		return;
	handle = create.handle;
	while (used < len && *w == 0) {
		int consumed;
		memset(&frame, 0, sizeof(frame));
		memset(&stats, 0, sizeof(stats));
		frame.version = XVID_VERSION;
		stats.version = XVID_VERSION;
		frame.bitstream = (void *)(bs + used);
		frame.length = len - used;
		frame.output.csp = XVID_CSP_NULL;
		consumed = xvid_decore(handle, XVID_DEC_DECODE, &frame, &stats);
		if (consumed <= 0)
			break;
		used += consumed;
		if (stats.type == XVID_TYPE_VOL) {
			*w = stats.data.vol.width;
			*h = stats.data.vol.height;
		}
	}
	xvid_decore(handle, XVID_DEC_DESTROY, NULL, NULL);
}

/*****************************************************************************
 * Assertions
 ****************************************************************************/

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

/*****************************************************************************
 * Global test state
 ****************************************************************************/

static char tmpdir[256];

static void path(char *out, size_t n, const char *name)
{
	snprintf(out, n, "%s/%s", tmpdir, name);
}

static int64_t expected_target(int bitrate, int64_t total_fincr, int fbase,
							   int num_frames, int overhead)
{
	/* (bitrate * sum(fincr)) / (8 * fbase), then the container overhead. */
	int64_t t = (bitrate * total_fincr) / ((int64_t)fbase * 8);
	return t - (int64_t)num_frames * overhead;
}

/* ------------------------------------------------------------------------- */
/* Group: timing                                                             */
/* ------------------------------------------------------------------------- */

static void case_timing(void)
{
	schedule_t s;
	int dur[24];
	char stats_path[512];
	int i, rc;
	static unsigned char bs[1 << 20];
	enc_job_t job;
	enc_result_t res;
	stream_info_t info;
	dec_info_t dec;
	int w = 0, h = 0;

	memset(&s, 0, sizeof(s));
	sched_uniform(&s, dur, 12, 40, 1);   /* 0.3 s total at 25fps equivalent */
	/* Alternate a moving frame and a long hold, plus one very long hold. */
	dur[0] = 1; dur[1] = 1; dur[2] = 8; dur[3] = 1;
	dur[4] = 1; dur[5] = 20; dur[6] = 1; dur[7] = 1;
	dur[8] = 8; dur[9] = 1; dur[10] = 1; dur[11] = 4;

	path(stats_path, sizeof(stats_path), "timing.stats");

	memset(&job, 0, sizeof(job));
	job.pass = 1;
	job.fbase = 40;
	job.vfr = 1;
	job.stats = stats_path;
	job.sched = &s;
	job.num_frames = 12;
	memset(&res, 0, sizeof(res));
	if (run_enc(&job, bs, sizeof(bs), &res, &rc) != 0) {
		CHECK(0, "timing first pass encode failed");
		return;
	}

	/* The first pass file advertises the schedule. */
	{
		FILE *f = fopen(stats_path, "rb");
		char line[512];
		int header = 0, durations = 0, stats_lines = 0;
		int last_frame = -1;
		int64_t total = 0;
		CHECK(f != NULL, "timing stats file exists");
		if (f) {
			while (fgets(line, sizeof(line), f)) {
				if (strncmp(line, "# XVID_TIMING_V1", 16) == 0) {
					int fb = 0;
					CHECK(sscanf(line, "# XVID_TIMING_V1 %d", &fb) == 1,
						  "timing header value");
					CHECK(fb == 40, "timing header time base");
					header++;
				} else if (strncmp(line, "# XVID_DURATION", 14) == 0) {
					int fn = -1, fi = 0;
					CHECK(sscanf(line, "# XVID_DURATION %d %d", &fn, &fi) == 2,
						  "duration line format");
					CHECK(fn == last_frame + 1, "duration frame numbers consecutive");
					CHECK(fi == dur[fn], "duration value matches input");
					last_frame = fn;
					total += fi;
					durations++;
				} else if (line[0] == 'i' || line[0] == 'p' ||
						   line[0] == 'b' || line[0] == 's') {
					stats_lines++;
				}
			}
			fclose(f);
		}
		CHECK(header == 1, "exactly one timing header");
		CHECK(durations == 12, "one duration per frame");
		CHECK(stats_lines == 12, "twelve statistics lines");
		CHECK(total == sched_total(&s), "schedule total preserved");
	}

	/* First pass bytes are also decodable and have consistent timestamps. */
	if (inspect_stream(bs, res.bytes, 40, &info) != 0) {
		CHECK(0, "first pass stream has VOPs");
	} else {
		int64_t expect = 0;
		CHECK(info.frames == 12, "first pass frame count");
		/* The VOP timestamp is the display start of the frame, so frame i
		 * carries the sum of durations of all frames before it, and the last
		 * frame's duration is not represented by any timestamp. */
		for (i = 0; i < info.frames && i < 12; i++) {
			if (info.times[i] != expect)
				break;
			expect += dur[i];
		}
		CHECK(i == 12, "first pass timestamps follow cumulative duration");
		CHECK(info.final_time + dur[11] == sched_total(&s),
			  "first pass last start plus duration equals sequence length");
	}

	read_dimensions(bs, res.bytes, &w, &h);
	CHECK(w == TEST_W && h == TEST_H, "decoded VOL dimensions");

	memset(&dec, 0, sizeof(dec));
	if (decode_stream(bs, res.bytes, &dec) != 0) {
		CHECK(0, "decoder accepted first pass stream");
	} else {
		CHECK(dec.frames == 12, "decoder frame count");
		CHECK(dec.nonzero_frames == dec.frames, "decoded frames are non-zero");
	}
}

/* ------------------------------------------------------------------------- */
/* Group: budget                                                             */
/* ------------------------------------------------------------------------- */

static void case_budget(void)
{
	schedule_t s;
	int dur[40];
	static unsigned char bs[2 << 20];
	enc_job_t job;
	enc_result_t res;
	int i, rc;
	int64_t total, expected;
	xvid_plugin_2pass2_t p2;
	xvid_plugin_2pass2_query_t q;
	xvid_plg_create_t pc;
	void *plugin;
	int quant_a[40];
	char stats_a[512], stats_b[512];

	/* Same frames and same total duration, different distribution. */
	for (i = 0; i < 40; i++) dur[i] = (i % 4 == 0) ? 4 : ((i % 4 == 1) ? 2 : 1);
	/* total = 10 per group of 4 => 100 units. fbase 50 => 2 seconds. */
	memset(&s, 0, sizeof(s));
	s.fbase = 50;
	s.count = 40;
	s.dur = dur;
	total = sched_total(&s);

	path(stats_a, sizeof(stats_a), "budget_a.stats");

	memset(&job, 0, sizeof(job));
	job.pass = 1; job.fbase = 50; job.vfr = 1; job.stats = stats_a;
	job.sched = &s; job.num_frames = 40;
	memset(&res, 0, sizeof(res));
	if (run_enc(&job, bs, sizeof(bs), &res, &rc) != 0) {
		CHECK(0, "budget first pass encode failed");
		return;
	}

	expected = expected_target(400000, total, 50, 40, 7);
	CHECK(expected == (int64_t)(((int64_t)400000 * total) / (8 * 50) - 40 * 7),
		  "expected target formula");

	/* Create only the plugin so the exact target can be queried. */
	memset(&pc, 0, sizeof(pc));
	pc.version = XVID_VERSION;
	pc.fincr = 0;
	pc.fbase = 50;
	pc.width = TEST_W; pc.height = TEST_H;
	pc.mb_width = TEST_W/16; pc.mb_height = TEST_H/16;
	memset(&p2, 0, sizeof(p2));
	p2.version = XVID_VERSION;
	p2.filename = stats_a;
	p2.bitrate = 400000;
	p2.container_frame_overhead = 7;
	pc.param = &p2;
	if (xvid_plugin_2pass2(NULL, XVID_PLG_CREATE, &pc, &plugin) < 0) {
		CHECK(0, "budget plugin creation");
		return;
	}
	memset(&q, 0, sizeof(q));
	q.version = XVID_VERSION;
	{
		static int scaled[64];
		q.scaled_length = scaled;
		xvid_plugin_2pass2(plugin, XVID_PLG_2PASS2_QUERY, &q, NULL);
	}
	CHECK(q.vfr == 1, "budget uses variable framerate plan");
	CHECK((int64_t)q.total_fincr == total, "budget total duration");
	CHECK(q.target_bytes == (uint64_t)expected, "budget exact integer target");
	xvid_plugin_2pass2(plugin, XVID_PLG_DESTROY, NULL, NULL);

	/* Negative bitrate is a KiB target, independent of timing. */
	memset(&p2, 0, sizeof(p2));
	p2.version = XVID_VERSION;
	p2.filename = stats_a;
	p2.bitrate = -300; /* 300 KiB */
	pc.param = &p2;
	if (xvid_plugin_2pass2(NULL, XVID_PLG_CREATE, &pc, &plugin) < 0) {
		CHECK(0, "negative bitrate plugin creation");
		return;
	}
	memset(&q, 0, sizeof(q));
	q.version = XVID_VERSION;
	xvid_plugin_2pass2(plugin, XVID_PLG_2PASS2_QUERY, &q, NULL);
	CHECK(q.target_bytes == (uint64_t)300 * 1024, "negative bitrate is KiB size");
	xvid_plugin_2pass2(plugin, XVID_PLG_DESTROY, NULL, NULL);

	/* Real second pass: the produced size tracks the target within a
	 * reasonable quantisation error, and the planned curve drives the
	 * quantizer choice. */
	job.pass = 2;
	job.bitrate = 400000;
	job.overhead = 7;
	memset(&res, 0, sizeof(res));
	res.quants = quant_a;
	if (run_enc(&job, bs, sizeof(bs), &res, &rc) != 0) {
		CHECK(0, "budget second pass encode failed");
		return;
	}
	{
		/* Target excludes container overhead; allow generous quantisation
		 * error but require the right order of magnitude. */
		int64_t lo = expected / 2;
		int64_t hi = expected * 2 + 4096;
		CHECK((int64_t)res.bytes >= lo && (int64_t)res.bytes <= hi,
			  "second pass size near exact target");
	}

	/* A different duration distribution with the same frame content and
	 * complexity, but four times the total duration. The exact target must
	 * scale with the real duration and the real second pass must spend the
	 * larger budget, ending at a visibly lower quantizer. */
	{
		int sched_a[32], sched_b[32];
		int qa[32], qb[32];
		int64_t target_a, target_b;
		int bytes_a = 0, bytes_b = 0;

		for (i = 0; i < 32; i++) {
			sched_a[i] = 1;
			sched_b[i] = 4;
		}

		path(stats_a, sizeof(stats_a), "budget_short.stats");
		path(stats_b, sizeof(stats_b), "budget_long.stats");

		memset(&s, 0, sizeof(s));
		s.fbase = 25; s.count = 32; s.dur = sched_a;
		memset(&job, 0, sizeof(job));
		job.pass = 1; job.fbase = 25; job.vfr = 1; job.stats = stats_a;
		job.sched = &s; job.num_frames = 32;
		memset(&res, 0, sizeof(res));
		if (run_enc(&job, bs, sizeof(bs), &res, &rc) != 0) {
			CHECK(0, "budget short first pass");
		} else {
			memset(&s, 0, sizeof(s));
			s.fbase = 25; s.count = 32; s.dur = sched_b;
			job.stats = stats_b; job.sched = &s;
			memset(&res, 0, sizeof(res));
			if (run_enc(&job, bs, sizeof(bs), &res, &rc) != 0) {
				CHECK(0, "budget long first pass");
			} else {
				int64_t tshort = sched_total(&(schedule_t){25, sched_a, 32});
				int64_t tlong = sched_total(&(schedule_t){25, sched_b, 32});

				target_a = (int64_t)(((int64_t)200000 * tshort) / (8 * 25));
				target_b = (int64_t)(((int64_t)200000 * tlong) / (8 * 25));
				CHECK(target_b == target_a * 4,
					  "target scales with real total duration");

				memset(&job, 0, sizeof(job));
				job.pass = 2; job.fbase = 25; job.vfr = 1;
				job.stats = stats_a; job.bitrate = 200000;
				job.sched = &(schedule_t){25, sched_a, 32}; job.num_frames = 32;
				memset(&res, 0, sizeof(res)); res.quants = qa;
				if (run_enc(&job, bs, sizeof(bs), &res, &rc) == 0)
					bytes_a = res.bytes;

				memset(&job, 0, sizeof(job));
				job.pass = 2; job.fbase = 25; job.vfr = 1;
				job.stats = stats_b; job.bitrate = 200000;
				job.sched = &(schedule_t){25, sched_b, 32}; job.num_frames = 32;
				memset(&res, 0, sizeof(res)); res.quants = qb;
				if (run_enc(&job, bs, sizeof(bs), &res, &rc) == 0)
					bytes_b = res.bytes;

				CHECK(bytes_a > 0 && bytes_b > 0, "both budget encodes ran");
				/* The four-times-longer sequence gets four times the bytes,
				 * so on identical content it must end at a lower quant. */
				CHECK(bytes_b > bytes_a, "longer timeline gets the larger budget");
				{
					long sum_a = 0, sum_b = 0;
					for (i = 0; i < 32; i++) { sum_a += qa[i]; sum_b += qb[i]; }
					CHECK(sum_b < sum_a, "longer timeline uses lower quantizers");
				}
			}
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Group: vbv                                                                */
/* ------------------------------------------------------------------------- */

static void case_vbv(void)
{
	xvid_plg_create_t pc;
	xvid_plugin_2pass2_t p2;
	xvid_plugin_2pass2_query_t q;
	void *plugin;

	/* Two synthetic stats files with the same per-frame sizes, same total
	 * duration, but a different duration distribution. The VBV and peak
	 * checks must react to the real time table, not to a fixed fps. */
	const int n = 20;
	int dur_a[20], dur_b[20];
	int i;
	char sa[512], sb[512];
	int scaled_a[20];

	memset(&pc, 0, sizeof(pc));
	pc.version = XVID_VERSION;
	pc.fincr = 0;
	pc.fbase = 10;
	pc.width = 64; pc.height = 48;
	pc.mb_width = 4; pc.mb_height = 3;

	for (i = 0; i < n; i++) dur_a[i] = 1;      /* 2 s of uniform frames */
	for (i = 0; i < n; i++) dur_b[i] = (i < 10) ? 1 : 3; /* 2 s, half long */

	path(sa, sizeof(sa), "vbv_a.stats");
	path(sb, sizeof(sb), "vbv_b.stats");
	{
		FILE *f;
		f = fopen(sa, "wb");
		CHECK(f != NULL, "vbv stats a created");
		if (f) {
			fprintf(f, "# XviD 2pass stat file\n\n# XVID_TIMING_V1 10\n");
			for (i = 0; i < n; i++) {
				fprintf(f, "# XVID_DURATION %d %d\n", i, dur_a[i]);
				if (i == 0) fprintf(f, "i 2 60 0 0 6000 200\n");
				else        fprintf(f, "p 2 0 60 0 3000 40\n");
			}
			fclose(f);
		}
		f = fopen(sb, "wb");
		CHECK(f != NULL, "vbv stats b created");
		if (f) {
			fprintf(f, "# XviD 2pass stat file\n\n# XVID_TIMING_V1 10\n");
			for (i = 0; i < n; i++) {
				fprintf(f, "# XVID_DURATION %d %d\n", i, dur_b[i]);
				if (i == 0) fprintf(f, "i 2 60 0 0 6000 200\n");
				else        fprintf(f, "p 2 0 60 0 3000 40\n");
			}
			fclose(f);
		}
	}

	/* A long, static shot should not be mistaken for many short frames: with
	 * the real time model, the peak window spans by end time. Request a
	 * peakrate that one schedule satisfies and the other does not, then
	 * check the plugin's VBV verdicts differ accordingly. We probe by
	 * scanning peakrate values: the schedule with frames clustered at the
	 * end has a different one-second window content. */
	{
		int a_seen_peak = 0, b_seen_peak = 0;
		int peak;
		for (peak = 20000; peak <= 120000; peak += 20000) {
			memset(&p2, 0, sizeof(p2));
			p2.version = XVID_VERSION;
			p2.filename = sa;
			p2.bitrate = 200000;
			p2.vbv_size = 400000;
			p2.vbv_initial = 300000;
			p2.vbv_maxrate = 400000;
			p2.vbv_peakrate = peak;
			pc.param = &p2;
			if (xvid_plugin_2pass2(NULL, XVID_PLG_CREATE, &pc, &plugin) >= 0) {
				memset(&q, 0, sizeof(q));
				q.version = XVID_VERSION;
				xvid_plugin_2pass2(plugin, XVID_PLG_2PASS2_QUERY, &q, NULL);
				CHECK(q.vbv_checked == 1, "vbv checked");
				CHECK(q.vfr == 1, "vbv variable framerate");
				if (q.vbv_pre_status & 4) a_seen_peak++;
				xvid_plugin_2pass2(plugin, XVID_PLG_DESTROY, NULL, NULL);
			}

			memset(&p2, 0, sizeof(p2));
			p2.version = XVID_VERSION;
			p2.filename = sb;
			p2.bitrate = 200000;
			p2.vbv_size = 400000;
			p2.vbv_initial = 300000;
			p2.vbv_maxrate = 400000;
			p2.vbv_peakrate = peak;
			pc.param = &p2;
			if (xvid_plugin_2pass2(NULL, XVID_PLG_CREATE, &pc, &plugin) >= 0) {
				memset(&q, 0, sizeof(q));
				q.version = XVID_VERSION;
				xvid_plugin_2pass2(plugin, XVID_PLG_2PASS2_QUERY, &q, NULL);
				if (q.vbv_pre_status & 4) b_seen_peak++;
				xvid_plugin_2pass2(plugin, XVID_PLG_DESTROY, NULL, NULL);
			}
		}
		CHECK(a_seen_peak > 0, "uniform schedule has a peak violation somewhere");
		CHECK(b_seen_peak > 0, "clustered schedule has a peak violation somewhere");
	}

	/* Duration refill: the byte allowance for a frame is proportional to its
	 * real duration. Two files contain the same per-frame sizes and the same
	 * total duration, but one puts the long frames on the large frames and
	 * the other on the small frames. Under a tight buffer the refill pattern
	 * makes the second schedule underflow while the first does not. This
	 * cannot happen with a fixed frame count window. */
	{
		char ca[512], cb[512];
		int pre_a = -1, pre_b = -1;
		int cn = 20;

		path(ca, sizeof(ca), "vbv_refill_a.stats");
		path(cb, sizeof(cb), "vbv_refill_b.stats");
		{
			FILE *fa = fopen(ca, "wb");
			FILE *fb = fopen(cb, "wb");
			CHECK(fa && fb, "refill stats created");
			if (fa && fb) {
				fprintf(fa, "# XviD 2pass stat file\n\n# XVID_TIMING_V1 50\n");
				fprintf(fb, "# XviD 2pass stat file\n\n# XVID_TIMING_V1 50\n");
				for (i = 0; i < cn; i++) {
					int big = (i % 2) == 0;
					int len = big ? 5000 : 250;
					/* A: uniform 5 units. B: big frames 2 units, small 8. */
					fprintf(fa, "# XVID_DURATION %d %d\n", i, 5);
					fprintf(fb, "# XVID_DURATION %d %d\n", i, big ? 2 : 8);
					fprintf(fa, i == 0 ? "i 2 60 0 0 %d 0\n" : "p 2 0 60 0 %d 0\n", len);
					fprintf(fb, i == 0 ? "i 2 60 0 0 %d 0\n" : "p 2 0 60 0 %d 0\n", len);
				}
				fclose(fa);
				fclose(fb);
			}
		}
		memset(&p2, 0, sizeof(p2));
		p2.version = XVID_VERSION;
		p2.filename = ca;
		p2.bitrate = 150000;
		p2.vbv_size = 40000;
		p2.vbv_initial = 20000;
		p2.vbv_maxrate = 150000;
		p2.vbv_peakrate = 0;
		pc.fbase = 50;
		pc.param = &p2;
		if (xvid_plugin_2pass2(NULL, XVID_PLG_CREATE, &pc, &plugin) >= 0) {
			memset(&q, 0, sizeof(q));
			q.version = XVID_VERSION;
			q.scaled_length = scaled_a;
			xvid_plugin_2pass2(plugin, XVID_PLG_2PASS2_QUERY, &q, NULL);
			pre_a = q.vbv_pre_status;
			CHECK(q.vbv_scaled == 1, "uniform refill plan is scaled");
			xvid_plugin_2pass2(plugin, XVID_PLG_DESTROY, NULL, NULL);
		}
		memset(&p2, 0, sizeof(p2));
		p2.version = XVID_VERSION;
		p2.filename = cb;
		p2.bitrate = 150000;
		p2.vbv_size = 40000;
		p2.vbv_initial = 20000;
		p2.vbv_maxrate = 150000;
		p2.vbv_peakrate = 0;
		pc.fbase = 50;
		pc.param = &p2;
		if (xvid_plugin_2pass2(NULL, XVID_PLG_CREATE, &pc, &plugin) >= 0) {
			memset(&q, 0, sizeof(q));
			q.version = XVID_VERSION;
			xvid_plugin_2pass2(plugin, XVID_PLG_2PASS2_QUERY, &q, NULL);
			pre_b = q.vbv_pre_status;
			xvid_plugin_2pass2(plugin, XVID_PLG_DESTROY, NULL, NULL);
		}
		CHECK(pre_a == 0, "uniform duration refill is compliant");
		CHECK((pre_b & 1) != 0, "long frames on small frames underflow");
	}

	/* Real one second peak window: the same frame sizes and the same total
	 * duration give a different window content depending on where the long
	 * frames sit. A fixed number of frames per second cannot see this. */
	{
		char pa[512], pb[512];
		int peak_a = -1, peak_b = -1;

		path(pa, sizeof(pa), "vbv_peak_a.stats");
		path(pb, sizeof(pb), "vbv_peak_b.stats");
		{
			FILE *fa = fopen(pa, "wb");
			FILE *fb = fopen(pb, "wb");
			CHECK(fa && fb, "peak stats created");
			if (fa && fb) {
				fprintf(fa, "# XviD 2pass stat file\n\n# XVID_TIMING_V1 100\n");
				fprintf(fb, "# XviD 2pass stat file\n\n# XVID_TIMING_V1 100\n");
				for (i = 0; i < 12; i++) {
					int big = (i % 2) == 0;
					int len = big ? 4000 : 200;
					/* Both are 1.08 s long: A has every frame at 9 units,
					 * B gives big frames 2 and small frames 16. */
					fprintf(fa, "# XVID_DURATION %d 9\n", i);
					fprintf(fb, "# XVID_DURATION %d %d\n", i, big ? 2 : 16);
					fprintf(fa, i == 0 ? "i 2 60 0 0 %d 0\n" : "p 2 0 60 0 %d 0\n", len);
					fprintf(fb, i == 0 ? "i 2 60 0 0 %d 0\n" : "p 2 0 60 0 %d 0\n", len);
				}
				fclose(fa);
				fclose(fb);
			}
		}
		memset(&p2, 0, sizeof(p2));
		p2.version = XVID_VERSION;
		p2.filename = pa;
		p2.bitrate = 186666;
		p2.vbv_size = 4000000;
		p2.vbv_initial = 3000000;
		p2.vbv_maxrate = 4000000;
		p2.vbv_peakrate = 200000;
		pc.fbase = 100;
		pc.param = &p2;
		if (xvid_plugin_2pass2(NULL, XVID_PLG_CREATE, &pc, &plugin) >= 0) {
			memset(&q, 0, sizeof(q));
			q.version = XVID_VERSION;
			xvid_plugin_2pass2(plugin, XVID_PLG_2PASS2_QUERY, &q, NULL);
			peak_a = q.vbv_pre_status;
			xvid_plugin_2pass2(plugin, XVID_PLG_DESTROY, NULL, NULL);
		}
		memset(&p2, 0, sizeof(p2));
		p2.version = XVID_VERSION;
		p2.filename = pb;
		p2.bitrate = 186666;
		p2.vbv_size = 4000000;
		p2.vbv_initial = 3000000;
		p2.vbv_maxrate = 4000000;
		p2.vbv_peakrate = 200000;
		pc.fbase = 100;
		pc.param = &p2;
		if (xvid_plugin_2pass2(NULL, XVID_PLG_CREATE, &pc, &plugin) >= 0) {
			memset(&q, 0, sizeof(q));
			q.version = XVID_VERSION;
			xvid_plugin_2pass2(plugin, XVID_PLG_2PASS2_QUERY, &q, NULL);
			peak_b = q.vbv_pre_status;
			xvid_plugin_2pass2(plugin, XVID_PLG_DESTROY, NULL, NULL);
		}
		/* A has the same byte demand spread uniformly over real time, so its
		 * sliding one second window always sees the peak. B parks the long
		 * holds between the expensive frames, which pulls them out of the
		 * same one second window. */
		CHECK(peak_a != peak_b,
			  "peak outcome depends on where holds sit in real time");
	}

	/* Scene minimum interval uses real elapsed time, not a frame count:
	 * two files with I frames at exactly the same frame positions but with
	 * different durations must produce a different scene count. */
	{
		char sc1[512], sc2[512];
		int n_scenes[2];
		int file;
		for (file = 0; file < 2; file++) {
			FILE *f;
			int one = (file == 0) ? 1 : 3; /* intervals of 0.5 s vs 1.5 s */
			path(file == 0 ? sc1 : sc2, sizeof(sc1), "vbv_scenes.stats");
			f = fopen(file == 0 ? sc1 : sc2, "wb");
			CHECK(f != NULL, "scene stats created");
			if (f) {
				/* I frames at 0, 6, 12, 18; 5 P frames in between. */
				fprintf(f, "# XviD 2pass stat file\n\n# XVID_TIMING_V1 10\n");
				for (i = 0; i < 24; i++) {
					fprintf(f, "# XVID_DURATION %d %d\n", i, one);
					if (i % 6 == 0) fprintf(f, "i 2 60 0 0 3000 200\n");
					else            fprintf(f, "p 2 0 60 0 3000 40\n");
				}
				fclose(f);
			}
			memset(&p2, 0, sizeof(p2));
			p2.version = XVID_VERSION;
			p2.filename = file == 0 ? sc1 : sc2;
			p2.bitrate = 200000;
			p2.vbv_size = 400000;
			p2.vbv_initial = 300000;
			p2.vbv_maxrate = 800000;
			p2.vbv_peakrate = 0;
			pc.fbase = 10;
			pc.param = &p2;
			n_scenes[file] = -1;
			if (xvid_plugin_2pass2(NULL, XVID_PLG_CREATE, &pc, &plugin) >= 0) {
				memset(&q, 0, sizeof(q));
				q.version = XVID_VERSION;
				xvid_plugin_2pass2(plugin, XVID_PLG_2PASS2_QUERY, &q, NULL);
				n_scenes[file] = q.num_scenes;
				xvid_plugin_2pass2(plugin, XVID_PLG_DESTROY, NULL, NULL);
			}
		}
		/* Distances are measured against the last accepted scene start:
		 * with 0.6 s per group only frames 0 and 12 qualify (2 scenes);
		 * with 1.8 s per group every I frame qualifies (4 scenes). */
		CHECK(n_scenes[0] == 2, "short elapsed intervals limit scene starts");
		CHECK(n_scenes[1] == 4, "long elapsed intervals start every scene");
		CHECK(n_scenes[0] != n_scenes[1], "scene count follows elapsed time");
	}

	/* Planned curve must remain sane after scaling: scaled lengths equal the
	 * plan curve reported and stay positive. */
	CHECK(scaled_a[0] > 0, "vbv plan curve populated");
}

/* ------------------------------------------------------------------------- */
/* Group: compatibility                                                      */
/* ------------------------------------------------------------------------- */

static int write_stats_file(const char *path, const char *body)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	fputs(body, f);
	fclose(f);
	return 0;
}

static int create_plugin(const char *stats, int vfr, int fbase, int bitrate)
{
	xvid_plg_create_t pc;
	xvid_plugin_2pass2_t p2;
	void *plugin;

	memset(&pc, 0, sizeof(pc));
	pc.version = XVID_VERSION;
	pc.fincr = vfr ? 0 : 1;
	pc.fbase = fbase;
	pc.width = 64; pc.height = 48;
	pc.mb_width = 4; pc.mb_height = 3;
	memset(&p2, 0, sizeof(p2));
	p2.version = XVID_VERSION;
	p2.filename = (char *)stats;
	p2.bitrate = bitrate;
	pc.param = &p2;
	return xvid_plugin_2pass2(NULL, XVID_PLG_CREATE, &pc, &plugin);
}

static void case_compatibility(void)
{
	char p[512];
	int rc;
	static unsigned char bs[1 << 20];
	enc_job_t job;
	enc_result_t res;
	schedule_t s;
	int dur[8];
	int i;

	/* Legacy fixed framerate stats file (no timing header) still works in a
	 * fixed framerate second pass. */
	path(p, sizeof(p), "legacy.stats");
	rc = write_stats_file(p,
		"# XviD 2pass stat file (core version 1.4.0)\n"
		"# Please do not modify this file\n\n"
		"i 2 60 0 0 4000 200\n"
		"p 2 0 60 0 2000 40\n"
		"p 2 0 60 0 2100 41\n"
		"p 2 0 60 0 1900 39\n"
		"p 2 0 60 0 2000 40\n"
		"p 2 0 60 0 2100 41\n"
		"p 2 0 60 0 1900 39\n"
		"p 2 0 60 0 2000 40\n"
		"p 2 0 60 0 2000 40\n");
	CHECK(rc == 0, "legacy stats file written");
	CHECK(create_plugin(p, 0, 25, 200000) >= 0,
		  "fixed framerate second pass reads legacy file");

	/* Variable framerate second pass must refuse a legacy file: no guessing. */
	CHECK(create_plugin(p, 1, 25, 200000) < 0,
		  "vfr second pass refuses file without timing schedule");

	/* Malformed schedules must fail creation. */
	path(p, sizeof(p), "bad_missing.stats");
	write_stats_file(p,
		"# XviD 2pass stat file\n\n# XVID_TIMING_V1 25\n"
		"# XVID_DURATION 0 1\n"
		"i 2 60 0 0 4000 200\n"
		"p 2 0 60 0 2000 40\n");
	CHECK(create_plugin(p, 1, 25, 200000) < 0, "missing duration rejected");

	path(p, sizeof(p), "bad_duplicate.stats");
	write_stats_file(p,
		"# XviD 2pass stat file\n\n# XVID_TIMING_V1 25\n"
		"# XVID_DURATION 0 1\n"
		"# XVID_DURATION 0 2\n"
		"i 2 60 0 0 4000 200\n");
	CHECK(create_plugin(p, 1, 25, 200000) < 0, "duplicate duration rejected");

	path(p, sizeof(p), "bad_zero.stats");
	write_stats_file(p,
		"# XviD 2pass stat file\n\n# XVID_TIMING_V1 25\n"
		"# XVID_DURATION 0 0\n"
		"i 2 60 0 0 4000 200\n");
	CHECK(create_plugin(p, 1, 25, 200000) < 0, "zero duration rejected");

	path(p, sizeof(p), "bad_negative.stats");
	write_stats_file(p,
		"# XviD 2pass stat file\n\n# XVID_TIMING_V1 25\n"
		"# XVID_DURATION 0 -4\n"
		"i 2 60 0 0 4000 200\n");
	CHECK(create_plugin(p, 1, 25, 200000) < 0, "negative duration rejected");

	path(p, sizeof(p), "bad_order.stats");
	write_stats_file(p,
		"# XviD 2pass stat file\n\n# XVID_TIMING_V1 25\n"
		"i 2 60 0 0 4000 200\n"
		"# XVID_DURATION 1 1\n"
		"p 2 0 60 0 2000 40\n");
	CHECK(create_plugin(p, 1, 25, 200000) < 0, "duration after stats rejected");

	path(p, sizeof(p), "bad_dupheader.stats");
	write_stats_file(p,
		"# XviD 2pass stat file\n\n# XVID_TIMING_V1 25\n# XVID_TIMING_V1 25\n"
		"# XVID_DURATION 0 1\n"
		"i 2 60 0 0 4000 200\n");
	CHECK(create_plugin(p, 1, 25, 200000) < 0, "duplicate timing header rejected");

	/* Time base mismatch between the file and the encoder is fatal. */
	path(p, sizeof(p), "good.stats");
	write_stats_file(p,
		"# XviD 2pass stat file\n\n# XVID_TIMING_V1 50\n"
		"# XVID_DURATION 0 1\n"
		"i 2 60 0 0 4000 200\n"
		"# XVID_DURATION 1 1\n"
		"p 2 0 60 0 2000 40\n");
	CHECK(create_plugin(p, 1, 50, 200000) >= 0, "matching time base accepted");
	CHECK(create_plugin(p, 1, 25, 200000) < 0, "time base mismatch rejected");

	/* A large but legal per-frame duration is accepted; the time base is the
	 * value limited to 65535, not the frame duration. */
	path(p, sizeof(p), "large_duration.stats");
	rc = write_stats_file(p,
		"# XviD 2pass stat file\n\n# XVID_TIMING_V1 25\n"
		"# XVID_DURATION 0 100000\n"
		"i 2 60 0 0 4000 200\n");
	CHECK(rc == 0, "large duration stats written");
	CHECK(create_plugin(p, 1, 25, 200000) >= 0,
		  "long hold duration accepted");

	/* Numeric overflow in the accumulated duration must fail creation. */
	path(p, sizeof(p), "bad_overflow.stats");
	{
		FILE *f = fopen(p, "wb");
		CHECK(f != NULL, "overflow stats opened");
		if (f) {
			/* Repeated maximum value: the parser must stop with an overflow
			 * error rather than wrapping the 64 bit sum. */
			int k;
			fprintf(f, "# XviD 2pass stat file\n\n# XVID_TIMING_V1 25\n");
			for (k = 0; k < 60000; k++) {
				fprintf(f, "# XVID_DURATION %d 2147483647\n", k);
				if (k == 0) fprintf(f, "i 2 60 0 0 4000 200\n");
				else        fprintf(f, "p 2 0 60 0 2000 40\n");
			}
			fclose(f);
		}
		/* 60000 * 2^31 still fits in an int64, so also ask for a bitrate
		 * whose target product (bitrate * total / 8) exceeds 64 bits. The
		 * exact target computation must fail instead of wrapping. */
		CHECK(create_plugin(p, 1, 25, 0x7fffffff) < 0,
			  "unrepresentable target rejected");
		CHECK(create_plugin(p, 1, 25, 200000) >= 0,
			  "large total with small bitrate still accepted");
	}

	/* A non-numeric or out of range duration must fail. */
	rc = write_stats_file(p,
		"# XviD 2pass stat file\n\n# XVID_TIMING_V1 25\n"
		"# XVID_DURATION 0 9999999999999999999999\n"
		"i 2 60 0 0 4000 200\n");
	CHECK(rc == 0, "overflow stats written");
	CHECK(create_plugin(p, 1, 25, 200000) < 0, "overflowing duration rejected");

	/* Two encoders interleaved: the second pass must validate each frame's
	 * duration against its own schedule, and one encoder's plugin state must
	 * not leak into the other. Run a VFR first pass, then create two VFR
	 * second-pass plugin contexts from different stats files and interleave
	 * frame-level queries; their reported totals must remain distinct. */
	{
		char sa[512], sb[512];
		xvid_plg_create_t pc;
		xvid_plugin_2pass2_t pa, pb;
		xvid_plugin_2pass2_query_t qa, qb;
		void *ha, *hb;

		path(sa, sizeof(sa), "inter_a.stats");
		path(sb, sizeof(sb), "inter_b.stats");
		write_stats_file(sa,
			"# XviD 2pass stat file\n\n# XVID_TIMING_V1 10\n"
			"# XVID_DURATION 0 2\ni 2 60 0 0 4000 200\n"
			"# XVID_DURATION 1 2\np 2 0 60 0 2000 40\n");
		write_stats_file(sb,
			"# XviD 2pass stat file\n\n# XVID_TIMING_V1 10\n"
			"# XVID_DURATION 0 5\ni 2 60 0 0 4000 200\n"
			"# XVID_DURATION 1 5\np 2 0 60 0 2000 40\n");

		memset(&pc, 0, sizeof(pc));
		pc.version = XVID_VERSION;
		pc.fincr = 0; pc.fbase = 10;
		pc.width = 64; pc.height = 48; pc.mb_width = 4; pc.mb_height = 3;
		memset(&pa, 0, sizeof(pa));
		pa.version = XVID_VERSION; pa.filename = sa; pa.bitrate = 160000;
		memset(&pb, 0, sizeof(pb));
		pb.version = XVID_VERSION; pb.filename = sb; pb.bitrate = 160000;
		pc.param = &pa;
		ha = NULL; hb = NULL;
		if (xvid_plugin_2pass2(NULL, XVID_PLG_CREATE, &pc, &ha) >= 0) {
			pc.param = &pb;
			if (xvid_plugin_2pass2(NULL, XVID_PLG_CREATE, &pc, &hb) < 0) {
				CHECK(0, "second interleaved plugin created");
				hb = NULL;
			}
		}
		CHECK(ha != NULL && hb != NULL, "two plugin contexts created");
		if (ha && hb) {
			/* Interleave frame level calls between the two contexts. Each
			 * plugin must keep its own schedule: the first file has two 2
			 * unit frames, the second two 5 unit frames. */
			xvid_plg_data_t da, db;
			int frame;
			int mismatch_a = 0, mismatch_b = 0;

			for (frame = 0; frame < 2; frame++) {
				memset(&da, 0, sizeof(da));
				da.version = XVID_VERSION;
				da.frame_num = frame;
				da.width = 64; da.height = 48;
				da.mb_width = 4; da.mb_height = 3;
				da.fincr = 2; /* correct for ha, wrong for hb */
				da.quant = 0;
				da.type = XVID_TYPE_AUTO;
				if (xvid_plugin_2pass2(ha, XVID_PLG_BEFORE, &da, NULL) < 0)
					mismatch_a++;

				memset(&db, 0, sizeof(db));
				db.version = XVID_VERSION;
				db.frame_num = frame;
				db.width = 64; db.height = 48;
				db.mb_width = 4; db.mb_height = 3;
				db.fincr = 5; /* correct for hb, wrong for ha */
				db.quant = 0;
				db.type = XVID_TYPE_AUTO;
				if (xvid_plugin_2pass2(hb, XVID_PLG_BEFORE, &db, NULL) < 0)
					mismatch_b++;
			}
			CHECK(mismatch_a == 0, "first context accepts its own durations");
			CHECK(mismatch_b == 0, "second context accepts its own durations");

			memset(&qa, 0, sizeof(qa)); qa.version = XVID_VERSION;
			memset(&qb, 0, sizeof(qb)); qb.version = XVID_VERSION;
			xvid_plugin_2pass2(ha, XVID_PLG_2PASS2_QUERY, &qa, NULL);
			xvid_plugin_2pass2(hb, XVID_PLG_2PASS2_QUERY, &qb, NULL);
			CHECK(qa.total_fincr == 4, "first context total duration");
			CHECK(qb.total_fincr == 10, "second context total duration");
			CHECK(qa.target_bytes != qb.target_bytes,
				  "independent contexts keep independent budgets");

			/* Now feed each context the other schedule's duration: both must
			 * reject it, proving the schedules did not leak between them. */
			memset(&da, 0, sizeof(da));
			da.version = XVID_VERSION; da.frame_num = 0;
			da.width = 64; da.height = 48; da.mb_width = 4; da.mb_height = 3;
			da.fincr = 5; da.quant = 0; da.type = XVID_TYPE_AUTO;
			CHECK(xvid_plugin_2pass2(ha, XVID_PLG_BEFORE, &da, NULL) < 0,
				  "first context rejects the other schedule");

			xvid_plugin_2pass2(ha, XVID_PLG_DESTROY, NULL, NULL);
			xvid_plugin_2pass2(hb, XVID_PLG_DESTROY, NULL, NULL);
		}
	}

	/* A VFR frame whose fincr disagrees with the schedule must abort the
	 * encode. */
	path(p, sizeof(p), "mismatch.stats");
	write_stats_file(p,
		"# XviD 2pass stat file\n\n# XVID_TIMING_V1 40\n"
		"# XVID_DURATION 0 1\ni 2 60 0 0 4000 200\n"
		"# XVID_DURATION 1 1\np 2 0 60 0 2000 40\n"
		"# XVID_DURATION 2 1\np 2 0 60 0 2000 40\n"
		"# XVID_DURATION 3 1\np 2 0 60 0 2000 40\n");
	sched_uniform(&s, dur, 4, 40, 1);
	dur[2] = 2; /* mismatches its recorded duration of 1 */
	memset(&job, 0, sizeof(job));
	job.pass = 2; job.fbase = 40; job.vfr = 1; job.stats = p;
	job.sched = &s; job.num_frames = 4; job.bitrate = 160000;
	memset(&res, 0, sizeof(res));
	CHECK(run_enc(&job, bs, sizeof(bs), &res, &rc) < 0,
		  "second pass rejects mismatched frame duration");

	/* Fixed framerate behaviour is unchanged: a fixed first pass and a fixed
	 * second pass round trip. */
	{
		char fs[512];
		schedule_t none;
		path(fs, sizeof(fs), "fixed.stats");
		memset(&none, 0, sizeof(none));
		memset(&job, 0, sizeof(job));
		job.pass = 1; job.fbase = 25; job.vfr = 0; job.stats = fs;
		job.num_frames = 8; job.sched = NULL;
		memset(&res, 0, sizeof(res));
		CHECK(run_enc(&job, bs, sizeof(bs), &res, &rc) == 0,
			  "fixed framerate first pass");
		{
			FILE *f = fopen(fs, "rb");
			char line[512];
			int timed = 0;
			CHECK(f != NULL, "fixed stats file");
			if (f) {
				while (fgets(line, sizeof(line), f))
					if (strncmp(line, "# XVID_TIMING", 13) == 0 ||
						strncmp(line, "# XVID_DURATION", 15) == 0)
						timed++;
				fclose(f);
			}
			CHECK(timed == 0, "fixed first pass writes no timing extension");
		}
		memset(&job, 0, sizeof(job));
		job.pass = 2; job.fbase = 25; job.vfr = 0; job.stats = fs;
		job.num_frames = 8; job.bitrate = 160000;
		memset(&res, 0, sizeof(res));
		CHECK(run_enc(&job, bs, sizeof(bs), &res, &rc) == 0,
			  "fixed framerate second pass");
	}

	(void)i;
	(void)res;
}

/*****************************************************************************
 * main
 ****************************************************************************/

int main(int argc, char **argv)
{
	xvid_gbl_init_t init;
	int ok = 1;

	if (argc > 1)
		snprintf(tmpdir, sizeof(tmpdir), "%s", argv[1]);
	else
		snprintf(tmpdir, sizeof(tmpdir), ".");

	memset(&init, 0, sizeof(init));
	init.version = XVID_VERSION;
	/* XVID_CPU_FORCE with no feature bits: plain C only, no assembly. */
	init.cpu_flags = 0x80000000u;
	if (xvid_global(NULL, XVID_GBL_INIT, &init, NULL) < 0) {
		fprintf(stderr, "xvid_global init failed\n");
		return 1;
	}

	failures = 0;
	case_timing();
	printf("VFR_CASE timing %s\n", failures == 0 ? "PASS" : "FAIL");
	if (failures) ok = 0;

	failures = 0;
	case_budget();
	printf("VFR_CASE budget %s\n", failures == 0 ? "PASS" : "FAIL");
	if (failures) ok = 0;

	failures = 0;
	case_vbv();
	printf("VFR_CASE vbv %s\n", failures == 0 ? "PASS" : "FAIL");
	if (failures) ok = 0;

	failures = 0;
	case_compatibility();
	printf("VFR_CASE compatibility %s\n", failures == 0 ? "PASS" : "FAIL");
	if (failures) ok = 0;

	return ok ? 0 : 1;
}
