/*
 * examples/plc_blocks.c
 *
 * Generate a short GSM stream, mark deterministic missing frames, and feed
 * every 160-sample timeline block to gsm_decode_plc().
 *
 * Build:
 *   cc -std=c99 -DSASR -DWAV49 -Iinc examples/plc_blocks.c \
 *      lib/libgsm.a -o plc_blocks_example
 *
 * Run:
 *   ./plc_blocks_example [output.pcm]
 */

#include "gsm.h"

#include <stdio.h>

#define EXAMPLE_FRAMES 12
#define SAMPLES        (EXAMPLE_FRAMES * 160)

int main(int argc, char **argv)
{
	gsm encoder, decoder;
	gsm_signal source[SAMPLES];
	gsm_signal output[SAMPLES];
	gsm_byte frames[EXAMPLE_FRAMES][33];

	/* 1 means that the compressed frame did not arrive. */
	const int lost[EXAMPLE_FRAMES] = {
		0, 0, 0, 1, 0, 1, 1, 1, 0, 0, 1, 0
	};

	unsigned t, f;
	unsigned missing_blocks = 0;
	unsigned nonzero_samples = 0;
	int rc;

	encoder = gsm_create();
	decoder = gsm_create();
	if (!encoder || !decoder) {
		fprintf(stderr, "gsm_create failed\n");
		return 1;
	}

	for (t = 0; t < SAMPLES; ++t) {
		int value = (int)(6000 * (t % 80 < 40 ? 1 : -1));
		value += (int)(18000 * ((t * 13) % 127) / 127 - 9000);
		if (value > 32767) value = 32767;
		if (value < -32768) value = -32768;
		source[t] = (gsm_signal)value;
	}

	for (f = 0; f < EXAMPLE_FRAMES; ++f)
		gsm_encode(encoder, source + f * 160, frames[f]);

	for (f = 0; f < EXAMPLE_FRAMES; ++f) {
		gsm_signal *block = output + f * 160;

		if (lost[f]) {
			++missing_blocks;
			rc = gsm_decode_plc(decoder, NULL, 0, 0,
					    block, 160);
			printf("frame %2u: lost, concealed %d samples\n",
			       f, rc);
		} else {
			rc = gsm_decode_plc(decoder, frames[f], 33, 1,
					    block, 160);
			printf("frame %2u: good, decoded %d samples\n",
			       f, rc);
		}

		if (rc != 160) {
			fprintf(stderr, "PLC block %u failed: %d\n", f, rc);
			gsm_destroy(encoder);
			gsm_destroy(decoder);
			return 1;
		}
	}

	for (t = 0; t < SAMPLES; ++t)
		if (output[t] != 0)
			++nonzero_samples;

	printf("blocks=%u missing=%u samples=%u nonzero=%u\n",
	       EXAMPLE_FRAMES, missing_blocks, SAMPLES, nonzero_samples);

	if (argc == 2) {
		FILE *out = fopen(argv[1], "wb");
		if (!out) {
			perror(argv[1]);
			return 1;
		}
		if (fwrite(output, sizeof(output[0]), SAMPLES, out) !=
		    SAMPLES) {
			fprintf(stderr, "short write to %s\n", argv[1]);
			fclose(out);
			return 1;
		}
		if (fclose(out) != 0) {
			perror(argv[1]);
			return 1;
		}
	}

	gsm_destroy(encoder);
	gsm_destroy(decoder);
	return 0;
}
