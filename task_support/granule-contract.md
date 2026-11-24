# Granule pull API

Add `granule.h` and `granule.c`. The public header includes stddef.h, stdint.h,
stream.h and synth.h. Implement these declarations (the opaque object owns all
input and codec state):

```c
struct mad_granule_decoder;
enum mad_granule_status {
  MAD_GRANULE_ERROR = -1, MAD_GRANULE_NEED_INPUT = 0,
  MAD_GRANULE_PCM = 1, MAD_GRANULE_END = 2
};
struct mad_granule_progress {
  uint64_t granules, physical_frames, sample_frames;
  size_t buffered_bytes;
  enum mad_error error;
};
struct mad_granule_decoder *mad_granule_new(int options);
void mad_granule_free(struct mad_granule_decoder *);
int mad_granule_feed(struct mad_granule_decoder *, const unsigned char *,
                     size_t length, size_t *accepted);
int mad_granule_finish(struct mad_granule_decoder *);
int mad_granule_next(struct mad_granule_decoder *, struct mad_pcm *);
int mad_granule_progress(const struct mad_granule_decoder *,
                          struct mad_granule_progress *);
void mad_granule_reset(struct mad_granule_decoder *);
```

new accepts 0, MAD_OPTION_IGNORECRC, MAD_OPTION_HALFSAMPLERATE and their OR;
unsupported bits or allocation failure return NULL. feed/finish/progress return
0 on success, -1 on error. feed sets *accepted to copied bytes, including 0
under backpressure. NULL data is permitted only when length=0. Each instance
accepts at least 8192 bytes when empty; its owned compressed queue plus pending
frame bytes never exceeds 16384. No input pointer remains borrowed after feed.
All other decoder workspace has a constant size. feed performs no spectral or
synthesis work. finish is idempotent. No subsequent feed is accepted until reset.
Physical frame, header/CRC and sideinfo preparation may run in next; full-frame
lookahead is allowed but is not permission to decode future granules.

next copies one granule to caller-owned mad_pcm on PCM. Native output is 576
samples/channel; HALFSAMPLERATE gives 288 at half rate. Unused output entries
are unspecified. NEED_INPUT, END and ERROR set pcm.length=0 and do not modify
sample entries. END repeats without work. An empty finished stream returns END.
Partial physical frame at finish is ERROR with MAD_ERROR_BUFLEN. Do not silently
resynchronize, conceal a failed granule, or synthesize its partial channels.
An MPEG-1 frame contributes two granules; MPEG-2 one. Support fixed per-stream
version, sampling rate and channel count; reject unsupported headers and format
changes without delivering their PCM. Error mapping follows existing libmad;
no continuation is required after a bitstream error. Header/sideinfo/CRC may
fail before either granule; main-data errors are encountered only when the
corresponding granule is reached. A completed first granule is irrevocable.
The late-error fixture uses legal table 1 and a valid part2_3_length;
the second granule lacks the Huffman sign bit within that declared span.
It does not use a reserved table selector as the main-data witness.

progress has no side effects. granules counts successful granule synthesis,
sample_frames counts delivered samples per channel, physical_frames increases
only after the last granule of that physical frame and its reservoir finalization.
On stream error these counters remain at the successful prefix and error is the
actual mad_error. Invalid API arguments return -1 without latching a stream
error or altering codec state (NULL accepted/progress/pcm is invalid). free(NULL)
and reset(NULL) do nothing; reset keeps options and empties all other state.

Use the same granule decode primitive for old mad_frame_decode and the new
path, and the same range synthesis primitive for old/new output. Preserve old
full-frame error returns and default PCM exactly. Do not route the new entry
through full-frame decode, speculate the second granule, decode twice, or copy
full-history snapshots to simulate suspension. Keep the first granule's SCFSI
values and exact bit/remaining-length state until the second consumes them.
The reservoir's next-frame preload must not invalidate in-flight main data.

The fixed build force-includes task_support/work.h. Put
MAD_GRANULE_SPECTRAL_BEGIN() at the real granule core entry immediately before
its scalefactor/Huffman processing, and MAD_GRANULE_SYNTH_BEGIN() once at the
real 18-subband-step synthesis entry, outside channel loops. Both old and new
APIs use these hooks. They count attempts, including a failing granule, not
returned PCM or API calls. They are observation hooks, not production state;
when work.h is not injected, make the macros no-ops. Do not edit task_support.
The consumer checks that only one spectral/synthesis pass ran per successful
next, and that feeding/querying/NEED_INPUT do not run either stage. New public
symbols also belong in the repository's export lists.
