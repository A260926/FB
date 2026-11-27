# hex_model — independent hex-string relation regression

This directory is a self-contained correctness harness for a strict subset of
YARA hex strings. The model never reads YARA's AST, atom tables, RE bytecode or
executor: patterns are plain C structures, expected matches are computed from
the reachability between input positions, and only the scan adapter links
against libyara.

## Layout

| File | Role |
| --- | --- |
| `hex_model.h/.c` | Pattern nodes, structural validation, rule rendering, the bitset reachability model (`hm_expected`), (de)serialization |
| `hex_generate.c` | Deterministic structured-pattern generator and benign-data planter |
| `hex_fixed.h/.c` | 13 hand-computable fixed regressions plus 8 explicit rejection checks |
| `hex_scan.h/.c` | `yr_compiler_add_string` → `yr_compiler_get_rules` → `yr_rules_scan_mem` adapter; reads chain-head `offset`/`match_length` and compares every pair |
| `hex_check.h/.c` | Shared checking core used by both drivers |
| `hex_driver.c` | Fixed-seed driver (no libFuzzer): `--seed 1125 --cases 240` |
| `hex_fuzz.c` | `LLVMFuzzerTestOneInput`, compiled separately so the normal driver never links the fuzz runtime |
| `check.sh` | Builds both thresholds, ASan/UBSan, and the mutation-sensitivity checks |

## Running

```
bash tests/hex_model/check.sh
bash task_support/baseline.sh      # unchanged four-case baseline
```

On success `check.sh` prints `HEX_RELATION_OK`, `HEX_CHAIN_SWITCH_OK` and
`HEX_SENSITIVITY_OK`, each on its own line.

## Supported subset and limits

Bytes, `??`, half-nibble wildcards, `~` byte/half-nibble negation (never
`~??`), concatenation, equal-width non-empty alternation whose branches hold
only fixed-width nodes, and bounded jumps that appear only in the outer
concatenation (at most two). Max 24 nodes, nesting depth 3, jump endpoints
0..202, data up to 640 bytes. Patterns may not start or end with a jump; the
zero-width jump is rendered `[0-0]` because the grammar rejects `[0]`.
Unsupported input is rejected with a reason, never counted as a non-match.

## Recorded real divergence (F13) — model intentionally not adjusted

The harness found a genuine library limitation on the split-string-chain path
(the path selected when a jump exceeds `YR_STRING_CHAINING_THRESHOLD`). For
`{ AA [0-2] BB [2-5] CC }` a head `AA` can reach both a short middle `BB` that
cannot complete the chain and a longer middle `BB` that can. The short middle
match is inserted into the unconfirmed list first; when it is negatively
confirmed and removed, the chain head that the still-valid longer match needs
is removed as well, so the split chain publishes nothing. The unsplit RE engine
at the default threshold 200 and the independent model both report the single
full match.

This is captured as fixed regression `F13` (11-byte buffer, one expected
match at offset 1 length 10). The scan adapter classifies it narrowly as
`HM_SCAN_KNOWN_DIVERGENCE`: the observed list must be a strict,
order-preserving subset of identical model pairs, and only on a split chain.
Any other disagreement — a wrong offset/length, an over-report, or a divergence
on the default build — remains a hard failure.
