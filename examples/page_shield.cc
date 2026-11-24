/*
 * Legal page-shield walk-through:
 *   allocate a populated small slab, remotely free every object touching an
 *   empty OS page, publish + receive those frees, protect the page, query
 *   its diagnostic state, prove live siblings still work, then restore.
 *
 * On success prints SNMALLOC_PAGE_SHIELD_EXAMPLE_OK.
 */

#include "../task_support/page_observer.h"

#include <cstdio>

int main()
{
  task_input::fixture f;
  if (!f.init())
  {
    std::fprintf(stderr, "example: fixture init failed\n");
    return 1;
  }
  if (!f.retire_remote())
  {
    std::fprintf(stderr, "example: remote retirement failed\n");
    return 1;
  }

  snmalloc::page_shield_info before{};
  if (!snmalloc::page_shield_query(*f.owner.alloc, f.page, before) ||
      before.outstanding != 0)
  {
    std::fprintf(stderr, "example: pre-shield query mismatch\n");
    return 1;
  }

  size_t held = snmalloc::page_shield_collect(*f.owner.alloc, 128u * 1024 * 1024);
  if (held < f.page_bytes ||
      task_input::permissions(f.page) != 0)
  {
    std::fprintf(stderr, "example: shield was not applied\n");
    return 1;
  }

  snmalloc::page_shield_info info{};
  if (!snmalloc::page_shield_query(*f.owner.alloc, f.page, info) ||
      info.state != snmalloc::page_shield_state::shielded ||
      info.outstanding != 0)
  {
    std::fprintf(stderr, "example: shielded query mismatch\n");
    return 1;
  }

  /* Flushing must not unprotect the page while siblings stay live. */
  f.owner->flush();
  if (task_input::permissions(f.page) != 0 || !f.live_contents())
  {
    std::fprintf(stderr, "example: flush/live-content check failed\n");
    return 1;
  }

  if (snmalloc::page_shield_release(*f.owner.alloc) != held)
  {
    std::fprintf(stderr, "example: release byte count mismatch\n");
    return 1;
  }
  int perm = task_input::permissions(f.page);
  if (perm < 0 || (perm & 3) != 3)
  {
    std::fprintf(stderr, "example: page was not restored\n");
    return 1;
  }

  if (!f.finish())
  {
    std::fprintf(stderr, "example: allocator not empty at finish\n");
    return 1;
  }

  std::puts("SNMALLOC_PAGE_SHIELD_EXAMPLE_OK");
  return 0;
}
