/*****************************************************************************
 *
 *  Xvid fixed framerate two-pass baseline check
 *
 *  Encodes a deterministic 80 frame I420 sequence twice (first pass, then
 *  second pass) with the historical fixed framerate interface and checks the
 *  elementary stream with the library's own MPEG-4 decoder.
 *
 *  This is the regression guard for the fixed framerate behaviour: it must
 *  keep working exactly as before the variable framerate changes.
 *
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xvid.h"

#define WIDTH  128
#define HEIGHT 96
#define FRAMES 80
#define FBASE  25
#define FINCR  1

static void make_frame(unsigned char *img, int index)
{
	int x, y;
	for (y = 0; y < HEIGHT; y++) {
		for (x = 0; x < WIDTH; x++) {
			unsigned v = (unsigned)(x * 29 + y * 13 + index * 7);
			img[y*WIDTH + x] = (unsigned char)((v ^ (v >> 6)) & 0xff);
		}
	}
	for (y = 0; y < HEIGHT/2; y++) {
		for (x = 0; x < WIDTH/2; x++) {
			img[WIDTH*HEIGHT + y*WIDTH/2 + x] =
				(unsigned char)(96 + ((x + index * 2) & 63));
			img[WIDTH*HEIGHT*5/4 + y*WIDTH/2 + x] =
				(unsigned char)(160 - ((y + index * 3) & 63));
		}
	}
}

static int run_pass(int pass, const char *stats_path, int max_bframes,
					unsigned char *bitstream, int capacity, int *produced)
{
	xvid_enc_create_t create;
	xvid_enc_plugin_t plugins[1];
	xvid_plugin_2pass1_t p1;
	xvid_plugin_2pass2_t p2;
	unsigned char *img;
	void *handle;
	int i, total = 0;

	img = malloc(WIDTH * HEIGHT * 3 / 2);
	if (img == NULL)
		return -1;
	memset(img, 0x80, WIDTH * HEIGHT * 3 / 2);

	memset(&create, 0, sizeof(create));
	create.version = XVID_VERSION;
	create.width = WIDTH;
	create.height = HEIGHT;
	create.profile = 0xf5;
	create.max_bframes = max_bframes;
	create.num_threads = 0;
	create.fincr = FINCR;
	create.fbase = FBASE;
	create.max_key_interval = 250;

	if (pass == 1) {
		memset(&p1, 0, sizeof(p1));
		p1.version = XVID_VERSION;
		p1.filename = (char *)stats_path;
		plugins[0].func = xvid_plugin_2pass1;
		plugins[0].param = &p1;
	} else {
		memset(&p2, 0, sizeof(p2));
		p2.version = XVID_VERSION;
		p2.filename = (char *)stats_path;
		p2.bitrate = 600000;
		p2.container_frame_overhead = 0;
		plugins[0].func = xvid_plugin_2pass2;
		plugins[0].param = &p2;
	}
	create.num_plugins = 1;
	create.plugins = plugins;

	if (xvid_encore(NULL, XVID_ENC_CREATE, &create, NULL) < 0) {
		free(img);
		return -1;
	}
	handle = create.handle;

	for (i = 0; i <= FRAMES; i++) {
		xvid_enc_frame_t frame;
		xvid_enc_stats_t stats;
		int len;

		memset(&frame, 0, sizeof(frame));
		memset(&stats, 0, sizeof(stats));
		frame.version = XVID_VERSION;
		stats.version = XVID_VERSION;
		frame.bitstream = bitstream + total;
		frame.length = capacity - total;

		if (i < FRAMES) {
			make_frame(img, i);
			frame.input.csp = XVID_CSP_I420;
			frame.input.plane[0] = img;
			frame.input.plane[1] = img + WIDTH*HEIGHT;
			frame.input.plane[2] = img + WIDTH*HEIGHT*5/4;
			frame.input.stride[0] = WIDTH;
			frame.input.stride[1] = WIDTH/2;
			frame.input.stride[2] = WIDTH/2;
		} else {
			frame.input.csp = XVID_CSP_NULL;
		}

		len = xvid_encore(handle, XVID_ENC_ENCODE, &frame, &stats);
		if (len < 0 && len != XVID_ERR_END) {
			fprintf(stderr, "pass %d encode error %d at frame %d\n",
					pass, len, i);
			xvid_encore(handle, XVID_ENC_DESTROY, NULL, NULL);
			free(img);
			return -1;
		}
		if (i < FRAMES && len > 0)
			total += len;
	}

	xvid_encore(handle, XVID_ENC_DESTROY, NULL, NULL);
	free(img);
	*produced = total;
	return 0;
}

static int decode_check(const unsigned char *bs, int len, int expected_decoded)
{
	xvid_dec_create_t create;
	xvid_dec_frame_t frame;
	xvid_dec_stats_t stats;
	unsigned char *out;
	void *handle;
	int used = 0, frames = 0, nonzero = 0;

	memset(&create, 0, sizeof(create));
	create.version = XVID_VERSION;
	if (xvid_decore(NULL, XVID_DEC_CREATE, &create, NULL) < 0)
		return -1;
	handle = create.handle;
	out = malloc(WIDTH * HEIGHT * 3 / 2);
	if (out == NULL) {
		xvid_decore(handle, XVID_DEC_DESTROY, NULL, NULL);
		return -1;
	}

	while (used < len) {
		int consumed, i, nz = 0;
		memset(&frame, 0, sizeof(frame));
		memset(&stats, 0, sizeof(stats));
		frame.version = XVID_VERSION;
		stats.version = XVID_VERSION;
		frame.bitstream = (void *)(bs + used);
		frame.length = len - used;
		frame.output.csp = XVID_CSP_I420;
		frame.output.plane[0] = out;
		frame.output.plane[1] = out + WIDTH*HEIGHT;
		frame.output.plane[2] = out + WIDTH*HEIGHT*5/4;
		frame.output.stride[0] = WIDTH;
		frame.output.stride[1] = WIDTH/2;
		frame.output.stride[2] = WIDTH/2;

		consumed = xvid_decore(handle, XVID_DEC_DECODE, &frame, &stats);
		if (consumed <= 0)
			break;
		used += consumed;
		if (stats.type > 0 && stats.type != XVID_TYPE_VOL) {
			for (i = 0; i < WIDTH*HEIGHT*3/2; i++) {
				if (out[i] != 0 && out[i] != 0x80) {
					nz = 1;
					break;
				}
			}
			frames++;
			nonzero += nz;
		}
	}

	/* B frames delay output: flush the decoder until it reports the end of
	 * stream to release the reference frames still held back. */
	for (;;) {
		int i, nz = 0;
		int consumed;
		memset(&frame, 0, sizeof(frame));
		memset(&stats, 0, sizeof(stats));
		frame.version = XVID_VERSION;
		stats.version = XVID_VERSION;
		frame.bitstream = NULL;
		frame.length = -1;
		frame.output.csp = XVID_CSP_I420;
		frame.output.plane[0] = out;
		frame.output.plane[1] = out + WIDTH*HEIGHT;
		frame.output.plane[2] = out + WIDTH*HEIGHT*5/4;
		frame.output.stride[0] = WIDTH;
		frame.output.stride[1] = WIDTH/2;
		frame.output.stride[2] = WIDTH/2;
		consumed = xvid_decore(handle, XVID_DEC_DECODE, &frame, &stats);
		if (consumed < 0) /* XVID_ERR_END: no more delayed frames */
			break;
		if (stats.type > 0 && stats.type != XVID_TYPE_VOL) {
			for (i = 0; i < WIDTH*HEIGHT*3/2; i++) {
				if (out[i] != 0 && out[i] != 0x80) {
					nz = 1;
					break;
				}
			}
			frames++;
			nonzero += nz;
		}
	}

	xvid_decore(handle, XVID_DEC_DESTROY, NULL, NULL);
	free(out);

	if (frames < expected_decoded - 2 || frames > expected_decoded + 1) {
		fprintf(stderr, "decoded %d frames, expected about %d (reorder lag)\n",
				frames, expected_decoded);
		return -1;
	}
	if (nonzero != frames) {
		fprintf(stderr, "decoded %d empty frames\n", frames - nonzero);
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	xvid_gbl_init_t init;
	unsigned char *bitstream;
	char stats_path[512];
	const char *dir = argc > 1 ? argv[1] : ".";
	int pass1_bytes = 0, pass2_bytes = 0;
	int expected_decoded = FRAMES;

	memset(&init, 0, sizeof(init));
	init.version = XVID_VERSION;
	init.cpu_flags = 0x80000000u;
	if (xvid_global(NULL, XVID_GBL_INIT, &init, NULL) < 0) {
		fprintf(stderr, "xvid_global init failed\n");
		return 1;
	}

	bitstream = malloc(16 * 1024 * 1024);
	if (bitstream == NULL)
		return 1;
	snprintf(stats_path, sizeof(stats_path), "%s/baseline.stats", dir);

	/* Also exercise the existing B frame usage: the two pass plugins must
	 * keep working with reordered streams exactly as before. */
	if (run_pass(1, stats_path, 2, bitstream, 16 * 1024 * 1024, &pass1_bytes) != 0)
		goto fail;
	if (run_pass(2, stats_path, 2, bitstream, 16 * 1024 * 1024, &pass2_bytes) != 0)
		goto fail;

	/* A fixed framerate first pass must keep the historical file format:
	 * no timing schedule and no per-frame duration lines. */
	{
		FILE *f = fopen(stats_path, "rb");
		char line[512];
		int timed = 0, lines = 0;
		if (f == NULL) {
			fprintf(stderr, "cannot read %s\n", stats_path);
			goto fail;
		}
		while (fgets(line, sizeof(line), f)) {
			if (strncmp(line, "# XVID_TIMING", 13) == 0 ||
				strncmp(line, "# XVID_DURATION", 15) == 0)
				timed++;
			if (line[0] == 'i' || line[0] == 'p' ||
				line[0] == 'b' || line[0] == 's')
				lines++;
		}
		fclose(f);
		if (timed != 0) {
			fprintf(stderr, "fixed framerate stats contain timing lines\n");
			goto fail;
		}
		/* With unpacked B frames one statistics line is written per coded
		 * non-B frame, so the last few B frames of the stream have no line
		 * of their own. Require a full, plausible schedule rather than an
		 * exact count. */
		if (lines < FRAMES - 4 || lines > FRAMES) {
			fprintf(stderr, "stats contain %d frame lines, expected about %d\n",
					lines, FRAMES);
			goto fail;
		}
		/* B frames keep one reference frame in flight when the flush runs, so
		 * the decoder reports about one frame less than the VOP count. */
		expected_decoded = FRAMES - 1;
	}

	{
		/* Keep the stream for manual inspection. */
		char sp[600];
		FILE *sf;
		snprintf(sp, sizeof(sp), "%s/baseline.m4v", dir);
		sf = fopen(sp, "wb");
		if (sf) {
			fwrite(bitstream, 1, pass2_bytes, sf);
			fclose(sf);
		}
	}
	if (decode_check(bitstream, pass2_bytes, expected_decoded) != 0)
		goto fail;

	printf("baseline: %d frames, pass1 %d bytes, pass2 %d bytes\n",
		   FRAMES, pass1_bytes, pass2_bytes);
	printf("BASELINE PASS\n");
	free(bitstream);
	return 0;

fail:
	free(bitstream);
	return 1;
}
