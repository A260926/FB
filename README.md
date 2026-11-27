# tlsf
Two-Level Segregated Fit memory allocator implementation.
Written by Matthew Conte (matt@baisoku.org).
Released under the BSD license.

Features
--------
  * O(1) cost for malloc, free, realloc, memalign
  * Extremely low overhead per allocation (4 bytes)
  * Low overhead per TLSF management of pools (~3kB)
  * Low fragmentation
  * Compiles to only a few kB of code and data
  * Support for adding and removing memory pool regions on the fly

Caveats
-------
  * Currently, assumes architecture can make 4-byte aligned accesses
  * Not designed to be thread safe; the user must provide this

Notes
-----
This code was based on the TLSF 1.4 spec and documentation found at:

	http://www.gii.upv.es/tlsf/main/docs

It also leverages the TLSF 2.0 improvement to shrink the per-block overhead from 8 to 4 bytes.

History
-------
2016/04/10 - v3.1
  * Code moved to github
  * tlsfbits.h rolled into tlsf.c
  * License changed to BSD

2014/02/08 - v3.0
  * This version is based on improvements from 3DInteractive GmbH
  * Interface changed to allow more than one memory pool
  * Separated pool handling from control structure (adding, removing, debugging)
  * Control structure and pools can still be constructed in the same memory block
  * Memory blocks for control structure and pools are checked for alignment
  * Added functions to retrieve control structure size, alignment size, min and max block size, overhead of pool structure, and overhead of a single allocation
  * Minimal Pool size is tlsf_block_size_min() + tlsf_pool_overhead()
  * Pool must be empty when it is removed, in order to allow O(1) removal

2011/10/20 - v2.0
  * 64-bit support
  * More compiler intrinsics for ffs/fls
  * ffs/fls verification during TLSF creation in debug builds

2008/04/04 - v1.9
  * Add tlsf_heap_check, a heap integrity check
  * Support a predefined tlsf_assert macro
  * Fix realloc case where block should shrink; if adjacent block is in use, execution would go down the slow path

2007/02/08 - v1.8
  * Fix for unnecessary reallocation in tlsf_realloc

2007/02/03 - v1.7
  * tlsf_heap_walk takes a callback
  * tlsf_realloc now returns NULL on failure
  * tlsf_memalign optimization for 4-byte alignment
  * Usage of size_t where appropriate

2006/11/21 - v1.6
  * ffs/fls broken out into tlsfbits.h
  * tlsf_overhead queries per-pool overhead

2006/11/07 - v1.5
  * Smart realloc implementation
  * Smart memalign implementation

2006/10/11 - v1.4
  * Add some ffs/fls implementations
  * Minor code footprint reduction

2006/09/14 - v1.3
  * Profiling indicates heavy use of blocks of size 1-128, so implement small block handling
  * Reduce pool overhead by about 1kb
  * Reduce minimum block size from 32 to 12 bytes
  * Realloc bug fix

2006/09/09 - v1.2
  * Add tlsf_block_size
  * Static assertion mechanism for invariants
  * Minor bugfixes 

2006/09/01 - v1.1
  * Add tlsf_realloc
  * Add tlsf_walk_heap

2006/08/25 - v1.0
  * First release

Bounded read-only heap audit
----------------------------
`tlsf_check()` / `tlsf_check_pool()` assume a trustworthy heap: if a size field
or a free-list link is corrupted, walking the structure can read out of bounds,
trip an assertion, or loop forever. For diagnosing a stopped process whose
mapped ranges are still known, `tlsf_audit()` provides a bounded, strictly
read-only audit:

  * The caller passes the readable control range and every pool's base/byte
    count. A pointer found in (possibly corrupted) heap contents is proven to
    lie fully inside one of those ranges before it is dereferenced, and all
    offset arithmetic is overflow-checked, so a bad link is reported rather
    than followed. The first block's out-of-pool `prev_phys_block` word is
    never read.
  * Each pool's physical blocks are checked (in caller order) for size,
    alignment, prev-free flags, back-links, coalescing of adjacent free
    blocks, and the end sentinel; then the two-level bitmaps and free lists
    are checked (in bucket order). Only blocks the physical scan proved free,
    plus the shared null node, may be free-list targets.
  * Runs never allocate; the caller supplies scratch workspace sized by
    `tlsf_audit_workspace_size()`, and an undersized workspace returns
    `TLSF_AUDIT_ENOSPC`. The four outcomes (OK, CORRUPT, EINVAL, ENOSPC) are
    distinct; a CORRUPT report carries a category, pool/control region, pool
    index, and relative offset. The audit is not O(1).

This verifies structural integrity only: it cannot decide whether an arbitrary
user pointer is valid, and a heap reported as corrupt must not be used for
further allocation. See the declarations and contract comments in `tlsf.h`,
and the corruption-injection / guard-page tests in `checks/audit_contract.c`.
