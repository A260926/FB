#include "../task_support/case_harness.h"
#include "pl_mpeg.h"
#include <string.h>
#include <stdint.h>

/*
 * Regression tests for the PES PTS pass-through.
 *
 * The same MPEG-1 video payload that ships in task_support/source.mpg is
 * de-muxed into its elementary stream (ES) and then re-packaged into brand
 * new MPEG-PS files in memory. The compressed bytes are never altered, so the
 * decoder produces exactly the same pictures; only the PES packet boundaries
 * and PTS values change, which is exactly what the binding logic has to track.
 */

#define PTS_MOD (1LL << 33)
#define PTS_HALF (1LL << 32)
#define SOURCE_PATH "task_support/source.mpg"
#define PS_PREFIX_LEN 27 /* pack header + system header preceding the first PES */

typedef struct {
	size_t offset;   /* offset of the picture start code's first 00 byte in the ES */
	int type;        /* picture coding type: 1=I, 2=P, 3=B */
} test_pic_t;

typedef struct {
	size_t start;    /* ES offset where this PES payload starts */
	size_t length;   /* payload length in bytes */
	int64_t pts90k;  /* raw 33 bit PTS in 90kHz ticks, or -1 for no PTS */
} test_segment_t;

typedef struct {
	int has_pts;
	int64_t pts90k;
	double time;
	uint32_t hash;
} test_frame_rec_t;

static uint8_t *test_read_file(const char *path, size_t *out_length) {
	FILE *fh = fopen(path, "rb");
	if (!fh) {
		return NULL;
	}
	fseek(fh, 0, SEEK_END);
	long size = ftell(fh);
	fseek(fh, 0, SEEK_SET);
	uint8_t *data = (uint8_t *)malloc((size_t)size);
	size_t read = fread(data, 1, (size_t)size, fh);
	fclose(fh);
	*out_length = read;
	return data;
}

static uint32_t test_hash_frame(plm_frame_t *f) {
	uint32_t h = 2166136261u;
	const plm_plane_t *planes[3] = { &f->y, &f->cr, &f->cb };
	for (int p = 0; p < 3; p++) {
		size_t n = (size_t)planes[p]->width * planes[p]->height;
		for (size_t i = 0; i < n; i++) {
			h = (h ^ planes[p]->data[i]) * 16777619u;
		}
	}
	return h;
}

/* Pull every decoded frame out of a freshly created in-memory plm instance. */

static int test_decode_all(const uint8_t *mpg, size_t mpg_length, int pts_enabled,
	test_frame_rec_t *out, size_t max_frames) {
	plm_t *plm = plm_create_with_memory((uint8_t *)mpg, mpg_length, 0);
	if (!plm) {
		return -1;
	}
	plm_set_audio_enabled(plm, 0);
	if (pts_enabled) {
		plm_set_video_pts_enabled(plm, 1);
	}

	size_t count = 0;
	plm_frame_t *frame;
	while ((frame = plm_decode_video(plm))) {
		if (count >= max_frames) {
			plm_destroy(plm);
			return -1;
		}
		out[count].has_pts = frame->has_pts;
		out[count].pts90k = frame->pts90k;
		out[count].time = frame->time;
		out[count].hash = test_hash_frame(frame);
		count++;
	}
	plm_destroy(plm);
	return (int)count;
}

/* Extract the concatenated video ES, the picture start code offsets and the
 * 27 byte program-stream prefix from the pristine fixture. */

static int test_extract_es(const uint8_t *file, size_t file_length,
	uint8_t **out_es, size_t *out_es_length,
	test_pic_t *pics, size_t max_pics, size_t *out_num_pics) {
	plm_buffer_t *buffer = plm_buffer_create_with_memory((uint8_t *)file, file_length, 0);
	plm_demux_t *demux = plm_demux_create(buffer, 1);

	size_t capacity = file_length;
	uint8_t *es = (uint8_t *)malloc(capacity);
	size_t es_length = 0;

	plm_packet_t *packet;
	while ((packet = plm_demux_decode(demux))) {
		if (packet->type != PLM_DEMUX_PACKET_VIDEO_1) {
			continue;
		}
		if (es_length + packet->length > capacity) {
			capacity *= 2;
			es = (uint8_t *)realloc(es, capacity);
		}
		memcpy(es + es_length, packet->data, packet->length);
		es_length += packet->length;
	}
	plm_demux_destroy(demux);

	size_t num_pics = 0;
	for (size_t i = 0; i + 4 <= es_length && num_pics < max_pics; i++) {
		if (es[i] == 0x00 && es[i + 1] == 0x00 && es[i + 2] == 0x01 && es[i + 3] == 0x00) {
			int type = (es[i + 5] >> 3) & 0x07;
			pics[num_pics].offset = i;
			pics[num_pics].type = type;
			num_pics++;
		}
	}

	*out_es = es;
	*out_es_length = es_length;
	*out_num_pics = num_pics;
	return 0;
}

/* Encode a 33 bit PTS into the five MPEG-1 PES header bytes. */

static void test_encode_pts(uint8_t *b, uint64_t pts) {
	b[0] = 0x20 | (uint8_t)((pts >> 29) & 0x0e) | 0x01;
	b[1] = (uint8_t)((pts >> 22) & 0xff);
	b[2] = (uint8_t)((pts >> 14) & 0xfe) | 0x01;
	b[3] = (uint8_t)((pts >> 7) & 0xff);
	b[4] = (uint8_t)((pts << 1) & 0xfe) | 0x01;
}

/* Re-package ES segments into a program stream in memory. */

static uint8_t *test_build_mpg(const uint8_t *prefix, const uint8_t *es,
	const test_segment_t *segments, size_t num_segments, size_t *out_length) {
	size_t capacity = PS_PREFIX_LEN + num_segments * 16;
	for (size_t i = 0; i < num_segments; i++) {
		capacity += segments[i].length;
	}
	uint8_t *out = (uint8_t *)malloc(capacity);
	size_t w = 0;
	memcpy(out, prefix, PS_PREFIX_LEN);
	w = PS_PREFIX_LEN;

	for (size_t i = 0; i < num_segments; i++) {
		int has_pts = segments[i].pts90k >= 0;
		size_t header = has_pts ? 5 : 1;
		size_t packet_length = header + segments[i].length;

		out[w++] = 0x00;
		out[w++] = 0x00;
		out[w++] = 0x01;
		out[w++] = 0xe0;
		out[w++] = (uint8_t)(packet_length >> 8);
		out[w++] = (uint8_t)(packet_length & 0xff);
		if (has_pts) {
			test_encode_pts(out + w, (uint64_t)segments[i].pts90k);
			w += 5;
		}
		else {
			out[w++] = 0x0f;
		}
		memcpy(out + w, es + segments[i].start, segments[i].length);
		w += segments[i].length;
	}

	*out_length = w;
	return out;
}

/* Independent reference: which pictures have their start code's first 00 byte
 * inside a segment, and the raw PTS (if any) of the owning segment. */

static int test_owner_pts(const test_segment_t *segments, size_t num_segments,
	size_t picture_offset, int64_t *out_pts) {
	for (size_t i = 0; i < num_segments; i++) {
		size_t end = segments[i].start + segments[i].length;
		if (picture_offset >= segments[i].start && picture_offset < end) {
			if (segments[i].pts90k < 0) {
				return 0;
			}
			*out_pts = segments[i].pts90k;
			return 1;
		}
	}
	return 0;
}

/* Reference 33 bit unwrap, mirroring the documented accumulation rule. */

static int64_t test_unwrap(int64_t *previous_raw, int *have_base,
	int64_t *base, int64_t raw_in) {
	int64_t raw = raw_in & (PTS_MOD - 1); /* retain all 33 bits */
	if (!*have_base) {
		*have_base = 1;
		*previous_raw = raw;
		*base = raw;
		return *base;
	}
	int64_t delta = raw - *previous_raw;
	if (delta > PTS_HALF) {
		delta -= PTS_MOD;
	}
	else if (delta < -PTS_HALF) {
		delta += PTS_MOD;
	}
	*base += delta;
	*previous_raw = raw;
	return *base;
}

static int test_int64_in_list(const int64_t *list, size_t n, int64_t value) {
	for (size_t i = 0; i < n; i++) {
		if (list[i] == value) {
			return 1;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------- */

void test_packet_binding(void) {
	size_t file_length = 0;
	uint8_t *file = test_read_file(SOURCE_PATH, &file_length);
	CASE();
	CHECK(file != NULL);

	uint8_t *es = NULL;
	size_t es_length = 0;
	test_pic_t pics[64];
	size_t num_pics = 0;
	CASE();
	CHECK(test_extract_es(file, file_length, &es, &es_length, pics, 64, &num_pics) == 0);
	CHECK(num_pics >= 20);

	/*
	 * Build a partition of the ES that exercises every binding rule:
	 *  - a cut placed *inside* a picture start code (between the two leading
	 *    00 bytes), so the code spans two PES packets but is owned by the one
	 *    holding the first 00;
	 *  - a segment containing only the tail of a picture (no start code);
	 *  - a segment holding several complete pictures;
	 *  - a segment without a PTS in the middle of the stream.
	 *
	 * Cuts are derived from real picture offsets so they always line up with
	 * the actual start codes.
	 */
	size_t split_pic = 2;
	size_t multi_pic = 6;
	size_t tail_pic = 9;

	size_t cuts[16];
	size_t num_cuts = 0;
	cuts[num_cuts++] = 0;
	cuts[num_cuts++] = pics[1].offset;                       /* multi-picture region starts */
	cuts[num_cuts++] = pics[split_pic].offset + 1;           /* inside pic2's 00 00 01 00 */
	cuts[num_cuts++] = pics[4].offset;
	cuts[num_cuts++] = pics[multi_pic].offset;               /* segment with several pics */
	cuts[num_cuts++] = pics[tail_pic].offset + 8;            /* tail-only segment begins mid picture */
	cuts[num_cuts++] = pics[tail_pic + 1].offset;
	cuts[num_cuts++] = pics[12].offset;
	cuts[num_cuts++] = (pics[23].offset > 64) ? pics[23].offset - 16 : pics[23].offset;
	cuts[num_cuts++] = es_length;
	/* keep cuts sorted and unique */
	for (size_t i = 1; i < num_cuts; i++) {
		if (cuts[i] <= cuts[i - 1]) {
			cuts[i] = cuts[i - 1] + 1;
		}
	}
	CHECK(cuts[num_cuts - 1] == es_length);

	test_segment_t segments[16];
	size_t num_segments = num_cuts - 1;
	/* Distinct, easily recognizable PTS values; leave two segments without. */
	int64_t pts_clock = 100000;
	for (size_t i = 0; i < num_segments; i++) {
		segments[i].start = cuts[i];
		segments[i].length = cuts[i + 1] - cuts[i];
		int no_pts = (i == 3 || i == 5);
		segments[i].pts90k = no_pts ? -1 : pts_clock;
		pts_clock += 3600;
	}

	size_t mpg_length = 0;
	uint8_t *mpg = test_build_mpg(file, es, segments, num_segments, &mpg_length);
	CASE();
	CHECK(mpg != NULL);

	test_frame_rec_t frames[64];
	int frame_count = test_decode_all(mpg, mpg_length, 1, frames, 64);
	CASE();
	CHECK(frame_count >= 20);

	/* Expected set of unwrapped tags: one per segment that (a) carries a PTS
	 * and (b) contains the first 00 of at least one picture. No wrapping
	 * happens here, so unwrapped == raw. */
	int64_t expected[64];
	size_t num_expected = 0;
	for (size_t s = 0; s < num_segments; s++) {
		if (segments[s].pts90k < 0) {
			continue;
		}
		int owns_picture = 0;
		for (size_t p = 0; p < num_pics; p++) {
			size_t end = segments[s].start + segments[s].length;
			if (pics[p].offset >= segments[s].start && pics[p].offset < end) {
				owns_picture = 1;
				break;
			}
		}
		if (owns_picture) {
			expected[num_expected++] = segments[s].pts90k;
		}
	}

	size_t tagged = 0;
	for (int i = 0; i < frame_count; i++) {
		if (frames[i].has_pts) {
			tagged++;
			CHECK(test_int64_in_list(expected, num_expected, frames[i].pts90k));
			/* a tag must never be synthesized on an untagged frame */
			CHECK(frames[i].pts90k != 0);
		}
		else {
			CHECK(frames[i].pts90k == 0);
		}
	}
	CASE();
	CHECK(tagged == num_expected);

	/* The split start code: the segment holding the first 00 owns the picture
	 * and contributes exactly one tag; the following segment must not have
	 * inherited it. */
	int64_t split_owner_pts = 0;
	CASE();
	CHECK(test_owner_pts(segments, num_segments, pics[split_pic].offset, &split_owner_pts));
	CHECK(test_int64_in_list(expected, num_expected, split_owner_pts));

	/* A segment that contains no picture start code never yields a tag and its
	 * PTS does not flow into the next packet. */
	for (size_t s = 0; s < num_segments; s++) {
		int has_picture = 0;
		for (size_t p = 0; p < num_pics; p++) {
			size_t end = segments[s].start + segments[s].length;
			if (pics[p].offset >= segments[s].start && pics[p].offset < end) {
				has_picture = 1;
				break;
			}
		}
		if (!has_picture) {
			CASE();
			CHECK(!test_int64_in_list(expected, num_expected, segments[s].pts90k));
		}
	}

	/* Multi-picture segment: a single PTS even though it holds several pics. */
	for (size_t s = 0; s < num_segments; s++) {
		size_t pics_in_segment = 0;
		size_t end = segments[s].start + segments[s].length;
		for (size_t p = 0; p < num_pics; p++) {
			if (pics[p].offset >= segments[s].start && pics[p].offset < end) {
				pics_in_segment++;
			}
		}
		if (pics_in_segment >= 2 && segments[s].pts90k >= 0) {
			size_t occurrences = 0;
			for (int i = 0; i < frame_count; i++) {
				if (frames[i].has_pts && frames[i].pts90k == segments[s].pts90k) {
					occurrences++;
				}
			}
			CASE();
			CHECK(occurrences == 1);
		}
	}

	/* 33 bit wraparound plus a B-frame style backward step. */
	test_segment_t wrap_segments[8];
	size_t wrap_cuts[9];
	wrap_cuts[0] = 0;
	for (size_t i = 1; i < 8; i++) {
		wrap_cuts[i] = pics[i * 2].offset;
	}
	wrap_cuts[8] = es_length;
	int64_t raw_pts[8];
	uint64_t near_top = (uint64_t)PTS_MOD - 100000;
	for (size_t i = 0; i < 8; i++) {
		wrap_segments[i].start = wrap_cuts[i];
		wrap_segments[i].length = wrap_cuts[i + 1] - wrap_cuts[i];
		if (i == 0) {
			raw_pts[i] = (int64_t)near_top;
		}
		else if (i == 2) {
			raw_pts[i] = raw_pts[i - 1] - 3600; /* backward PTS, no wrap */
		}
		else if (i == 4) {
			raw_pts[i] = 50000;                 /* wraps forward over 2^33 */
		}
		else if (i == 5) {
			raw_pts[i] = 90000;
		}
		else {
			raw_pts[i] = raw_pts[i - 1] + 3600;
		}
		wrap_segments[i].pts90k = raw_pts[i] & (PTS_MOD - 1);
	}

	size_t wrap_mpg_length = 0;
	uint8_t *wrap_mpg = test_build_mpg(file, es, wrap_segments, 8, &wrap_mpg_length);
	test_frame_rec_t wrap_frames[64];
	int wrap_count = test_decode_all(wrap_mpg, wrap_mpg_length, 1, wrap_frames, 64);
	CASE();
	CHECK(wrap_count >= 20);

	int have_base = 0;
	int64_t previous_raw = 0, base = 0;
	int64_t wrap_expected[64];
	size_t num_wrap_expected = 0;
	for (size_t s = 0; s < 8; s++) {
		size_t end = wrap_segments[s].start + wrap_segments[s].length;
		int owns_picture = 0;
		for (size_t p = 0; p < num_pics; p++) {
			if (pics[p].offset >= wrap_segments[s].start && pics[p].offset < end) {
				owns_picture = 1;
				break;
			}
		}
		int64_t unwrapped = test_unwrap(&previous_raw, &have_base, &base,
			wrap_segments[s].pts90k);
		if (owns_picture) {
			wrap_expected[num_wrap_expected++] = unwrapped;
		}
	}
	size_t wrap_tagged = 0;
	for (int i = 0; i < wrap_count; i++) {
		if (wrap_frames[i].has_pts) {
			wrap_tagged++;
			CHECK(test_int64_in_list(wrap_expected, num_wrap_expected, wrap_frames[i].pts90k));
		}
	}
	CASE();
	CHECK(wrap_tagged == num_wrap_expected);
	/* the unwrapped continuation must exceed the 33 bit modulus */
	CHECK(base > PTS_MOD);

	free(wrap_mpg);
	free(mpg);
	free(es);
	free(file);
}

/* ------------------------------------------------------------------------- */

void test_reordered_frames(void) {
	size_t file_length = 0;
	uint8_t *file = test_read_file(SOURCE_PATH, &file_length);
	CASE();
	CHECK(file != NULL);

	/* Golden display-order PTS table of the fixed fixture: -1 = no PES tag.
	 * B-frame PTS values deliberately go backwards in decode order and the
	 * delayed I/P references carry their tags onto later display frames. */
	const int64_t golden[24] = {
		48600, -1, 55800, 59400, 63000, -1, -1, 73800, -1, 81000,
		84600, -1, 91800, -1, 99000, 102600, 106200, 109800, -1, 117000,
		120600, -1, 127800, -1
	};

	test_frame_rec_t frames[64];
	int count = test_decode_all(file, file_length, 1, frames, 64);
	CASE();
	CHECK(count == 24);

	int tagged = 0;
	for (int i = 0; i < count; i++) {
		CASE();
		if (golden[i] < 0) {
			CHECK(frames[i].has_pts == 0);
			CHECK(frames[i].pts90k == 0);
		}
		else {
			CHECK(frames[i].has_pts == 1);
			CHECK(frames[i].pts90k == golden[i]);
			tagged++;
		}
	}
	CHECK(tagged == 15);

	/* Explicit B-frame regression: display frame 2 precedes frame 3 in PTS. */
	CASE();
	CHECK(frames[2].has_pts && frames[3].has_pts);
	CHECK(frames[2].pts90k < frames[3].pts90k);

	/* The first displayed frame is the delayed intra picture and still
	 * carries the first PES PTS rather than whatever packet was read last. */
	CASE();
	CHECK(frames[0].has_pts == 1);
	CHECK(frames[0].pts90k == 48600);

	/* Local frame clock is untouched and strictly frame-rate driven. */
	CASE();
	CHECK(frames[0].time == 0.0);
	for (int i = 1; i < count; i++) {
		CHECK(frames[i].time > frames[i - 1].time);
	}

	free(file);
}

/* ------------------------------------------------------------------------- */

static int test_lifecycle_called = 0;

void test_lifecycle(void) {
	size_t file_length = 0;
	uint8_t *file = test_read_file(SOURCE_PATH, &file_length);
	CASE();
	CHECK(file != NULL);

	/* Default is off: fields stay zero. */
	{
		test_frame_rec_t off[64];
		int count = test_decode_all(file, file_length, 0, off, 64);
		CASE();
		CHECK(count == 24);
		for (int i = 0; i < count; i++) {
			CHECK(off[i].has_pts == 0);
			CHECK(off[i].pts90k == 0);
		}
	}

	/* Enable/disable freely before decoding starts; locked afterwards. */
	plm_t *plm = plm_create_with_memory(file, file_length, 0);
	plm_set_audio_enabled(plm, 0);
	CASE();
	CHECK(plm_get_video_pts_enabled(plm) == 0);
	CHECK(plm_set_video_pts_enabled(plm, 1) == 1);
	CHECK(plm_get_video_pts_enabled(plm) == 1);
	CHECK(plm_set_video_pts_enabled(plm, 0) == 1);
	CHECK(plm_set_video_pts_enabled(plm, 1) == 1);

	/* Constructor pre-reads the first packet; enabling right away must still
	 * surface the first picture's PTS. */
	plm_frame_t *frame = plm_decode_video(plm);
	CASE();
	CHECK(frame != NULL);
	CHECK(frame->has_pts == 1);
	CHECK(frame->pts90k == 48600);

	CASE();
	CHECK(plm_set_video_pts_enabled(plm, 0) == 0); /* already started */
	CHECK(plm_get_video_pts_enabled(plm) == 1);    /* configuration unchanged */
	while (plm_decode_video(plm)) {
	}

	/* rewind clears bindings and history; re-reading gives identical tags. */
	plm_rewind(plm);
	test_frame_rec_t rerun[64];
	int rerun_count = 0;
	while ((frame = plm_decode_video(plm))) {
		rerun[rerun_count].has_pts = frame->has_pts;
		rerun[rerun_count].pts90k = frame->pts90k;
		rerun_count++;
	}
	CASE();
	CHECK(rerun_count == 24);
	CHECK(rerun[0].has_pts == 1 && rerun[0].pts90k == 48600);
	int rerun_tagged = 0;
	for (int i = 0; i < rerun_count; i++) {
		if (rerun[i].has_pts) {
			rerun_tagged++;
		}
	}
	CHECK(rerun_tagged == 15);

	/* Repeated rewinds must keep producing bounded, identical results rather
	 * than accumulating packet markers. */
	for (int iteration = 0; iteration < 8; iteration++) {
		plm_rewind(plm);
		int n = 0, tags = 0;
		while ((frame = plm_decode_video(plm))) {
			if (frame->has_pts) {
				tags++;
			}
			n++;
		}
		CASE();
		CHECK(n == 24);
		CHECK(tags == 15);
	}

	/* seek clears old bindings and establishes a fresh local base. */
	int seek_locked = plm_set_video_pts_enabled(plm, 0);
	CHECK(seek_locked == 0);
	frame = plm_seek_frame(plm, 0.5, 0);
	CASE();
	CHECK(frame != NULL);
	CHECK(frame->has_pts == 1);
	CHECK(frame->pts90k > 0);
	int post_tags = 0, post_inconsistent = 0, post_frames = 0;
	while ((frame = plm_decode_video(plm))) {
		post_frames++;
		if (frame->has_pts) {
			if (frame->pts90k <= 0) {
				post_inconsistent = 1;
			}
			post_tags++;
		}
		else if (frame->pts90k != 0) {
			post_inconsistent = 1;
		}
	}
	CASE();
	CHECK(post_frames > 0);
	CHECK(post_inconsistent == 0);
	CHECK(post_tags > 0);

	plm_destroy(plm);
	test_lifecycle_called = 1;
	free(file);
}

/* ------------------------------------------------------------------------- */

typedef struct {
	test_frame_rec_t records[64];
	int count;
} test_callback_collect_t;

static void test_compat_callback(plm_t *self, plm_frame_t *frame, void *user) {
	(void)self;
	test_callback_collect_t *collect = (test_callback_collect_t *)user;
	if ((size_t)collect->count >= 64) {
		return;
	}
	collect->records[collect->count].has_pts = frame->has_pts;
	collect->records[collect->count].pts90k = frame->pts90k;
	collect->records[collect->count].time = frame->time;
	collect->records[collect->count].hash = test_hash_frame(frame);
	collect->count++;
}

void test_compatibility(void) {
	size_t file_length = 0;
	uint8_t *file = test_read_file(SOURCE_PATH, &file_length);
	CASE();
	CHECK(file != NULL);

	test_frame_rec_t off[64], on[64];
	int off_count = test_decode_all(file, file_length, 0, off, 64);
	int on_count = test_decode_all(file, file_length, 1, on, 64);
	CASE();
	CHECK(off_count == on_count);
	CHECK(off_count == 24);

	/* Pixel data (all three planes), local time and frame count are identical
	 * regardless of the PTS setting. */
	for (int i = 0; i < off_count; i++) {
		CASE();
		CHECK(off[i].hash == on[i].hash);
		CHECK(off[i].time == on[i].time);
	}

	/* Pull and callback delivery expose the very same tags and pixels. */
	test_callback_collect_t collect;
	memset(&collect, 0, sizeof(collect));
	plm_t *plm = plm_create_with_memory(file, file_length, 0);
	plm_set_audio_enabled(plm, 0);
	plm_set_video_pts_enabled(plm, 1);
	plm_set_video_decode_callback(plm, test_compat_callback, &collect);
	while (!plm_has_ended(plm)) {
		plm_decode(plm, 1.0);
	}
	CASE();
	CHECK(collect.count == on_count);
	for (int i = 0; i < collect.count; i++) {
		CHECK(collect.records[i].has_pts == on[i].has_pts);
		CHECK(collect.records[i].pts90k == on[i].pts90k);
		CHECK(collect.records[i].hash == on[i].hash);
		CHECK(collect.records[i].time == on[i].time);
	}
	plm_destroy(plm);

	/* File and memory inputs agree on the tag sequence. */
	plm_t *file_plm = plm_create_with_filename(SOURCE_PATH);
	CASE();
	CHECK(file_plm != NULL);
	plm_set_audio_enabled(file_plm, 0);
	plm_set_video_pts_enabled(file_plm, 1);
	int file_index = 0;
	plm_frame_t *frame;
	while ((frame = plm_decode_video(file_plm))) {
		CASE();
		CHECK(file_index < on_count);
		CHECK(frame->has_pts == on[file_index].has_pts);
		CHECK(frame->pts90k == on[file_index].pts90k);
		file_index++;
	}
	CHECK(file_index == on_count);
	plm_destroy(file_plm);

	/* The standalone elementary stream decoder has no PES layer: the new
	 * fields must remain zero even though the high level path populates them. */
	{
		plm_buffer_t *src = plm_buffer_create_with_memory(file, file_length, 0);
		plm_demux_t *demux = plm_demux_create(src, 1);
		size_t cap = file_length;
		uint8_t *es = (uint8_t *)malloc(cap);
		size_t es_length = 0;
		plm_packet_t *packet;
		while ((packet = plm_demux_decode(demux))) {
			if (packet->type != PLM_DEMUX_PACKET_VIDEO_1) {
				continue;
			}
			memcpy(es + es_length, packet->data, packet->length);
			es_length += packet->length;
		}
		plm_demux_destroy(demux);

		plm_buffer_t *es_buffer = plm_buffer_create_with_memory(es, es_length, 1);
		plm_video_t *video = plm_video_create_with_buffer(es_buffer, 1);
		plm_frame_t *es_frame;
		int leaked = 0;
		while ((es_frame = plm_video_decode(video))) {
			if (es_frame->has_pts || es_frame->pts90k) {
				leaked = 1;
			}
		}
		CASE();
		CHECK(leaked == 0);
		plm_video_destroy(video);
	}

	free(file);
}
