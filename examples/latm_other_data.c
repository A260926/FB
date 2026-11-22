/* -----------------------------------------------------------------------------
 * Example: attach short timing markers as AudioMuxElement otherData to a
 * LOAS AAC-LC stream using aacEncSetLatmOtherData().
 *
 * The marker is carried outside the AAC access unit (after the payload,
 * before the element's byte alignment), so it does not alter the coded audio.
 * It is stored inside the encoder and can be replaced or removed at any time;
 * the change takes effect at the start of the next fully emitted
 * AudioMuxElement.
 *
 * Build (against the built static library):
 *   cc -std=c99 -O2 \
 *      -I. -Icpp/libAACenc/include -Icpp/libSYS/include \
 *      -Icpp/libFDK/include -Icpp/libMpegTPEnc/include \
 *      examples/latm_other_data.c .task-build/upstream/libfdk-aac.a \
 *      -lm -lstdc++ -o latm_other_data_example
 * ---------------------------------------------------------------------------*/

#include <stdio.h>
#include <string.h>

#include "aacenc_lib.h"

#define SAMPLE_RATE 48000
#define CHANNELS 2
#define FRAME_SAMPLES 1024

int main(void) {
  HANDLE_AACENCODER encoder = NULL;
  AACENC_BufDesc in_buf, out_buf;
  AACENC_InArgs in_args;
  AACENC_OutArgs out_args;
  INT_PCM pcm[FRAME_SAMPLES * CHANNELS];
  unsigned char bitstream[16384];

  int in_id = IN_AUDIO_DATA, out_id = OUT_BITSTREAM_DATA;
  int in_size = (int)sizeof(pcm), out_size = (int)sizeof(bitstream);
  int in_elem = (int)sizeof(INT_PCM), out_elem = 1;
  void *in_ptr = pcm, *out_ptr = bitstream;
  int frame;

  /* A non-zero, distinctive marker. 96 bits = 12 bytes. The bytes are read
     most-significant-bit first; unused low bits of the final byte are
     ignored. */
  unsigned char marker[12] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB,
                              0xCD, 0xEF, 0xFE, 0xDC, 0xBA, 0x98};
  const UINT marker_bits = 96;

  if (aacEncOpen(&encoder, 0, CHANNELS) != AACENC_OK) {
    fprintf(stderr, "aacEncOpen failed\n");
    return 1;
  }

  aacEncoder_SetParam(encoder, AACENC_AOT, AOT_AAC_LC);
  aacEncoder_SetParam(encoder, AACENC_SAMPLERATE, SAMPLE_RATE);
  aacEncoder_SetParam(encoder, AACENC_CHANNELMODE, MODE_2);
  aacEncoder_SetParam(encoder, AACENC_CHANNELORDER, 1);
  aacEncoder_SetParam(encoder, AACENC_BITRATEMODE, 0); /* CBR */
  aacEncoder_SetParam(encoder, AACENC_BITRATE, 192000);
  aacEncoder_SetParam(encoder, AACENC_TRANSMUX, TT_MP4_LOAS);
  aacEncoder_SetParam(encoder, AACENC_AUDIOMUXVER, 1);

  if (aacEncEncode(encoder, NULL, NULL, NULL, NULL) != AACENC_OK) {
    fprintf(stderr, "encoder initialization failed\n");
    aacEncClose(&encoder);
    return 1;
  }

  /* Attach the marker. It is copied into the encoder, so the local buffer can
     be reused immediately. A zero bit count with NULL removes any marker. */
  if (aacEncSetLatmOtherData(encoder, marker, marker_bits) != AACENC_OK) {
    fprintf(stderr, "aacEncSetLatmOtherData failed\n");
    aacEncClose(&encoder);
    return 1;
  }

  memset(&in_buf, 0, sizeof(in_buf));
  memset(&out_buf, 0, sizeof(out_buf));
  in_buf.numBufs = out_buf.numBufs = 1;
  in_buf.bufs = &in_ptr;
  out_buf.bufs = &out_ptr;
  in_buf.bufferIdentifiers = &in_id;
  out_buf.bufferIdentifiers = &out_id;
  in_buf.bufSizes = &in_size;
  out_buf.bufSizes = &out_size;
  in_buf.bufElSizes = &in_elem;
  out_buf.bufElSizes = &out_elem;

  for (frame = 0; frame < 20; frame++) {
    int sample;
    /* Continuous, non-silent PCM (a simple chirp). */
    for (sample = 0; sample < FRAME_SAMPLES * CHANNELS; sample++) {
      pcm[sample] =
          (INT_PCM)(12000.0 *
                    ((sample % 197) / 197.0 - 0.5));
    }

    memset(&in_args, 0, sizeof(in_args));
    memset(&out_args, 0, sizeof(out_args));
    in_args.numInSamples = FRAME_SAMPLES * CHANNELS;

    if (aacEncEncode(encoder, &in_buf, &out_buf, &in_args, &out_args) !=
        AACENC_OK) {
      fprintf(stderr, "encode failed at frame %d\n", frame);
      aacEncClose(&encoder);
      return 1;
    }

    if (out_args.numOutBytes > 0) {
      /* Each emitted buffer is one (or more) complete LOAS AudioMuxElement.
         The 96-bit marker is embedded in every element and the three leading
         bytes are the LOAS syncword (0x56E..) plus 13-bit mux length. */
      printf("frame %2d: %4d LOAS bytes, sync=0x%02X%02X, marker active\n",
             frame, out_args.numOutBytes, bitstream[0], bitstream[1]);
    }
  }

  /* Remove the marker; subsequent elements carry no otherData. */
  if (aacEncSetLatmOtherData(encoder, NULL, 0) != AACENC_OK) {
    fprintf(stderr, "marker removal failed\n");
    aacEncClose(&encoder);
    return 1;
  }
  printf("marker removed; subsequent elements have no otherData\n");

  aacEncClose(&encoder);
  return 0;
}
