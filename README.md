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

### NAL HRD timing signaling

By default the encoder writes no VUI/HRD information. A single-layer AVC
session can optionally embed constant frame-rate timing directly in the
bitstream: the SPS gains VUI timing info and a one-CPB NAL HRD block, every
IDR picture is preceded by a `buffering_period` SEI and every picture by a
`pic_timing` SEI, so the recording side needs no separate per-frame timing
file. Set `hrd_bitrate_bps` to a non-zero value to enable it; leave it zero
for the legacy, byte-identical behavior.

When enabled, `hrd_num_units_in_tick` and `hrd_time_scale` must be non-zero,
`vbv_size_bytes` must be positive, `num_layers` must be 1 and `max_threads`
must not exceed 1. The bitrate and CPB size have to be exactly representable
in the HRD base units (64-bit and 16-bit respectively), otherwise
`H264E_sizeof`/`H264E_init` reject the configuration. Rate control is
unchanged: keep supplying `desired_frame_bytes` as before.

```c
#include "minih264e.h"

H264E_create_param_t cp;
memset(&cp, 0, sizeof(cp));
cp.width  = 640;
cp.height = 480;
cp.gop    = 30;
cp.const_input_flag      = 1;
cp.vbv_size_bytes        = 200000;   /* positive, defines the CPB size */
cp.num_layers            = 1;        /* single-layer AVC only          */
cp.max_threads           = 1;        /* no multi-thread mode           */

/* 1 Mbit/s, 25 fps: a frame is two clock ticks (num_units_in_tick=1,
   time_scale=50 gives a 50 Hz tick clock, i.e. 25 frames per second). */
cp.hrd_bitrate_bps        = 1000000;
cp.hrd_num_units_in_tick  = 1;
cp.hrd_time_scale         = 50;

int sizeof_persist = 0, sizeof_scratch = 0;
int error = H264E_sizeof(&cp, &sizeof_persist, &sizeof_scratch);
if (error) { /* H264E_STATUS_BAD_PARAMETER: unsupported HRD combination */ }

H264E_persist_t  *enc     = malloc(sizeof_persist);
H264E_scratch_t  *scratch = malloc(sizeof_scratch);
if (H264E_init(enc, &cp)) { /* handle error */ }

H264E_run_param_t rp;
memset(&rp, 0, sizeof(rp));
rp.qp_min = 10;
rp.qp_max = 50;
rp.desired_frame_bytes = cp.hrd_bitrate_bps / 8 / 25; /* still caller-owned */

H264E_io_yuv_t frame;
/* ... fill frame.yuv[] / frame.stride[] with an I420 picture ... */

unsigned char *coded;
int coded_size;
/* Annex-B output now contains SPS(VUI/HRD)+PPS+SEI timing before the VCL */
error = H264E_encode(enc, scratch, &rp, &frame, &coded, &coded_size);
```

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
