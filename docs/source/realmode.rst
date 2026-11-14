Real-Mode Physical Graph API
============================

``inc/bdx86_realmode.h`` and ``bddisasm/bdx86_realmode.c`` provide a small, dependency-free C API that turns a set of
immutable physical ROM fragments into a read-only logical control-flow graph. The input is never executed: the graph
is produced by statically decoding instructions with ``NdDecodeEx`` in 16-bit mode and by following control flow
across the supplied entry points. The emulator (bdshemu) is not involved.

The API is part of the ``bddisasm`` library target; no additional library is required. Only C and the standard
library are used by the implementation.

Why
---

In real-mode code the same physical byte can be addressed through several different ``CS:IP`` pairs. A jump inside a
segment is a 16-bit wrap-around, while the physical successor of that jump depends on the segment base. Drawing the
graph on flat physical addresses therefore merges instructions that are logically distinct and loses the
segment-relative branch and return relationships. This API keeps the graph in logical ``(CS, IP)`` node space, and
keeps physical provenance per fetched byte.

Input
-----

.. code-block:: c

    typedef struct _RM_FRAGMENT
    {
        uint32_t        Id;             // Stable caller identifier.
        uint32_t        PhysicalBase;   // Physical address of Bytes[0].
        const uint8_t   *Bytes;         // Immutable fragment bytes.
        size_t          Size;
    } RM_FRAGMENT;

    typedef struct _RM_ENTRY
    {
        uint16_t        Cs;
        uint16_t        Ip;
    } RM_ENTRY;

    RM_GRAPH *RmBuildGraph(const RM_FRAGMENT *Fragments, size_t FragmentCount, int A20Enabled,
                           const RM_ENTRY *Entries, size_t EntryCount);

Fragments must be non-overlapping, non-empty and have unique IDs; otherwise ``RmBuildGraph`` returns ``NULL``.
Entry points are logical ``(CS, IP)`` pairs.

The builder reads the fragment bytes only during ``RmBuildGraph``. Every reported byte, every byte source and every
disassembly string is copied into the graph, so it remains valid after the caller frees the input.

Fragment and entry order does not influence the result. Internally fragments are sorted by physical base and entries
by ``(CS, IP)``, and all output tables are sorted into a canonical order, so permuting the input produces byte-identical
graph content and ordering.

Address translation
-------------------

The physical address of a logical byte is ``(CS << 4) + IP``. When A20 is disabled, bit 20 is masked off; when A20 is
enabled, it is preserved. Near branches and sequential successors wrap in the 16-bit ``IP``; far immediate transfers
update ``CS:IP``.

Two different ``(CS, IP)`` pairs may name the same physical byte while having different logical successors. For
example, ``0001:fffe`` and ``1000:000e`` both point at physical ``0x1000e``, but after a two-byte ``EB 02`` the
follow-up addresses are ``0001:0000`` (physical ``0x10``) and ``1000:0010`` (physical ``0x10010``). Nodes are keyed
by logical ``(CS, IP)``, never by physical address, so they are never merged.

Instruction fetch collects bytes individually, each resolved through its own physical address. An instruction may
span physical fragment boundaries and the A20 boundary; in the latter case the bytes on the far side are masked per
byte. Each byte keeps its fragment ID, in-fragment offset and effective physical address. A single instruction may
not fetch past ``IP = 0xFFFF``; if a byte is missing before the instruction is complete, the path stops without
zero padding.

Routines and ownership
----------------------

A routine is rooted at a logical entry: a caller-supplied entry point, or the direct target of a supported near or
far ``CALL``. A routine contains sequential flow, direct jumps and both arms of conditional branches. Bytes shared
by several logical routines produce one node with several routine owners; the same holds for shared edges and
termination records, which are reported once with the complete owner set.

Edges and calls
---------------

The graph reports the following transfer kinds:

===============================  ==========================================================
Kind                             Meaning
===============================  ==========================================================
``RM_EDGE_SEQUENTIAL``           Ordinary successor.
``RM_EDGE_JUMP_NEAR``/``FAR``    Direct jump. A jump never creates a new call frame.
``RM_EDGE_BRANCH_TAKEN``         Conditional branch, taken. Flags are never evaluated.
``RM_EDGE_BRANCH_FALLTHROUGH``   Conditional branch, not taken.
``RM_EDGE_CALL_NEAR``/``FAR``    Direct call into the target routine.
``RM_EDGE_RETURN_NEAR``/``FAR``  Matched return: call site, return site and continuation.
===============================  ==========================================================

Direct near and immediate far ``CALL`` instructions enter the target routine. A ``CALL`` is **not** automatically
transparent: the continuation is reached only when the callee has a matching return summary. Conditional branches
keep both successors without evaluating flags.

Each routine has a return summary: the set of ``RET``/``RETF`` sites reachable at call-depth zero. When a call
site's kind matches an entry in the callee summary, the corresponding return edge is produced and annotated
with:

* ``CallSiteNode`` - the exact ``CALL`` instruction that was matched;
* ``ReturnSiteNode`` - the ``RET``/``RETF`` instruction that produced the edge.

Sibling calls are never stitched together: each return edge refers to its own call site and its own continuation.

* A near ``RET`` restores the saved ``IP`` but keeps the ``CS`` of the return site. If a routine reached through a
  near call performs a far tail jump before returning, the returned-to ``CS`` is the one of that far ``RET``,
  not the caller's original ``CS``.
* A far ``RETF`` restores both the saved ``CS`` and ``IP``.

Because summaries are computed as a least fixpoint:

* recursion with a base return exit propagates the base return to callers and the recursive continuation is
  reachable;
* recursion without a base return exit produces an empty summary, so no continuation and no fabricated return is
  created, regardless of any depth limit;
* a jump never introduces a new call frame, and a top-level ``RET``/``RETF`` is an **open return exit** (a
  termination record), because there is no call frame to match.

Terminations
------------

The supported subset is exactly paired 16-bit near/far calls and ``RET``/``RETF`` without an immediate. Everything
else that ends a path produces an opaque termination record carrying the instruction position. Reasons:

* ``RM_TERM_HLT`` - normal terminal instruction.
* ``RM_TERM_OPEN_RETURN`` - top-level return with no call frame.
* ``RM_TERM_UNRESOLVED_RETURN`` - a ``RET``/``RETF`` which does not match the annotated call site (for example, a
  near call to a routine whose only exit is ``RETF``). The record names the mismatching call site; stack tricks are
  never simulated.
* ``RM_TERM_INDIRECT`` - indirect transfer.
* ``RM_TERM_CONTROL32`` - 32-bit control transfer (rel32 branches, 32-bit far pointers, 32-bit ``RET``/``IRET``).
* ``RM_TERM_INTERRUPT`` - ``INT``/``INT1``/``INT3``/``INTO``/``IRET``/``UIRET``.
* ``RM_TERM_SP_WRITE`` - a non-call/ret instruction that writes ``SP``.
* ``RM_TERM_STACK_ACCESS`` - a non-call/ret instruction that accesses the stack.
* ``RM_TERM_MEMORY_WRITE`` - a non-call/ret instruction that writes memory.
* ``RM_TERM_RET_IMMEDIATE`` - ``RET``/``RETF`` with an immediate stack adjustment.
* ``RM_TERM_UNSUPPORTED_TRANSFER`` - other control transfers (``SYSENTER``, ``XBEGIN``, ...).
* ``RM_TERM_TRUNCATED`` - a byte is missing, or the fetch would pass ``IP = 0xFFFF``.
* ``RM_TERM_DECODE_ERROR`` - ``NdDecodeEx`` rejected the fetched bytes.

An instruction that cannot be classified as a path terminator continues to its sequential successor. If one entry
cannot be resolved, the other entries are still reported.

Read-only accessors
-------------------

.. code-block:: c

    int      RmA20Enabled(const RM_GRAPH *);
    size_t   RmRoutineCount(const RM_GRAPH *);
    const RM_ROUTINE *RmRoutineAt(const RM_GRAPH *, size_t);
    int      RmFindRoutine(const RM_GRAPH *, uint16_t Cs, uint16_t Ip, uint32_t *Index);
    size_t   RmNodeCount(const RM_GRAPH *);
    const RM_NODE *RmNodeAt(const RM_GRAPH *, size_t);
    int      RmFindNode(const RM_GRAPH *, uint16_t Cs, uint16_t Ip, uint32_t *Index);
    size_t   RmNodeRoutineCount(const RM_GRAPH *, uint32_t NodeIndex);
    uint32_t RmNodeRoutineAt(const RM_GRAPH *, uint32_t NodeIndex, size_t OwnerIndex);
    size_t   RmEdgeCount(const RM_GRAPH *);
    const RM_EDGE *RmEdgeAt(const RM_GRAPH *, size_t);
    size_t   RmEdgeRoutineCount(const RM_GRAPH *, size_t EdgeIndex);
    uint32_t RmEdgeRoutineAt(const RM_GRAPH *, size_t EdgeIndex, size_t OwnerIndex);
    size_t   RmTerminationCount(const RM_GRAPH *);
    const RM_TERMINATION_RECORD *RmTerminationAt(const RM_GRAPH *, size_t);
    void     RmFreeGraph(RM_GRAPH *);

``RmFreeGraph(NULL)`` is allowed. Routines and nodes are returned in ascending ``(CS, IP)`` order; edges in ascending
``(from node, kind, to node, call site, return site)`` order; terminations in ascending
``(node, reason, call site)`` order.

Every node exposes:

* its logical ``Cs``/``Ip``;
* ``Decoded`` and ``Length``;
* ``SourceCount``, the copied ``Bytes[]`` and a parallel ``Sources[]`` array of
  ``{FragmentId, FragmentOffset, PhysicalAddress}``;
* a best-effort ``Text[]`` disassembly produced by ``NdToText`` (empty when formatting is unavailable).

Compatibility
-------------

The existing ``NdDecodeEx`` and formatting APIs are unchanged; the new module only consumes them. Decoding always
uses ``ND_CODE_16``/``ND_DATA_16``.
