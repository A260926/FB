# Structured MSVC thunk information

`include/rz_msvc_info.h` exposes a machine-readable view over the Microsoft
C++ mangling scheme. It runs the same parser used by
`libdemangle_handler_msvc()` and, in addition to the display string, reports the
`this`-pointer adjustment carried by thunk symbols.

## API

```c
RzMsvcInfo *libdemangle_msvc_info(const char *symbol, RzDemangleOpts opts);
void libdemangle_msvc_info_free(RzMsvcInfo *info);
```

* The input is the mangled symbol (string) plus the usual `RzDemangleOpts`.
* The returned structure owns every string it points at. They stay valid after
  the input buffer is freed or overwritten, and are released by
  `libdemangle_msvc_info_free()` (which accepts `NULL`).
* `NULL` is only returned on allocation failure. Parse problems are reported in
  `info->err` / `info->err_offset`.
* The entry point is strict: the whole symbol must be consumed. Trailing bytes
  are reported as `RZ_MSVC_INFO_ERR_TRAILING_GARBAGE`. The legacy
  `libdemangle_handler_msvc()` keeps its historical lenient behaviour.

## Fields

| field | meaning |
| --- | --- |
| `err` | `RZ_MSVC_INFO_OK` on success, otherwise an error category |
| `err_offset` | byte offset in the original symbol for the error |
| `display` | owned demangled text (`NULL` on error) |
| `is_thunk` | true for adjustor / vtordisp / vtordispex thunks |
| `thunk_kind` | `NONE`, `STATIC_ADJUSTOR`, `VTORDISP` or `VTORDISPEX` |
| `access` | `NONE`, `PRIVATE`, `PROTECTED` or `PUBLIC` |
| `is_far` | true when the selector marked a far thunk |
| `this_modifier` | cv-qualification of the implicit `this` (`""`, `const`, `volatile`, `const volatile`); always owned and never `NULL` on success |
| `adjust[]` | up to four `RzMsvcAdjustValue` slots |

Each `RzMsvcAdjustValue` has:

* `present` — false when the symbol does not carry this field (the value is
  then irrelevant; absent slots are not reported as zero);
* `value` — signed 32-bit value;
* `begin`, `end` — half-open byte range of the token in the original input
  (including a leading `?` and the terminating `@`).

The slots always appear in mangled-symbol order:

| kind | slots used | display suffix |
| --- | --- | --- |
| static adjustor (`W`..`X`) | `STATIC_OFFSET` | `` `adjustor{n}' `` |
| vtordisp (`$0`..`$5`) | `VTORDISP`, `STATIC_OFFSET` | `` `vtordisp{a,b}' `` |
| vtordispex (`$R0`..`$R5`) | `VBPTR`, `VBTABLE`, `VTORDISP`, `STATIC_OFFSET` | `` `vtordispex{a,b,c,d}' `` |

Access level and far flag for the `$` forms follow the selector digit:
`$0/1` private, `$2/3` protected, `$4/5` public; odd selectors are far.

## Number decoding

The adjustment numbers use the original MSVC number encoding:

* `0`..`9` mean `1`..`10`;
* `A`..`P` are hexadecimal nibbles terminated by `@` (`A@` is `0`);
* a leading `?` negates the value.

Without a `?` sign, the 32-bit pattern is interpreted as two's complement, so
`PPPPPPPM@` is `-4` (not `4294967292`). Over-wide numbers, numbers missing their
`@` terminator, and truncated adjustment vectors are rejected with a specific
error category and offset.

## Example

See `examples/msvc_info_example.c` for a complete, standard-C usage sample.
