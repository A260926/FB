/* Example: fixed per-frame bit budget with low-frequency-first allocation.
 *
 * Compile (against an in-tree build):
 *   cc -std=c99 -Iinclude -Ilibspeex -DHAVE_CONFIG_H \
 *      -include devcheck/include/config.h \
 *      doc/frame_budget_example.c .build/libcodec.a -lm -o frame_budget_example
 *
 * Run:
 *   ./frame_budget_example
 *
 * The program encodes a synthetic signal in all three modes with a sequence
 * of per-frame budgets, reports the bits each frame actually appended to the
 * SpeexBits stream, and decodes the result back. Every frame must stay within
 * its budget and every decoded sample must be finite.
 */
#include <speex/speex.h>
#include <speex/speex_bits.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

static void fill_pcm(short *pcm, int n, int frame)
{
   int i;
   for (i = 0; i < n; i++)
   {
      double t = (double)(frame * n + i);
      /* A tonal low band plus a noisy high band; never all zero. */
      pcm[i] = (short)(5000.0 * sin(t * 0.05) + 1500.0 * sin(t * 0.21)
                       + ((i * 37) % 101) - 50);
   }
}

static int run_mode(int mode_id, const char *name,
                    const int *budgets, int nframes)
{
   const SpeexMode *mode = speex_lib_get_mode(mode_id);
   void *enc = speex_encoder_init(mode);
   void *dec = speex_decoder_init(mode);
   SpeexBits bits, read;
   spx_int32_t fs = 0, on = 1;
   float vbr_quality = 8.f;
   short pcm[640], out[640];
   char data[8192];
   int i, frame, ok = 1;

   speex_encoder_ctl(enc, SPEEX_GET_FRAME_SIZE, &fs);
   /* The mode requires VBR (or ABR) and sub-mode encoding, both on by default
    * for sub-mode encoding. */
   speex_encoder_ctl(enc, SPEEX_SET_VBR, &on);
   speex_encoder_ctl(enc, SPEEX_SET_VBR_QUALITY, &vbr_quality);

   speex_bits_init(&bits);
   speex_bits_init(&read);

   /* A three-bit caller prefix, on a non-byte boundary. It is not part of the
    * audio budget. */
   speex_bits_pack(&bits, 5, 3);

   printf("%s:\n", name);
   for (frame = 0; frame < nframes; frame++)
   {
      spx_int32_t budget = budgets[frame];
      int before = bits.nbBits;
      int appended;

      /* The budget may be changed before every frame. */
      if (speex_encoder_ctl(enc, SPEEX_SET_VBR_FRAME_BITS, &budget) != 0)
      {
         printf("  frame %d: setting budget %d failed\n", frame, budget);
         ok = 0;
         break;
      }
      fill_pcm(pcm, fs, frame);
      if (speex_encode_int(enc, pcm, &bits) < 0)
      {
         printf("  frame %d: encode failed\n", frame);
         ok = 0;
         break;
      }
      appended = bits.nbBits - before;
      printf("  frame %d: budget %4d, wrote %4d bits\n", frame, budget, appended);
      if (appended > budget)
         ok = 0;
   }

   /* Decode the whole stream back. */
   {
      int len = speex_bits_write(&bits, data, sizeof(data));
      int decoded = 0;
      speex_bits_read_from(&read, data, len);
      speex_bits_advance(&read, 3);
      for (frame = 0; ok && frame < nframes; frame++)
      {
         int j;
         if (speex_decode_int(dec, &read, out) != 0)
         {
            ok = 0;
            break;
         }
         for (j = 0; j < fs; j++)
            if (!isfinite((double)out[j]))
               ok = 0;
         decoded++;
      }
      printf("  decoded %d of %d frames: %s\n", decoded, nframes,
             ok ? "ok" : "FAILED");
   }

   speex_bits_destroy(&bits);
   speex_bits_destroy(&read);
   speex_encoder_destroy(enc);
   speex_decoder_destroy(dec);
   return ok;
}

int main(void)
{
   /* Narrowband minimum is 43, wideband 47, ultra-wideband 51. */
   static const int nb_budgets[6] = { 43, 43, 119, 79, 200, 43 };
   static const int wb_budgets[6] = { 47, 117, 300, 47, 160, 47 };
   static const int uwb_budgets[6] = { 51, 51, 436, 300, 51, 560 };
   int ok = 1;

   ok &= run_mode(SPEEX_MODEID_NB, "narrowband", nb_budgets, 6);
   ok &= run_mode(SPEEX_MODEID_WB, "wideband", wb_budgets, 6);
   ok &= run_mode(SPEEX_MODEID_UWB, "ultra-wideband", uwb_budgets, 6);

   return ok ? 0 : 1;
}
