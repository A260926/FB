/* Extended Module Player
 * Copyright (C) 1996-2026 Claudio Matsuoka and Hipolito Carraro Jr
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * sinc_voice.c - libxmp XMP_INTERP_SINC example
 *
 * Loads a real module, selects the windowed-sinc interpolation after
 * xmp_start_player() and before the first rendered frame, plays a few
 * seconds into a memory buffer and exits successfully only when the
 * SINC path actually produced non-zero PCM.  No sound card or network
 * is used.
 *
 * Build (from the repository root):
 *   cc -Iinclude -Isrc examples/sinc_voice.c -l xmp -lm -o sinc_voice
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <xmp.h>

static const char *default_files[] = {
	"test-dev/data/test.xm",
	"test-dev/data/it_sample_porta.it",
	"data/test.xm",
	NULL
};

/* Find an instrument/sample pair with actual sample data and start a
 * note on channel 0. */
static int start_tone(xmp_context ctx)
{
	struct xmp_module_info mi;
	struct xmp_event ev;
	int i;

	xmp_get_module_info(ctx, &mi);
	memset(&ev, 0, sizeof(ev));

	for (i = 0; i < mi.mod->ins; i++) {
		if (mi.mod->xxi[i].nsm > 0) {
			int sid = mi.mod->xxi[i].sub[0].sid;
			if (sid >= 0 && sid < mi.mod->smp &&
			    mi.mod->xxs[sid].len > 0) {
				ev.note = 60;
				ev.ins = i + 1;
				ev.vol = 64;
				xmp_inject_event(ctx, 0, &ev);
				return 0;
			}
		}
	}
	return -1;
}

int main(int argc, char **argv)
{
	xmp_context ctx;
	const char *path = NULL;
	/* about two seconds at 44.1 kHz stereo 16-bit */
	const size_t bytes = 44100 * 2 * 2 * 2;
	unsigned char *out;
	size_t done = 0;
	uint64_t nonzero = 0;
	int rc = 1;
	int i;

	if (argc > 1)
		path = argv[1];

	ctx = xmp_create_context();
	if (ctx == NULL) {
		fprintf(stderr, "xmp_create_context failed\n");
		return 1;
	}

	if (path != NULL) {
		if (xmp_load_module(ctx, path) != 0) {
			fprintf(stderr, "cannot load %s\n", path);
			goto out;
		}
	} else {
		for (i = 0; default_files[i] != NULL; i++) {
			if (xmp_load_module(ctx, default_files[i]) == 0) {
				path = default_files[i];
				break;
			}
		}
		if (path == NULL) {
			fprintf(stderr, "no example module found\n");
			goto out;
		}
	}

	/* 44.1 kHz, 16-bit stereo (format 0). */
	if (xmp_start_player(ctx, 44100, 0) != 0) {
		fprintf(stderr, "xmp_start_player failed\n");
		goto rel;
	}

	/* Required selection window: after start_player(), before the
	 * first xmp_play_buffer()/xmp_play_frame() call. */
	if (xmp_set_player(ctx, XMP_PLAYER_INTERP, XMP_INTERP_SINC) != 0) {
		fprintf(stderr, "XMP_INTERP_SINC rejected\n");
		goto endp;
	}
	if (xmp_get_player(ctx, XMP_PLAYER_INTERP) != XMP_INTERP_SINC) {
		fprintf(stderr, "SINC interpolation did not stick\n");
		goto endp;
	}

	start_tone(ctx);

	out = (unsigned char *)calloc(1, bytes);
	if (out == NULL)
		goto endp;

	while (done < bytes) {
		int want = (int)(bytes - done);
		if (want > 16384)
			want = 16384;
		if (xmp_play_buffer(ctx, out + done, want, 0) < 0)
			break;
		done += (size_t)want;
	}

	for (i = 0; i + 1 < (int)done; i += 2) {
		int16_t s = (int16_t)(out[i] | (out[i + 1] << 8));
		if (s != 0)
			nonzero++;
	}

	printf("sinc_voice example: %s, %zu bytes, %llu nonzero 16-bit samples\n",
	       path, done, (unsigned long long)nonzero);

	if (nonzero > 1000) {
		printf("SINC_VOICE_EXAMPLE_OK\n");
		rc = 0;
	} else {
		fprintf(stderr, "SINC path produced almost no audio\n");
	}

	free(out);
endp:
	xmp_end_player(ctx);
rel:
	xmp_release_module(ctx);
out:
	xmp_free_context(ctx);
	return rc;
}
