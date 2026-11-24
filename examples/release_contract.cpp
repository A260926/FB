/*
 * Release-contract example.
 *
 * Allocate through the C entry point, query the live contract without
 * disturbing the block, then release it through the same contract
 * decision the allocator's free/delete paths use.  The final marker is
 * printed by this run and matched by the task harness.
 */

#define DMALLOC_DISABLE

#include <cstdio>
#include <cstring>

extern "C" {
#include "dmalloc.h"
#include "debug_tok.h"
}

#include "allocation-contract.h"

int
main(void)
{
  dmalloc_release_report report;

  dmalloc_debug(DMALLOC_DEBUG_CHECK_FENCE | DMALLOC_DEBUG_ALLOC_BLANK |
		DMALLOC_DEBUG_FREE_BLANK);

  const size_t size = 128;
  char *p = static_cast<char *>(
    dmalloc_malloc(__FILE__, __LINE__, size, DMALLOC_FUNC_MALLOC, 0, 0));
  if (p == NULL) {
    std::fprintf(stderr, "allocation failed\n");
    return 1;
  }
  std::memset(p, 0x42, size);

  /* the query reports the recorded family, size and alignment */
  if (dmalloc_contract_query(p, DMALLOC_RELEASE_C, size, 0,
			     DMALLOC_RELEASE_HAS_SIZE,
			     &report) != 1) {
    std::fprintf(stderr, "contract query refused a live allocation\n");
    return 1;
  }
  if (report.code != DMALLOC_RELEASE_OK ||
      report.allocation_family != DMALLOC_RELEASE_C ||
      report.requested_size != size ||
      report.requested_alignment != 0) {
    std::fprintf(stderr, "unexpected contract report\n");
    return 1;
  }
  if (dmalloc_verify(p) != DMALLOC_VERIFY_NOERROR) {
    std::fprintf(stderr, "fence verification failed\n");
    return 1;
  }

  /* the release performs the real free inside the allocator lock */
  if (dmalloc_contract_release(__FILE__, __LINE__, p, DMALLOC_RELEASE_C,
			       size, 0, DMALLOC_RELEASE_HAS_SIZE,
			       &report) != 1) {
    std::fprintf(stderr, "contract release refused a compatible block\n");
    return 1;
  }

  std::puts("RELEASE_CONTRACT_EXAMPLE_OK");
  return 0;
}
