/*
 * Baseline demonstration against the ORIGINAL VisualOn AAC encoder.
 *
 * It encodes 96 ADTS frames and, halfway through, re-applies the very same
 * bit rate through the legacy VO_PID_AAC_ENCPARAM parameter. That path
 * reopens the encoding core, so the bit reservoir, fractional frame-length
 * phase and the threshold-adaptation history are all reset even though the
 * bit rate did not change. The internal state before and after is printed so
 * the reset is directly observable.
 *
 * Only legacy symbols and fields present in the original headers are used, so
 * this file compiles unchanged against the pristine library.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "voAAC.h"
#include "cmnMemory.h"
#include "aacenc_core.h"

#define FRAME_SAMPLES   1024
#define CHANNELS        2
#define TOTAL_FRAMES    96
#define RESET_AT        48
#define SAMPLE_RATE     44100
#define BIT_RATE        64000

static short gen_sample(int frame, int index, int channel)
{
    unsigned int t = (unsigned int) (frame * FRAME_SAMPLES + index);
    int v = (int) ((t * 263u + (unsigned int) channel * 719u) % 2001u) - 1000;
    v += (int) ((t * 97u + (unsigned int) channel * 53u) % 401u) - 200;
    return (short) v;
}

static void snapshot(const char *tag, AAC_ENCODER *e)
{
    ATS_ELEMENT *a = &e->qcKernel.adjThr.adjThrStateElem;
    printf("  %-28s bitRate=%-7d phase=%-7lld reservoir=%-6d/%d "
           "peLast=%-5d dynBitsLast=%-5d corr=%d\n",
           tag,
           e->config.bitRate,
           (long long) e->qcKernel.padding.paddingRest,
           e->qcKernel.elementBits.bitResLevel,
           e->qcKernel.elementBits.maxBitResBits,
           a->peLast, a->dynBitsLast, a->peCorrectionFactor);
}

int main(int argc, char **argv)
{
    const char *out_path = (argc > 1) ? argv[1] : "baseline.aac";
    VO_AUDIO_CODECAPI api;
    VO_MEM_OPERATOR memop;
    VO_CODEC_INIT_USERDATA user_data;
    VO_HANDLE handle = NULL;
    AACENC_PARAM params;
    FILE *out;
    short pcm[FRAME_SAMPLES * CHANNELS];
    unsigned char bitstream[8192];
    int frame, frames_written = 0;

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
        fprintf(stderr, "init failed\n");
        return 1;
    }

    memset(&params, 0, sizeof(params));
    params.sampleRate = SAMPLE_RATE;
    params.bitRate    = BIT_RATE;
    params.nChannels  = CHANNELS;
    params.adtsUsed   = 1;
    if (api.SetParam(handle, VO_PID_AAC_ENCPARAM, &params) != VO_ERR_NONE) {
        fprintf(stderr, "initial parameters rejected\n");
        return 1;
    }

    out = fopen(out_path, "wb");
    if (!out) { perror(out_path); return 1; }

    printf("legacy VO_PID_AAC_ENCPARAM path, identical %d bit/s re-applied at "
           "frame %d\n", BIT_RATE, RESET_AT);

    for (frame = 0; frame < TOTAL_FRAMES; frame++) {
        VO_CODECBUFFER input, output;
        VO_AUDIO_OUTPUTINFO out_info;
        int i, ch;

        if (frame == RESET_AT) {
            AAC_ENCODER *e = (AAC_ENCODER *) handle;
            printf("before legacy re-set at frame %d:\n", frame);
            snapshot("accumulated state", e);
            /* same config, but the legacy entry point reopens the core */
            if (api.SetParam(handle, VO_PID_AAC_ENCPARAM, &params) != VO_ERR_NONE) {
                fprintf(stderr, "legacy re-set failed\n");
                return 1;
            }
            printf("after  legacy re-set at frame %d:\n", frame);
            snapshot("core reopened -> reset", e);
        }

        for (i = 0; i < FRAME_SAMPLES; i++)
            for (ch = 0; ch < CHANNELS; ch++)
                pcm[i * CHANNELS + ch] = gen_sample(frame, i, ch);

        memset(&input, 0, sizeof(input));
        input.Buffer = (VO_PBYTE) pcm;
        input.Length = (VO_U32) sizeof(pcm);
        api.SetInputData(handle, &input);

        memset(&output, 0, sizeof(output));
        output.Buffer = bitstream;
        output.Length = sizeof(bitstream);
        if (api.GetOutputData(handle, &output, &out_info) != VO_ERR_NONE) {
            fprintf(stderr, "encode failed at frame %d\n", frame);
            return 1;
        }
        fwrite(bitstream, 1, output.Length, out);
        frames_written++;
    }

    fclose(out);
    api.Uninit(handle);

    printf("wrote %d ADTS frames to %s\n", frames_written, out_path);
    if (frames_written != TOTAL_FRAMES) {
        fprintf(stderr, "expected %d frames\n", TOTAL_FRAMES);
        return 1;
    }
    return 0;
}
