# Structured fuzzing of open upvalues

This directory is a self-contained, model-based fuzz chain for the Wren
upvalue machinery. It targets the failure mode where a value still looks
correct but an aliasing relationship has silently broken: a Fiber suspended
with an open capture, a Value-stack relocation, a cell rewrite and a scope
exit, combined with closures that share one captured variable.

The reference is an independent C interpreter (`upvalue_model.c`), never
another scripting engine and never Wren's own bytecode, upvalue structures or
return values. The model and Wren are both driven from one decoded *plan* and
their emitted event streams are compared item by item.

## Files

| File | Role |
| --- | --- |
| `upvalue_fuzz.h` | Shared types, wire opcodes, limits and entry points |
| `upvalue_plan.c` | Wire decoder/validator/encoder, plan builder, Wren renderer, deterministic generator, fixed corpus |
| `upvalue_model.c` | Independent reference model, moving allocator, lockstep Wren execution and comparison |
| `upvalue_fuzz_main.c` | `LLVMFuzzerTestOneInput` plus the no-libFuzzer fixed-seed driver |
| `check-upvalues.sh` | One-command build, normal + ASan/UBSan runs and the two source-mutation detectors |
| `example/` | A directly runnable rendered plan and its encoded bytes |

## Run

```sh
bash test/fuzz/check-upvalues.sh
```

On success it prints `UPVALUE_MODEL_OK`, `UPVALUE_MOVE_OK` and
`UPVALUE_REPLAY_OK`, then deliberately breaks two VM routines in throwaway
source copies and confirms the same checks catch each break.

The standalone driver can also be built directly:

```sh
cc -std=c99 -O1 -DWREN_FUZZ_TEST_HOOKS \
   -Isrc/include -Isrc/vm -Isrc/optional -Itest/fuzz \
   test/fuzz/upvalue_plan.c test/fuzz/upvalue_model.c \
   test/fuzz/upvalue_fuzz_main.c src/vm/*.c src/optional/*.c -lm -o upfuzz
./upfuzz                 # fixed corpus + 320 generated plans, each replayed twice
./upfuzz file.upv ...    # replay raw corpus files
./upfuzz --render 0      # print the rendered Wren source of fixed plan 0
./upfuzz --corpus dir    # write the fixed + generated corpus as raw files
```

When a libFuzzer runtime is available, compile the same driver with
`-DWREN_LIBFUZZER -fsanitize=fuzzer`; that build exports only
`LLVMFuzzerTestOneInput` (the standalone `main` is compiled out). The
environment used here has no libFuzzer runtime, so the fixed-seed driver is the
guaranteed path.

## Wire format

All integers are big endian. Every legal input starts with the two header
bytes and ends with `END` (opcode `0`):

```
byte 0      UP_MAGIC  = 0x55 ('U')
byte 1      UP_VERSION = 1
byte 2..n-1 opcode / operand groups
byte n      UP_OP_END = 0
```

Operands use unsigned (`u8`, `u16`) or signed (`s16`, range -2000..2000)
integers. Opcodes:

| Opcode | Name | Operands | Meaning |
| --- | --- | --- | --- |
| 1 | SCOPE_ENTER | – | open a nested lexical scope |
| 2 | SCOPE_EXIT | – | close the current scope (closes its upvalues) |
| 3 | VAR | u16 id, s16 initial | declare an integer local |
| 4 | SET | u16 id, s16 value | assign a local |
| 5 | CLOSURE | u16 id, u8 n, n×(u16 varId, u8 mode, s16 writeConst) | build a closure over n cells; mode 1=read, 2=write, 3=read+write |
| 6 | ALIAS | u16 newId, u16 sourceId | second name for a closure or wrapper result; cells are shared, not copied |
| 7 | WRAP_BEGIN | u16 id | open an intermediate function stream (indirect capture) |
| 8 | WRAP_END | u16 wrapId, u16 resultClosureId | close the wrapper; it returns the named closure |
| 9 | CALL | u16 closureId | invoke a closure |
| 10 | DROP | u16 id | overwrite the script-side reference with null; the host keeps its handle |
| 11 | FIBER_BEGIN | u16 id | open a Fiber body stream |
| 12 | FIBER_END | – | close the Fiber body stream |
| 13 | YIELD | s16 value | suspend the enclosing Fiber |
| 14 | DEPTH | u8 depth (1..140) | bounded deep recursive call while an upvalue is open, forcing stack growth |
| 15 | GC | – | explicit host garbage collection at this boundary |
| 16 | RESUME | u16 fiberId | resume a suspended Fiber |
| 17 | SHADOW | u16 outerId, s16, u16 innerId, s16, u16 innerClosureId | same textual name, distinct declaration identities and cells |
| 18 | SLOTREUSE | u16,s16,u16, u16,s16,u16 | two sequential scopes that reuse one stack slot but keep distinct cells |

### Length and legality rules

* Maximum input is bounded by the decoded limits in `upvalue_fuzz.h`:
  240 ops, 200 declarations, 16 streams, 12 scopes, 8 captures per closure,
  nesting depth 4 and call depth 140.
* A truncated operand group, a missing `END`, an unknown opcode, an out-of-range
  integer or any limit overflow is rejected with an `UpDecodeStatus` before a
  VM is created.
* Every declaration reference must name an existing declaration. A closure may
  only capture a variable that is lexically visible at its definition; an
  object may only be called, resumed, aliased or dropped after it is born and
  before it is dropped; `YIELD`/`DEPTH` may only appear inside a Fiber/wrapper
  stream and `GC` only in the main stream.
* Out-of-range or dangling references are therefore controlled rejections, not
  interpreter failures. The checker never turns its own invalid handle into a
  reported Wren defect.

## What "the same plan" exercises

A single plan is interpreted by the C model and rendered to a benign Wren
program that is then run through the real public chain: `wrenInterpret`,
`wrenMakeCallHandle`, `wrenCall`, `wrenGetVariable`, list slot access and
`WrenHandle` rooting. Every closure read/write and every Fiber hand-back is an
encoded event appended to a Wren list; the host compares each event against the
model and records the first differing declaration, operation and value.

The fixed mega-plan (`--render 0`) contains, in one program: closures that both
read and write one shared cell, a Fiber suspended while holding an open
capture, two-level indirect capture through an intermediate wrapper, a capture
closed after its scope exits, same-name shadowing, same-slot reuse, a deep call
that grows the Value stack while upvalues are open, explicit GCs between call
boundaries, and host-retained handles used after every script reference is
dropped.

## Memory and observability

`upMovingReallocate` implements `WrenConfiguration.reallocateFn`: every
non-zero reallocation is placed at a fresh address, the valid old contents are
copied and the old block is freed explicitly, so a stack relocation cannot pass
merely because `realloc` kept the address. The model and the allocator manage
their own memory; model objects are never placed inside Wren.

When the VM is compiled with `-DWREN_FUZZ_TEST_HOOKS`, read-only counters in
`wren_vm.c`/`wren_value.c` report Value-stack moves that occurred while open
upvalues existed and the number of GCs. Normal builds do not define the macro,
so the production struct layout and semantics are unchanged; the accessor
symbols then do not exist. The initial heap threshold is deliberately small and
GC is invoked explicitly between call boundaries, so every run really moves
memory and collects rather than merely iterating many times.
