/* reset_roundtrip.c -- Streaming Speex round-trip that periodically resets
 * one persistent encoder/decoder pair.
 *
 * Usage: reset_roundtrip INPUT.wav OUTPUT.wav CUT
 *
 * Reads a plain little-endian mono PCM16 WAVE file at 8, 16 or 32 kHz and
 * re-codes it frame by frame through Speex. Before encoding every block of
 * CUT audio frames the same encoder and decoder handles are reset with
 * SPEEX_RESET_STATE instead of being destroyed and recreated.
 *
 * Memory use is fixed: the whole file is never buffered. The last frame is
 * zero padded for encoding, but only the original number of PCM samples is
 * written; no extra look-ahead/delay samples are appended.
 */

#include "speex/speex.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAME_SAMPLES 640
#define MAX_PACKET_BYTES  512

static uint16_t rd_u16(const unsigned char *p)
{
   return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32(const unsigned char *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
          ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_u16(FILE *f, uint16_t v)
{
   unsigned char b[2];
   b[0] = (unsigned char)(v & 0xff);
   b[1] = (unsigned char)((v >> 8) & 0xff);
   if (fwrite(b, 1, 2, f) != 2) { perror("write"); exit(1); }
}

static void wr_u32(FILE *f, uint32_t v)
{
   unsigned char b[4];
   b[0] = (unsigned char)(v & 0xff);
   b[1] = (unsigned char)((v >> 8) & 0xff);
   b[2] = (unsigned char)((v >> 16) & 0xff);
   b[3] = (unsigned char)((v >> 24) & 0xff);
   if (fwrite(b, 1, 4, f) != 4) { perror("write"); exit(1); }
}

/* Locate the fmt and data chunks of a PCM WAVE file and validate the format.
 * Leaves the file positioned at the first PCM sample on success. */
static int open_input_wav(FILE *f, int *rate, uint32_t *data_bytes)
{
   unsigned char hdr[12];
   int have_fmt = 0, have_data = 0;
   uint16_t fmt_code, channels, bits;
   uint32_t sample_rate, byte_rate, block_align;

   if (fread(hdr, 1, 12, f) != 12) return -1;
   if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
      return -1;

   while (!have_data) {
      unsigned char ch[8];
      uint32_t size;
      if (fread(ch, 1, 8, f) != 8) return -1;
      size = rd_u32(ch + 4);

      if (memcmp(ch, "fmt ", 4) == 0) {
         unsigned char fmt[16];
         if (size < 16 || fread(fmt, 1, 16, f) != 16) return -1;
         fmt_code = rd_u16(fmt + 0);
         channels = rd_u16(fmt + 2);
         sample_rate = rd_u32(fmt + 4);
         byte_rate = rd_u32(fmt + 8);
         block_align = rd_u16(fmt + 12);
         bits = rd_u16(fmt + 14);
         if (fmt_code != 1 || channels != 1 || bits != 16) return -1;
         if (sample_rate != 8000 && sample_rate != 16000 &&
             sample_rate != 32000) return -1;
         if (byte_rate != sample_rate * 2 || block_align != 2) return -1;
         *rate = (int)sample_rate;
         have_fmt = 1;
         /* Skip any extension bytes and the odd-size padding byte. */
         if (size > 16 && fseek(f, (long)(size - 16), SEEK_CUR) != 0) return -1;
         if (size & 1u) { if (fgetc(f) == EOF) return -1; }
      } else if (memcmp(ch, "data", 4) == 0) {
         if (!have_fmt) return -1;
         *data_bytes = size;
         have_data = 1;
      } else {
         /* Unknown chunk: skip body and its padding byte. */
         if (fseek(f, (long)size, SEEK_CUR) != 0) return -1;
         if (size & 1u) { if (fgetc(f) == EOF) return -1; }
      }
   }
   return 0;
}

static void write_output_header(FILE *f, int rate)
{
   if (fwrite("RIFF", 1, 4, f) != 4) { perror("write"); exit(1); }
   wr_u32(f, 0);                                  /* patched at close */
   if (fwrite("WAVEfmt ", 1, 8, f) != 8) { perror("write"); exit(1); }
   wr_u32(f, 16);
   wr_u16(f, 1);                                  /* PCM */
   wr_u16(f, 1);                                  /* mono */
   wr_u32(f, (uint32_t)rate);
   wr_u32(f, (uint32_t)rate * 2);                 /* byte rate */
   wr_u16(f, 2);                                  /* block align */
   wr_u16(f, 16);                                 /* bits per sample */
   if (fwrite("data", 1, 4, f) != 4) { perror("write"); exit(1); }
   wr_u32(f, 0);                                  /* patched at close */
}

int main(int argc, char **argv)
{
   FILE *in, *out;
   const SpeexMode *mode;
   void *enc, *dec;
   SpeexBits bits;
   int rate, frame_size, cut;
   uint32_t data_bytes, total_samples, samples_written = 0;
   long data_size_pos, riff_size_pos;
   spx_int16_t frame_in[MAX_FRAME_SAMPLES];
   spx_int16_t frame_out[MAX_FRAME_SAMPLES];
   char packet[MAX_PACKET_BYTES];
   char *endp;
   unsigned frame_index = 0;

   if (argc != 4) {
      fprintf(stderr, "usage: %s INPUT.wav OUTPUT.wav CUT\n", argv[0]);
      return 2;
   }
   if (strcmp(argv[1], argv[2]) == 0) {
      fprintf(stderr, "input and output paths must differ\n");
      return 2;
   }
   cut = (int)strtol(argv[3], &endp, 10);
   if (endp == argv[3] || *endp != '\0' || cut <= 0) {
      fprintf(stderr, "CUT must be a positive integer\n");
      return 2;
   }

   in = fopen(argv[1], "rb");
   if (!in) { perror(argv[1]); return 1; }
   if (open_input_wav(in, &rate, &data_bytes) != 0) {
      fprintf(stderr, "unsupported or corrupted WAVE: need mono PCM16 at 8/16/32 kHz\n");
      fclose(in);
      return 1;
   }
   total_samples = data_bytes / 2u;

   if (rate == 8000) mode = speex_lib_get_mode(SPEEX_MODEID_NB);
   else if (rate == 16000) mode = speex_lib_get_mode(SPEEX_MODEID_WB);
   else mode = speex_lib_get_mode(SPEEX_MODEID_UWB);

   enc = speex_encoder_init(mode);
   dec = speex_decoder_init(mode);
   if (!enc || !dec) {
      fprintf(stderr, "failed to initialise Speex\n");
      return 1;
   }
   speex_encoder_ctl(enc, SPEEX_GET_FRAME_SIZE, &frame_size);
   if (frame_size > MAX_FRAME_SAMPLES) {
      fprintf(stderr, "unexpected frame size %d\n", frame_size);
      return 1;
   }
   speex_bits_init(&bits);

   out = fopen(argv[2], "wb");
   if (!out) { perror(argv[2]); return 1; }
   write_output_header(out, rate);
   riff_size_pos = 4;
   data_size_pos = 40;

   while (samples_written < total_samples) {
      uint32_t want = (uint32_t)frame_size;
      uint32_t remaining = total_samples - samples_written;
      uint32_t take = want < remaining ? want : remaining;
      size_t got;
      int packet_bytes, i;

      got = fread(frame_in, sizeof(spx_int16_t), (size_t)take, in);
      if (got != (size_t)take) {
         fprintf(stderr, "short read in PCM data\n");
         return 1;
      }
      for (i = (int)take; i < frame_size; ++i)
         frame_in[i] = 0;                 /* zero pad the final frame */

      /* Reset the persistent pair at every CUT-frame boundary. */
      if (frame_index > 0 && (frame_index % (unsigned)cut) == 0) {
         if (speex_encoder_ctl(enc, SPEEX_RESET_STATE, NULL) != 0 ||
             speex_decoder_ctl(dec, SPEEX_RESET_STATE, NULL) != 0) {
            fprintf(stderr, "SPEEX_RESET_STATE failed\n");
            return 1;
         }
      }

      speex_bits_reset(&bits);
      if (speex_encode_int(enc, frame_in, &bits) < 0) {
         fprintf(stderr, "encode failed\n");
         return 1;
      }
      packet_bytes = speex_bits_write(&bits, packet, MAX_PACKET_BYTES);
      if (packet_bytes <= 0) {
         fprintf(stderr, "empty packet\n");
         return 1;
      }
      speex_bits_read_from(&bits, packet, packet_bytes);
      if (speex_decode_int(dec, &bits, frame_out) != 0) {
         fprintf(stderr, "decode failed\n");
         return 1;
      }

      if (fwrite(frame_out, sizeof(spx_int16_t), (size_t)take, out) != (size_t)take) {
         perror("write");
         return 1;
      }
      samples_written += take;
      ++frame_index;
   }

   /* Patch the RIFF and data chunk sizes now that the length is known. */
   if (fseek(out, data_size_pos, SEEK_SET) != 0) { perror("seek"); return 1; }
   wr_u32(out, samples_written * 2u);
   if (fseek(out, riff_size_pos, SEEK_SET) != 0) { perror("seek"); return 1; }
   wr_u32(out, 36u + samples_written * 2u);

   speex_bits_destroy(&bits);
   speex_encoder_destroy(enc);
   speex_decoder_destroy(dec);
   if (fclose(in) != 0) { perror("close input"); return 1; }
   if (fclose(out) != 0) { perror("close output"); return 1; }
   return 0;
}
