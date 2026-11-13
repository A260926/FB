minih264
==========

[![Build Status](https://travis-ci.org/lieff/minih264.svg)](https://travis-ci.org/lieff/minih264)

Small, but yet reasonably fast H264/SVC encoder single-header library with SSE/NEON optimizations.
Decoder can be popped up in future.

Disclaimer: code highly experimental.

## Comparison with [x264](https://www.videolan.org/developers/x264.html)

Rough comparison with x264 on an i7-6700K:

`x264 -I 30 --profile baseline --preset veryfast --tune zerolatency -b 0 -r 1 --qp 33 --ipratio 1.0 --qcomp 1.0 -o x264.264 --fps 30 vectors/foreman.cif --input-res 352x288 --slices 1 --threads 1`

vs

`./h264enc_x64 vectors/foreman.cif`

| x264         | minih264 |
| ------------ | -------- |
| source: ~4.6mb | 409kb |
| binary: 1.2mb | 100kb |
| time: 0,282s | 0,503s |
| out size: 320kb | 391kb  |

PSNR:
```
x264:     PSNR y:32.774824 u:38.874450 v:39.926132 average:34.084281 min:31.842667 max:36.630286
minih264: PSNR y:33.321686 u:38.858879 v:39.955914 average:34.574459 min:32.389171 max:37.174073
```

First intra frame screenshot (left-to-right: original 152064, minih264 5067, x264 5297 bytes):

![Intra screenshot](images/intra.png?raw=true)

You can compare results in motion using ffplay/mpv players on vectors/out_ref.264 and vectors/x264.264 .

## Usage

```c
#include "minih264e.h"

H264E_create_param_t cp;
memset(&cp, 0, sizeof(cp));
cp.width = 64;
cp.height = 64;
cp.gop = 30;
cp.const_input_flag = 1;
cp.vbv_size_bytes = 8192;
cp.num_layers = 1;
cp.max_threads = 1;

/* Optional: embed constant frame-rate timing and NAL HRD signaling.
 * Leave hrd_bitrate_bps at 0 for the legacy stream (no VUI, no SEI).
 * When enabled, the SPS gets timing_info plus one NAL HRD CPB, and each
 * IDR is preceded by a buffering_period SEI and every picture by a
 * pic_timing SEI, so the recorder needs no separate per-frame timing file.
 * Rate control is unchanged: qp_min/qp_max and desired_frame_bytes still
 * drive quantization and the existing VBV logic. */
cp.hrd_bitrate_bps = 64000;       /* 0 disables HRD signaling entirely */
cp.hrd_num_units_in_tick = 1001;  /* one frame = two ticks ... */
cp.hrd_time_scale = 60000;        /* ... => 60000 / (2 * 1001) ~= 29.97 fps */

int sizeof_persist, sizeof_scratch;
if (H264E_sizeof(&cp, &sizeof_persist, &sizeof_scratch) != H264E_STATUS_SUCCESS)
    return 1;                      /* invalid HRD combination -> BAD_PARAMETER */

H264E_persist_t *enc = malloc(sizeof_persist);
H264E_scratch_t *scratch = malloc(sizeof_scratch);
if (H264E_init(enc, &cp) != H264E_STATUS_SUCCESS)
    return 1;

unsigned char *yuv = malloc(64 * 64 * 3 / 2);
H264E_io_yuv_t in = {
    { yuv, yuv + 64 * 64, yuv + 64 * 64 * 5 / 4 },
    { 64, 32, 32 }
};

H264E_run_param_t rp;
memset(&rp, 0, sizeof(rp));
rp.qp_min = rp.qp_max = 30;
rp.desired_frame_bytes = 1000;     /* still supplied and honored */

for (;;)
{
    /* fill yuv with one I420 picture ... */
    unsigned char *out;
    int out_size;
    if (H264E_encode(enc, scratch, &rp, &in, &out, &out_size) != H264E_STATUS_SUCCESS)
        break;
    /* out is Annex-B: SPS/PPS, optional SEI, then slice NAL units */
}
```

HRD constraints (all rejected up front by `H264E_sizeof`/`H264E_init` with
`H264E_STATUS_BAD_PARAMETER`): when `hrd_bitrate_bps` is non-zero,
`hrd_num_units_in_tick` and `hrd_time_scale` must be non-zero,
`vbv_size_bytes` must be positive, `num_layers` must be 1 (no SVC) and
`max_threads` must not exceed 1. Bitrate and CPB size are encoded with the
smallest scale that represents them exactly (bases 64 and 16 bits); values
that cannot be represented are rejected rather than rounded. The signaling
describes constant frame-rate, no-reorder coding only; it does not by itself
guarantee any target size.

## SVC

Minih264 supports both spatial and temporal layers. Spatial layers are almost same as encode 2 independent AVC streams except for Intra frames prediction.
Following diagram shows minih264 SVC scheme for two spatial layers:

![SVC diargam](images/svc.png?raw=true)

That's because P frames spatial prediction are almost useless in practice. But for Intra frames there is a ~20% benefit in full resolution frame size.
Note that decoder must have both base layer I frame _and_ full resolution SVC I frame to decode whole sequence of next P frames in full resolution.

## Limitations

The following major features are not supported compared to x264 (baseline):

 * Trellis quantization.
 * Select prediction mode using Sum of Absolute Transform Differences (SATD).
 * 4x4 motion compensation.

## Interesting links

 * https://www.videolan.org/developers/x264.html
 * https://www.openh264.org/
 * https://github.com/cisco/openh264
 * http://iphome.hhi.de/suehring/tml/
 * https://github.com/oneam/h264bsd
 * https://github.com/fhunleth/hollowcore-h264
 * https://github.com/digetx/h264_decoder
 * https://github.com/lspbeyond/p264decoder
 * https://github.com/jcasal-homer/HomerHEVC
 * https://github.com/ultravideo/kvazaar
 * https://github.com/neocoretechs/h264j
 * https://github.com/jcodec/jcodec
