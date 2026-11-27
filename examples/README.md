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

Combine a ROI map with rate control using the `--roi-rc` switch. The ROI then
redistributes the frame bit budget before the lambda rate control derives the
per-CTU QP. It requires `--bitrate` and `--rc-algorithm=lambda`.

## ROI rate control example
`roi_rc_demo.c` generates synthetic I420 material and a per-frame changing ROI
map, encodes it with the real Kvazaar library, and writes the HEVC output and
the reconstructed frames. Build and run it from the repository root:

```
cc -std=c99 -Isrc -Ibuild examples/roi_rc_demo.c build/libkvazaar.a \
   -lm -pthread -o roi_rc_demo
./roi_rc_demo out.hevc out.yuv
```

(Use the matching build directory, e.g. `devcheck/build`, if that is where the
static library was produced.)
