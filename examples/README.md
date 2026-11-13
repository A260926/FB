Examples
========
Examples of external files for use with Kvazaar.

## Region of interest (roi) files
A simple text file can be used with the `--roi` switch to setup regions of interest for encoding.
Header row of the file will tell how many regions the encoded frames are divided (columns, rows).
The header must be followed by a data row with number entries equal to columns * rows.
The data row will tell the encoder which delta QP value will be assigned to each region.
The included example file will split frames into four regions with the top regions having a delta QP of +5
```
2 2
5 5 0 0
```

## ROI-aware rate control (`--roi-rc`)

The classic ROI path adds every map value directly to the frame QP of the
matching CTU, bypassing the per-CTU rate model. `--roi-rc` instead keeps the
map as a *spatial budget* input to the existing lambda rate controller:

- It only works with a positive `--bitrate` and `--rc-algorithm=lambda`.
  Enabling it with any other combination fails `encoder_open` with an error;
  OBA keeps its own, unchanged ROI path.
- Both ROI input mechanisms are supported unchanged: a per-frame map on
  `picture.roi` (used by `roi_rc_example.c`) and the `--roi` text/binary
  file. A map that is present but all zero, or no map at all, follows the
  exact non-ROI rate control path.

### Spatial weights

Once per frame, before any CTU is coded, the encoder forms one full-frame
budget. For CTU *i* it takes the classic spatial weight `b_i` (uniform on
the first frame, the normalized complexity weight afterwards), the active
pixel area `p_i` and the ROI factor `r_i = 2^(-d_i/6)` from the mapped ROI
delta `d_i`, then computes

```
t_i = clamp(c * b_i * r_i, B*p_i/(4P), 4*B*p_i/P)
```

with a single non-negative `c` chosen so that `sum(t_i) = B`, where `B` is
the normal frame-level target and `P = sum(p_i)`. Individual zero weights
are lifted to a tiny area share first, so the normalization always has a
solution even when several CTUs saturate the lower or upper limit together.
The mapping is the same floor-based scaling as before: CTU `(x,y)` reads
index `(floor(x*roi_width/ctu_columns), floor(y*roi_height/ctu_rows))`,
including tile offsets.

Each CTU then converts its planned `t_i / p_i` to lambda through the
existing alpha/beta model and to QP, so the ROI delta is never added back
to QP. VAQ is applied afterwards as usual, and the actual search,
quantization, RDO and syntax writing all use the same final QP/lambda.

### The budget is not a bit cap

`t_i` is a *target*, not an upper bound. The bits a CTU really spends are
still measured from the CABAC bitstream; those real bits, together with the
final (VAQ-adjusted) lambda, train the alpha/beta model used for the next
frame. Changing the ROI map only changes the current frame's budget: it
neither resets the learned alpha/beta nor feeds the previous frame's
weighted budget back in as this frame's complexity weight. The budget is
per-frame state, so WPP, tiles and OWF never renormalize or overwrite it.

### Example program

`roi_rc_example.c` generates a moving I420 sequence and a per-frame changing
ROI map entirely in memory, encodes it through the real library with
`roi-rc` enabled, and writes both the HEVC bitstream (`<prefix>.hevc`) and
the reconstructed frames (`<prefix>_rec.yuv`):

```
cc -std=c99 -I<kvazaar-src>/src roi_rc_example.c -L<build-dir> \
   -lkvazaar -lm -lpthread -o roi_rc_example
./roi_rc_example output_prefix
``` 