# Prefix tracing side-band

`distorm_decompose_prefix_trace()` is the tracing companion of
`distorm_decompose()`. It answers the question `unusedPrefixesMask` cannot:
not only *which* prefix bytes were ineffective, but *why* (overridden by a
later byte of the same group, a REX that was too far from the opcode, a REX.W
that neutralized 0x66, ignored by the decoding mode, or simply unused by the
instruction).

## Calling convention

```c
#include "distorm.h"
#include "prefix_trace.h"

_CodeInfo ci;                 /* same input as distorm_decompose */
_DInst inst[MAX];             /* caller-allocated instruction array */
_PrefixTraceInfo trace[MAX];  /* side-band array, SAME capacity */
unsigned used = 0;

_DecodeResult r =
    distorm_decompose_prefix_trace(&ci, inst, trace, MAX, &used);
```

The `_CodeInfo`, instruction array, capacity, feature flags, result code,
`nextOffset` advancement and not-decodable behavior are all identical to
`distorm_decompose()`. The only addition is the parallel `trace` array:
`trace[i]` always describes `inst[i]`, one-to-one, including instructions
emitted after filtering or recovery. A not-decodable byte gets an empty trace
slot (`count == 0`), so records can never leak from one instruction into the
next.

## Result structures

`_PrefixTraceInfo` (per instruction):

| field     | meaning |
|-----------|---------|
| `count`   | number of `_PrefixTrace` records, in original stream order; 0 when the instruction has no prefixes (including failure bytes) |
| `prefixes[15]` | the records themselves; indexes `0 .. count-1` are valid |

`_PrefixTrace` (one record per scanned prefix; a whole VEX is one record):

| field | meaning |
|-------|---------|
| `offset` | byte offset of the prefix relative to the start of its instruction |
| `length` / `byteCount` | 1 for every legacy/REX prefix; 2 or 3 for a VEX prefix |
| `bytes[3]` | original prefix bytes, only `byteCount` entries are valid; no virtual bytes are ever invented |
| `group` | one of `PT_GROUP_OPERAND_SIZE`, `PT_GROUP_ADDRESS_SIZE`, `PT_GROUP_SEGMENT`, `PT_GROUP_LOCK_REPEAT`, `PT_GROUP_REX`, `PT_GROUP_VEX` |
| `roles` | bit mask of `PT_ROLE_*` describing what the prefix actually did; `PT_ROLE_NONE` when unused |
| `reason` | `PT_REASON_NONE` when used, otherwise why it had no effect |
| `overriddenByOffset` | for group-override / REX.W-over-66 reasons, the offset of the byte that superseded this one; `0xff` otherwise |
| `vexImpliedPp` | VEX only: the `pp` field (0..3), i.e. the implied virtual mandatory selector (0 none, 1 = 0x66, 2 = 0xf3, 3 = 0xf2); `0xff` for non-VEX records |

### Roles

A record may carry several roles at once (for example a VEX record with
`pp != 0` carries both `PT_ROLE_VEX` and `PT_ROLE_MANDATORY_SELECTOR`):

- `PT_ROLE_OPERAND_SIZE` - 0x66 switched the effective operand size.
- `PT_ROLE_ADDRESS_SIZE` - 0x67 switched the effective address size.
- `PT_ROLE_SEGMENT` - the override selected the segment of a memory operand.
- `PT_ROLE_LOCK_REPEAT` - LOCK/REP/REPNE kept its lock/repeat meaning.
- `PT_ROLE_MANDATORY_SELECTOR` - the byte (or the VEX `pp`) selected the
  opcode as a mandatory prefix rather than acting as a normal prefix.
- `PT_ROLE_REX` - REX extension bits took effect.
- `PT_ROLE_VEX` - the VEX prefix and its fields took effect.

### Reasons

- `PT_REASON_GROUP_OVERRIDE` - a later byte from the same prefix group won;
  `overriddenByOffset` points at it.
- `PT_REASON_REX_POSITION` - the REX was not immediately before the opcode, so
  long mode ignores it.
- `PT_REASON_REX_W_OVER_66` - an adjacent, valid REX.W forced 64-bit operand
  size and neutralized an ordinary 0x66; `overriddenByOffset` points at the
  REX. A 0x66 consumed as a mandatory selector is never suppressed this way.
- `PT_REASON_MODE_IGNORED` - the mode ignores the prefix (CS/DS/ES/SS
  overrides in 64-bit mode).
- `PT_REASON_INSTRUCTION_UNUSED` - legal here, but the decoded instruction has
  no use for it.

The reasons are recorded at the exact production decision points (prefix
scan, opcode selection, operand processing); they are never inferred from the
final mnemonic or `unusedPrefixesMask`, and the stream is not decoded twice.

## Worked examples

| bytes | record | explanation |
|-------|--------|-------------|
| `66 66 90` (64-bit) | +0 0x66 `GROUP_OVERRIDE` by +1; +1 0x66 `INSTRUCTION_UNUSED` | duplicate group, and NOP ignores operand size |
| `48 66 0f 10 c0` (64-bit) | +0 REX `REX_POSITION`; +1 0x66 mandatory selector | REX was not adjacent; 0x66 selects MOVUPD |
| `66 48 98` (64-bit) | +0 0x66 `REX_W_OVER_66` by +1; +1 REX used | adjacent REX.W promotes to CDQE |
| `f3 0f 10 c0` (32-bit) | +0 F3 mandatory selector | selects MOVSS; it is not a string repeat |
| `66 f3 0f bd c0` (32-bit) | +0 0x66 operand-size; +1 F3 mandatory selector | LZCNT AX, AX - both bytes genuinely used |
| `2e 90` (64-bit) | +0 CS `MODE_IGNORED` | long mode ignores CS/DS/ES/SS overrides |
| `c5 f9 10 c0` (64-bit) | one 2-byte VEX record, `vexImpliedPp=1` | pp=1 is reported as a virtual mandatory selector, no fake byte |
| `c5 00 ...` (32-bit) | no VEX record | Mod != 11, so C5 is a normal LDS opcode |

Run the demo in this directory (`make`) to see live output.
