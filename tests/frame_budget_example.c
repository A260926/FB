/* frame_budget_example.c - using the per-frame VBR/ABR bit budget
 *
 * Build (after the library is built):
 *   cc -std=c99 -O2 -Iinclude frame_budget_example.c libspeex.a -lm
 *
 * The budget is the number of audio bits one encode call may append to the
 * SpeexBits structure: the narrowband CELP frame plus the mode header of every
 * higher layer.  Bits already in the stream before the call, byte-alignment
 * padding, the terminator and in-band control messages are not counted.
 *
 * A positive budget requires VBR or ABR to be on and submode encoding on.
 * It may be changed between frames.  Setting it to 0 restores the legacy
 * layered VBR_MAX_BITRATE behaviour.
 */
#include <speex/speex.h>
#include <speex/speex_bits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
   const SpeexMode *mode = speex_lib_get_mode(SPEEX_MODEID_WB);
   void *enc = speex_encoder_init(mode);
   void *dec = speex_decoder_init(mode);
   SpeexBits bits, read;
   spx_int32_t frame_size = 0, on = 1, budget, got;
   float vbr_quality = 8.f;
   short *pcm, *out;
   int f, i;

   if (!enc || !dec)
      return 1;

   speex_encoder_ctl(enc, SPEEX_GET_FRAME_SIZE, &frame_size);
   speex_encoder_ctl(enc, SPEEX_SET_VBR, &on);
   speex_encoder_ctl(enc, SPEEX_SET_VBR_QUALITY, &vbr_quality);
   speex_encoder_ctl(enc, SPEEX_SET_SUBMODE_ENCODING, &on);

   pcm = (short *)malloc(frame_size * sizeof(short));
   out = (short *)malloc(frame_size * sizeof(short));
   speex_bits_init(&bits);
   speex_bits_init(&read);

   /* Alternate a tight and a loose budget from frame to frame. */
   for (f = 0; f < 20; f++) {
      int bytes;
      char data[2048];

      budget = (f & 1) ? 320 : 96;
      if (speex_encoder_ctl(enc, SPEEX_SET_VBR_FRAME_BITS, &budget) != 0) {
         fprintf(stderr, "budget %d rejected\n", budget);
         return 1;
      }
      speex_encoder_ctl(enc, SPEEX_GET_VBR_FRAME_BITS, &got);

      /* Make a simple, non-zero waveform. */
      for (i = 0; i < frame_size; i++)
         pcm[i] = (short)(((i * 37 + f * 11) % 4000) - 2000);

      speex_bits_reset(&bits);
      if (speex_encode_int(enc, pcm, &bits) < 0) {
         fprintf(stderr, "frame %d did not fit its budget\n", f);
         return 1;
      }

      bytes = speex_bits_write(&bits, data, sizeof(data));
      speex_bits_read_from(&read, data, bytes);
      if (speex_decode_int(dec, &read, out) != 0) {
         fprintf(stderr, "frame %d failed to decode\n", f);
         return 1;
      }
      printf("frame %2d: budget=%d bits=%d bytes=%d decoded\n",
             f, got, bits.nbBits, bytes);
   }

   /* Turn the budget off; the encoder is back on the legacy path. */
   budget = 0;
   speex_encoder_ctl(enc, SPEEX_SET_VBR_FRAME_BITS, &budget);

   speex_bits_destroy(&bits);
   speex_bits_destroy(&read);
   speex_encoder_destroy(enc);
   speex_decoder_destroy(dec);
   free(pcm);
   free(out);
   return 0;
}
