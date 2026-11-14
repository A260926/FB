/*
PL_MPEG Example - reverse video playback from a raw MPEG1 elementary stream
SPDX-License-Identifier: MIT


-- Usage

pl_mpeg_reverse_video <video-file.m1v> [max-cached-frames]

The file must be a complete MPEG1 Video elementary stream (as produced by
`ffmpeg -f mpeg1video ...`), not an MPEG-PS/program stream. The example plays it
in reverse display order, printing each frame's time and a few luma samples, and
performs one middle seek to show random access.

If max-cached-frames is omitted it defaults to 64. It must be at least G, the
largest number of display frames between two I pictures (including the leading
and trailing partial intervals). The library rejects smaller values with NULL.


-- About

This demonstrates the plm_video_reverse_* API:

	plm_video_reverse_t *r = plm_video_reverse_create(bytes, length, capacity);
	size_t count = plm_video_reverse_get_count(r);

	for (plm_frame_t *f; (f = plm_video_reverse_next(r));) {
		// use f until the next call
	}

	plm_video_reverse_seek(r, frame_number);  // 1 on success, 0 out of range
	plm_video_reverse_destroy(r);

Ownership and lifetime:
- The caller's `bytes` must stay valid and unchanged until destroy(). The
library never modifies or copies the compressed stream.
- A returned plm_frame_t* (and its Y/Cr/Cb planes) is borrowed from the reverse
instance: it stays valid only until the next plm_video_reverse_next(), a
successful plm_video_reverse_seek(), or plm_video_reverse_destroy(). Do not
retain the pointer across those calls; copy the pixels if you need them longer.
- frame->time is the picture's original forward local time
  (display_frame / frame_rate), not a reverse elapsed time.
- seek() is zero-based in display order. After exhaustion, seek() re-arms
  playback (including seeking back to the frame you just consumed).

Only a bounded GOP-plus-boundary working set is decoded at a time. No frame
pixels are allocated while consuming frames; the cache was created up front.
*/

#include <stdio.h>
#include <stdlib.h>

#define PL_MPEG_IMPLEMENTATION
#include "../pl_mpeg.h"

#define REVERSE_DEFAULT_CACHE 64

static uint8_t *read_file(const char *path, size_t *length) {
	FILE *fh = fopen(path, "rb");
	if (!fh) {
		return NULL;
	}
	fseek(fh, 0, SEEK_END);
	long size = ftell(fh);
	fseek(fh, 0, SEEK_SET);
	if (size <= 0) {
		fclose(fh);
		return NULL;
	}
	uint8_t *bytes = (uint8_t *)malloc((size_t)size);
	if (!bytes) {
		fclose(fh);
		return NULL;
	}
	if (fread(bytes, 1, (size_t)size, fh) != (size_t)size) {
		free(bytes);
		fclose(fh);
		return NULL;
	}
	fclose(fh);
	*length = (size_t)size;
	return bytes;
}

int main(int argc, char *argv[]) {
	if (argc < 2) {
		printf("Usage: %s <video-file.m1v> [max-cached-frames]\n", argv[0]);
		return 1;
	}

	size_t length = 0;
	uint8_t *bytes = read_file(argv[1], &length);
	if (!bytes) {
		fprintf(stderr, "Couldn't read elementary stream %s\n", argv[1]);
		return 1;
	}

	size_t capacity = REVERSE_DEFAULT_CACHE;
	if (argc >= 3) {
		long value = atol(argv[2]);
		if (value <= 0) {
			fprintf(stderr, "max-cached-frames must be positive\n");
			free(bytes);
			return 1;
		}
		capacity = (size_t)value;
	}

	plm_video_reverse_t *reverse =
		plm_video_reverse_create(bytes, length, capacity);
	if (!reverse) {
		fprintf(stderr,
			"Couldn't create reverse player (capacity below G or invalid stream)\n");
		free(bytes);
		return 1;
	}

	size_t count = plm_video_reverse_get_count(reverse);
	printf("frames=%zu capacity=%zu\n", count, capacity);

	// Play a few frames from the end, in reverse display order. next() returns
	// display frames count-1, count-2, ...; frame->time is the original forward
	// local time, so it decreases as playback moves backwards.
	printf("-- reverse tail --\n");
	plm_frame_t *frame;
	for (int i = 0; i < 5 && (frame = plm_video_reverse_next(reverse)); i++) {
		printf(
			"display=%zu time=%.4f size=%ux%u luma=%u,%u,%u\n",
			count - 1 - (size_t)i, frame->time,
			frame->width, frame->height,
			frame->y.data[0],
			frame->y.data[frame->y.width],
			frame->y.data[2 * frame->y.width]
		);
	}

	// Seek to the middle and play a few frames from there. The next call returns
	// exactly the sought frame, then successively earlier ones.
	if (count > 0) {
		size_t middle = count / 2;
		if (plm_video_reverse_seek(reverse, middle)) {
			printf("-- seek to %zu --\n", middle);
			for (int i = 0; i < 3 && (frame = plm_video_reverse_next(reverse)); i++) {
				printf("display=%zu time=%.4f\n", middle - (size_t)i, frame->time);
			}
		}
	}

	// Exhausting playback and then seeking back in is also supported.
	if (count > 0 && plm_video_reverse_seek(reverse, 0)) {
		frame = plm_video_reverse_next(reverse);
		if (frame) {
			printf("-- seek to first: display=0 time=%.4f --\n", frame->time);
		}
	}

	plm_video_reverse_stats_t stats;
	plm_video_reverse_get_stats(reverse, &stats);
	printf(
		"stats: index_bytes=%zu pictures_decoded=%zu cached_frames_peak=%zu\n",
		stats.index_bytes, stats.pictures_decoded, stats.cached_frames_peak
	);

	// The caller still owns `bytes`; destroy() does not free it.
	plm_video_reverse_destroy(reverse);
	free(bytes);
	return 0;
}
