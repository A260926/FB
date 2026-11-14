# L-SMASH isom/mov remuxer — lossless cut & reorder

This tree extends the L-SMASH `remuxer` with a `--cuts` mode that reorders
several complete shots of one MP4 input into a new MP4. The compressed audio
and video samples are copied byte-for-byte; nothing is re-encoded.

## Building

```sh
sh task_support/build.sh        # configure + make liblsmash.a and cli/remuxer
```

The cut logic lives in `cli/cutplan.h` and `cli/cutplan.c`. `configure` lists
`cli/cutplan.c` among the shared CLI sources, so every tool links it through
the generated `config.mak` / `config.mak2`.

The dedicated target build used by the regression is fully explicit and uses
only the C toolchain and system libraries:

```sh
cc -std=c99 -O1 -I. cli/remuxer.c cli/cutplan.c cli/cli.c liblsmash.a -lm \
    -o .task-build/remuxer_target
```

## Regression scripts

```sh
sh task_support/baseline.sh     # original real two-track remux still works
sh task_support/target.sh       # builds remuxer_target + tests, then runs baseline
```

`target.sh` compiles `cli/cutplan.c` together with the remuxer into
`.task-build/remuxer_target`, builds and runs `tests/target_cases.c`
(`test_reorder`, `test_audio_edges`, `test_mapping`, `test_reject`, no
`main` of their own — `task_support/target_main.c` provides it), and finally
runs the baseline. The tests drive the built program and re-open the outputs
with the library to compare sample bytes, timestamps, edits and durations.

## The cuts file

Each non-empty line holds two decimal unsigned integers:

```
start end
```

Both are expressed in the **input movie timescale** and denote a half-open
interval `[start, end)`. Blank lines are ignored. Line order is the output
order: segments may be selected backwards and may repeat; lines are never
sorted or merged. The shipped example `task_support/cuts.txt`

```
1000 2000
0 1000
1000 2000
```

takes the second second, then the first, then the second again from the
three-second `task_support/source.mp4` (movie timescale 1000).

Run it with:

```sh
cli/remuxer -i task_support/source.mp4 --cuts task_support/cuts.txt -o out.mp4
```

## What the cut mode guarantees

- One self-contained MP4 input: exactly one `avc1` video track and at most one
  `mp4a` audio track, each with a single sample description.
- Video is closed-GOP. A cut start, and every cut end that is not the end of
  the file, must land on the **display (composition) time of an IDR picture** —
  not merely on a DTS, and not on an arbitrary intra frame. IDR status is
  confirmed by parsing the AVC NAL units, not by trusting flags alone.
- Each track may have at most one non-empty `rate = 1.0` edit, optionally
  preceded by empty edits. Any other edit shape is rejected. Cuts must lie in
  the region where every track presents media.
- Endpoints are converted from the movie timescale to each media timescale
  with exact integer arithmetic; any non-integer result is rejected.
- Video is copied per complete GOP in decode order. Per-sample cts-dts
  offsets, sample payloads, sync points and the decoder configuration are
  preserved.
- Output DTS of every track starts at 0 and segments are concatenated by real
  sample duration; absolute source timestamps are never spliced together.
- Repeated shots are genuine new samples (new chunks/bytes), not references
  back into the source file.
- Audio selection is driven by the media interval, not video sample numbers:
  every whole AAC packet intersecting an interval is copied. Packets shared by
  adjacent cuts are copied once per cut.
- Each track gets one fresh edit list entry per cut that selects the copied
  media sub-range and hides AAC head overhang and the B-frame reorder delay.
  The source edit list is never copied; no visible silence or black frames are
  inserted.
- Track language, display size and media timescales are retained; the output
  movie timescale equals the input's. The displayed duration is the sum of
  `end - start` over all cuts.
- Without `--cuts` the original remuxer behavior is unchanged.

## Rejections

Malformed lines, an empty plan, out-of-range intervals, non-integer time
conversion, non-IDR boundaries, unsupported edit lists, non-self-contained
inputs, and combining `--cuts` with `--fragment`/`--dash`, multiple inputs,
track `remove`, or `seek`/`safe-seek` all fail before any output file is
created.
