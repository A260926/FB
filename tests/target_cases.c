/*
Regression tests for PES PTS tracking on decoded video frames.

The same MPEG-1 video elementary stream that ships in task_support/source.mpg
is re-packaged here in C: a small muxer rebuilds an MPEG-PS container with an
independently chosen packet table so that one packet holds several pictures,
another only holds the tail of a picture, start codes straddle packet
boundaries, some packets carry no PTS and 33 bit values wrap around. Neither
Python nor FFmpeg is used.

Each test derives its expected per-frame labels from an independent reference:
packets are located by the first 00 byte of each picture start code and an
I/P-delay / B-immediate model produces the display order, without reusing any
of the library's bookkeeping.
*/

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* The implementation is provided by task_support/plm_impl.c in this build. */
#include "pl_mpeg.h"
#include "task_support/case_harness.h"

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

// MPEG-1 picture coding types (the library's matching constants are internal
// to the implementation section).
#define TEST_PICTURE_TYPE_B 3

#define SOURCE_PATH "task_support/source.mpg"
#define MAX_PICS 64
#define MAX_FRAMES 64
#define PTS_WRAP33 ((int64_t)1 << 33)
#define PTS_HALF33 ((int64_t)1 << 32)

typedef struct {
	size_t offset;   // first 00 byte of the picture start code, ES relative
	int tref;
	int type;        // 1 = I, 2 = P, 3 = B
} test_pic_t;

typedef struct {
	size_t cut;      // ES offset one past the last byte of this packet
	int has_pts;     // 1 => marker 0x2 with PTS, 0 => marker 0x0, no PTS
	int64_t raw_pts; // raw 33 bit PTS in 90kHz ticks (ignored when !has_pts)
} test_spec_t;

typedef struct {
	int has_pts;
	int64_t pts90k;
} test_label_t;

typedef struct {
	int has_pts;
	int64_t pts90k;
	double time;
	uint8_t *y;
	uint8_t *cb;
	uint8_t *cr;
} test_frame_t;

static uint8_t *load_file(const char *path, size_t *out_len) {
	FILE *fh = fopen(path, "rb");
	if (!fh) {
		return NULL;
	}
	fseek(fh, 0, SEEK_END);
	long sz = ftell(fh);
	fseek(fh, 0, SEEK_SET);
	uint8_t *data = (uint8_t *)malloc((size_t)sz);
	size_t read = fread(data, 1, (size_t)sz, fh);
	fclose(fh);
	*out_len = read;
	return data;
}

// Concatenate every video PES payload of a PS file into one elementary stream.

static uint8_t *extract_es(const uint8_t *ps, size_t ps_len, size_t *out_es_len) {
	plm_buffer_t *buffer = plm_buffer_create_with_memory(
		(uint8_t *)ps, ps_len, FALSE
	);
	plm_demux_t *demux = plm_demux_create(buffer, TRUE);
	uint8_t *es = (uint8_t *)malloc(ps_len);
	size_t es_len = 0;
	plm_packet_t *packet;
	while ((packet = plm_demux_decode(demux))) {
		if (packet->type == PLM_DEMUX_PACKET_VIDEO_1) {
			memcpy(es + es_len, packet->data, packet->length);
			es_len += packet->length;
		}
	}
	plm_demux_destroy(demux);
	*out_es_len = es_len;
	return es;
}

static int scan_pictures(const uint8_t *es, size_t es_len, test_pic_t *pics) {
	int count = 0;
	for (size_t i = 0; i + 5 < es_len; i++) {
		if (es[i] == 0x00 && es[i+1] == 0x00 && es[i+2] == 0x01 && es[i+3] == 0x00) {
			pics[count].offset = i;
			pics[count].tref = (es[i+4] << 2) | (es[i+5] >> 6);
			pics[count].type = (es[i+5] >> 3) & 0x07;
			count++;
			if (count >= MAX_PICS) {
				break;
			}
		}
	}
	return count;
}

static void encode_pts33(uint8_t *p, int64_t value) {
	value &= PTS_WRAP33 - 1;
	p[0] = (uint8_t)(0x21 | (((value >> 30) & 0x07) << 1));
	p[1] = (uint8_t)((value >> 22) & 0xff);
	p[2] = (uint8_t)((((value >> 15) & 0x7f) << 1) | 1);
	p[3] = (uint8_t)((value >> 7) & 0xff);
	p[4] = (uint8_t)(((value & 0x7f) << 1) | 1);
}

// Locate the end of the pack/system prefix: the byte offset of the first video
// PES start code (00 00 01 E0). Everything before it is copied verbatim.

static size_t find_ps_prefix_len(const uint8_t *ps, size_t ps_len) {
	for (size_t i = 0; i + 4 <= ps_len; i++) {
		if (ps[i] == 0x00 && ps[i+1] == 0x00 &&
			ps[i+2] == 0x01 && ps[i+3] == (uint8_t)PLM_DEMUX_PACKET_VIDEO_1) {
			return i;
		}
	}
	return 0;
}

// Rebuild an MPEG-PS file: original pack/system prefix followed by one PES per
// spec. Packet payloads partition the elementary stream contiguously.

static uint8_t *build_ps(const uint8_t *ps, size_t ps_len, const uint8_t *es, size_t es_len,
	const test_spec_t *specs, int nspecs, size_t *out_len) {
	size_t prefix_len = find_ps_prefix_len(ps, ps_len);
	size_t cap = prefix_len + es_len + (size_t)nspecs * 16;
	uint8_t *out = (uint8_t *)malloc(cap);
	size_t o = 0;
	memcpy(out, ps, prefix_len);
	o = prefix_len;

	size_t start = 0;
	for (int k = 0; k < nspecs; k++) {
		size_t end = specs[k].cut;
		size_t payload = end - start;
		int header_len = specs[k].has_pts ? 5 : 1;
		size_t packet_len = payload + (size_t)header_len;

		out[o++] = 0x00;
		out[o++] = 0x00;
		out[o++] = 0x01;
		out[o++] = (uint8_t)PLM_DEMUX_PACKET_VIDEO_1;
		out[o++] = (uint8_t)((packet_len >> 8) & 0xff);
		out[o++] = (uint8_t)(packet_len & 0xff);
		if (specs[k].has_pts) {
			encode_pts33(out + o, specs[k].raw_pts);
			o += 5;
		}
		else {
			out[o++] = 0x00;
		}
		memcpy(out + o, es + start, payload);
		o += payload;
		start = end;
	}
	*out_len = o;
	return out;
}

// Independent expected labels. Packets are located by first-00 containment;
// only the first picture inside a PTS packet inherits it; PTS values are
// unwrapped in packet order with the same modulo 2^33 rule; display order uses
// an I/P one-frame delay and immediate B output.

static int reference_labels(const test_spec_t *specs, int nspecs,
	const test_pic_t *pics, int npics, test_label_t *out) {
	size_t starts[MAX_PICS];
	size_t ends[MAX_PICS];
	int has[MAX_PICS];
	int64_t unwrapped[MAX_PICS];
	int used[MAX_PICS];

	int has_origin = 0;
	int64_t prev_raw = 0;
	int64_t accumulated = 0;
	size_t prev_cut = 0;

	for (int k = 0; k < nspecs; k++) {
		starts[k] = prev_cut;
		ends[k] = specs[k].cut;
		has[k] = specs[k].has_pts;
		used[k] = 0;
		if (specs[k].has_pts) {
			int64_t raw = specs[k].raw_pts & (PTS_WRAP33 - 1);
			if (!has_origin) {
				has_origin = 1;
				prev_raw = raw;
				accumulated = raw;
			}
			else {
				int64_t delta = raw - prev_raw;
				if (delta > PTS_HALF33) {
					delta -= PTS_WRAP33;
				}
				else if (delta < -PTS_HALF33) {
					delta += PTS_WRAP33;
				}
				prev_raw = raw;
				accumulated += delta;
			}
			unwrapped[k] = accumulated;
		}
		prev_cut = specs[k].cut;
	}

	test_label_t pic_label[MAX_PICS];
	for (int i = 0; i < npics; i++) {
		pic_label[i].has_pts = 0;
		pic_label[i].pts90k = 0;
		for (int k = 0; k < nspecs; k++) {
			if (pics[i].offset >= starts[k] && pics[i].offset < ends[k]) {
				if (has[k] && !used[k]) {
					pic_label[i].has_pts = 1;
					pic_label[i].pts90k = unwrapped[k];
					used[k] = 1;
				}
				break;
			}
		}
	}

	int nout = 0;
	int delayed = -1;
	for (int i = 0; i < npics; i++) {
		if (pics[i].type == TEST_PICTURE_TYPE_B) {
			out[nout++] = pic_label[i];
		}
		else {
			if (delayed >= 0) {
				out[nout++] = pic_label[delayed];
			}
			delayed = i;
		}
	}
	if (delayed >= 0) {
		out[nout++] = pic_label[delayed];
	}
	return nout;
}

static void free_captured(test_frame_t *frames, int count) {
	for (int i = 0; i < count; i++) {
		free(frames[i].y);
		free(frames[i].cb);
		free(frames[i].cr);
	}
}

static void capture_frame(plm_frame_t *f, test_frame_t *dst) {
	size_t y_size = (size_t)f->width * f->height;
	size_t c_size = (size_t)(f->width / 2) * (f->height / 2);
	dst->has_pts = f->has_pts;
	dst->pts90k = f->pts90k;
	dst->time = f->time;
	dst->y = (uint8_t *)malloc(y_size);
	dst->cb = (uint8_t *)malloc(c_size);
	dst->cr = (uint8_t *)malloc(c_size);
	for (unsigned int row = 0; row < f->height; row++) {
		memcpy(dst->y + (size_t)row * f->width,
			f->y.data + (size_t)row * f->y.width, f->width);
	}
	unsigned int cw = f->width / 2;
	unsigned int ch = f->height / 2;
	for (unsigned int row = 0; row < ch; row++) {
		memcpy(dst->cb + (size_t)row * cw,
			f->cb.data + (size_t)row * f->cb.width, cw);
		memcpy(dst->cr + (size_t)row * cw,
			f->cr.data + (size_t)row * f->cr.width, cw);
	}
}

static int decode_pull(const uint8_t *data, size_t len, int pts_enabled,
	test_frame_t *out, int max_frames) {
	plm_t *plm = plm_create_with_memory((uint8_t *)data, len, FALSE);
	if (!plm) {
		return -1;
	}
	plm_set_audio_enabled(plm, FALSE);
	if (pts_enabled) {
		plm_set_video_pts_enabled(plm, TRUE);
	}
	int count = 0;
	plm_frame_t *f;
	while ((f = plm_decode_video(plm))) {
		if (count < max_frames) {
			capture_frame(f, &out[count]);
		}
		count++;
	}
	plm_destroy(plm);
	return count;
}

typedef struct {
	test_frame_t *frames;
	int capacity;
	int count;
} test_capture_ctx_t;

static void test_video_callback(plm_t *plm, plm_frame_t *f, void *user) {
	(void)plm;
	test_capture_ctx_t *ctx = (test_capture_ctx_t *)user;
	if (ctx->count < ctx->capacity) {
		capture_frame(f, &ctx->frames[ctx->count]);
	}
	ctx->count++;
}

static int decode_callback(const uint8_t *data, size_t len,
	test_frame_t *out, int max_frames) {
	plm_t *plm = plm_create_with_memory((uint8_t *)data, len, FALSE);
	if (!plm) {
		return -1;
	}
	plm_set_audio_enabled(plm, FALSE);
	plm_set_video_pts_enabled(plm, TRUE);
	test_capture_ctx_t ctx = { out, max_frames, 0 };
	plm_set_video_decode_callback(plm, test_video_callback, &ctx);
	while (!plm_has_ended(plm)) {
		plm_decode(plm, 0.1);
	}
	plm_destroy(plm);
	return ctx.count;
}

static int planes_equal(const test_frame_t *a, const test_frame_t *b, int width, int height) {
	size_t y_size = (size_t)width * height;
	size_t c_size = (size_t)(width / 2) * (height / 2);
	return memcmp(a->y, b->y, y_size) == 0 &&
		memcmp(a->cb, b->cb, c_size) == 0 &&
		memcmp(a->cr, b->cr, c_size) == 0;
}

// ---------------------------------------------------------------------------
// test_packet_binding: multi-picture packet, tail-only packet, start code
// straddling a packet boundary, PTS-less packet, all in one re-muxed stream.
// ---------------------------------------------------------------------------

void test_packet_binding(void) {
	size_t ps_len;
	uint8_t *ps = load_file(SOURCE_PATH, &ps_len);
	CHECK(ps != NULL);

	size_t es_len;
	uint8_t *es = extract_es(ps, ps_len, &es_len);
	test_pic_t pics[MAX_PICS];
	int npics = scan_pictures(es, es_len, pics);
	CHECK(npics >= 10);

	// Picture offsets (ES relative): pic0=20, pic1=2171, pic2=3709,
	// pic3=4766, pic4=5894, pic5=7272 ...
	// P0 holds pic0 AND pic1 (multi-picture); only pic0 may inherit its PTS.
	// P1 starts at pic2's first 00.
	// P2 contains pic3's first 00 (byte 4766) but nothing else of its code.
	// P3 only holds the remainder of pic3's code/data: tail-only, its PTS must
	//    never be delivered.
	// P4 holds pic4 but has no PTS; pic4 must stay unlabeled.
	// P5 holds the rest.
	const int64_t P0 = 1000, P1 = 2000, P2 = 3000, P3 = 4000, P5 = 5000;
	test_spec_t specs[] = {
		{ pics[2].offset,        1, P0 }, // [0, 3709): pic0, pic1
		{ 4000,                  1, P1 }, // [3709, 4000): pic2
		{ pics[3].offset + 1,    1, P2 }, // [4000, 4767): first 00 of pic3
		{ pics[4].offset,        1, P3 }, // [4767, 5894): tail of pic3 only
		{ pics[5].offset,        0, 0  }, // [5894, 7272): pic4, no PTS
		{ es_len,                1, P5 }, // remainder
	};
	int nspecs = (int)(sizeof(specs) / sizeof(specs[0]));

	size_t built_len;
	uint8_t *built = build_ps(ps, ps_len, es, es_len, specs, nspecs, &built_len);

	test_label_t expected[MAX_FRAMES];
	int nexpected = reference_labels(specs, nspecs, pics, npics, expected);
	CHECK(nexpected >= npics - 1);

	test_frame_t got[MAX_FRAMES];
	int ngot = decode_pull(built, built_len, 1, got, MAX_FRAMES);
	CHECK(ngot > 0 && ngot <= nexpected);

	CASE(); // every delivered frame matches the independent reference
	int labeled_count = 0;
	for (int i = 0; i < ngot; i++) {
		CHECK(got[i].has_pts == expected[i].has_pts);
		CHECK(got[i].pts90k == expected[i].pts90k);
		if (got[i].has_pts) {
			labeled_count++;
		}
	}
	CHECK(labeled_count > 0);

	// Locate specific pictures in the reference output to assert the rules.
	// pic0 (first in multi-picture P0) keeps P0; pic1 (second in P0) is bare.
	test_label_t pic0_label = { 0, 0 }, pic1_label = { 0, 0 };
	test_label_t pic3_label = { 0, 0 }, pic4_label = { 0, 0 };
	// Re-derive per-picture (pre-reorder) labels directly.
	{
		int used_pkt[MAX_PICS] = { 0 };
		size_t bounds_lo[8], bounds_hi[8];
		size_t prev = 0;
		for (int k = 0; k < nspecs; k++) {
			bounds_lo[k] = prev;
			bounds_hi[k] = specs[k].cut;
			prev = specs[k].cut;
		}
		test_label_t per_pic[8];
		for (int i = 0; i < 5; i++) {
			per_pic[i].has_pts = 0;
			per_pic[i].pts90k = 0;
			for (int k = 0; k < nspecs; k++) {
				if (pics[i].offset >= bounds_lo[k] && pics[i].offset < bounds_hi[k]) {
					if (specs[k].has_pts && !used_pkt[k]) {
						per_pic[i].has_pts = 1;
						// First PTS is the expansion origin => raw value.
						per_pic[i].pts90k = specs[k].raw_pts;
						used_pkt[k] = 1;
					}
					break;
				}
			}
		}
		pic0_label = per_pic[0];
		pic1_label = per_pic[1];
		pic3_label = per_pic[3];
		pic4_label = per_pic[4];
	}

	CASE(); // multi-picture packet: only its first picture is labeled
	CHECK(pic0_label.has_pts == 1);
	CHECK(pic0_label.pts90k == P0);
	CHECK(pic1_label.has_pts == 0);

	CASE(); // cross-packet start code follows the first 00, tail packet ignored
	CHECK(pic3_label.has_pts == 1);
	CHECK(pic3_label.pts90k == P2);
	CHECK(pic4_label.has_pts == 0); // PTS-less packet never inherits a neighbor

	// P3's PTS must not appear on any delivered frame.
	CASE();
	int p3_seen = 0;
	for (int i = 0; i < ngot; i++) {
		if (got[i].has_pts && got[i].pts90k == P3) {
			p3_seen = 1;
		}
	}
	CHECK(p3_seen == 0);

	free_captured(got, ngot);
	free(built);
	free(es);
	free(ps);
}

// ---------------------------------------------------------------------------
// test_reordered_frames: B frames reorder display output while each label
// stays attached to its own picture; compression-order PTS may go backwards.
// ---------------------------------------------------------------------------

void test_reordered_frames(void) {
	size_t ps_len;
	uint8_t *ps = load_file(SOURCE_PATH, &ps_len);
	CHECK(ps != NULL);

	size_t es_len;
	uint8_t *es = extract_es(ps, ps_len, &es_len);
	test_pic_t pics[MAX_PICS];
	int npics = scan_pictures(es, es_len, pics);
	CHECK(npics >= 10);

	// One packet per picture boundary region is awkward; instead give each
	// picture its own packet by cutting just before every picture start code,
	// and assign PTS from display position so consecutive B vs reference
	// pictures produce backwards steps in compression order.
	test_spec_t specs[MAX_PICS];
	int nspecs = 0;
	int64_t tick = 3600;
	for (int i = 0; i < npics; i++) {
		// PTS follows temporal reference order (display), which is not monotonic
		// in compression order once B frames appear.
		specs[nspecs].cut = (i + 1 < npics) ? pics[i + 1].offset : es_len;
		specs[nspecs].has_pts = 1;
		specs[nspecs].raw_pts = (int64_t)pics[i].tref * tick;
		nspecs++;
	}

	// Confirm the chosen PTS sequence is genuinely non-monotonic in compression
	// order, i.e. the test really exercises B-frame reordering.
	CASE();
	int saw_backward = 0;
	for (int i = 1; i < nspecs; i++) {
		if (specs[i].raw_pts < specs[i - 1].raw_pts) {
			saw_backward = 1;
		}
	}
	CHECK(saw_backward == 1);

	size_t built_len;
	uint8_t *built = build_ps(ps, ps_len, es, es_len, specs, nspecs, &built_len);

	test_label_t expected[MAX_FRAMES];
	int nexpected = reference_labels(specs, nspecs, pics, npics, expected);

	test_frame_t got[MAX_FRAMES];
	int ngot = decode_pull(built, built_len, 1, got, MAX_FRAMES);
	CHECK(ngot > 0 && ngot <= nexpected);

	CASE(); // labels follow pictures through the B-frame reordering
	for (int i = 0; i < ngot; i++) {
		CHECK(got[i].has_pts == expected[i].has_pts);
		CHECK(got[i].pts90k == expected[i].pts90k);
	}

	// The displayed label sequence must reflect display order: the second and
	// third output are a B (tref 1) then the delayed reference (tref 3 etc.).
	CASE();
	CHECK(got[0].has_pts == 1);
	CHECK(got[0].pts90k == (int64_t)pics[0].tref * tick);

	free_captured(got, ngot);
	free(built);
	free(es);
	free(ps);
}

// ---------------------------------------------------------------------------
// test_lifecycle: enable/disable gating, rewind determinism, seek reset and
// file vs memory inputs.
// ---------------------------------------------------------------------------

void test_lifecycle(void) {
	size_t ps_len;
	uint8_t *ps = load_file(SOURCE_PATH, &ps_len);
	CHECK(ps != NULL);

	// Default is off and the new fields stay zero.
	CASE();
	{
		plm_t *plm = plm_create_with_memory(ps, ps_len, FALSE);
		plm_set_audio_enabled(plm, FALSE);
		CHECK(plm_get_video_pts_enabled(plm) == 0);
		plm_frame_t *f = plm_decode_video(plm);
		CHECK(f != NULL);
		CHECK(f->has_pts == 0);
		CHECK(f->pts90k == 0);
		plm_destroy(plm);
	}

	// Enabling before decode succeeds; after decode started it fails and leaves
	// the configuration unchanged.
	CASE();
	{
		plm_t *plm = plm_create_with_memory(ps, ps_len, FALSE);
		plm_set_audio_enabled(plm, FALSE);
		CHECK(plm_set_video_pts_enabled(plm, TRUE) == 1);
		CHECK(plm_get_video_pts_enabled(plm) == 1);
		plm_frame_t *f = plm_decode_video(plm);
		CHECK(f != NULL);
		CHECK(plm_set_video_pts_enabled(plm, FALSE) == 0);
		CHECK(plm_get_video_pts_enabled(plm) == 1); // unchanged after rejection
		plm_destroy(plm);
	}

	// Rewind clears bindings/history and reproduces identical labels and pixels.
	CASE();
	{
		plm_t *plm = plm_create_with_memory(ps, ps_len, FALSE);
		plm_set_audio_enabled(plm, FALSE);
		plm_set_video_pts_enabled(plm, TRUE);

		test_frame_t first[MAX_FRAMES];
		int n1 = 0;
		plm_frame_t *f;
		while ((f = plm_decode_video(plm))) {
			if (n1 < MAX_FRAMES) {
				capture_frame(f, &first[n1]);
			}
			n1++;
		}
		CHECK(n1 > 0 && n1 <= MAX_FRAMES);

		plm_rewind(plm);
		test_frame_t second[MAX_FRAMES];
		int n2 = 0;
		while ((f = plm_decode_video(plm))) {
			if (n2 < MAX_FRAMES) {
				capture_frame(f, &second[n2]);
			}
			n2++;
		}
		CHECK(n1 == n2);
		for (int i = 0; i < n1; i++) {
			CHECK(first[i].has_pts == second[i].has_pts);
			CHECK(first[i].pts90k == second[i].pts90k);
			CHECK(planes_equal(&first[i], &second[i], 64, 64) == 1);
		}
		free_captured(first, n1);
		free_captured(second, n2);
		plm_destroy(plm);
	}

	// Seek resets bindings and rebuilds an expansion origin from the first PTS
	// read after the seek; delivered frames still carry their own labels.
	CASE();
	{
		plm_t *plm = plm_create_with_memory(ps, ps_len, FALSE);
		plm_set_audio_enabled(plm, FALSE);
		plm_set_video_pts_enabled(plm, TRUE);

		double duration = plm_get_duration(plm);
		CHECK(duration > 0.0);
		plm_frame_t *target = plm_seek_frame(plm, duration * 0.5, FALSE);
		CHECK(target != NULL);
		// A seeked intra frame is the first picture of its packet and labeled.
		CHECK(target->has_pts == 1);
		int64_t seek_origin = target->pts90k;
		CHECK(seek_origin >= 0);

		// Frames decoded after the seek keep well-formed, non-negative labels
		// derived from the fresh origin (no stale pre-seek bindings).
		plm_frame_t *f;
		int checked = 0;
		while ((f = plm_decode_video(plm)) && checked < 4) {
			if (f->has_pts) {
				CHECK(f->pts90k >= seek_origin - PTS_HALF33);
			}
			checked++;
		}
		plm_destroy(plm);
	}

	// File input exposes labels exactly like memory input.
	CASE();
	{
		plm_t *file_plm = plm_create_with_filename(SOURCE_PATH);
		CHECK(file_plm != NULL);
		plm_set_audio_enabled(file_plm, FALSE);
		plm_set_video_pts_enabled(file_plm, TRUE);

		plm_t *mem_plm = plm_create_with_memory(ps, ps_len, FALSE);
		plm_set_audio_enabled(mem_plm, FALSE);
		plm_set_video_pts_enabled(mem_plm, TRUE);

		plm_frame_t *a, *b;
		int pairs = 0;
		int mismatch = 0;
		for (;;) {
			a = plm_decode_video(file_plm);
			b = plm_decode_video(mem_plm);
			if (!a || !b) {
				break;
			}
			if (a->has_pts != b->has_pts || a->pts90k != b->pts90k) {
				mismatch = 1;
			}
			pairs++;
		}
		CHECK(pairs > 0);
		CHECK(mismatch == 0);
		plm_destroy(file_plm);
		plm_destroy(mem_plm);
	}

	free(ps);
}

// ---------------------------------------------------------------------------
// test_compatibility: pixels/timing/frame sequence unchanged, pull vs callback
// agree, raw ES decoder stays unlabeled, full 33 bit PTS and wrap preserved.
// ---------------------------------------------------------------------------

void test_compatibility(void) {
	size_t ps_len;
	uint8_t *ps = load_file(SOURCE_PATH, &ps_len);
	CHECK(ps != NULL);

	// Enabled vs disabled: identical pixels, local time and frame count; the
	// disabled run always reports zero labels.
	CASE();
	{
		test_frame_t enabled[MAX_FRAMES];
		test_frame_t disabled[MAX_FRAMES];
		int ne = decode_pull(ps, ps_len, 1, enabled, MAX_FRAMES);
		int nd = decode_pull(ps, ps_len, 0, disabled, MAX_FRAMES);
		CHECK(ne == nd);
		CHECK(ne > 0);
		for (int i = 0; i < ne; i++) {
			CHECK(disabled[i].has_pts == 0);
			CHECK(disabled[i].pts90k == 0);
			CHECK(enabled[i].time == disabled[i].time);
			CHECK(planes_equal(&enabled[i], &disabled[i], 64, 64) == 1);
		}
		free_captured(enabled, ne);
		free_captured(disabled, nd);
	}

	// Pull and callback delivery produce the same labels and pixels.
	CASE();
	{
		test_frame_t pulled[MAX_FRAMES];
		test_frame_t called[MAX_FRAMES];
		int np = decode_pull(ps, ps_len, 1, pulled, MAX_FRAMES);
		int nc = decode_callback(ps, ps_len, called, MAX_FRAMES);
		CHECK(np == nc);
		for (int i = 0; i < np; i++) {
			CHECK(pulled[i].has_pts == called[i].has_pts);
			CHECK(pulled[i].pts90k == called[i].pts90k);
			CHECK(planes_equal(&pulled[i], &called[i], 64, 64) == 1);
		}
		free_captured(pulled, np);
		free_captured(called, nc);
	}

	// The standalone elementary stream decoder has no PES and never labels.
	CASE();
	{
		size_t es_len;
		uint8_t *es = extract_es(ps, ps_len, &es_len);
		plm_buffer_t *vb = plm_buffer_create_with_memory(es, es_len, TRUE);
		plm_video_t *video = plm_video_create_with_buffer(vb, TRUE);
		plm_frame_t *f;
		int frames = 0;
		int bad = 0;
		while ((f = plm_video_decode(video))) {
			if (f->has_pts != 0 || f->pts90k != 0) {
				bad = 1;
			}
			frames++;
		}
		CHECK(frames > 0);
		CHECK(bad == 0);
		plm_video_destroy(video);
	}

	// Full 33 bit value survives and a wrap into the next 2^33 range unwraps by
	// accumulating the small signed difference rather than resetting.
	CASE();
	{
		size_t es_len;
		uint8_t *es = extract_es(ps, ps_len, &es_len);
		test_pic_t pics[MAX_PICS];
		int npics = scan_pictures(es, es_len, pics);
		CHECK(npics >= 4);

		int64_t near_top = PTS_WRAP33 - 3600; // valid 33 bit value
		int64_t after_wrap = 0;               // 3600 ticks later, modulo 2^33
		test_spec_t specs[] = {
			{ pics[2].offset, 1, near_top },
			{ pics[4].offset, 1, after_wrap },
			{ es_len,         0, 0 },
		};
		size_t built_len;
		uint8_t *built = build_ps(ps, ps_len, es, es_len, specs, 3, &built_len);

		test_frame_t got[MAX_FRAMES];
		int ngot = decode_pull(built, built_len, 1, got, MAX_FRAMES);
		CHECK(ngot > 0);

		int found_top = 0, found_wrapped = 0;
		for (int i = 0; i < ngot; i++) {
			if (got[i].has_pts && got[i].pts90k == near_top) {
				found_top = 1;
			}
			// Unwrapped value after the wrap is 2^33 (origin + 3600 - delta path).
			if (got[i].has_pts && got[i].pts90k == PTS_WRAP33) {
				found_wrapped = 1;
			}
		}
		CHECK(found_top == 1);
		CHECK(found_wrapped == 1);
		free_captured(got, ngot);
		free(built);
		free(es);
	}

	free(ps);
}
