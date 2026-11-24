#ifndef SNMALLOC_PAGE_SHIELD_H
#define SNMALLOC_PAGE_SHIELD_H

/*
 * Optional local empty-page access barrier ("page shield").
 *
 * Public interface for the feature.  Including this header is self contained
 * and pulls in the snmalloc allocator declarations.  The implementation is
 * in src/page_shield.cc.
 *
 * The SNMALLOC_ENABLE_PAGE_SHIELD switch controls compile-time instrumentation
 * of the default allocation paths: when it is not defined, snmalloc behaves
 * exactly as the ordinary header-only library (no hook calls are emitted and
 * no extra link dependency is introduced by snmalloc headers themselves).
 * Building src/page_shield.cc without the switch yields no shield symbols;
 * the task build always compiles that translation unit with the switch on.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus

/*
 * Marker visible to the core allocator headers.  It must be defined before
 * including snmalloc.h below.  Hook *calls* additionally require
 * SNMALLOC_ENABLE_PAGE_SHIELD on the command line, so an off build is
 * completely uninstrumented.
 */
#  define SNMALLOC_PAGE_SHIELD_HEADER 1

namespace snmalloc
{
  /*
   * Internal narrow hooks invoked from the default allocator implementation.
   * Declared before including snmalloc.h so corealloc.h/metadata.h see
   * matching prototypes while being parsed.
   */
  void page_shield_hook_alloc_address(const void* p) noexcept;
  void page_shield_hook_free_address(const void* p) noexcept;
  void* page_shield_hook_parked_meta(void* alloc, size_t sc_raw) noexcept;
  void page_shield_hook_pre_free_list(void* meta, void* entropy) noexcept;
  void page_shield_hook_clear_slab(void* meta, void* entropy) noexcept;
} // namespace snmalloc

#  include <snmalloc/snmalloc.h>

namespace snmalloc
{
  /**
   * Diagnostic state of a page reported by page_shield_query().
   *
   * active   - at least one not-yet-returned object intersects the page.
   * free     - no outstanding object intersects it and the OS permits
   *            ordinary read/write access.
   * shielded - no outstanding object intersects it and the OS page is
   *            currently inaccessible.
   */
  enum class page_shield_state : unsigned char
  {
    active = 0,
    free = 1,
    shielded = 2
  };

  /**
   * Result of page_shield_query().  `page` and `slab` are aligned starts.
   * `outstanding` counts objects intersecting the queried page that have not
   * yet been returned to the owner Alloc.
   */
  struct page_shield_info
  {
    uintptr_t page = 0;
    uintptr_t slab = 0;
    size_t page_bytes = 0;
    size_t outstanding = 0;
    page_shield_state state = page_shield_state::free;
  };

  /**
   * Receive already published remote frees, then protect as many eligible
   * empty OS pages of small slabs owned by @p alloc as fit in
   * floor(max_bytes / page_size) pages, in ascending integer page order over
   * the union of new candidates and previously protected pages.  Pages outside
   * the retained set are restored.
   *
   * Returns the number of bytes currently protected for @p alloc.  A zero
   * budget restores every protected page and returns zero.
   */
  size_t page_shield_collect(Alloc& alloc, size_t max_bytes) noexcept;

  /**
   * Restore every page protected for @p alloc and return the number of bytes
   * restored.
   */
  size_t page_shield_release(Alloc& alloc) noexcept;

  /**
   * Read-only diagnostic query.  Returns true and fills @p out iff @p address
   * lies in a small slab currently owned by @p alloc.  Other addresses yield
   * false and leave @p out untouched.  This never unprotects pages, never
   * drives reclamation and never dereferences the queried address.
   */
  bool page_shield_query(
    Alloc& alloc, uintptr_t address, page_shield_info& out) noexcept;
} // namespace snmalloc

#endif /* __cplusplus */

#endif /* SNMALLOC_PAGE_SHIELD_H */
