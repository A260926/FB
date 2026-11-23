/* reset_roundtrip.c -- streaming Speex round-trip that reuses one
 * encoder/decoder pair across independent audio segments.
 *
 * Usage: reset_roundtrip INPUT.wav OUTPUT.wav CUT
 *
 * Reads a plain little-endian, mono, 16-bit PCM WAVE at 8, 16 or 32 kHz,
 * encodes and decodes it one Speex frame at a time, and writes a WAVE with
 * the same sample rate.  Every CUT Speex frames, before encoding the next
 * frame, the same encoder and decoder handles are reset with
 * SPEEX_RESET_STATE instead of being destroyed and recreated.
 *
 * The last frame is zero-padded for the codec but only the original number
 * of samples is written; no compensation is made for codec look-ahead.
 * Working memory is fixed size and does not grow with the input length.
 */

#include "speex/speex.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAME_SIZE 640
#define PACKET_BYTES 256

static unsigned int rd_u16(const unsigned char *p)
{
   return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static unsigned int rd_u32(const unsigned char *p)
{
   return (unsigned int)p[0] | ((unsigned int)p[1] << 8)
          | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static void wr_u16(FILE *f, unsigned int v)
{
   unsigned char b[2];
   b[0] = (unsigned char)(v & 0xff);
   b[1] = (unsigned char)((v >> 8) & 0xff);
   if (fwrite(b, 1, 2, f) != 2) { /* reported by the caller via ferror */ }
}

static void wr_u32(FILE *f, unsigned int v)
{
   unsigned char b[4];
   b[0] = (unsigned char)(v & 0xff);
   b[1] = (unsigned char)((v >> 8) & 0xff);
   b[2] = (unsigned char)((v >> 16) & 0xff);
   b[3] = (unsigned char)((v >> 24) & 0xff);
   if (fwrite(b, 1, 4, f) != 4) { /* reported by the caller via ferror */ }
}

static int write_wav_header(FILE *f, int rate, unsigned int data_bytes)
{
   if (fwrite("RIFF", 1, 4, f) != 4) return -1;
   wr_u32(f, 36u + data_bytes);
   if (fwrite("WAVEfmt ", 1, 8, f) != 8) return -1;
   wr_u32(f, 16);                    /* PCM fmt chunk size */
   wr_u16(f, 1);                     /* PCM format tag */
   wr_u16(f, 1);                     /* mono */
   wr_u32(f, (unsigned int)rate);
   wr_u32(f, (unsigned int)rate * 2); /* byte rate */
   wr_u16(f, 2);                     /* block align */
   wr_u16(f, 16);                    /* bits per sample */
   if (fwrite("data", 1, 4, f) != 4) return -1;
   wr_u32(f, data_bytes);
   return ferror(f) ? -1 : 0;
}

/* Parse the WAVE header.  On success leaves *f positioned at the first data
   sample and returns the sample count; returns -1 for unsupported input. */
static long read_wav_info(FILE *f, int *rate)
{
   unsigned char hdr[12];
   int got_fmt = 0, got_data = 0;
   unsigned int fmt_rate = 0, fmt_channels = 0, fmt_bits = 0, fmt_tag = 0;
   unsigned int data_bytes = 0;
   long data_pos = -1;

   if (fread(hdr, 1, 12, f) != 12) return -1;
   if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
      return -1;

   for (;;) {
      unsigned char ch[8];
      unsigned int chunk_size, i;
      if (fread(ch, 1, 8, f) != 8) {
         if (got_fmt && got_data) break;
         return -1;
      }
      chunk_size = rd_u32(ch + 4);
      if (memcmp(ch, "fmt ", 4) == 0) {
         unsigned char fmt[16];
         if (got_fmt || chunk_size < 16) return -1;
         if (fread(fmt, 1, 16, f) != 16) return -1;
         fmt_tag = rd_u16(fmt);
         fmt_channels = rd_u16(fmt + 2);
         fmt_rate = rd_u32(fmt + 4);
         fmt_bits = rd_u16(fmt + 14);
         got_fmt = 1;
         /* Skip any remaining bytes of an extended fmt chunk. */
         for (i = 16; i < chunk_size; ++i) {
            if (fgetc(f) == EOF) return -1;
         }
      } else if (memcmp(ch, "data", 4) == 0) {
         if (got_data) return -1;
         data_bytes = chunk_size;
         data_pos = ftell(f);
         if (data_pos < 0) return -1;
         got_data = 1;
         /* The samples are streamed from here; stop parsing. */
         break;
      } else {
         /* Unknown chunk: skip it. */
         if (fseek(f, (long)chunk_size + (chunk_size & 1), SEEK_CUR) != 0)
            return -1;
      }
   }

   if (!got_fmt || !got_data || data_pos < 0) return -1;
   if (fmt_tag != 1 || fmt_channels != 1 || fmt_bits != 16) return -1;
   if (fmt_rate != 8000 && fmt_rate != 16000 && fmt_rate != 32000) return -1;
   if (data_bytes & 1u) return -1;
   if (fseek(f, data_pos, SEEK_SET) != 0) return -1;
   *rate = (int)fmt_rate;
   return (long)(data_bytes / 2);
}

static int mode_for_rate(int rate)
{
   if (rate == 8000) return SPEEX_MODEID_NB;
   if (rate == 16000) return SPEEX_MODEID_WB;
   return SPEEX_MODEID_UWB;
}

int main(int argc, char **argv)
{
   FILE *in = NULL, *out = NULL;
   void *enc = NULL, *dec = NULL;
   SpeexBits bits;
   spx_int16_t in_frame[MAX_FRAME_SIZE], out_frame[MAX_FRAME_SIZE];
   char packet[PACKET_BYTES];
   int rate = 0, frame_size = 0, cut, mode_id, frame_index = 0;
   long total_samples, samples_written = 0;
   char *endptr = NULL;
   int rc = 1;

   if (argc != 4) {
      fprintf(stderr, "usage: %s INPUT.wav OUTPUT.wav CUT\n",
              argc > 0 ? argv[0] : "reset_roundtrip");
      return 1;
   }
   if (strcmp(argv[1], argv[2]) == 0) {
      fprintf(stderr, "input and output must be different paths\n");
      return 1;
   }
   errno = 0;
   cut = (int)strtol(argv[3], &endptr, 10);
   if (errno != 0 || endptr == argv[3] || *endptr != '\0' || cut <= 0) {
      fprintf(stderr, "CUT must be a positive integer\n");
      return 1;
   }

   in = fopen(argv[1], "rb");
   if (!in) {
      fprintf(stderr, "cannot open input '%s'\n", argv[1]);
      goto fail;
   }
   total_samples = read_wav_info(in, &rate);
   if (total_samples < 0) {
      fprintf(stderr, "unsupported WAVE format (need mono PCM16 at 8/16/32 kHz)\n");
      goto fail;
   }

   out = fopen(argv[2], "wb");
   if (!out) {
      fprintf(stderr, "cannot open output '%s'\n", argv[2]);
      goto fail;
   }
   if (write_wav_header(out, rate, (unsigned int)(total_samples * 2)) != 0) {
      fprintf(stderr, "cannot write output header\n");
      goto fail;
   }

   mode_id = mode_for_rate(rate);
   enc = speex_encoder_init(speex_lib_get_mode(mode_id));
   dec = speex_decoder_init(speex_lib_get_mode(mode_id));
   if (!enc || !dec) {
      fprintf(stderr, "speex state allocation failed\n");
      goto fail;
   }
   speex_encoder_ctl(enc, SPEEX_GET_FRAME_SIZE, &frame_size);
   if (frame_size <= 0 || frame_size > MAX_FRAME_SIZE) {
      fprintf(stderr, "unsupported frame size %d\n", frame_size);
      goto fail;
   }
   speex_bits_init(&bits);

   while (samples_written < total_samples) {
      long remaining = total_samples - samples_written;
      int take = frame_size, i, nbytes;
      if ((long)take > remaining)
         take = (int)remaining;

      for (i = 0; i < take; ++i) {
         unsigned char s[2];
         if (fread(s, 1, 2, in) != 2) {
            fprintf(stderr, "short read on input samples\n");
            goto fail;
         }
         in_frame[i] = (spx_int16_t)rd_u16(s);
      }
      /* Zero-pad the final partial frame. */
      for (i = take; i < frame_size; ++i)
         in_frame[i] = 0;

      /* A new segment starts here: reset before encoding its first frame. */
      if (frame_index > 0 && (frame_index % cut) == 0) {
         if (speex_encoder_ctl(enc, SPEEX_RESET_STATE, NULL) != 0
             || speex_decoder_ctl(dec, SPEEX_RESET_STATE, NULL) != 0) {
            fprintf(stderr, "SPEEX_RESET_STATE failed\n");
            goto fail;
         }
      }

      speex_bits_reset(&bits);
      if (speex_encode_int(enc, in_frame, &bits) < 0) {
         fprintf(stderr, "speex_encode_int failed at frame %d\n", frame_index);
         goto fail;
      }
      nbytes = speex_bits_write(&bits, packet, sizeof(packet));
      if (nbytes <= 0) {
         fprintf(stderr, "empty packet at frame %d\n", frame_index);
         goto fail;
      }
      speex_bits_read_from(&bits, packet, nbytes);
      if (speex_decode_int(dec, &bits, out_frame) != 0) {
         fprintf(stderr, "speex_decode_int failed at frame %d\n", frame_index);
         goto fail;
      }
      if (fwrite(out_frame, sizeof(spx_int16_t), (size_t)take, out)
          != (size_t)take) {
         fprintf(stderr, "short write on output samples\n");
         goto fail;
      }
      samples_written += take;
      ++frame_index;
   }

   if (fflush(out) != 0 || ferror(out) || ferror(in)) {
      fprintf(stderr, "I/O error while finishing output\n");
      goto fail;
   }
   rc = 0;

fail:
   if (enc || dec)
      speex_bits_destroy(&bits);
   if (enc)
      speex_encoder_destroy(enc);
   if (dec)
      speex_decoder_destroy(dec);
   if (in)
      fclose(in);
   if (out)
      fclose(out);
   return rc;
}
