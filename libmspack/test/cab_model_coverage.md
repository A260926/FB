# CAB decoder test coverage: static samples vs. generated models

Two complementary suites exercise `mspack/cabd.c`:

## Static sample files (`test/cabd_test.c` + `test/test_files/cabd/`)

433 checks over a fixed corpus of checked-in files. They cover:

- header parsing: normal/reserve/hidden-file cabinets, 255-char names,
  atypical file offsets, salvage mode;
- malformed inputs: bad signatures, truncated headers/folders/files,
  zero folders/files, bad folder indices, overlong strings;
- historical CVE reproducers (infinite loops, over-reads, segfaults);
- `search()` scanning, basic `append()`/`prepend()` argument validation
  and a real 5-part cabinet-set merge;
- extraction of all four compression methods from recorded files, and
  MD5 comparison against recorded digests.

The samples are immutable: they prove the decoder keeps handling exactly
those byte streams, but they cannot say *why* the output bytes are right,
and they only cover the layouts someone once recorded.

## Generated models (`test/cab_model_test.c`, driven by `checks/cab_model_main.c`)

Builds legal cabinets from an explicit model and checks content against
plaintext the model expands itself (never the decoder's own output):

- **model**: a 3-frame MSZIP folder where later frames begin with
  matches reaching up to 32768 bytes back into earlier frames (the
  preserved dictionary), multiple files per folder, file boundaries
  inside and across frames, a zero-length file, non-zero folder offsets,
  non-contiguous extraction order, and proof the input is unchanged.
- **split**: one CFDATA block cut across three cabinets (leading pieces
  with `cbUncomp == 0`, per-piece checksums, non-empty reserved areas),
  merged with `append()`/`prepend()`; two different physical cut points
  give identical bytes; missing/corrupted pieces are not reported as
  successfully extracted; a differing set ID keeps the current
  warn-and-merge behaviour.
- **partial**: a recording in-memory `mspack_system`; short reads and
  hard read errors surface as extraction failures; interleaved
  extraction across files, backwards jumps and a second decoder
  instance; per-handle write intervals prove a pending frame tail is
  neither re-sent, lost, nor written into another file; one whole-span
  output equals output split at logical file boundaries.

`test/cab_model_mutants.cjs` then proves the suite's teeth: two
compilable degradations of `mspack/mszipd.c` (clearing the inter-frame
dictionary, and not advancing the pending-output pointer) are both
rejected by these content assertions.

In short: the static samples pin down parser robustness and historical
bug fixes on real-world bytes; the generated models pin down the
*semantics* of legal MSZIP content — dictionary carry-over, pending
output routing, and multi-volume block assembly — with independently
computed expectations.
