# Read-only x86-64 control flow recovery

`cs_cfg_recover_x86_64()` (declared in `include/capstone/cfg.h`, implemented
in `cfg.c`) recovers a control flow graph from offline x86-64 firmware
fragments. It is a pure analysis facility: it never executes machine code,
never dereferences a host pointer computed from a target address, and only
reads bytes from the `cs_cfg_region` buffers supplied by the caller. Every
report is independently owned and released with `cs_cfg_report_free()`.

## Inputs and validation

* Each region carries a virtual start address, a byte buffer, a length and
  independent `executable` / `read_only` flags.
* Address intervals must not wrap (`address + size` must not overflow) and
  must not overlap; invalid intervals, NULL buffers, zero-length regions,
  entries outside an executable region, NULL output pointers and non-empty
  entry arrays with NULL storage return `CS_ERR_OPTION`. Allocation failure
  returns `CS_ERR_MEM`.
* An empty entry array always yields an empty, non-incomplete report.
* Decoding uses a detail enabled Capstone handle opened by the engine
  itself; no second instruction decoder exists and skipdata behavior and the
  existing `cs_*` decode API are unchanged.

## Graph construction

* Recovery starts from the explicit entries with every general purpose
  register abstractly unknown, then runs a FIFO worklist to a fixed point.
  Loops and indirect edges that add predecessors later simply re-propagate;
  first-answer target sets are never cached and loop trip counts are never
  enumerated.
* Direct `JMP`, conditional jumps, `CALL` and `RET` use the decoded target.
  Conditional jumps keep both successors. `RET` is a normal path end.
* `CALL` records its target with a `CS_CFG_EDGE_CALL` edge but only enqueues
  the synthetic return successor, on which all 16 GPRs are reset to unknown.
  A callee is entered only when it is also an explicit entry.
* A branch into a non-executable or unmapped address, a truncated or illegal
  instruction, and an unresolvable indirect jump each produce an addressed
  diagnostic and mark the report incomplete; everything else is still
  recovered.
* Data gaps are never linearly swept: straight-line decoding stops when the
  next address is not backed by an executable region.

## Register abstraction

* Each of the 16 general purpose registers (RAX..R15, including RSP/RBP) is
  either unknown or a sorted, unique set of at most `CS_CFG_MAX_VALUES`
  (eight) `uint64_t` values. Entry state is fully unknown; control-flow joins
  take the union and promote to unknown past the cap.
* Modeled register writes: `MOV` (register and immediate forms), `LEA`,
  register/immediate `ADD`, `SUB`, `AND`, and same-register `XOR` (which
  yields zero). Operands are 32 or 64 bits. 32 bit writes zero extend to 64
  bits; arithmetic wraps at the operand width.
* Any other register-writing instruction conservatively invalidates the full
  64 bit register identity through `cs_regs_access`, including 8 and 16 bit
  aliases (`AL`, `BX`, `R8B`, ...), so stale constants cannot feed later
  address computations.

## Memory and indirect jumps

* Read support is limited to 4 or 8 byte `MOV` loads and 4 byte `MOVSXD`
  (sign extended) whose effective address is a finite combination of a known
  base, index, scale and displacement. RIP relative addressing is evaluated
  against the address of the following instruction.
* Every access must stay inside a region marked `read_only`; crossing a
  region boundary, touching a non-read-only region, an unknown index/base or
  a result set exceeding the value cap makes the loaded value unknown.
* This recovers absolute eight-byte jump tables and PIC tables of signed
  four-byte displacements added to a table base. The engine does not scan
  data regions for values that look like addresses.
* Resolved indirect-jump candidates each enter the graph as an
  `CS_CFG_EDGE_INDIRECT` edge and are also listed in `indirect_targets`; the
  unknown case leaves a `CS_CFG_DIAG_UNRESOLVED_INDIRECT` diagnostic.

## Overlapping decode

* Every start address is decoded at most once. A new target landing inside
  an already decoded instruction starts a second, independent decode chain;
  both chains are retained and the intersecting byte range plus both start
  addresses appear in `overlaps[]`.
* A target landing on an existing instruction boundary splits the basic
  block there and reconnects the old edges.

## Output ordering

All output arrays are canonically sorted so that permuting the input regions
or entries gives identical results: blocks by start address; edges by
(source, kind, target); indirect targets by (site, target); overlaps by
(start, first, second, end); diagnostics by (address, kind). The
per-block register array follows the block order and holds
`CS_CFG_GP_REG_COUNT` entries per block.

## Current scope limits

* x86-64 (`CS_ARCH_X86` / `CS_MODE_64`) only.
* Memory reads are restricted to read-only regions, 4/8 byte widths, and the
  addressing forms listed above; writes to memory are not modeled.
* The tracked arithmetic is MOV/LEA/ADD/SUB/AND/XOR on 32/64 bit GPRs;
  everything else invalidates the affected registers rather than guessing.
