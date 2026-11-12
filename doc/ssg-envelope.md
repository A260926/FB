# SSG-EG envelope regression tests

This document describes the test-only SSG-EG regression added in
`tests/ssg_envelope.cpp` and its driver `tests/run_ssg_envelope.sh`. Nothing in
`src/` is changed by this work: the tests instantiate the existing templates
and drive them through the public API.

## What is covered

The tests target the behaviour that is hard to notice from a "is the final
audio non-zero" check:

1. **Eight-shape trajectories** (`shape_trajectories`): every SSG-EG mode
   0-7 starts from a real key-on and runs past three midpoint (0x200)
   boundaries, or settles into its held end state for the hold modes. Each
   clock is checked against an independent model for raw attenuation, the
   inversion flag, EG state, effective attenuation and phase.
2. **Constructed midpoint boundary** (`constructed_boundary`): the 0x1ff and
   exactly-0x200 cases are built through the public save/restore interface,
   read back before use, and compared against continuously-run notes so the
   constructed states are never the only evidence.
3. **Key-off / release** (`release_behavior`): key-off during inverted and
   non-inverted decay/sustain, at both held ends, and one clock before/at the
   midpoint, followed by a re-key-on.
4. **Live register changes** (`dynamic_register_changes`): switching shape
   while a note sounds, and disabling then re-enabling SSG-EG. Register
   writes are not treated as new key-ons.
5. **SSG-disabled control** (`ssg_disabled_control`): the same fixed rates
   with SSG-EG off, as an ordinary-envelope reference.
6. **Real synthesis path** (`audio_generate_path`): a YM2612 driven through
   its public register writes and `generate()` entry point, producing
   non-zero, time-varying stereo PCM. All four channel-0 operators are
   configured (otherwise the unconfigured operators stay at maximum
   attenuation and only a DC level comes out), with a faster block/fnum
   (0x3400) so the phase sweeps the sine table inside the window. A
   continuously-sounding note is compared whole-buffer against 1/7/127
   chunked generation and against a second chip instance reached purely
   through save/restore; a fresh chip that never received the saved state is
   asserted to differ, so the equality checks are not vacuous. A second run
   places a key-off and a shape write at the same absolute sample positions
   in the fine-step, chunked and restored timelines.

## Where the register mapping, effective rates and envelope ticks come from

All of these are read from the production sources, not reimplemented as a
copy of the production state machine:

- **Register map**: `src/ymfm_opn.h`, the comment block above
  `opn_registers_base` and the `op_*` accessors. The test uses 0x30/0x40/
  0x50/0x60/0x70/0x80 (detune/mul, total level, KSR+attack, decay, sustain
  rate, sustain level+release), 0x90 (SSG-EG enable/mode), 0xa0/0xa4
  (fnum/block), 0xb0/0xb4 (algorithm/feedback, pan) and 0x28 (key on/off).
- **Effective rates**: `opn_registers_base::cache_operator_data()` in
  `src/ymfm_opn.cpp`. Attack/decay/sustain use
  `effective_rate(raw * 2, ksrval)`, release uses
  `effective_rate(raw * 4 + 2, ksrval)`, with
  `ksrval = keycode >> (op_ksr ^ 3)` and the 63 clamp in
  `fm_registers_base::effective_rate()` (`src/ymfm_fm.h`). The fixed scenario
  picks block/fnum 0x0c00 so the keycode (and therefore the KSR adjustment)
  is 0; raw rates 31/30/30/15 then resolve to effective rates
  62/60/60/62.
- **Envelope increments and tick timing**: `attenuation_increment()` and
  `fm_operator::clock_envelope()` in `src/ymfm_fm.ipp`. At effective rates
  60-63 the increment table is a constant 8; an SSG-EG stage applies 4x that
  (0x20 per tick) only while the raw attenuation is below 0x200.
  `EG_CLOCK_DIVIDER = 3` (in `src/ymfm_opn.h`) together with the x.2
  envelope counter in `fm_engine_base::clock()` makes an envelope tick land
  every 3 clocks. Attack rate 62 snaps attenuation to 0 in
  `start_attack()`. With these constants one SSG ramp is 16 ticks of 0x20,
  i.e. 48 clocks from 0 to the 0x200 midpoint.
- **Midpoint detection ordering**: `fm_operator::clock()` calls
  `clock_ssg_eg_state()` *before* `clock_envelope()`, so the 0x200 crossing
  is detected on the state present before that clock's envelope update.
  `clock_ssg_eg_state()` is also where hold modes force their end value,
  continuous modes restart the attack and toggle inversion, and modes 0/4
  reset `m_phase`. `clock_phase()` still runs afterwards on the same clock,
  which is why a phase reset is always followed by that clock's phase step.
- **Release hand-off**: `fm_operator::start_release()` in
  `src/ymfm_fm.ipp`. When the inversion flag is set it snaps the raw
  attenuation to `(0x200 - raw) & 0x3ff` and clears the flag, so the
  effective envelope continues from the pre-key-off value instead of
  restarting the original decay. In release state, `clock_ssg_eg_state()`
  forces 0x3ff once the midpoint is reached, while alternating modes keep
  toggling the flag.
- **Phase step**: `opn_registers_base::compute_phase_step()` in
  `src/ymfm_opn.cpp`. With no detune/LFO and multiple 1, block/fnum 0x0c00
  yields a 10.10 step of exactly 1024, so the public `phase()` getter (the
  top bits of `m_phase`) advances by one per clock. The test's independent
  formula (`((fnum << block) >> 2)`, clamped and multiplied) is checked
  against the cached step during development and matches for every probe
  frequency.

## The independent SSG-EG event model

The expectations are generated inside the test translation unit from the
mode bit meanings and the fixed timing constants:

- bit 0 selects a single run followed by a hold,
- bit 1 selects inversion toggling each cycle,
- bit 2 selects an inverted start,
- continuous non-alternating modes (0/4) reset the phase at each SSG event,
- the held-end polarity is `bit2 ^ bit1`, holding raw 0x3ff when clear and
  raw 0x200 inverted when set.

`model_held()` expands this event table into a per-clock trajectory using
only the constants above (48-clock cycles, event clock 49, 1024 phase step).
It never reads emulator state. The release cases use explicit expectation
tables built from the same event rules plus the release snap and force-max
rules, so failures name the mode, event number, clock, field and both
expected and actual values rather than an audio hash.

## Boundary details that are asserted on purpose

- At a constructed **0x1ff**, two clocks pass with no SSG action; the
  envelope tick then overshoots to 0x21f and the event fires one clock
  later than the exactly-0x200 construction.
- At a constructed **0x200**, the event fires immediately: attack restarts
  and the phase resets, yet `phase()` is 1 after the clock because this
  clock's step is still taken.
- Key-off one clock before the midpoint still applies the 4x increment;
  key-off exactly at the midpoint does not, and both then hit the
  release-state 0x3ff forcing with the mode-0 phase reset.

## Save/restore use

`ymfm_saved_state` (see `src/ymfm.h`) serialises the engine, register file,
channels and operators in a fixed order. The test does not hard-code a byte
offset: it saves a note at a known point, locates operator 0's attenuation
field by its neighbouring state byte, refuses the construction if that
pattern is not unique, patches the four operator bytes, restores into a
fresh engine and reads the constructed state back before clocking it. The
audio group additionally saves a running YM2612 and restores it into a
different chip instance.

## Running

```sh
bash tests/run_ssg_envelope.sh
```

The script first builds and runs all six groups against the unmodified
library and requires every group's `PASS` marker, a non-trivial assertion
count and zero failures. It then copies `src/` into three temporary trees,
applies one of `baseline/mutations/{alternate,release,phase}.patch` per
copy, rebuilds the identical test binary and requires:

- a successful compile (a compile failure is reported as an error, never as
  detection),
- an assertion-failure exit status,
- a concrete numeric `FAIL ... field= ... expected= ... actual= ...` line,

printing `DETECTED_alternate`, `DETECTED_release`, `DETECTED_phase` with the
first failing location. The mutations map onto the assertions as follows:

- `alternate.patch` breaks inversion toggling, first rejected by the
  mode-2 inversion/effective-attenuation check at the clock-49 SSG event;
- `release.patch` removes the inverted-attenuation snap, first rejected by
  the inverted-decay release hand-off check;
- `phase.patch` inverts the phase-reset condition, first rejected by the
  mode-0 post-event phase check.

The full regression, including the pre-existing smoke/buildall checks, is:

```sh
bash tests/run_ssg_envelope.sh && bash baseline/run.sh
```

No sound device, ROM or external download is required.
