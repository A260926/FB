/*
 * Allocation release contract for the dmalloc library.
 *
 * This header is shared by C and C++17 consumers.  It describes the
 * compatibility check that every release path (free, delete, delete[],
 * the sized/aligned variants and the explicit contract API) applies to
 * a live allocation: the allocation family, the originally requested
 * size and the originally requested alignment must agree with the
 * arguments supplied by the releasing language runtime.
 *
 * The header deliberately depends only on <stddef.h> so that the pure C
 * library can be built without any C++ runtime support.
 */

#ifndef __ALLOCATION_CONTRACT_H__
#define __ALLOCATION_CONTRACT_H__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* allocation families recorded on every live slot */
#define DMALLOC_RELEASE_C		1u	/* malloc/calloc/strdup/memalign/valloc/realloc */
#define DMALLOC_RELEASE_SCALAR		2u	/* operator new */
#define DMALLOC_RELEASE_SCALAR_NEW	DMALLOC_RELEASE_SCALAR
#define DMALLOC_RELEASE_ARRAY		3u	/* operator new[] */

/* report codes */
#define DMALLOC_RELEASE_OK		0	/* live, base address, contract matches */
#define DMALLOC_RELEASE_NULL		1	/* NULL argument: compatible no-op */
#define DMALLOC_RELEASE_NOT_LIVE	2	/* not present in the live allocation table */
#define DMALLOC_RELEASE_NOT_BASE	3	/* live block but pointer is not its user base */
#define DMALLOC_RELEASE_MISMATCH	4	/* live base but family/size/alignment disagree */

/* flags for the query/release calls: which factual arguments participate */
#define DMALLOC_RELEASE_HAS_SIZE	0x1u
#define DMALLOC_RELEASE_HAS_ALIGNMENT	0x2u
#define DMALLOC_RELEASE_FLAG_KNOWN	(DMALLOC_RELEASE_HAS_SIZE | \
					 DMALLOC_RELEASE_HAS_ALIGNMENT)

/* independent mismatch bits, accumulated together rather than short-circuiting */
#define DMALLOC_RELEASE_BAD_FAMILY	0x1u
#define DMALLOC_RELEASE_BAD_SIZE	0x2u
#define DMALLOC_RELEASE_BAD_ALIGNMENT	0x4u

/* largest explicit alignment promise accepted by the allocator (1 MiB) */
#define DMALLOC_CONTRACT_MAX_ALIGNMENT	(1u << 20)

/*
 * Report filled by dmalloc_contract_query() and dmalloc_contract_release().
 *
 * code				- one of the DMALLOC_RELEASE_* codes.
 * mismatch			- OR-ed DMALLOC_RELEASE_BAD_* facts (0 otherwise).
 * allocation_family		- family recorded on the live slot.
 * requested_size		- byte count originally requested by the user.
 * requested_alignment		- alignment originally requested; 0 for ordinary
 *				  allocations, BLOCK_SIZE for valloc, the caller's
 *				  memalign/aligned-new request otherwise.
 */
typedef struct {
  int		code;
  unsigned	mismatch;
  unsigned	allocation_family;
  size_t	requested_size;
  size_t	requested_alignment;
} dmalloc_release_report;

/*
 * int dmalloc_contract_query
 *
 * Classify a pointer against a proposed release without changing the
 * user area, the allocation records, dmalloc_errno or the tracking
 * callback.  No temporary allocation is made to build the report.
 *
 * Returns 1 if the release would be compatible, 0 if it must be
 * refused, -1 on illegal arguments.  out is mandatory and is always
 * written when the arguments are legal.
 */
int	dmalloc_contract_query(const void *p, unsigned family,
			       size_t size, size_t alignment, unsigned flags,
			       dmalloc_release_report *out);

/*
 * int dmalloc_contract_release
 *
 * Run the same classification as dmalloc_contract_query() and, only
 * when it is compatible, perform the real release inside the same
 * allocator lock hold that did the lookup.  A refused request leaves
 * the user area, slot, fences and statistics untouched.
 *
 * Returns 1 when the block was released (or p was NULL), 0 when
 * refused, -1 on illegal arguments.
 */
int	dmalloc_contract_release(const char *file, int line, void *p,
				 unsigned family, size_t size,
				 size_t alignment, unsigned flags,
				 dmalloc_release_report *out);

#ifdef __cplusplus
}
#endif

#endif /* ! __ALLOCATION_CONTRACT_H__ */
