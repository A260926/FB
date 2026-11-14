This seed contains input support only. It does not contain the requested model or target tests.
Run bash checks/baseline.sh for existing production behavior; bash task_support/run.sh target compiles and runs the new target file.
The target file registers task_cases/task_case_count and uses TASK_CHECK. Each failed content comparison must print CONTENT_MISMATCH with its location, expected and actual values before returning through TASK_CHECK. Setup failures must be reported separately.
Run bash checks/run-mutants.sh only after the normal target passes. It recompiles three isolated source copies and accepts only driver status 4 with both CONTENT_MISMATCH and FAIL records. Build failure, signal termination, and mere nonzero exit are rejected.
All upstream licenses and tests are retained. Mutation patches are test inputs, not production changes or target answers.

## Envelope node/clock model (`tests/envelope_model.c`)

The target is an independent re-implementation of the tracker volume-envelope
machine, written from the format rules rather than recorded from libxmp. It
never calls the production `get_envelope()`/`update_envelope()` and never
records a golden output from the library. The library is exercised only
through the public entry points (`xmp_start_player`, `xmp_play_frame`,
`xmp_play_buffer`) on modules assembled in-memory with
`test-dev/simple_module.c`; nothing is downloaded and no external player is
used.

Model inputs are the node list, the enable flag, the ordinary loop, the
sustain (point or loop) and the key-off tick. Its outputs are, per synthesis
tick, the envelope position and its integer value. Two personalities are
modelled separately so one format's rules are never applied to the other:

- **IT** keeps a held note inside the sustain loop; a key-off on the frame that
  would leave the sustain-loop end performs one more jump to the sustain
  start before continuing. Ordinary loops wrap when the position is strictly
  past the loop end. Sources: ITTECH.TXT/S3MTECH.TXT and the OpenMPT IT
  envelope reference.
- **FastTracker 2 (XM)** clamps a lone sustain point while held and applies
  the ordinary loop afterward, wrapping only when the position is exactly at
  the loop end; a sustain point coincident with a loop end therefore keeps
  cycling while held, and releasing on that point lets the envelope leave the
  loop. Sources: FT2.DOC and the OpenMPT FT2 reference.

Both share the same interpolator: `y1 + (y2-y1)*(x-x1)/(x2-x1)` with C integer
division truncating toward zero on the whole numerator (the slope is not
truncated before the multiply). Nodes use strictly increasing x and y in
[0,64], with positive and negative non-integer slopes; the position is invalid
before the first sound, evaluates 0 on the first synthesis tick, and holds the
last node value past the end. Disabled, zero-node and single-node envelopes
are covered; zero nodes are treated as "no envelope".

Each deterministic material (no loop, ordinary loop, single-point sustain, IT
multi-point sustain loop, coincident and separated sustain/loop) is compared
between model and engine tick by tick, including values strictly between
nodes, so a one-step volume error or an early slope truncation cannot pass.

### Sound cross-check (audio group)

A non-zero, looping 8-bit sample with amplitude headroom drives a real voice
(fadeout 0, nearest interpolation, lowpass filter disabled, no volume slide
or randomization). A varying envelope is compared against an independently
created constant-64 envelope: per tick the measured PCM mean-amplitude ratio
must equal the envelope-volume ratio. Analysis uses samples 300..700 of each
frame to avoid the anticlick ramp and frame edges; a 6% relative tolerance
absorbs fixed-point quantization and ramp noise (measured well under 0.1%),
while the smallest real envelope step here is larger than 15%, so a missed
step is still rejected. Sound is also compared across a loop jump and across
key-off. Finally the same module is read through `xmp_play_frame` and through
`xmp_play_buffer` with 1/17/257-byte requests; the effective prefixes must be
byte identical, proving the envelope clock advances per synthesis tick rather
than per API call.

Every case prints a `SCENE` line with the number of ticks actually compared,
key-off (release) count and PCM comparisons performed. Content failures print
`CONTENT_MISMATCH` with the tick/sample location, expected and actual values.

