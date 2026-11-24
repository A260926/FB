/*
 * C++17 allocation adapter for the dmalloc library.
 *
 * This file redirects the global operator new/delete family through
 * the debug allocator so C++ allocations get the same fence checks,
 * delayed reuse and release-contract enforcement as the C entry
 * points.  It implements the throwing and nothrow, scalar and array,
 * sized and aligned C++17 de/allocation functions plus the matching
 * failure-cleanup deallocators.  The placement new/delete pair
 * (operator new(size_t, void*)) is deliberately not provided.
 *
 * Compile and link this in with the C++ program you want to debug.
 */

#include <cstddef>
#include <new>

extern "C" {
#include <stdlib.h>

#define DMALLOC_DISABLE

#include "dmalloc.h"
#include "return.h"
}

#include "allocation-contract.h"

namespace {

/*
 * Core allocation loop shared by every throwing operator new.
 *
 * dmalloc_malloc takes and releases the allocator lock internally, so
 * when it returns NULL the lock is free.  The current new_handler is
 * therefore invoked outside the lock and may legitimately release
 * existing memory through operator delete without deadlocking.  With
 * no handler installed, a throwing allocation reports std::bad_alloc.
 */
void *
dmalloc_cxx_allocate(std::size_t size, const int func_id,
		     std::size_t alignment)
{
  char	*file;
  GET_RET_ADDR(file);

  for (;;) {
    void	*pnt;

    pnt = dmalloc_malloc(file, 0 /* line */, size, func_id,
			 alignment, 0 /* no xalloc messages */);
    if (pnt != NULL) {
      return pnt;
    }

    std::new_handler	handler = std::get_new_handler();
    if (handler == nullptr) {
      throw std::bad_alloc();
    }
    handler();
    /* the handler may have freed memory; retry the allocation */
  }
}

/*
 * Core release path.  The known size/alignment are forwarded to the
 * shared contract decision; unknown values simply do not participate.
 * A refusal leaves the block and all allocator state untouched.
 */
void
dmalloc_cxx_deallocate(void *pnt, const unsigned family,
		       std::size_t size, std::size_t alignment,
		       unsigned flags)
{
  char			*file;
  dmalloc_release_report	report;

  GET_RET_ADDR(file);
  (void)dmalloc_contract_release(file, 0 /* line */, pnt, family, size,
				 alignment, flags, &report);
}

} /* anonymous namespace */

/******************************** scalar new ********************************/

void *
operator new(std::size_t size)
{
  return dmalloc_cxx_allocate(size, DMALLOC_FUNC_NEW, 0 /* ordinary */);
}

void *
operator new(std::size_t size, std::align_val_t alignment)
{
  return dmalloc_cxx_allocate(size, DMALLOC_FUNC_NEW,
			      static_cast<std::size_t>(alignment));
}

void *
operator new(std::size_t size, const std::nothrow_t &) noexcept
{
  try {
    return dmalloc_cxx_allocate(size, DMALLOC_FUNC_NEW, 0 /* ordinary */);
  }
  catch (const std::bad_alloc &) {
    return nullptr;
  }
}

void *
operator new(std::size_t size, std::align_val_t alignment,
	     const std::nothrow_t &) noexcept
{
  try {
    return dmalloc_cxx_allocate(size, DMALLOC_FUNC_NEW,
				static_cast<std::size_t>(alignment));
  }
  catch (const std::bad_alloc &) {
    return nullptr;
  }
}

/********************************* array new *********************************/

void *
operator new[](std::size_t size)
{
  return dmalloc_cxx_allocate(size, DMALLOC_FUNC_NEW_ARRAY, 0 /* ordinary */);
}

void *
operator new[](std::size_t size, std::align_val_t alignment)
{
  return dmalloc_cxx_allocate(size, DMALLOC_FUNC_NEW_ARRAY,
			      static_cast<std::size_t>(alignment));
}

void *
operator new[](std::size_t size, const std::nothrow_t &) noexcept
{
  try {
    return dmalloc_cxx_allocate(size, DMALLOC_FUNC_NEW_ARRAY,
				0 /* ordinary */);
  }
  catch (const std::bad_alloc &) {
    return nullptr;
  }
}

void *
operator new[](std::size_t size, std::align_val_t alignment,
	       const std::nothrow_t &) noexcept
{
  try {
    return dmalloc_cxx_allocate(size, DMALLOC_FUNC_NEW_ARRAY,
				static_cast<std::size_t>(alignment));
  }
  catch (const std::bad_alloc &) {
    return nullptr;
  }
}

/****************************** scalar operator delete **********************/

void
operator delete(void *pnt) noexcept
{
  dmalloc_cxx_deallocate(pnt, DMALLOC_RELEASE_SCALAR, 0, 0, 0);
}

void
operator delete(void *pnt, std::size_t size) noexcept
{
  dmalloc_cxx_deallocate(pnt, DMALLOC_RELEASE_SCALAR, size, 0,
			 DMALLOC_RELEASE_HAS_SIZE);
}

void
operator delete(void *pnt, std::align_val_t alignment) noexcept
{
  dmalloc_cxx_deallocate(pnt, DMALLOC_RELEASE_SCALAR, 0,
			 static_cast<std::size_t>(alignment),
			 DMALLOC_RELEASE_HAS_ALIGNMENT);
}

void
operator delete(void *pnt, std::size_t size,
		std::align_val_t alignment) noexcept
{
  dmalloc_cxx_deallocate(pnt, DMALLOC_RELEASE_SCALAR, size,
			 static_cast<std::size_t>(alignment),
			 DMALLOC_RELEASE_HAS_SIZE |
			 DMALLOC_RELEASE_HAS_ALIGNMENT);
}

/* nothrow new failure-cleanup deallocator */
void
operator delete(void *pnt, const std::nothrow_t &) noexcept
{
  dmalloc_cxx_deallocate(pnt, DMALLOC_RELEASE_SCALAR, 0, 0, 0);
}

/* aligned nothrow new failure-cleanup deallocator */
void
operator delete(void *pnt, std::align_val_t alignment,
		const std::nothrow_t &) noexcept
{
  dmalloc_cxx_deallocate(pnt, DMALLOC_RELEASE_SCALAR, 0,
			 static_cast<std::size_t>(alignment),
			 DMALLOC_RELEASE_HAS_ALIGNMENT);
}

/******************************* array operator delete **********************/

void
operator delete[](void *pnt) noexcept
{
  dmalloc_cxx_deallocate(pnt, DMALLOC_RELEASE_ARRAY, 0, 0, 0);
}

void
operator delete[](void *pnt, std::size_t size) noexcept
{
  dmalloc_cxx_deallocate(pnt, DMALLOC_RELEASE_ARRAY, size, 0,
			 DMALLOC_RELEASE_HAS_SIZE);
}

void
operator delete[](void *pnt, std::align_val_t alignment) noexcept
{
  dmalloc_cxx_deallocate(pnt, DMALLOC_RELEASE_ARRAY, 0,
			 static_cast<std::size_t>(alignment),
			 DMALLOC_RELEASE_HAS_ALIGNMENT);
}

void
operator delete[](void *pnt, std::size_t size,
		  std::align_val_t alignment) noexcept
{
  dmalloc_cxx_deallocate(pnt, DMALLOC_RELEASE_ARRAY, size,
			 static_cast<std::size_t>(alignment),
			 DMALLOC_RELEASE_HAS_SIZE |
			 DMALLOC_RELEASE_HAS_ALIGNMENT);
}

/* nothrow new[] failure-cleanup deallocator */
void
operator delete[](void *pnt, const std::nothrow_t &) noexcept
{
  dmalloc_cxx_deallocate(pnt, DMALLOC_RELEASE_ARRAY, 0, 0, 0);
}

/* aligned nothrow new[] failure-cleanup deallocator */
void
operator delete[](void *pnt, std::align_val_t alignment,
		  const std::nothrow_t &) noexcept
{
  dmalloc_cxx_deallocate(pnt, DMALLOC_RELEASE_ARRAY, 0,
			 static_cast<std::size_t>(alignment),
			 DMALLOC_RELEASE_HAS_ALIGNMENT);
}
