# AAC-LC TNS numeric regression

`support/tns_cases.c` is a content check for AAC-LC temporal noise shaping.
The existing fuzzer entry point only notices crashes, so a decoder that
plays something plausible while interpreting the transmission parameters
wrongly (for example choosing the wrong coefficient table or running the
all-pole filter in the wrong direction) would survive it.  These tests
compare actual numbers against an independent reference at three levels:
transmission parameters to LPC to spectrum, real band tables to region
boundaries, and a full legal bitstream to output PCM.

The tests are additive.  The production algorithm, the public API, the
allocation pattern and the normal build are unchanged.

## How to run

```
bash support/build.sh <build-dir>      # CMake build of libfaad.a
cc -std=c99 -DHAVE_INTTYPES_H=1 -DHAVE_LRINTF=1 -Isupport -Iinclude -Ilibfaad \
   support/driver.c support/tns_cases.c <build-dir>/libfaad.a -lm -o cases
<cases>
```

`support/target.sh` does exactly that, then builds two source mutants and
requires both of them to be rejected with an `ASSERT` line and exit status
42.  `support/baseline.sh` exercises the pre-existing TNS entry point and
four raw AAC packets and prints the `TNS_BASELINE` / `AAC_BASELINE` lines.

Everything here is C99 and only needs CMake, a C compiler and the system
math library (`-lm`).  No external encoder or audio file is used.

## Reference basis

The reference never calls the production coefficient conversion or the
production filters, and does not copy the decimal table from
`libfaad/tns.c`.

1. **Transmitted integer to reflection coefficient.**  The AAC tables are
   closed forms.  For a coefficient resolution `res` (3 or 4 bits, i.e.
   `coef_res + 3` in the bitstream):

   ```
   index i <  2^(res-1):  c = sin(pi * i / (2^res - 1))
   index i >= 2^(res-1):  c = -sin(pi * (2^res - i) / (2^res + 1))
   ```

   `support/tns_cases.c` evaluates these expressions with `sin()` and
   never reads the tables.  With `coef_compress` set, the wire carries
   `res - 1` bits; the upper half of the wire range folds back onto high
   negative indices, and the test applies that fold explicitly:
   `index = 2^res - (2^(res-1) - wire)`.

2. **Reflection coefficient to LPC.**  The step-up recursion is done in
   `double` on its own:

   ```
   a[0] = 1
   a[m] = refl[m-1]
   for i in 1..m-1: b[i] = a[i] + a[m]*a[m-i]
   for i in 1..m-1: a[i] = b[i]
   ```

3. **LPC to spectrum.**  The reference uses the direct all-pole difference
   equation

   ```
   y(n) = x(n) - sum_{j=1..order} a[j] * y(n-j)
   ```

   with a fresh zero state for every filter region.  It never substitutes
   one whole-array pass for the per-region start/end/direction decision.

The comparison tolerance for the spectrum is
`2e-5 * (1 + max |reference|)`, as required.  The coefficient group
contains only finite, stable sample sets; measured worst-case deviation is
about 0.17 of that budget.  Casts of the probe spectra are exact in
`float`, so unless a strong resonance amplifies round-off, only genuine
arithmetic differences show up.

Scale factors and inverse quantisation in the stream group use the
closed forms `2^((sf-100)/4)` and `sign(q)*|q|^(4/3)`; those are the
specification's definitions, not the decoder's lookup tables.

## The observation point

`libfaad/tns_observe.h` declares, only when `FAAD_TNS_TEST_OBSERVE` is
defined, a read-only callback:

```c
typedef void (*faad_tns_observe_cb)(uint8_t channel, uint16_t frame_len,
                                    const real_t *spectrum);
extern faad_tns_observe_cb faad_tns_observe;
```

`libfaad/specrec.c` calls it immediately after the real
`tns_decode_frame(...)` call and before DRC and `ifilter_bank(...)`, for
the single-channel element path and for both channels of a channel-pair
element.  It records the channel's final spectrum.  The callback treats
the array as read-only.

`libfaad/tns_observe.c` defines the callback pointer in dedicated test
builds and contains only an unused typedef in normal builds.  The CMake
option `FAAD_TNS_TEST_OBSERVE` (off by default) adds the macro.  With the
option off there is neither the call nor the symbol, so the normal build
keeps its behaviour, ABI and allocations.  `support/build.sh` enables the
option for the regression suite; `support/baseline.sh` does not.

In the stream group the callback's copy is compared against a spectrum
rebuilt by the generator itself from its own quantised values and scale
factors, so the observed output is never used as the expected value.

## Bitstream layout

The stream group synthesises raw `raw_data_block` payloads in C (20 KiB
writer, byte padded at the end of each block).  Each block is a single
SCE:

| Field | Value |
| --- | --- |
| `id_syn_ele` | `ID_SCE` (0), 3 bits |
| `element_instance_tag` | 0, 4 bits |
| `global_gain` | 100, 8 bits |
| `ics_reserved_bit` | 0 |
| `window_sequence` | 0 / 1 / 2 / 3 |
| `window_shape` | 0 |
| long: `max_sfb` | 6 bits, then `predictor_data_present` = 0 |
| short: `max_sfb` | 4 bits, then `scale_factor_grouping` 7 bits |
| `section_data` | one section per scalefactor band |
| `scale_factor_data` | one huffman symbol per coded band |
| `pulse_data_present` | 0 |
| `tns_data_present` | 1, then Table 4.4.27 |
| `gain_control_data_present` | 0 |
| `spectral_data` | codebook 1 for the generated bands |
| `ID_END` | 3 bits, then byte alignment |

`section_data`: for every group and every band below `max_sfb`, the
section codebook (4 bits) followed by `sect_len_incr` (5 bits for long
windows, 3 for short).  Bands inside the generator's active range use
codebook 1; bands outside it use the zero codebook and send no
coefficients.  Because sections are one band wide, the per-band section
offsets collapse to one width per band and the decoder consumes exactly
one codebook-1 group of four values per active band and window.

`scale_factor_data`: only coded bands are present.  With `global_gain`
100 and a delta of 0 (huffman symbol 60, the one-bit codeword `0`) the
reconstructed scale factor of every active band is 100.

`spectral_data`: codebook 1 encodes a quadruple; the test uses patterns
0, `(1,0,0,0)`, `(-1,0,0,0)`, `(0,0,0,-1)`, `(0,1,0,0)`,
`(0,0,0,1)`, `(0,0,-1,0)`, `(0,0,1,0)` and their fixed codewords
(`0`, `10000`, `10001`, `10010`, `10011`, `10100`, `10101`, `10110`).
A deterministic seed picks one pattern per band and window, so the
resulting spectrum is asymmetric and non-flat.

`tns_data`: `n_filt` is 2 bits for long windows and 1 bit for short
windows, `coef_res` one bit when any filter is present, then per filter:
`length` (6 or 4 bits), `order` (5 or 3 bits), and when the order is
nonzero `direction`, `coef_compress` and `order` coefficients of
`res - compress` bits each.

The generated transitions are legal: `ONLY_LONG_SEQUENCE`,
`LONG_START_SEQUENCE`, `EIGHT_SHORT_SEQUENCE` (with non-uniform
`scale_factor_grouping`), `LONG_STOP_SEQUENCE`, `ONLY_LONG_SEQUENCE`.
A short window carries one filter, so no short frame declares two.

The AAC AudioSpecificConfig for `NeAACDecInit2` is two bytes: object
type LC (2), the sample-rate index and channel configuration 1 with
`frameLengthFlag`, `dependsOnCoreCoder`, `extensionFlag` all zero.

## Coverage

### `coefficients`

* resolutions 3 and 4, each with and without `coef_compress`;
* every transmitted integer of every resolution/compression
  combination at order one, in both directions
  (`coefficients_every_wire_value`);
* orders 2, 3 and 8 for both directions, with positive and negative
  coefficients mixed;
* order 0 (zero order must be a no-op, matching the reference);
* a full-frame filter each time, so region handling cannot mask a
  coefficient error;
* the reference must agree with the decoder about whether any sample
  changed as well as about the value, so a legal all-zero coefficient is
  not mistaken for a missing filter.

### `ranges`

Real `num_swb`/`swb_offset` tables for sample-rate indices 0/1 (96 kHz),
3/4 (48/44.1 kHz), 5 (32 kHz), 11 (8 kHz) and the short-window tables
(44.1 kHz, 8 kHz).  Long and short windows are actually executed.

* several adjacent long filters, forward and reverse, at different
  resolutions and compression flags;
* zero-length regions and zero-order declarations between active
  neighbours (no change, and no disturbance of the neighbour's zero
  history);
* declarations that cross the permitted TNS band limit (`max_tns_sfb`),
  `max_sfb`, and the per-window `swb_offset_max` (an overlong declaration
  clamps to band 0);
* eight short windows with non-uniform grouping (2+3+3), different
  parameters per window, and windows with TNS off in between; each
  window's filters start from zero history;
* per-frame sentinels at the extreme coefficients: untouched positions
  must stay bit-identical, and every declared nonzero-order region must
  have changed something;
* per-region, per-window agreement on the exact span that region covers,
  so a swapped direction or a one-off boundary cannot hide in a
  frame-level match.

### `stream`

* legal long -> long start -> eight short -> long stop -> long
  transition, five frames with adjacent-frame parameter changes and one
  frame with TNS off;
* a second 32 kHz instance decoded interleaved with the first (and once
  more in the opposite order), two live decoder handles and two
  independent filter banks;
* constant generated content with different TNS declarations produces
  measurably different final spectra, so an ignored adjacent-frame
  update cannot pass;
* for every frame: no decode error, exactly 1024 samples (the decoder's
  first frame is the expected 0-sample priming frame), nonzero energy
  after priming, exactly one observation, a spectrum match against the
  generator's reconstruction, and a float PCM match against the
  reference spectrum pushed through an independently owned filter bank
  (the TNS/overlap hand-off is isolated this way).

## Mutation check

`support/target.sh` builds the suite against two source variants:

* `support/direction.patch` ignores the transmitted direction;
* `support/compression.patch` ignores `coef_compress` when selecting the
  coefficient table.

Both must terminate with an `ASSERT` line and status 42.  The mutants are
not part of the normal build and the fixed driver, the patches and the
baseline are untouched.  With the current suite the direction mutant
fails 71 of 601 checks spread over all three groups and the compression
mutant fails 49, so the rejection does not depend on a single case.
