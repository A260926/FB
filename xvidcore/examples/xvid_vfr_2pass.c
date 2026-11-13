/*****************************************************************************
 *
 *  Xvid variable framerate two-pass example
 *
 *  Generates a short I420 clip that alternates moving frames with long static
 *  holds, runs both passes with the real per-frame durations, and stores:
 *    vfr_example.stats  - the first pass statistics with the timing schedule
 *    vfr_example.m4v    - the second pass elementary stream
 *
 *  Usage: vfr_2pass_example [output directory]
 *
 *  Plain C, no dependency other than the C library.
 *
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xvid.h"

#define WIDTH  128
#define HEIGHT 96
#define FRAMES 24
#define FBASE  40          /* time base; fincr is a number of 1/40 s ticks */

/* One frame duration is 1 unit; a hold is 8 units (0.2 s at 40 Hz). The
 * sequence therefore lasts sum(fincr)/fbase seconds rather than
 * FRAMES/constant_fps. */
static const int durations[FRAMES] = {
	1, 1, 8, 8, 8, 1,
	1, 1, 8, 8, 8, 1,
	1, 1, 8, 8, 8, 1,
	1, 1, 8, 8, 8, 1
};

static void make_frame(unsigned char *img, int index, int hold)
{
	int x, y;

	if (hold)
		return; /* keep the previous picture: a real static shot */

	/* Moving, textured picture. */
	for (y = 0; y < HEIGHT; y++) {
		for (x = 0; x < WIDTH; x++) {
			unsigned v = (unsigned)(x * 37 + y * 17 + index * 11);
			img[y*WIDTH + x] = (unsigned char)((v ^ (v >> 5)) & 0xff);
		}
	}
	for (y = 0; y < HEIGHT/2; y++) {
		for (x = 0; x < WIDTH/2; x++) {
			img[WIDTH*HEIGHT + y*WIDTH/2 + x] =
				(unsigned char)(80 + ((x * 3 + index) & 63));
			img[WIDTH*HEIGHT*5/4 + y*WIDTH/2 + x] =
				(unsigned char)(170 - ((y * 2 + index) & 63));
		}
	}
}

/* Run a complete pass. pass==1 writes the stats file, pass==2 reads it and
 * writes the elementary stream. */
static int run_pass(int pass, const char *stats_path, const char *stream_path,
					int bitrate)
{
	xvid_enc_create_t create;
	xvid_enc_plugin_t plugins[1];
	xvid_plugin_2pass1_t p1;
	xvid_plugin_2pass2_t p2;
	unsigned char *img;
	unsigned char *bitstream;
	FILE *out = NULL;
	void *handle;
	int i;
	int total = 0;

	bitstream = malloc(4 * 1024 * 1024);
	img = malloc(WIDTH * HEIGHT * 3 / 2);
	if (bitstream == NULL || img == NULL) {
		free(bitstream);
		free(img);
		return -1;
	}
	memset(img, 0x80, WIDTH * HEIGHT * 3 / 2);

	memset(&create, 0, sizeof(create));
	create.version = XVID_VERSION;
	create.width = WIDTH;
	create.height = HEIGHT;
	create.profile = 0xf5;      /* unrestricted */
	create.max_bframes = 0;     /* B frames keep their fixed framerate path */
	create.num_threads = 0;
	create.frame_drop_ratio = 0;
	create.fincr = 0;           /* variable framerate */
	create.fbase = FBASE;
	create.max_key_interval = 1000;

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
		p2.bitrate = bitrate;
		p2.container_frame_overhead = 0;
		plugins[0].func = xvid_plugin_2pass2;
		plugins[0].param = &p2;
	}
	create.num_plugins = 1;
	create.plugins = plugins;

	if (xvid_encore(NULL, XVID_ENC_CREATE, &create, NULL) < 0) {
		fprintf(stderr, "encoder creation failed for pass %d\n", pass);
		free(bitstream);
		free(img);
		return -1;
	}
	handle = create.handle;

	if (pass == 2) {
		out = fopen(stream_path, "wb");
		if (out == NULL) {
			fprintf(stderr, "cannot open %s\n", stream_path);
			xvid_encore(handle, XVID_ENC_DESTROY, NULL, NULL);
			free(bitstream);
			free(img);
			return -1;
		}
	}

	for (i = 0; i <= FRAMES; i++) {
		xvid_enc_frame_t frame;
		xvid_enc_stats_t stats;
		int len;

		memset(&frame, 0, sizeof(frame));
		memset(&stats, 0, sizeof(stats));
		frame.version = XVID_VERSION;
		stats.version = XVID_VERSION;
		frame.bitstream = bitstream;
		frame.length = 4 * 1024 * 1024;

		if (i < FRAMES) {
			/* A hold repeats the previous image; it is still one frame
			 * carrying one long duration, never a run of duplicates. */
			make_frame(img, i, (i > 0 && durations[i] > 1));
			frame.fincr = durations[i];
			frame.input.csp = XVID_CSP_I420;
			frame.input.plane[0] = img;
			frame.input.plane[1] = img + WIDTH*HEIGHT;
			frame.input.plane[2] = img + WIDTH*HEIGHT*5/4;
			frame.input.stride[0] = WIDTH;
			frame.input.stride[1] = WIDTH/2;
			frame.input.stride[2] = WIDTH/2;
		} else {
			frame.input.csp = XVID_CSP_NULL; /* flush */
			frame.fincr = durations[FRAMES-1];
		}

		len = xvid_encore(handle, XVID_ENC_ENCODE, &frame, &stats);
		if (len < 0 && len != XVID_ERR_END) {
			fprintf(stderr, "encode error %d at frame %d\n", len, i);
			if (out)
				fclose(out);
			xvid_encore(handle, XVID_ENC_DESTROY, NULL, NULL);
			free(bitstream);
			free(img);
			return -1;
		}
		if (i < FRAMES) {
			if (len > 0 && out != NULL)
				fwrite(bitstream, 1, len, out);
			if (len > 0)
				total += len;
		}
	}

	if (out != NULL)
		fclose(out);
	xvid_encore(handle, XVID_ENC_DESTROY, NULL, NULL);
	free(bitstream);
	free(img);

	printf("pass %d: %d frames, %d bytes\n", pass, FRAMES, total);
	return 0;
}

/* Decode the saved stream with the library's own decoder and report what came
 * out. This guards the example's output end to end, not only the file size. */
static int decode_check(const char *stream_path)
{
	xvid_dec_create_t create;
	xvid_dec_frame_t frame;
	xvid_dec_stats_t stats;
	unsigned char *bs, *out;
	FILE *f;
	long len;
	long used = 0;
	int frames = 0, nonzero = 0, width = 0, height = 0;
	void *handle;

	f = fopen(stream_path, "rb");
	if (f == NULL)
		return -1;
	bs = malloc(8 * 1024 * 1024);
	out = malloc(WIDTH * HEIGHT * 3 / 2);
	if (bs == NULL || out == NULL) {
		fclose(f);
		free(bs);
		free(out);
		return -1;
	}
	len = (long)fread(bs, 1, 8 * 1024 * 1024, f);
	fclose(f);

	memset(&create, 0, sizeof(create));
	create.version = XVID_VERSION;
	if (xvid_decore(NULL, XVID_DEC_CREATE, &create, NULL) < 0) {
		free(bs);
		free(out);
		return -1;
	}
	handle = create.handle;

	while (used < len) {
		int consumed, i, nz = 0;
		memset(&frame, 0, sizeof(frame));
		memset(&stats, 0, sizeof(stats));
		frame.version = XVID_VERSION;
		stats.version = XVID_VERSION;
		frame.bitstream = bs + used;
		frame.length = (int)(len - used);
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
		if (stats.type == XVID_TYPE_VOL) {
			width = stats.data.vol.width;
			height = stats.data.vol.height;
		} else if (stats.type > 0) {
			for (i = 0; i < WIDTH*HEIGHT*3/2; i++) {
				if (out[i] != 0) {
					nz = 1;
					break;
				}
			}
			frames++;
			nonzero += nz;
		}
	}

	xvid_decore(handle, XVID_DEC_DESTROY, NULL, NULL);
	free(bs);
	free(out);

	printf("decoded %d frames, %dx%d, %d with non-zero pixels\n",
		   frames, width, height, nonzero);
	if (frames != FRAMES || nonzero != FRAMES ||
		width != WIDTH || height != HEIGHT) {
		fprintf(stderr, "decoded stream does not match the example\n");
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	xvid_gbl_init_t init;
	char stats_path[512];
	char stream_path[512];
	const char *dir = argc > 1 ? argv[1] : ".";
	int64_t total_units = 0;
	int i;

	for (i = 0; i < FRAMES; i++)
		total_units += durations[i];

	memset(&init, 0, sizeof(init));
	init.version = XVID_VERSION;
	init.cpu_flags = 0x80000000u; /* force, no optional CPU features */
	if (xvid_global(NULL, XVID_GBL_INIT, &init, NULL) < 0) {
		fprintf(stderr, "xvid_global init failed\n");
		return 1;
	}

	snprintf(stats_path, sizeof(stats_path), "%s/vfr_example.stats", dir);
	snprintf(stream_path, sizeof(stream_path), "%s/vfr_example.m4v", dir);

	printf("Xvid variable framerate two-pass example\n");
	printf("  %d frames, time base %d, total duration %lld/%d = %.3f s\n",
		   FRAMES, FBASE, (long long)total_units, FBASE,
		   (double)total_units / (double)FBASE);

	if (run_pass(1, stats_path, stream_path, 0) != 0)
		return 1;
	if (run_pass(2, stats_path, stream_path, 300000) != 0)
		return 1;

	printf("wrote %s and %s\n", stats_path, stream_path);

	/* Verify the saved stream with the library's own MPEG-4 decoder. */
	if (decode_check(stream_path) != 0)
		return 1;
	return 0;
}

