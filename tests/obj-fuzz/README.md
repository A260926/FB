# Structured OBJ Fuzzer

This directory contains a standalone randomized OBJ generator and verifier.

The goal is different from libFuzzer:

- generate reproducible, somewhat coherent OBJ text
- exercise faces, groups, objects, smoothing groups, vertex colors, weights,
  texcoord `w`, comments, and relative indices
- compare successful parses across `LoadObj`, `ObjReader`,
  `LoadObjOpt`, and the experimental stream loader

The runner is deterministic for a given seed and prints the exact failing OBJ
text when a mismatch is found.

By default the differential checks are:

- `LoadObj` vs `ObjReader`
- `LoadObj` vs experimental stream loader

`LoadObjOpt` is always executed for stability coverage, and can be promoted to a
strict differential target with `--strict-opt`.

## Build

```bash
cd tests/obj-fuzz
make
```

## Run

```bash
cd tests/obj-fuzz
./obj_fuzz --iterations 5000 --seed 12345
./obj_fuzz --iterations 1000 --seed 12345 --strict-opt
```

Optional material coverage:

```bash
./obj_fuzz --iterations 2000 --seed 99 --allow-mtllib
```

`--allow-mtllib` keeps the run useful for the legacy, `ObjReader`, and stream
paths, but the standalone cross-check against `LoadObjOpt` is skipped because
the optimized in-memory API does not accept a custom material reader.

## State-aware declaration/event layer

`state_model.h`, `state_model.cc`, `state_emit.cc`, `state_reduce.cc` and
`state_driver.cc` add a local defensive test layer on top of the structured
fuzzer.  It does not change `obj_fuzz` defaults and does not add syntax to the
parser; it only drives the existing public API.

The trace (`case.trace`) is the authoritative record and is produced by the
generator, never by the parser:

- `D V/T/N <id> ...` — stable per-kind declaration ids, in the order declared.
- `E O/G/U/S <event-id> <name-or-value>` — object, group, material and
  smoothing state transitions.
- `F <face-id> <shape> <material> <smoothing> V<id><+|->[/T<id><+|->][//N<id><+|->] ...`
  — stable face ids and the *declaration identity* of every corner, with the
  index style (`+`/`-`) recorded separately from the identity.

Negative indices therefore never depend on the final pool size: every
component stores which declaration it means, and the `+`/`-` style it was
written with.  The generator always emits an all-negative `v/vt/vn` anchor face
`F0`, an early declaration F0 does not reference, declarations after F0, and
interleaves declarations with faces across independently sized pools.

### Commands

```bash
.task-build/obj-state generate SEED DIR      # DIR/case.trace, case.obj, case.mtl
.task-build/obj-state check DIR              # OBJ_STATE_VALID on success
.task-build/obj-state worker DIR             # canonical per-face record stream
.task-build/obj-state reduce IN OUT --anchor F0 --budget N
```

`check` writes the two normal OBJ serializations (original index style and
all-positive indices) plus `state.mtl` next to the trace, then re-executes the
same binary as `worker DIR`.  The worker reads both files through
`ObjReader::ParseFromString` and streaming `LoadObj` (triangulation off, materials
only from an in-memory `MaterialStreamReader`) and prints the full per-face
record set.  `check` compares those records against the independent
declaration-event model, including the `-1` texcoord/normal sentinel for
corners that lack those components.  A nonzero exit, empty output, truncation
or any per-face disagreement is a failure, reported as
`OBJ_STATE_MISMATCH face=... corner=... component=... entry=...`.

`reduce` walks the trace in order (non-anchor faces first, then declarations and
state events), rebuilds the v/vt/vn pools from the surviving prefix for every
candidate, and re-encodes negative components from their identity instead of
reusing the old numbers.  It commits only candidates that strictly shrink the
event count and whose two serializations both agree with the model; the budget
counts every attempted candidate.  The log records each attempted event's
stable id, accept/reject, and the rejection reason.

Run the whole target (including the example) with:

```bash
bash task_support/run.sh target
```

`examples/obj_state_demo.cc` takes an output directory, generates/checks/reduces
and re-checks, copies `case.obj`/`case.mtl` and `reduced/case.obj`/`case.mtl`
there, and prints `OBJ_STATE_DEMO_OK`.
