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

### Progressive intra refresh

For low-latency transport over lossy links, a periodic IDR is a large, bursty
key frame and a single lost reference picture makes every following P frame
drift until the next IDR. Progressive intra refresh (PIR) removes that
weakness: every P frame forces one horizontal band of macroblocks to intra
coding, and the band walks from the top row to the bottom, then wraps. After
one full cycle the decoder's reference picture is identical to the encoder's
again, with no extra IDR and no error concealment.

Enable it by setting `intra_refresh_rows` to the number of macroblock rows
refreshed per P frame:

```c
#include "minih264e.h"

H264E_create_param_t cp;
memset(&cp, 0, sizeof(cp));
cp.width  = 64;
cp.height = 96;                 /* 6 macroblock rows */
cp.num_layers = 1;              /* single AVC layer   */
cp.intra_refresh_rows = 2;      /* refresh 2 rows per P frame -> 3 P frames/cycle */
/* gop stays 0 (infinite), no long-term refs, no VBV, no temporal denoise,
 * max_threads <= 1, and width/height must be multiples of 16. Any other
 * combination with intra_refresh_rows > 0 is rejected at sizeof/init. */

int sizeof_persist, sizeof_scratch;
if (H264E_sizeof(&cp, &sizeof_persist, &sizeof_scratch) != H264E_STATUS_SUCCESS)
    abort();
void *enc = malloc(sizeof_persist);
void *scratch = malloc(sizeof_scratch);
H264E_init(enc, &cp);

H264E_run_param_t rp;
memset(&rp, 0, sizeof(rp));
rp.qp_min = rp.qp_max = 30;
rp.encode_speed = 5;
/* only H264E_FRAME_TYPE_DEFAULT, _KEY and _P are accepted while PIR is on */

H264E_io_yuv_t io; /* point at a 16-aligned planar YUV 4:2:0 input ... */

for (;;)
{
    unsigned char *out;
    int n;
    if (H264E_encode(enc, scratch, &rp, &io, &out, &n) != H264E_STATUS_SUCCESS)
        abort();
    /* `out` now holds one complete Annex-B access unit (start codes included),
       `n` bytes long; hand it to your transport/sink. */
}
```

How many frames make one cycle: a picture of `H/16` macroblock rows refreshed
`r` rows at a time recovers in `ceil((H/16) / r)` P frames. The final band of
a cycle may be shorter than `r`; the next P frame then starts a new band at
the top. An explicit `H264E_FRAME_TYPE_KEY` still emits a normal IDR and
restarts the band position at row zero.

Why forcing one band to intra is not enough by itself: macroblocks above the
band are still coded inter, and an ordinary motion search could reach down
into rows that were never refreshed in the current cycle, pulling corrupted
samples back into the trusted area. The encoder therefore constrains every
inter motion vector so that its *whole* interpolation support — including the
6-tap luma filter wings on fractional positions and the bilinear chroma cell —
stays inside the already trusted prefix. Integer and fractional motion search
otherwise run unchanged; when no safe inter candidate exists the macroblock
falls back to intra rather than being forced intra across the whole picture.
A slice boundary (with `disable_deblocking_filter_idc == 2`) is emitted on the
band's lower edge so that intra neighbour availability and deblocking follow
the same rules in the bitstream and in the encoder's local reconstruction;
soft `desired_nalu_bytes` splits remain supported and are row-aligned.

#### PIR versus the existing GOLDEN/RECOVERY frames

Both target packet loss, but they rely on different mechanisms:

 * **GOLDEN/RECOVERY** need a long-term reference frame that both sides keep.
   A recovery P frame points back at that stored long-term picture, so the
   receiver must retain an extra reference and the stream carries long-term
   reference-marking syntax. Recovery only converges while that long-term
   picture itself stays intact.
 * **Progressive intra refresh** uses only the ordinary short-term reference.
   No extra reference picture or long-term marking is needed (the feature is
   rejected if `max_long_term_reference_frames != 0`). A decoder that lost its
   previous short-term picture re-synchronizes purely from the intra bands and
   the constrained inter prediction, one horizontal strip per frame, until the
   whole picture matches the encoder again after a full cycle.


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
