# MSVC thunk / `this` adjustment fields

`libdemangle_msvc_info()` (declared in `include/rz_msvc_info.h`) returns a
`RzMsvcInfo` describing how a mangled MSVC symbol must be interpreted by a
call-relationship analysis layer.  It uses the same production parser as
`libdemangle_handler_msvc()`; the structured fields are recorded while the
function category is parsed, not recovered from the demangled text.

## Result ownership

| field    | type           | ownership |
|----------|----------------|-----------|
| `display`| `char *`       | owned by the result, release with `libdemangle_msvc_info_free()` |
| all other fields | value types | copied into the result |

The input string is never referenced after the call returns; it may be freed
immediately.  `libdemangle_msvc_info()` returns `NULL` only for a `NULL` input
or an allocation failure.  Parsing errors are reported through `error` and
`error_offset`, with `display == NULL`.

## Adjustment kinds

| `kind`                    | mangled shape                          | fields |
|---------------------------|----------------------------------------|--------|
| `RZ_MSVC_ADJUST_NONE`     | ordinary function                      | none present |
| `RZ_MSVC_ADJUST_STATIC`   | `W`/`G`/`O`/`X` access letter + number | `static_offset` |
| `RZ_MSVC_ADJUST_VTORDISP` | `$0`..`$5` + two numbers               | `vtordisp`, `static_offset` |
| `RZ_MSVC_ADJUST_VTORDISPEX`| `$R0`..`$R5` + four numbers           | `vbptr`, `vbtable_slot`, `vtordisp`, `static_offset` |

`is_thunk` is true for the three thunk kinds.

## `$n` index bits

For `$0`..`$5` and `$R0`..`$R5` the trailing digit selects access level and
near/far:

| index | `access`                  | `distance` |
|-------|---------------------------|------------|
| 0     | `RZ_MSVC_ACCESS_PRIVATE`  | near |
| 1     | `RZ_MSVC_ACCESS_PRIVATE`  | far  |
| 2     | `RZ_MSVC_ACCESS_PROTECTED`| near |
| 3     | `RZ_MSVC_ACCESS_PROTECTED`| far  |
| 4     | `RZ_MSVC_ACCESS_PUBLIC`   | near |
| 5     | `RZ_MSVC_ACCESS_PUBLIC`   | far  |

For the regular member function access letters A..X the same even/odd rule
applies (A/C/E/... near, B/D/F/... far), and `Y`/`Z` are the near/far codes
without an access level.

## `RzMsvcNumber`

Each adjustment value is an `RzMsvcNumber`:

| field     | meaning |
|-----------|---------|
| `present` | `false` when the field does not exist for this adjustment kind; absent fields are never silently reported as 0 |
| `value`   | signed 32 bit value |
| `start`   | half-open range `[start, end)` of the token in the **original** input |
| `end`     | one past the last byte of the token |

### Number encoding

- `0`..`8` encode 1..9, `9` encodes 10, each is one self-terminating byte.
- `A`..`P` are hexadecimal digits (`A` = 0) terminated by `@`; `A@` is 0.
- A lone `@` encodes 0.
- A leading `?` marks an explicitly negative value (`?BA@` is -16).
- Without `?` the 32 bit pattern is two's complement, so `PPPPPPPM@` is
  **-4** and never 4294967292.
- More than eight hexadecimal digits, a missing `@`, or an out of range
  explicit magnitude is rejected.

## Errors

| `error`                              | `error_offset` points at |
|--------------------------------------|--------------------------|
| `RZ_MSVC_INFO_ERR_NONE`              | 0 |
| `RZ_MSVC_INFO_ERR_NOT_MSVC`          | 0 (input is not `?`/`.?` prefixed) |
| `RZ_MSVC_INFO_ERR_NUMBER`            | first byte of the malformed number |
| `RZ_MSVC_INFO_ERR_NUMBER_RANGE`      | first byte of the over-wide number |
| `RZ_MSVC_INFO_ERR_INCOMPLETE_ADJUST` | byte where the missing field was expected |
| `RZ_MSVC_INFO_ERR_TRAILING`          | first unconsumed byte |
| `RZ_MSVC_INFO_ERR_SYNTAX`            | reported parse position |
| `RZ_MSVC_INFO_ERR_MEMORY`            | 0 |

The structured entry point requires the whole symbol to be consumed; trailing
bytes after a complete symbol produce `RZ_MSVC_INFO_ERR_TRAILING`.
`libdemangle_handler_msvc()` keeps its original, more lenient behaviour.
