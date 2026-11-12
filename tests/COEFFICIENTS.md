# MPEG-1 coefficient reconstruction regression

Offline numerical regression for the coefficient path of `pl_mpeg.h`.
Everything here runs without a player, FFmpeg or any downloaded media: the
MPEG-1 elementary streams are generated inside `tests/coefficients.c`, decoded
through the public API
(`plm_buffer_create_with_memory` → `plm_video_create_with_buffer` →
`plm_video_decode`), and compared against an independent reference written in
the same file.

Run it with:

```
bash baseline/check-target.sh
```

That compiles and runs this regression (`node tests/run_coefficients.mjs`) and
then the existing DC picture check in `baseline/run.sh`. Only `cc`, the
standard C math library, the existing header and Node built-in modules are
used; no npm package is installed and no Python is involved.

## What is tested

`tests/coefficients.c` maintains each 8×8 block as a coefficient model in
**natural** (row `u`, column `v`) coordinates. The stream writer turns that
model into a legal MPEG-1 video elementary stream (sequence header, picture
headers, slices, macroblock/block VLCs), so every assertion exercises the real
bitstream parser, dequantizer and IDCT. Nothing is written straight into
`block_data` to claim the decode chain works.

The 11 fixed scenarios cover:

| Scene | What it pins down |
| --- | --- |
| `intra_dc_only_default` | DC-only blocks reconstruct **exactly**; distinct luma/Cb/Cr DC values |
| `intra_multislice_dc` | A whole 32×32 picture over two slices; luma and chroma DC prediction inside a slice and the predictor reset at a slice start |
| `intra_ac_default` | Horizontal `(0,1)` and vertical `(1,0)` basis functions in different luma sub-blocks, default intra matrix |
| `intra_run_escape_default` | Run-length skips and the escape-coded coefficient path |
| `intra_ac_custom_asymmetric_matrix` | The same natural coefficients under default vs. asymmetric custom intra matrix; pixels change only in the targeted block |
| `intra_matrix_zigzag_remap` | Sensitive asymmetric positions where a missing matrix zigzag remap moves pixels past the tolerance |
| `workspace_consecutive_ac_blocks` | Two blocks with energy at different frequency positions; the second never sees the first's transform workspace |
| `inter_dc_oddify_rounding_and_clips` | Non-intra DC on the even→odd/rounding critical point (exact hand value), plus positive and negative saturation on a known prediction base |
| `inter_ac_residual_add` | Inter AC residual **added** to a known prediction (distinct from intra overwrite) |
| `intra_chroma_ac_independent` | Independent Cb and Cr AC checks while luma stays flat |
| `inter_custom_nonintra_matrix` | Inter residual under the asymmetric custom non-intra matrix |

Inputs include default and asymmetric custom intra/non-intra matrices, small
positive and negative coefficients, run/escape coding, different chroma DC,
and consecutive blocks whose energy sits at different frequency positions.

## Independent reference

The expected values are computed from scratch, never copied from the decoder:

- **Zigzag** is generated from the anti-diagonal scan rule (even diagonals
  walked bottom-up, odd diagonals top-down); a self-check pins the first and
  last scan positions.
- **Dequantization** follows the MPEG-1 distinction:
  - intra DC: `F00 = 8 * dc`;
  - intra AC: `(2*level)*q*w >> 4`;
  - non-intra: `(2*level + sign(level))*q*w >> 4`;
  then the even→odd adjustment and clipping to `[-2048, 2047]`.
- **Inverse transform** is a direct double-precision 2-D cosine sum
  (`ref_idct_2d`). The library's premultiplier table, butterfly IDCT and VLC
  walk tables are not used to compute any expectation.

## Pixel tolerance

- DC-only blocks are compared **per pixel, exactly** (tolerance 0), because a
  lone DC term is a constant block.
- Small-amplitude AC blocks are compared **per pixel with tolerance 1**, to
  absorb the integer fixed-point IDCT versus the double-precision cosine sum.
  The comparison always runs on the final, clamped output plane after
  prediction is added; it is never an integer-frame average, which would hide
  a localized, frequency-specific error.
- The inter critical point and both saturation cases use exact hand-computed
  pixel values.

## The three deliberate header variants

`tests/run_coefficients.mjs` copies `pl_mpeg.h` into separate temporary
directories, applies one auditable textual mutation per copy, and recompiles
the **same** `coefficients.c` with no tolerance change and no variant-specific
expectation. A variant counts as a detection only if it compiles, runs a
non-empty set of scenes and exits `42` on a `COEFF_ASSERT`; a compile error,
crash, timeout or empty run is rejected.

| Marker printed | Mutation (location in `pl_mpeg.h`) | Assertion that catches it |
| --- | --- | --- |
| `MATRIX_MAP_CAUGHT` | In `plm_video_decode_sequence_header()`, store custom matrix entries with `idx = i` instead of `idx = PLM_VIDEO_ZIG_ZAG[i]` (both matrix loaders) | `intra_matrix_zigzag_remap`: at natural `(1,2)`/`(3,0)` the wrong weight shifts the reconstructed pixel beyond ±1, e.g. `(0,0) actual=136 expected=133 tol=1` |
| `ODDIFY_CAUGHT` | In `plm_video_decode_block()`, delete the even→odd adjustment after dequantization | `inter_dc_oddify_rounding_and_clips`: non-intra DC level 1, qscale 4, weight 16 gives raw 12 → odd 11 and pixel 101; without oddify it stays even 12 and rounds to 102 |
| `WORKSPACE_CLEAR_CAUGHT` | In `plm_video_decode_block()`, remove both `memset(block_data, 0, ...)` calls after the full IDCT path | `intra_ac_default`: the second AC block inherits the first block's leftover transform coefficients, e.g. block-1 `(0,0) actual=162 expected=131 tol=1` |

On success the runner prints `MPEG_COEFF_OK cases=11`, the three `*_CAUGHT`
markers, and for each variant the header location, the failing scene and the
concrete assertion text.
