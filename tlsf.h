#ifndef INCLUDED_tlsf
#define INCLUDED_tlsf

/*
** Two Level Segregated Fit memory allocator, version 3.1.
** Written by Matthew Conte
**	http://tlsf.baisoku.org
**
** Based on the original documentation by Miguel Masmano:
**	http://www.gii.upv.es/tlsf/main/docs
**
** This implementation was written to the specification
** of the document, therefore no GPL restrictions apply.
** 
** Copyright (c) 2006-2016, Matthew Conte
** All rights reserved.
** 
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are met:
**     * Redistributions of source code must retain the above copyright
**       notice, this list of conditions and the following disclaimer.
**     * Redistributions in binary form must reproduce the above copyright
**       notice, this list of conditions and the following disclaimer in the
**       documentation and/or other materials provided with the distribution.
**     * Neither the name of the copyright holder nor the
**       names of its contributors may be used to endorse or promote products
**       derived from this software without specific prior written permission.
** 
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
** ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
** WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
** DISCLAIMED. IN NO EVENT SHALL MATTHEW CONTE BE LIABLE FOR ANY
** DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
** (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
** LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
** ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
** SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stddef.h>

#if defined(__cplusplus)
extern "C" {
#endif

/* tlsf_t: a TLSF structure. Can contain 1 to N pools. */
/* pool_t: a block of memory that TLSF can manage. */
typedef void* tlsf_t;
typedef void* pool_t;

/* Create/destroy a memory pool. */
tlsf_t tlsf_create(void* mem);
tlsf_t tlsf_create_with_pool(void* mem, size_t bytes);
void tlsf_destroy(tlsf_t tlsf);
pool_t tlsf_get_pool(tlsf_t tlsf);

/* Add/remove memory pools. */
pool_t tlsf_add_pool(tlsf_t tlsf, void* mem, size_t bytes);
void tlsf_remove_pool(tlsf_t tlsf, pool_t pool);

/* malloc/memalign/realloc/free replacements. */
void* tlsf_malloc(tlsf_t tlsf, size_t bytes);
void* tlsf_memalign(tlsf_t tlsf, size_t align, size_t bytes);
void* tlsf_realloc(tlsf_t tlsf, void* ptr, size_t size);
void tlsf_free(tlsf_t tlsf, void* ptr);

/* Returns internal block size, not original request size */
size_t tlsf_block_size(void* ptr);

/* Overheads/limits of internal structures. */
size_t tlsf_size(void);
size_t tlsf_align_size(void);
size_t tlsf_block_size_min(void);
size_t tlsf_block_size_max(void);
size_t tlsf_pool_overhead(void);
size_t tlsf_alloc_overhead(void);

/* Debugging. */
typedef void (*tlsf_walker)(void* ptr, size_t size, int used, void* user);
void tlsf_walk_pool(pool_t pool, tlsf_walker walker, void* user);
/* Returns nonzero if any internal consistency check fails. */
int tlsf_check(tlsf_t tlsf);
int tlsf_check_pool(pool_t pool);

/*
** Bounded, read-only heap audit.
**
** tlsf_check() and tlsf_check_pool() assume a trustworthy heap: once a size
** field or a free-list link is corrupted, walking the structure can read out
** of bounds, trip assertions, or loop forever. The audit entry point below is
** intended for the opposite situation: a stopped process whose mapped memory
** ranges are still known to the caller, while the heap contents themselves may
** be arbitrarily corrupted.
**
** Guarantees:
**  - Strictly read-only with respect to the control area and pool bytes,
**    including on every error path. No links are repaired, no bitmaps cleared,
**    no user payloads touched.
**  - Every pointer derived from heap contents is proven to lie inside one of
**    the caller-supplied ranges before it is dereferenced; a bad link is
**    reported instead of followed. Offset arithmetic is overflow-checked.
**  - Bounded running time: physical block scans stop at the first untrusted
**    boundary, and free-list traversal is limited to the number of free
**    blocks discovered by the physical scan. This audit is not O(1).
**  - No malloc()/TLSF allocation is performed. The caller supplies a scratch
**    workspace; see tlsf_audit_workspace_size().
**
** Limitations:
**  - This verifies structural integrity only. It cannot decide whether an
**    arbitrary user pointer is valid, and a heap reported as corrupt must not
**    be used for further allocation.
**
** Descriptor conventions:
**  - control/control_bytes describe the full readable control structure
**    (tlsf_size() bytes when healthy).
**  - Each pool descriptor uses the same base and byte count that were passed
**    to tlsf_add_pool(); the audit treats exactly [base, base+bytes) as the
**    readable mapping. TLSF's first block header starts one word before base,
**    so its size word lands at base (inside the mapping) while its
**    prev_phys_block word lands at base-8 (outside the mapping). That
**    out-of-pool word is never dereferenced, so it need not be mapped.
**  - Ranges must be readable for their full extent and must not overlap
**    (adjacent ranges are allowed).
**  - Pool result offsets are relative to the first block header, i.e.
**    pool_base - tlsf_alloc_overhead() (the first block is therefore at
**    offset 0); control offsets are relative to the control base.
*/

/* Outcome of an audit run. The four values are always distinguishable. */
typedef enum tlsf_audit_status
{
	TLSF_AUDIT_OK = 0,        /* heap is structurally consistent */
	TLSF_AUDIT_CORRUPT = 1,   /* inconsistent heap contents found */
	TLSF_AUDIT_EINVAL = 2,    /* invalid descriptor/argument */
	TLSF_AUDIT_ENOSPC = 3     /* supplied workspace is too small */
} tlsf_audit_status_t;

/* Which supplied range a report refers to. */
typedef enum tlsf_audit_region
{
	TLSF_AUDIT_REGION_CONTROL = 0,
	TLSF_AUDIT_REGION_POOL = 1
} tlsf_audit_region_t;

/* Fine-grained corruption category. Meaningful only when status is CORRUPT. */
typedef enum tlsf_audit_corruption
{
	TLSF_AUDIT_CORRUPT_NONE = 0,
	/* Physical block chain (per pool, checked in caller pool order). */
	TLSF_AUDIT_CORRUPT_BLOCK_SIZE,      /* zero/short/unaligned/oversize/wrapping size */
	TLSF_AUDIT_CORRUPT_BLOCK_ALIGN,     /* block header is not aligned to tlsf_align_size() */
	TLSF_AUDIT_CORRUPT_PREV_FLAG,       /* prev-free flag disagrees with predecessor */
	TLSF_AUDIT_CORRUPT_PREV_POINTER,    /* prev_phys_block back-link wrong or unmapped */
	TLSF_AUDIT_CORRUPT_COALESCE,        /* two adjacent free blocks were not merged */
	TLSF_AUDIT_CORRUPT_SENTINEL,        /* zero-size end sentinel missing or malformed */
	TLSF_AUDIT_CORRUPT_TRUNCATED,       /* block chain runs past the supplied pool range */
	/* Bitmaps and free lists (checked in fl/sl index order). */
	TLSF_AUDIT_CORRUPT_BITMAP,          /* fl/sl bitmap disagrees with list heads */
	TLSF_AUDIT_CORRUPT_LIST_TARGET,     /* link outside ranges, to a used block, or null impostor */
	TLSF_AUDIT_CORRUPT_LIST_ALIGN,      /* link value is not properly aligned */
	TLSF_AUDIT_CORRUPT_LIST_LINK,       /* next/prev links are not reciprocal */
	TLSF_AUDIT_CORRUPT_LIST_CYCLE,      /* free-list traversal exceeded its bounded length */
	TLSF_AUDIT_CORRUPT_LIST_DUPLICATE,  /* a free block appears twice in the lists */
	TLSF_AUDIT_CORRUPT_LIST_MISSING,    /* a physically free block is absent from every list */
	TLSF_AUDIT_CORRUPT_WRONG_BUCKET     /* block size does not map to its list's fl/sl */
} tlsf_audit_corruption_t;

typedef struct tlsf_audit_result
{
	tlsf_audit_status_t status;
	tlsf_audit_corruption_t corruption;
	tlsf_audit_region_t region;
	int pool_index;       /* -1 when region is CONTROL */
	size_t offset;        /* byte offset within the offending region */
	const char* detail;   /* static human-readable description, never NULL */
} tlsf_audit_result_t;

/* A pool descriptor: the exact arguments originally given to tlsf_add_pool(). */
typedef struct tlsf_audit_pool
{
	const void* base;
	size_t bytes;
} tlsf_audit_pool_t;

/*
** Required workspace size in bytes for pool_count pools whose raw sizes add
** up to total_pool_bytes. The audit only needs scratch space proportional to
** the number of physical blocks, never to the number of list links it may
** refuse to follow. Returns (size_t)-1 if the requirement overflows size_t.
** If tlsf_audit() receives fewer bytes it returns TLSF_AUDIT_ENOSPC without
** examining any heap contents beyond descriptor validation.
*/
size_t tlsf_audit_workspace_size(size_t pool_count, size_t total_pool_bytes);

/*
** Run a bounded, read-only audit.
**
** Descriptor problems (null/misaligned bases, undersized control range,
** zero-length or undersized pools, overlapping ranges) yield TLSF_AUDIT_EINVAL.
** Otherwise the first structural inconsistency found is returned through
** result (may be NULL if only the status code is wanted). Physical pools are
** examined in the order given and free-list buckets in fl/sl index order, so
** repeated calls on unchanged memory produce identical results.
*/
tlsf_audit_status_t tlsf_audit(
	const void* control, size_t control_bytes,
	const tlsf_audit_pool_t* pools, size_t pool_count,
	void* workspace, size_t workspace_bytes,
	tlsf_audit_result_t* result);

#if defined(__cplusplus)
};
#endif

#endif
