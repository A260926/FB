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
** Read-only heap audit.
**
** tlsf_audit() examines a TLSF control structure and its pools without
** modifying them and without trusting any metadata it reads. It is meant
** for post-mortem analysis of a possibly corrupted heap: the process is
** stopped and the caller knows which address ranges hold the control
** structure and each pool. Unlike tlsf_check()/tlsf_check_pool(), every
** pointer and size taken from the image is validated against the caller
** supplied ranges before it is dereferenced, so a damaged free list or
** block header cannot make the audit read outside those ranges or walk
** a list forever. The control structure, the pools and user payload are
** never written, whether the audit succeeds or fails.
**
** The caller passes the readable range holding the control structure
** (control_size must cover tlsf_size() bytes) plus one descriptor per
** pool with the pool start (the value returned by tlsf_add_pool()) and
** the raw byte count of that pool mapping. All ranges must be readable,
** must not overlap, and must not be accessed by anyone else while the
** audit runs. A null or misaligned start, an impossible length, or a
** nonzero pool count without a descriptor array is reported as
** TLSF_AUDIT_BAD_PARAM.
**
** The audit never allocates memory. The caller supplies a scratch
** workspace; tlsf_audit_workspace_size() returns how many bytes it must
** hold (including alignment slack, so the workspace pointer may have any
** alignment), or 0 when no workspace is needed or a descriptor is
** invalid. A smaller workspace makes tlsf_audit() fail with
** TLSF_AUDIT_NO_SPACE. The workspace is only scratch and does not have
** to survive the call. The audit is not O(1): its cost grows with the
** pool sizes and the number of free blocks.
**
** The return value is one of enum tlsf_audit_status. On TLSF_AUDIT_OK
** and on TLSF_AUDIT_CORRUPT, info (when not NULL) is filled in: a
** category from enum tlsf_audit_code, the index of the pool in the
** caller's array or TLSF_AUDIT_CONTROL when the problem is in the
** control range, and the byte offset of the offending field within that
** range. Only the first problem found is reported. Pools are examined
** in the caller's order and free list buckets in index order, so
** repeated calls on an unchanged image report the same problem.
**
** The audit checks structural integrity only: physical block coverage
** of each pool (sizes, alignment, prev-free flags and back pointers,
** coalescing of neighbours, the terminating zero-size block) and the
** free list machinery (both bitmap levels, list heads, linkage, bucket
** membership, duplicates and cycles, and that every physically free
** block is listed). It cannot tell whether an arbitrary user pointer is
** valid, and a heap in which corruption was reported must not be used
** for further allocation.
*/
enum tlsf_audit_status
{
	TLSF_AUDIT_OK = 0,	/* structure is consistent */
	TLSF_AUDIT_CORRUPT,	/* structural corruption found, see info */
	TLSF_AUDIT_BAD_PARAM,	/* a descriptor start/length/count is invalid */
	TLSF_AUDIT_NO_SPACE	/* workspace too small */
};

enum tlsf_audit_code
{
	TLSF_AUDIT_CODE_NONE = 0,	/* no corruption (status is OK) */
	TLSF_AUDIT_SIZE,		/* block size invalid or runs past the pool */
	TLSF_AUDIT_PREV_FREE,		/* prev-free flag disagrees with physical order */
	TLSF_AUDIT_PREV_PHYS,		/* prev_phys_block back pointer mismatch */
	TLSF_AUDIT_UNMERGED,		/* adjacent free blocks not coalesced */
	TLSF_AUDIT_SENTINEL,		/* zero-size end block missing, misplaced or mis-flagged */
	TLSF_AUDIT_BITMAP,		/* bitmaps disagree with each other or list heads */
	TLSF_AUDIT_LIST_TARGET,		/* list pointer wild, unaligned, or not a free block */
	TLSF_AUDIT_LIST_LINK,		/* next_free/prev_free links inconsistent */
	TLSF_AUDIT_LIST_BUCKET,		/* block size does not match its bucket */
	TLSF_AUDIT_LIST_DUP,		/* block listed twice (this is also how cycles report) */
	TLSF_AUDIT_LIST_MISSING		/* physically free block missing from all lists */
};

/* tlsf_audit_info_t.pool value meaning "the offset is into the control range". */
#define TLSF_AUDIT_CONTROL ((size_t)-1)

typedef struct tlsf_audit_pool_t
{
	const void* start;	/* start of the pool mapping (the pool_t address) */
	size_t size;		/* raw byte count of the pool mapping */
} tlsf_audit_pool_t;

typedef struct tlsf_audit_info_t
{
	int code;		/* one of enum tlsf_audit_code */
	size_t pool;		/* pool index, or TLSF_AUDIT_CONTROL */
	size_t offset;		/* offset of the offending field within that range */
} tlsf_audit_info_t;

/* Bytes of scratch workspace tlsf_audit() needs for these pool descriptors. */
size_t tlsf_audit_workspace_size(const tlsf_audit_pool_t* pools, size_t pool_count);

/* Audit a stopped heap. Returns one of enum tlsf_audit_status. */
int tlsf_audit(const void* control, size_t control_size,
	const tlsf_audit_pool_t* pools, size_t pool_count,
	void* workspace, size_t workspace_size,
	tlsf_audit_info_t* info);

#if defined(__cplusplus)
};
#endif

#endif
