# Dual subband stem decoding for dr_mp3

This adds a two-path ("stems") decoder on top of the `dr_mp3` floating-point build.
One MP3 stream is decoded once and split into two independent float outputs — a **low**
path and a **high** path — suitable for previewing the two frequency bands separately,
including after seeking.

## Where the split happens

Per MP3 granule the ordinary shared stages run exactly once: frame sync, bit
reservoir, Huffman, joint-stereo (M/S and intensity) reconstruction, short-window
reorder and antialiasing.

The split is applied **after antialiasing and before the IMDCT**. At that point the
granule spectrum is 32 polyphase subbands of 18 coefficients per channel. For a cut
value `cut` (0..32):

- subbands `[0, cut)` are routed to the **low** path;
- subbands `[cut, 32)` are routed to the **high** path.

Each path then runs its own continuation of the pipeline with independent state:

- its own IMDCT overlap history (`mdct_overlap`), and
- its own polyphase/QMF synthesis history (`qmf_state`).

Because filtering is linear and every subband is assigned to exactly one path, the two
outputs add sample-by-sample back to the native floating-point decode. No PCM is FFT
split, the stream is never decoded twice, and the native PCM is never proportionally
divided. When a path receives no new spectrum for a band (because the cut moved that
band to the other path), its retained overlap and synthesis history keep emitting the
natural decaying tail instead of being cleared.

Boundary cuts are meaningful: `cut = 32` puts everything in the low path (high is
silent) and `cut = 0` puts everything in the high path (low is silent).

## Cut schedule coordinates

The high-level session takes an ordered, copied schedule of points:

```c
typedef struct { drmp3_uint64 granule; unsigned low_subbands; } drmp3_stem_point;
```

- `granule` is counted in **absolute audio granules from the start of audio**. Every
  MPEG-1 Layer III physical frame contributes two granules, so the first frame's
  granules are 0 and 1, the second frame's are 2 and 3, and so on.
- A Xing/Info tag frame is **not** an audio granule and is not counted.
- From `granule` onwards (until the next point) the low path owns
  `[0, low_subbands)` and the high path owns `[low_subbands, 32)`.
- The first point must be at granule `0`; granule numbers must be strictly increasing
  and every `low_subbands` value must be in 0..32.
- A cut change takes effect at that granule's IMDCT. The decoder never clears the
  outgoing path's overlap or synthesis history, so the old tail rings out.

## PCM clock, gapless trimming and seeking

`drmp3_stems_tell()` and `drmp3_stems_length()` count **trimmed per-channel PCM
frames** — not compressed frames and not granules. The native gapless delay/padding
interpretation (from the Info/LAME tag when present) is applied exactly once on the
single PCM clock shared by both paths.

`drmp3_stems_seek()` takes a trimmed PCM frame index in `0..length`. Seeking to the
end is legal; an out-of-range or invalid request fails and leaves the prior state
untouched, and a later seek can leave EOF. To make a seek exact without replaying from
the beginning or storing whole-song PCM, the session keeps only:

- the copied cut schedule, and
- an index of compressed-frame byte offsets with their source granule numbers.

A seek restarts decoding at most 64 physical audio frames before the target so the bit
reservoir and both paths' histories are rebuilt, then discards frames up to the target.
The cut schedule is always evaluated at absolute granules — it is never restarted at
the seek point — so tails surviving a cut change are preserved across seeks. All decode
work buffers hold at most one MP3 frame, so working memory is independent of song
length, and separate sessions do not interfere.

## APIs

Low level (in `dr_mp3.h`, float builds):

```c
int drmp3dec_decode_frame_stems(drmp3dec *dec, const drmp3_uint8 *mp3, int mp3_bytes,
    float *low, float *high, drmp3dec_frame_info *info, const unsigned cuts[2]);
```

`cuts[0]`/`cuts[1]` are the cuts of the frame's two granules. Call `drmp3dec_init()`
before switching a decoder instance between native and stems mode.

High level (in `dr_mp3_stems.h`):

```c
drmp3_stems *drmp3_stems_open_memory(const void *data, size_t size,
                                     const drmp3_stem_point *points, size_t count);
void         drmp3_stems_close(drmp3_stems *s);
drmp3_uint64 drmp3_stems_read(drmp3_stems *s, drmp3_uint64 frames, float *low, float *high);
int          drmp3_stems_seek(drmp3_stems *s, drmp3_uint64 frame);
drmp3_uint64 drmp3_stems_tell(const drmp3_stems *s);
drmp3_uint64 drmp3_stems_length(const drmp3_stems *s);
unsigned     drmp3_stems_channels(const drmp3_stems *s);
unsigned     drmp3_stems_sample_rate(const drmp3_stems *s);
```

The input buffer is borrowed for the session lifetime and is never modified. A read
delivers the same number of complete channel frames to both outputs; a zero-capacity
read is allowed with NULL buffers and does not advance the cursor.

## Building and testing

```sh
bash task_support/run.sh target
```

This compiles the new header/source and fixed consumers, runs the `split`, `timeline`
and `seek` test groups, the example and an API probe, and finally the native baseline.
Everything is C and shell and needs no sound card or external decoder. The example
lives in `examples/stems.c`.
