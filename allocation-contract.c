/*
 * Public release-contract wrappers around the locked chunk layer.
 *
 * This file is plain C (compiled as gnu99) and pulls in no C++ runtime.
 * It owns argument validation, the allocator lock boundary and the
 * success-release tracking callback; all slot/table work stays in
 * chunk.c under the same lock.
 */

#define DMALLOC_DISABLE

#include <stddef.h>

#include "conf.h"
#include "dmalloc.h"
#include "allocation-contract.h"

#include "chunk.h"
#include "dmalloc_loc.h"

/* internal user-level helpers implemented in user_malloc.c */
extern
int	_dmalloc_contract_enter(const char *file, const int line,
				const int check_heap_b);
extern
void	_dmalloc_contract_exit(void);
extern
void	_dmalloc_contract_track_release(const char *file,
				       const unsigned int line,
				       const DMALLOC_PNT pnt);

/* legal argument check shared by both entry points; runs pre-heap */
static	int	contract_args_legal(const unsigned family,
				    const unsigned flags,
				    const dmalloc_release_report *out)
{
  if (out == NULL) {
    return 0;
  }
  if (family != DMALLOC_RELEASE_C
      && family != DMALLOC_RELEASE_SCALAR
      && family != DMALLOC_RELEASE_ARRAY) {
    return 0;
  }
  if (flags & ~DMALLOC_RELEASE_FLAG_KNOWN) {
    return 0;
  }
  return 1;
}

/*
 * int dmalloc_contract_query
 *
 * Read-only classification.  It does not change the user area, the
 * allocation records, dmalloc_errno or the tracking callback and it
 * never allocates to build the report.
 */
int	dmalloc_contract_query(const void *p, unsigned family,
			       size_t size, size_t alignment, unsigned flags,
			       dmalloc_release_report *out)
{
  int	ret;

  if (! contract_args_legal(family, flags, out)) {
    return -1;
  }

  if (! _dmalloc_contract_enter(DMALLOC_DEFAULT_FILE, DMALLOC_DEFAULT_LINE,
				0 /* do not trigger a heap check */)) {
    /*
     * The library is aborting; report conservatively without touching
     * any allocation state.
     */
    out->code = DMALLOC_RELEASE_NOT_LIVE;
    out->mismatch = 0;
    out->allocation_family = 0;
    out->requested_size = 0;
    out->requested_alignment = 0;
    return 0;
  }

  ret = _dmalloc_chunk_contract_classify(p, family,
					 (unsigned long)size,
					 (unsigned long)alignment,
					 flags, out);

  _dmalloc_contract_exit();

  return ret;
}

/*
 * int dmalloc_contract_release
 *
 * Classify and, when compatible, release within a single lock hold.
 * The success tracking callback runs only after the lock is released.
 */
int	dmalloc_contract_release(const char *file, int line, void *p,
				 unsigned family, size_t size,
				 size_t alignment, unsigned flags,
				 dmalloc_release_report *out)
{
  int	ret;
  int	func_id;

  if (! contract_args_legal(family, flags, out)) {
    return -1;
  }

  if (! _dmalloc_contract_enter(file, line, 1 /* normal heap checking */)) {
    out->code = DMALLOC_RELEASE_NOT_LIVE;
    out->mismatch = 0;
    out->allocation_family = 0;
    out->requested_size = 0;
    out->requested_alignment = 0;
    return 0;
  }

  if (family == DMALLOC_RELEASE_C) {
    func_id = DMALLOC_FUNC_FREE;
  }
  else if (family == DMALLOC_RELEASE_SCALAR) {
    func_id = DMALLOC_FUNC_DELETE;
  }
  else {
    func_id = DMALLOC_FUNC_DELETE_ARRAY;
  }

  ret = _dmalloc_chunk_contract_release(file, (unsigned int)line, p, family,
					 (unsigned long)size,
					 (unsigned long)alignment,
					 flags, func_id, out);

  _dmalloc_contract_exit();

  /*
   * Only a real, successful release fires the success callback.  A
   * NULL argument is a compatible no-op that never reached the release
   * machinery, so it does not count.
   */
  if (ret == 1 && p != NULL) {
    _dmalloc_contract_track_release(file, (unsigned int)line, p);
  }

  return ret;
}
