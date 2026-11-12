# Local function calls in uBPF

This document describes the interpreter-only extension that adds intra-bytecode
function calls to uBPF.  The public API (`ubpf_load`, `ubpf_exec`,
`ubpf_translate`, `ubpf_compile`, `ubpf_register`, ...) is unchanged; the new
behaviour is encoded in the bytecode and enabled automatically at load time.

## Encoding

A `CALL` instruction (`opcode = 0x85`) selects its callee with the `src`
field:

| `src` | meaning |
|-------|---------|
| 0     | registered C helper.  `imm` is the helper index, as before. |
| 1     | local call.  Target is `call_pc + 1 + imm`, where `call_pc` is the index of the CALL instruction. |
| other | rejected at load time. |

The target is a signed addition in **8-byte instruction slots**.  `LDDW`
occupies two slots, so slot indices (not byte offsets) are used consistently.
For a local call, `dst` and `offset` must both be zero; they are reserved and a
nonzero value is rejected.

Example: a local call at PC 9 with `imm = 2` calls the instruction at PC 12.

## Function segmentation

A program that contains at least one local call is divided into function
segments before it is accepted:

1. **Function entries** are instruction 0 plus every local-call target.
   Entries are sorted by position; duplicates (a function reached from several
   call sites) collapse to one entry.
2. A **function segment** runs from one entry up to (but not including) the
   next entry, or to the end of the code for the last one.  Instruction 0 is
   always the entry function.
3. Every local-call target must be:
   * inside the code,
   * the first slot of a real instruction (never the data slot of an `LDDW`),
   * computed without wrapping the addition.

## Control-flow containment

All instructions are checked, including those that are unreachable at run time.

* An ordinary sequential successor must stay inside its function.  For `LDDW`
   the successor is two slots ahead.  For a conditional jump the fallthrough
   successor must also stay inside, in addition to the branch target.
* A jump target (`JA` or any conditional jump) must be inside the same
   function as the jump.
* A local call's return continuation (`call_pc + 1`) must be inside the
   caller's function.
* `EXIT` has no sequential successor, so an `EXIT` immediately before the next
   function entry is legal.

Loops fully contained in a single function are allowed.

## Call graph

The call graph is built from every local call.  It must be acyclic: direct
recursion (a function calling itself) and indirect recursion (any cycle) are
both rejected.  The depth limit is the **longest call chain**, not the number
of functions and not the number of nodes visited:

* the entry frame counts as one frame;
* at most `UBPF_MAX_FRAMES` (8) frames may be active at the same time.

A chain of entry + 7 callees (8 frames) is accepted; entry + 8 callees
(9 frames) is rejected at load time.  Shared callees are counted once per
distinct path, so a wide tree with many functions is fine as long as no path is
deeper than the limit.  Both forward and backward calls, including backward
calls to an already-seen function, are supported as long as no recursion
results.

## Interpreter calling convention

When a local call is executed:

* `r1`-`r5` are passed as arguments, unchanged.
* `r6`-`r9` are inherited from the caller.  The callee may modify them, but
  the caller's values are saved in the frame and restored when the callee
  returns.
* `r10` points at the end of a fresh `UBPF_STACK_SIZE` stack that belongs to
  this frame.  Each active frame has its own stack; writes in one frame are not
  visible in another.
* `r0` is the return value.
* `r1`-`r5` are not promised to be preserved across a call; do not rely on
  their contents after returning.

`EXIT` in a callee returns to the instruction following the call and restores
`r6`-`r9` and the caller's `r10`.  Only `EXIT` in the entry function returns to
the host.

Each `ubpf_exec` call starts with a fresh interpreter state: return addresses
and frame depth are not carried over between executions.  A failed execution
does not write the caller's `bpf_return_value`.  The frames live on the
interpreter's C stack for the duration of one `ubpf_exec`; re-executing,
executing after a failed nested call, and loading different code after
unloading all start from scratch.

## Bounds checking

With bounds checking enabled, the valid memory regions for a frame are:

* the original host buffer passed to `ubpf_exec` (as `r1`/`r2`),
* the frame's own stack,
* the stacks of **active ancestor frames**, if the program obtained a pointer
  to them (for example through an argument).

The whole byte interval accessed must fall inside **one** of those regions.
The contiguous pool that holds the individual stacks is not treated as a
single valid block, so an access that straddles a frame boundary is rejected.

A callee's frame stops being valid as soon as it returns.  Storing the address
of a returned frame's stack slot and then loading from it is an error, because
the frame is no longer in the active set.

Checks are interval-based (address plus size must lie inside a region).  They
do not attempt to prove where a pointer came from, and they cannot detect a
pointer to a reused stack slot after a later call re-enters the same physical
frame.  Bounds checking can be disabled with `ubpf_toggle_bounds_check`, exactly
as before; when disabled these checks do no work.

Address arithmetic is performed without wrapping: an interval whose end
computed from `base + size` wraps around is rejected, as is a base address
outside every allowed region.

## Helpers and local targets

Helper indices and local call targets are completely separate namespaces.  A
local call with `src = 1` never dispatches to a helper, even if its `imm` happens
to equal a registered helper index.  `src = 0` calls behave exactly as before.

## Unwind helpers

The unwind helper set with `ubpf_set_unwind_function_index` keeps its
semantics.  When it returns 0, the whole interpretation ends immediately —
regardless of how many local frames are active — and the host receives 0.
Returning nonzero continues normally.  This differs from `EXIT`, which pops
only the innermost local frame.

## Validation errors

Load errors use the existing `errmsg` mechanism and include the original PC
of the offending call or control-flow instruction, for example:

```
Failed to load code: local call with nonzero dst at PC 3
Failed to load code: local call target out of bounds at PC 4
Failed to load code: local call target is middle of lddw at PC 5
Failed to load code: cross-function jump at PC 6
Failed to load code: fallthrough crosses function boundary at PC 7
Failed to load code: recursive local call at PC 8
Failed to load code: local call chain too deep at PC 9
```

## Scope: interpreter versus JIT

Local calls are implemented only in `ubpf_exec` (the interpreter).

* `ubpf_translate` and `ubpf_compile` **fail** for any program that contains a
  local call, with an error naming the offending PC.  No x86 machine code and
  no cached compiled version is produced for such a program.
* After a refused translation, the program can still be interpreted.
* Translating ordinary programs (no local calls) is unaffected, including
  after unloading a local-call program and loading a plain one.
* No local-call machine code is generated for any architecture; translation
  results are not executed on non-x86 hosts.

The ELF loader (`ubpf_load_elf`) is unchanged: it resolves and relocates
`src = 0` helper symbols as before, and it does not describe local-call
symbols or add new relocation types.  Existing ELF helper programs therefore
continue to work.
