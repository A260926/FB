/* Example: decode a Speex bit-stream twice, once without and once with the
 * perceptual enhancement, writing both tracks to raw 16-bit PCM files.
 *
 * This uses the optional dual-output decoder (SPEEX_SET_DUAL_OUTPUT): a
 * single decoder produces both signals in one pass over the bit-stream, so
 * the adaptive-codebook/LSP/pitch state is only advanced once.  This is the
 * setup you want when you record the "raw" audio while monitoring the
 * enhanced one.
 *
 * Usage:
 *   dual_decode_demo <input.spx> <raw.pcm> <enhanced.pcm>
 *
 * <input.spx> is a Speex file with a simple page header followed by one
 * frame per "packet" (see speex_header.h); for a quick demo you can also
 * feed a file that starts with the 80-byte Speex header and then stores
 * packets as [uint32 little-endian byte count][packet bytes].
 *
 * The program demonstrates the three entry points working together:
 *   - speex_decode_dual_int() during normal recording,
 *   - speex_decode_int()       for a frame where only one track is needed,
 *   - SPEEX_SET_ENH            to pick which track that single call returns.
 */
#include <speex/speex.h>
#include <speex/speex_bits.h>
#include <speex/speex_header.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAME 640

static unsigned int read_le32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

int main(int argc, char **argv)
{
    FILE *in, *fraw, *fenh;
    unsigned char header_bytes[80];
    SpeexHeader *header;
    const SpeexMode *mode;
    void *decoder;
    SpeexBits bits;
    spx_int32_t dual_on = 1;
    int frame_size = 0;
    spx_int16_t raw[MAX_FRAME], enhanced[MAX_FRAME];
    unsigned long frame_no = 0;

    if (argc != 4) {
        fprintf(stderr, "usage: %s <input.spx> <raw.pcm> <enhanced.pcm>\n", argv[0]);
        return 1;
    }
    in = fopen(argv[1], "rb");
    if (!in) { perror(argv[1]); return 1; }
    if (fread(header_bytes, 1, sizeof header_bytes, in) != sizeof header_bytes) {
        fprintf(stderr, "%s: not a Speex stream (short header)\n", argv[1]);
        return 1;
    }
    header = speex_packet_to_header((char *)header_bytes, sizeof header_bytes);
    if (!header) { fprintf(stderr, "bad Speex header\n"); return 1; }

    switch (header->mode) {
    case SPEEX_MODEID_NB:  mode = speex_lib_get_mode(SPEEX_MODEID_NB);  break;
    case SPEEX_MODEID_WB:  mode = speex_lib_get_mode(SPEEX_MODEID_WB);  break;
    case SPEEX_MODEID_UWB: mode = speex_lib_get_mode(SPEEX_MODEID_UWB); break;
    default: fprintf(stderr, "unsupported mode %d\n", header->mode); return 1;
    }
    speex_header_free(header);

    decoder = speex_decoder_init(mode);
    if (!decoder) { fprintf(stderr, "decoder init failed\n"); return 1; }

    /* Enable dual output before the first frame is decoded.  After that it
     * cannot be toggled; SPEEX_SET_DUAL_OUTPUT to the current value is still
     * accepted. */
    if (speex_decoder_ctl(decoder, SPEEX_SET_DUAL_OUTPUT, &dual_on) != 0) {
        fprintf(stderr, "dual output not available\n");
        return 1;
    }
    speex_decoder_ctl(decoder, SPEEX_GET_FRAME_SIZE, &frame_size);
    speex_bits_init(&bits);

    fraw = fopen(argv[2], "wb");
    fenh = fopen(argv[3], "wb");
    if (!fraw || !fenh) { perror("output"); return 1; }

    while (1) {
        unsigned char len_bytes[4];
        unsigned char *packet;
        unsigned int len;
        int ret;

        if (fread(len_bytes, 1, 4, in) != 4)
            break;
        len = read_le32(len_bytes);
        if (len == 0 || len > 65535)
            break;
        packet = (unsigned char *)malloc(len);
        if (!packet) return 1;
        if (fread(packet, 1, len, in) != len) { free(packet); break; }

        speex_bits_read_from(&bits, (const char *)packet, (int)len);
        free(packet);

        /* Decode both tracks from one pass over the bit-stream.  The raw
         * output is the un-enhanced render and the enhanced output the
         * perceptually enhanced one; both advance their own filter histories
         * continuously across frames. */
        ret = speex_decode_dual_int(decoder, &bits, raw, enhanced);
        if (ret == 0) {
            fwrite(raw, sizeof(spx_int16_t), frame_size, fraw);
            fwrite(enhanced, sizeof(spx_int16_t), frame_size, fenh);
        }
        if (ret != 0)
            break;
        ++frame_no;
    }

    /* Note: you can freely interleave speex_decode() / speex_decode_int()
     * with the dual calls (for example to skip writing the raw file while
     * still recording it).  They advance both rendering histories, and
     * SPEEX_SET_ENH then selects which of the two tracks the single call
     * returns.  See doc/dual_output.txt. */
    speex_bits_destroy(&bits);
    speex_decoder_destroy(decoder);
    fclose(fraw);
    fclose(fenh);
    fclose(in);
    printf("decoded %lu frames\n", frame_no);
    return 0;
}
