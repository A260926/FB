/*
 * Continuous live bit-rate example for the VisualOn AAC-LC encoder.
 *
 * Generates interleaved stereo PCM with a fixed integer generator (no audio
 * device, no floating point), encodes a continuous ADTS stream and changes
 * the target bit rate twice through VO_PID_AAC_LIVE_BITRATE:
 *
 *   frames  0..31 :  64 kbit/s
 *   frames 32..63 :  96 kbit/s   (request issued before frame 32)
 *   frames 64..   :  64 kbit/s   (request issued before frame 64)
 *
 * The sample rate (44.1 kHz) and channel count (2) never change. The program
 * reports the first frame at which each requested rate actually takes effect
 * and writes one continuous ADTS file.
 *
 * Build: see README or devcheck/target.sh. Only the C library and libc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "voAAC.h"
#include "cmnMemory.h"

#define FRAME_SAMPLES   1024
#define CHANNELS        2
#define TOTAL_FRAMES    128
#define SWITCH_1        32
#define SWITCH_2        64
#define START_RATE      64000
#define MID_RATE        96000
#define SAMPLE_RATE     44100

/* Deterministic integer-only interleaved stereo sample generator. */
static short gen_sample(int frame, int index, int channel)
{
    unsigned int t = (unsigned int) (frame * FRAME_SAMPLES + index);
    int v = (int) ((t * 263u + (unsigned int) channel * 719u) % 2001u) - 1000;
    v += (int) ((t * 97u + (unsigned int) channel * 53u) % 401u) - 200;
    return (short) v;
}

int main(int argc, char **argv)
{
    const char *out_path = (argc > 1) ? argv[1] : "live_bitrate.aac";
    VO_AUDIO_CODECAPI api;
    VO_MEM_OPERATOR memop;
    VO_CODEC_INIT_USERDATA user_data;
    VO_HANDLE handle = NULL;
    AACENC_PARAM params;
    VO_S32 live_rate;
    FILE *out;
    short pcm[FRAME_SAMPLES * CHANNELS];
    unsigned char bitstream[8192];
    int frame, applied_mid = -1, applied_back = -1, current;

    voGetAACEncAPI(&api);
    memop.Alloc = cmnMemAlloc;
    memop.Copy  = cmnMemCopy;
    memop.Free  = cmnMemFree;
    memop.Set   = cmnMemSet;
    memop.Check = cmnMemCheck;

    memset(&user_data, 0, sizeof(user_data));
    user_data.memflag = VO_IMF_USERMEMOPERATOR;
    user_data.memData = &memop;
    if (api.Init(&handle, VO_AUDIO_CodingAAC, &user_data) != VO_ERR_NONE) {
        fprintf(stderr, "encoder init failed\n");
        return 1;
    }

    memset(&params, 0, sizeof(params));
    params.sampleRate = SAMPLE_RATE;
    params.bitRate    = START_RATE;
    params.nChannels  = CHANNELS;
    params.adtsUsed   = 1;
    if (api.SetParam(handle, VO_PID_AAC_ENCPARAM, &params) != VO_ERR_NONE) {
        fprintf(stderr, "initial encoder parameters rejected\n");
        return 1;
    }

    out = fopen(out_path, "wb");
    if (!out) {
        perror(out_path);
        return 1;
    }

    current = START_RATE;
    for (frame = 0; frame < TOTAL_FRAMES; frame++) {
        VO_CODECBUFFER input, output;
        VO_AUDIO_OUTPUTINFO out_info;
        VO_U32 ret;
        int i, ch;

        /* queue a continuous bit-rate change before the target frame */
        if (frame == SWITCH_1) {
            live_rate = MID_RATE;
            if (api.SetParam(handle, VO_PID_AAC_LIVE_BITRATE, &live_rate) != VO_ERR_NONE) {
                fprintf(stderr, "live bit-rate set to %d failed\n", MID_RATE);
                return 1;
            }
        } else if (frame == SWITCH_2) {
            live_rate = START_RATE;
            if (api.SetParam(handle, VO_PID_AAC_LIVE_BITRATE, &live_rate) != VO_ERR_NONE) {
                fprintf(stderr, "live bit-rate set to %d failed\n", START_RATE);
                return 1;
            }
        }

        for (i = 0; i < FRAME_SAMPLES; i++)
            for (ch = 0; ch < CHANNELS; ch++)
                pcm[i * CHANNELS + ch] = gen_sample(frame, i, ch);

        memset(&input, 0, sizeof(input));
        input.Buffer = (VO_PBYTE) pcm;
        input.Length = (VO_U32) sizeof(pcm);
        if (api.SetInputData(handle, &input) != VO_ERR_NONE) {
            fprintf(stderr, "SetInputData failed at frame %d\n", frame);
            return 1;
        }

        memset(&output, 0, sizeof(output));
        output.Buffer = bitstream;
        output.Length = sizeof(bitstream);
        ret = api.GetOutputData(handle, &output, &out_info);
        if (ret != VO_ERR_NONE) {
            fprintf(stderr, "GetOutputData failed at frame %d (0x%lx)\n",
                    frame, (unsigned long) ret);
            return 1;
        }
        fwrite(bitstream, 1, output.Length, out);

        /* report the frame at which each request becomes effective */
        if (api.GetParam(handle, VO_PID_AAC_LIVE_BITRATE, &live_rate) != VO_ERR_NONE) {
            fprintf(stderr, "GetParam failed at frame %d\n", frame);
            return 1;
        }
        if (live_rate != current) {
            if (current == START_RATE && applied_mid < 0)
                applied_mid = frame;
            else if (current == MID_RATE && applied_back < 0)
                applied_back = frame;
            current = live_rate;
        }
    }

    fclose(out);
    api.Uninit(handle);

    printf("wrote %d ADTS frames to %s (%d Hz, %d channels)\n",
           TOTAL_FRAMES, out_path, SAMPLE_RATE, CHANNELS);
    printf("bit rate effective: %d bit/s frames 0..%d\n", START_RATE, SWITCH_1 - 1);
    printf("                   %d bit/s from frame %d (requested at %d)\n",
           MID_RATE, applied_mid, SWITCH_1);
    printf("                   %d bit/s from frame %d (requested at %d)\n",
           START_RATE, applied_back, SWITCH_2);
    return 0;
}
