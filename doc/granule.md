# Bounded granule pull decoding

`granule.h` / `granule.c` add a single-threaded pull interface to libmad for
MPEG-1 and MPEG-2 Layer III streams.  It decodes **one granule at a time**
instead of decoding a whole physical frame and then handing out its two PCM
halves.  The ordinary `mad_frame_decode()` / `mad_synth_frame()` entry points
are preserved and share the same granule and range-synthesis primitives, so
both paths produce bit-identical fixed-point output.

The interface is:

```c
struct mad_granule_decoder *mad_granule_new(int options);
void mad_granule_free(struct mad_granule_decoder *);
int  mad_granule_feed(struct mad_granule_decoder *, const unsigned char *,
                      size_t length, size_t *accepted);
int  mad_granule_finish(struct mad_granule_decoder *);
int  mad_granule_next(struct mad_granule_decoder *, struct mad_pcm *);
int  mad_granule_progress(const struct mad_granule_decoder *,
                          struct mad_granule_progress *);
void mad_granule_reset(struct mad_granule_decoder *);
```

`new()` accepts `0`, `MAD_OPTION_IGNORECRC`, `MAD_OPTION_HALFSAMPLERATE`, or
either OR-ed combination.  Any other option bit, or an allocation failure,
returns `NULL`.  The options are fixed for the lifetime of the object and are
preserved across `reset()`.

## Supported input

The decoder handles untagged MPEG-1 and MPEG-2 Layer III ordinary-bitrate
streams whose version, sampling rate, and channel count are fixed for the
whole stream.  It supports:

- single and dual channel, stereo, and joint (MS/intensity) stereo;
- variable bitrate and per-frame padding;
- CRC-protected frames;
- long, start, short, and stop block types, including mixed blocks.

Free-format streams, MPEG-2.5, Layer I/II, reserved header values, and
mid-stream format changes are rejected; their PCM is never delivered.  Only C
and the accompanying source are used; there are no third-party dependencies.

## Feeding and back-pressure

`feed()` copies only the compressed bytes it can accept and reports them
through `*accepted`.  It never borrows the caller's pointer: after `feed()`
returns, the caller may move or overwrite its input buffer.

- An empty instance accepts at least 8192 bytes.  The owned compressed queue
  plus any pending frame bytes never exceeds 16384 bytes, independent of
  stream length.
- Under back-pressure `feed()` succeeds and sets `*accepted` to the number of
  bytes copied, which may be `0`.  The caller keeps the remainder and feeds it
  later.
- `data == NULL` is allowed only when `length == 0`.
- `feed()` performs no spectral or synthesis work.

`finish()` is idempotent.  Once finished, no further `feed()` is accepted
(returns `-1`) until `reset()`; a zero-length feed after `finish()` is also
rejected.  A stream may be fed incrementally or entirely up front.

## Pulling granules

`mad_granule_next()` returns one of:

| status | meaning |
| --- | --- |
| `MAD_GRANULE_PCM` | one granule delivered in `pcm` |
| `MAD_GRANULE_NEED_INPUT` | more compressed bytes required |
| `MAD_GRANULE_END` | finished stream fully drained |
| `MAD_GRANULE_ERROR` | latched decode error (see `progress().error`) |

Each `PCM` result advanced exactly one granule: its scalefactors, Huffman
decode, stereo processing, IMDCT/overlap, and one 18-subband-step QMF pass.
The caller-owned `mad_pcm` receives 576 samples per channel, or 288 when
`MAD_OPTION_HALFSAMPLERATE` was selected.  Output entries beyond `length` are
unspecified.

`NEED_INPUT`, `END`, and `ERROR` set `pcm.length = 0` and leave the sample
entries untouched.  `END` repeats without doing work; an empty finished stream
returns `END` immediately.

A physical frame may be collected in full and the following frame header may
be looked ahead (to reproduce `main_data_begin`), but the second granule is
never speculatively decoded.  After the first half is returned the caller may
move or overwrite its own input, call `feed()`, call `progress()`/`next()`, or
operate on other instances.  What must not be lost — and is retained inside
the object — is the SCFSI-reused scalefactors, the main-data bit position, the
remaining bit count, and the overlap/QMF history.

## Progress

`mad_granule_progress()` has no side effects.  It reports:

- `granules`: successfully synthesized granules;
- `physical_frames`: physical frames whose last granule *and* reservoir
  finalization have completed;
- `sample_frames`: delivered samples per channel;
- `buffered_bytes`: owned compressed bytes, including any in-flight frame not
  yet retired;
- `error`: the latched `mad_error`, or `MAD_ERROR_NONE`.

These counters reflect completed work, never call counts.  On a stream error
they remain at the successful prefix, with `error` set to the actual
`mad_error`.

## State lifetime and ordering guarantees

1. **Preparation.**  The header, CRC, and complete side information of a
   physical frame are validated before its first granule.  A structural error
   here (bad CRC, bad side information, unexpected format) is reported by the
   first `next()` call without delivering PCM.
2. **First granule commits.**  Once the first granule has been delivered it is
   irrevocable.  If an error is found only in the second granule's Huffman
   data, the first `next()` returns the correct 576 samples, and the second
   `next()` reports the error and latches the session.  The already-delivered
   prefix is retained.
3. **Reservoir finalization.**  Each physical frame's reservoir is finalized
   exactly once, after its last granule has been consumed.  The following
   frame's preloaded main data cannot overwrite bytes that are still being
   read by the current frame.
4. **Synthesis phase.**  The 18-subband-step phase of the filterbank is
   carried across granule calls, so concatenated granule output equals whole-
   frame output sample for sample.
5. **Errors latch.**  After a bitstream error, further `next()` calls keep
   returning `MAD_GRANULE_ERROR` until `reset()`.  There is no silent
   resynchronization, concealment of a failed granule, or partial-channel
   synthesis.

## EOF

`finish()` marks the end of input but does not itself commit anything.  The
decoder drains every real granule already accepted, then returns `END`.  The
synthetic `MAD_BUFFER_GUARD` bytes are never treated as a new frame and no
granule is delivered twice.  A physical frame left partially fed at `finish()`
is an error with `MAD_ERROR_BUFLEN`.

## Recovering from errors and reusing an instance

Invalid API arguments (`NULL` decoder, `NULL` `accepted`/`progress`/`pcm`, or
`NULL` data with nonzero length) return `-1` without latching a stream error
or altering codec state.  `free(NULL)` and `reset(NULL)` do nothing.

`reset()` discards all compressed bytes, overlap/QMF history, EOF state, and
any latched error, while keeping the construction options.  The object is then
equivalent to a freshly constructed instance.  To recover from an error,
either create a new object or call `reset()` and begin feeding from the start
of a stream.

## Compatibility

The existing synchronous API keeps its full-frame failure semantics:
`mad_frame_decode()` still fails the whole frame if either granule's main data
is invalid, and `mad_synth_frame()` output is unchanged.  Both are built on
`mad_iii_decode_granule()` and `mad_synth_step()`.
