# L-SMASH

L-SMASH is a library and a set of command line tools for ISO Base Media File
Format (MP4/ISOBMFF) muxing, demuxing and remuxing.  Everything here is written
in C and uses only the C library and system libraries.

## Building

POSIX build (a C99 compiler and `make` are enough):

```sh
./configure
make
```

The static library `liblsmash.a` and the tools under `cli/` (`muxer`,
`remuxer`, `boxdumper`, `timelineeditor`) are produced.  The shared CLI code
lives in `cli/cli.c` and `cli/cutplan.c` and is linked into every tool.

## remuxer

Remux one or more movies without re-encoding the compressed audio and video:

```sh
cli/remuxer -i input.mp4 -o output.mp4
```

Run `cli/remuxer --help` for the full option list.

### Lossless cut and reorder: `--cuts`

```sh
cli/remuxer -i input.mp4 --cuts cuts.txt -o output.mp4
```

`cuts.txt` contains one interval per non-empty line as two decimal unsigned
integers separated by whitespace:

```
start end
```

* `start` and `end` are in the **input movie timescale** and form a
  half-open interval `[start, end)`.
* Line order is the output order.  Intervals may be selected backwards and may
  be repeated; they are never sorted and never merged.
* Blank lines are ignored.  Anything else on a line (extra tokens, signs,
  non-decimal numbers, an empty or reversed interval) makes the whole run fail
  before any output byte is written.

Supported input for cutting:

* exactly one self-contained MP4 input,
* one `avc1` video track using closed GOPs, and optionally one `mp4a` audio
  track,
* one sample description per track,
* each track has at most one rate 1.0 non-empty edit, optionally preceded by
  empty edits; cuts must lie inside the region covered by media on every track.

Cut boundaries must be display (composition) times of IDR pictures; a plain
intra sample is not accepted.  Endpoints must convert to an integer time in
every media timescale (integer arithmetic only, no floating point).
Conflicts with seek/safe-seek, fragmentation (`--fragment`, `--dash`),
multiple inputs and track removal are rejected up front.

How the output is built:

* Video is copied GOP by GOP in decoding order, keeping every sample body, the
  cts-dts differences, sync points and the decoder configuration.  Repeated
  shots are physically duplicated samples; the new chunks never reference the
  source file's offsets.
* Each output track restarts its DTS at 0 and segments are concatenated using
  the actual copied sample durations, never the source absolute timestamps.
* Audio is selected per intersecting packet: every complete AAC sample whose
  `[cts, cts+delta)` interval meets the cut is copied, even when a boundary
  falls inside a packet.  A packet shared by two adjacent segments is copied
  once for each segment.
* One edit list entry is created per segment per track.  The entries point at
  the freshly copied media sub-ranges and hide AAC packet head/tail overhang
  and video reorder delay; no source edit list is copied and no leading silence
  or black frames are inserted.
* Track language, display size and media timescales are preserved, the output
  movie timescale equals the input one, and the presented duration is exactly
  the sum of `end - start` over all cuts.

Without `--cuts` the original whole-movie remuxing behavior is unchanged.

## Task support and tests

```sh
sh task_support/baseline.sh   # build the library/tools and run a plain remux
sh task_support/target.sh     # build the cut-enabled remuxer and run all checks
```

`target.sh` compiles `cli/remuxer.c` and `cli/cutplan.c` into
`.task-build/remuxer_target`, then builds and runs the cases in
`tests/target_cases.c` (driven by `task_support/target_main.c`, no `main` in
the case file).  The cases re-open generated outputs with the library and
verify sample bytes, timestamp differences, per-segment edits, presentation
start/end and total duration, including packets that straddle boundaries,
repeated segments, video-only files and rejected invalid boundaries.
