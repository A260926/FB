/*
 * Release-contract regression suite.
 *
 * Three groups are registered:
 *
 *   classification - family/size/alignment judgments, interior and
 *                    non-live addresses, refusal invariance, realloc
 *                    family handling and reuse metadata cleanup.
 *   alignment      - explicit alignment (64, 4096, 16384 and values
 *                    larger than BLOCK_SIZE) with fences on and off,
 *                    including small request sizes.
 *   cpp_entry      - real C++ new/delete expressions, arrays with
 *                    destructor counts, over-aligned objects, the
 *                    sized/aligned operators, capacity exhaustion with
 *                    a reentrant new_handler and bad_alloc behavior.
 *
 * Refusal scenarios always go through the safe contract query/release
 * API; a mismatched language-level delete is never executed.
 */

#define DMALLOC_DISABLE

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

extern "C" {
#include "dmalloc.h"
#include "debug_tok.h"
}

#include "allocation-contract.h"
#include "task_support/check.h"

/* exported by the C library */
extern "C" unsigned long _dmalloc_memory_limit;

namespace {

const unsigned FENCE_FLAGS =
  DMALLOC_DEBUG_CHECK_FENCE | DMALLOC_DEBUG_ALLOC_BLANK |
  DMALLOC_DEBUG_FREE_BLANK;
const unsigned BLANK_ONLY_FLAGS =
  DMALLOC_DEBUG_ALLOC_BLANK | DMALLOC_DEBUG_FREE_BLANK;

/*
 * A handful of live allocations are kept across cases so that a fresh
 * allocation cannot silently alias an address that an earlier case
 * released; freed addresses must always classify as NOT_LIVE.
 */
void *anchor[8];

} /* namespace */

/****************************** classification ******************************/

static int
case_three_families(void)
{
  dmalloc_release_report r;

  anchor[0] = dmalloc_malloc(__FILE__, __LINE__, 41, DMALLOC_FUNC_MALLOC,
			     0, 0);
  TASK_CHECK(anchor[0] != NULL);
  TASK_CHECK(dmalloc_contract_query(anchor[0], DMALLOC_RELEASE_C, 41, 0,
				    DMALLOC_RELEASE_HAS_SIZE, &r) == 1);
  TASK_CHECK(r.code == DMALLOC_RELEASE_OK && r.mismatch == 0);
  TASK_CHECK(r.allocation_family == DMALLOC_RELEASE_C);
  TASK_CHECK(r.requested_size == 41 && r.requested_alignment == 0);

  anchor[1] = dmalloc_malloc(__FILE__, __LINE__, 29, DMALLOC_FUNC_NEW, 0, 0);
  TASK_CHECK(anchor[1] != NULL);
  TASK_CHECK(dmalloc_contract_query(anchor[1], DMALLOC_RELEASE_SCALAR, 29, 0,
				    DMALLOC_RELEASE_HAS_SIZE, &r) == 1);
  TASK_CHECK(r.allocation_family == DMALLOC_RELEASE_SCALAR);
  TASK_CHECK(r.requested_size == 29);

  anchor[2] = dmalloc_malloc(__FILE__, __LINE__, 31, DMALLOC_FUNC_NEW_ARRAY,
			     0, 0);
  TASK_CHECK(anchor[2] != NULL);
  TASK_CHECK(dmalloc_contract_query(anchor[2], DMALLOC_RELEASE_ARRAY, 31, 0,
				    DMALLOC_RELEASE_HAS_SIZE, &r) == 1);
  TASK_CHECK(r.allocation_family == DMALLOC_RELEASE_ARRAY);
  TASK_CHECK(r.requested_size == 31);

  /* scalar and array are distinct identities */
  TASK_CHECK(dmalloc_contract_query(anchor[1], DMALLOC_RELEASE_ARRAY, 29, 0,
				    DMALLOC_RELEASE_HAS_SIZE, &r) == 0);
  TASK_CHECK(r.code == DMALLOC_RELEASE_MISMATCH);
  TASK_CHECK(r.mismatch == DMALLOC_RELEASE_BAD_FAMILY);
  return 0;
}

static int
case_all_three_mismatches(void)
{
  dmalloc_release_report r;

  anchor[3] = dmalloc_malloc(__FILE__, __LINE__, 40, DMALLOC_FUNC_MALLOC,
			     0, 0);
  TASK_CHECK(anchor[3] != NULL);

  /* wrong family, wrong size and wrong alignment at the same time */
  TASK_CHECK(dmalloc_contract_query(anchor[3], DMALLOC_RELEASE_SCALAR, 41, 8,
				    DMALLOC_RELEASE_HAS_SIZE |
				    DMALLOC_RELEASE_HAS_ALIGNMENT, &r) == 0);
  TASK_CHECK(r.code == DMALLOC_RELEASE_MISMATCH);
  TASK_CHECK(r.mismatch == (DMALLOC_RELEASE_BAD_FAMILY |
			    DMALLOC_RELEASE_BAD_SIZE |
			    DMALLOC_RELEASE_BAD_ALIGNMENT));

  /* the report still carries every fact about the real object */
  TASK_CHECK(r.allocation_family == DMALLOC_RELEASE_C);
  TASK_CHECK(r.requested_size == 40);
  TASK_CHECK(r.requested_alignment == 0);
  return 0;
}

static int
case_interior_and_not_live(void)
{
  dmalloc_release_report r;

  TASK_CHECK(dmalloc_contract_query(static_cast<char *>(anchor[3]) + 1,
				    DMALLOC_RELEASE_C, 40, 0,
				    DMALLOC_RELEASE_HAS_SIZE, &r) == 0);
  TASK_CHECK(r.code == DMALLOC_RELEASE_NOT_BASE);
  /* rounding slack is not user capacity: the facts are the slot's */
  TASK_CHECK(r.allocation_family == DMALLOC_RELEASE_C);
  TASK_CHECK(r.requested_size == 40);

  int on_stack;
  TASK_CHECK(dmalloc_contract_query(&on_stack, DMALLOC_RELEASE_C, 0, 0, 0,
				    &r) == 0);
  TASK_CHECK(r.code == DMALLOC_RELEASE_NOT_LIVE);
  /* unknown object: every fact field and the mismatch mask are zero */
  TASK_CHECK(r.mismatch == 0 && r.allocation_family == 0);
  TASK_CHECK(r.requested_size == 0 && r.requested_alignment == 0);
  return 0;
}

static int
case_null_and_bad_arguments(void)
{
  dmalloc_release_report r;

  TASK_CHECK(dmalloc_contract_query(NULL, DMALLOC_RELEASE_C, 0, 0, 0,
				    &r) == 1);
  TASK_CHECK(r.code == DMALLOC_RELEASE_NULL);
  TASK_CHECK(r.mismatch == 0 && r.allocation_family == 0);
  TASK_CHECK(r.requested_size == 0 && r.requested_alignment == 0);

  TASK_CHECK(dmalloc_contract_release(__FILE__, __LINE__, NULL,
				      DMALLOC_RELEASE_ARRAY, 0, 0, 0,
				      &r) == 1);
  TASK_CHECK(r.code == DMALLOC_RELEASE_NULL);

  /* illegal arguments are rejected before any heap operation */
  TASK_CHECK(dmalloc_contract_query(anchor[3], 999, 0, 0, 0, &r) == -1);
  TASK_CHECK(dmalloc_contract_query(anchor[3], DMALLOC_RELEASE_C, 0, 0,
				    0x80, &r) == -1);
  TASK_CHECK(dmalloc_contract_query(anchor[3], DMALLOC_RELEASE_C, 0, 0, 0,
				    NULL) == -1);
  TASK_CHECK(dmalloc_contract_release(__FILE__, __LINE__, anchor[3], 0, 0, 0,
				      0, &r) == -1);
  return 0;
}

static int
case_refusal_keeps_content_and_examine(void)
{
  dmalloc_release_report r;
  unsigned char *p;
  DMALLOC_SIZE size;

  p = static_cast<unsigned char *>(
    dmalloc_malloc(__FILE__, __LINE__, 48, DMALLOC_FUNC_MALLOC, 0, 0));
  TASK_CHECK(p != NULL);
  for (int i = 0; i < 48; ++i) {
    p[i] = static_cast<unsigned char>(i + 1);
  }

  /* a legal but incompatible release must be refused */
  TASK_CHECK(dmalloc_contract_release(__FILE__, __LINE__, p,
				      DMALLOC_RELEASE_SCALAR, 48, 0,
				      DMALLOC_RELEASE_HAS_SIZE, &r) == 0);
  TASK_CHECK(r.code == DMALLOC_RELEASE_MISMATCH);

  /* user area untouched */
  bool content_ok = true;
  for (int i = 0; i < 48; ++i) {
    if (p[i] != static_cast<unsigned char>(i + 1)) {
      content_ok = false;
    }
  }
  TASK_CHECK(content_ok);

  /* slot metadata and examine results untouched */
  TASK_CHECK(dmalloc_examine(p, &size, NULL, NULL, NULL, NULL, NULL,
			     NULL) == DMALLOC_NOERROR);
  TASK_CHECK(size == 48);
  TASK_CHECK(dmalloc_contract_query(p, DMALLOC_RELEASE_C, 48, 0,
				    DMALLOC_RELEASE_HAS_SIZE, &r) == 1);
  TASK_CHECK(r.code == DMALLOC_RELEASE_OK);
  TASK_CHECK(dmalloc_verify(p) == DMALLOC_VERIFY_NOERROR);

  TASK_CHECK(dmalloc_contract_release(__FILE__, __LINE__, p,
				      DMALLOC_RELEASE_C, 48, 0,
				      DMALLOC_RELEASE_HAS_SIZE, &r) == 1);
  TASK_CHECK(r.code == DMALLOC_RELEASE_OK);
  return 0;
}

static int
case_realloc_in_place_and_move(void)
{
  dmalloc_release_report r;
  unsigned char *p, *q;

  /* moving realloc of an aligned block: new layout is an ordinary C block */
  p = static_cast<unsigned char *>(
    dmalloc_malloc(__FILE__, __LINE__, 120, DMALLOC_FUNC_MEMALIGN, 256, 0));
  TASK_CHECK(p != NULL && (reinterpret_cast<uintptr_t>(p) % 256) == 0);
  std::memset(p, 0x7E, 120);

  q = static_cast<unsigned char *>(
    dmalloc_realloc(__FILE__, __LINE__, p, 500, DMALLOC_FUNC_REALLOC, 0));
  TASK_CHECK(q != NULL);
  bool copied = true;
  for (int i = 0; i < 120; ++i) {
    if (q[i] != 0x7E) {
      copied = false;
    }
  }
  TASK_CHECK(copied);
  TASK_CHECK(dmalloc_contract_query(q, DMALLOC_RELEASE_C, 500, 0,
				    DMALLOC_RELEASE_HAS_SIZE |
				    DMALLOC_RELEASE_HAS_ALIGNMENT, &r) == 1);
  TASK_CHECK(r.requested_size == 500 && r.requested_alignment == 0);
  TASK_CHECK(r.allocation_family == DMALLOC_RELEASE_C);
  TASK_CHECK(dmalloc_verify(q) == DMALLOC_VERIFY_NOERROR);
  /* the old address is no longer live */
  TASK_CHECK(dmalloc_contract_query(p, DMALLOC_RELEASE_C, 0, 0, 0, &r) == 0);
  TASK_CHECK(r.code == DMALLOC_RELEASE_NOT_LIVE);
  TASK_CHECK(dmalloc_free(__FILE__, __LINE__, q, DMALLOC_FUNC_FREE) ==
	     FREE_NOERROR);

  /* in-place shrink of an ordinary C allocation keeps the same address */
  p = static_cast<unsigned char *>(
    dmalloc_malloc(__FILE__, __LINE__, 300, DMALLOC_FUNC_MALLOC, 0, 0));
  TASK_CHECK(p != NULL);
  std::memset(p, 0x33, 300);
  q = static_cast<unsigned char *>(
    dmalloc_realloc(__FILE__, __LINE__, p, 50, DMALLOC_FUNC_REALLOC, 0));
  TASK_CHECK(q == p);
  TASK_CHECK(dmalloc_contract_query(q, DMALLOC_RELEASE_C, 50, 0,
				    DMALLOC_RELEASE_HAS_SIZE |
				    DMALLOC_RELEASE_HAS_ALIGNMENT, &r) == 1);
  TASK_CHECK(r.requested_size == 50 && r.requested_alignment == 0);
  TASK_CHECK(dmalloc_free(__FILE__, __LINE__, q, DMALLOC_FUNC_FREE) ==
	     FREE_NOERROR);

  /* realloc refuses a non-C family before allocating or freeing */
  p = static_cast<unsigned char *>(
    dmalloc_malloc(__FILE__, __LINE__, 100, DMALLOC_FUNC_NEW, 0, 0));
  TASK_CHECK(p != NULL);
  TASK_CHECK(dmalloc_realloc(__FILE__, __LINE__, p, 200,
			     DMALLOC_FUNC_REALLOC, 0) == NULL);
  TASK_CHECK(dmalloc_contract_query(p, DMALLOC_RELEASE_SCALAR, 100, 0,
				    DMALLOC_RELEASE_HAS_SIZE, &r) == 1);
  TASK_CHECK(r.requested_size == 100);
  TASK_CHECK(dmalloc_free(__FILE__, __LINE__, p, DMALLOC_FUNC_DELETE) ==
	     FREE_NOERROR);
  return 0;
}

static int
case_reuse_metadata_cleanup(void)
{
  dmalloc_release_report r;
  void *blocks[32];

  /* a released block loses its live contract immediately */
  void *a = dmalloc_malloc(__FILE__, __LINE__, 88, DMALLOC_FUNC_NEW_ARRAY,
			   0, 0);
  TASK_CHECK(a != NULL);
  TASK_CHECK(dmalloc_contract_query(a, DMALLOC_RELEASE_ARRAY, 88, 0,
				    DMALLOC_RELEASE_HAS_SIZE, &r) == 1);
  TASK_CHECK(dmalloc_free(__FILE__, __LINE__, a,
			  DMALLOC_FUNC_DELETE_ARRAY) == FREE_NOERROR);
  TASK_CHECK(dmalloc_contract_query(a, DMALLOC_RELEASE_ARRAY, 0, 0, 0,
				    &r) == 0);
  TASK_CHECK(r.code == DMALLOC_RELEASE_NOT_LIVE && r.mismatch == 0);
  TASK_CHECK(r.allocation_family == 0 && r.requested_size == 0);
  TASK_CHECK(r.requested_alignment == 0);

  /*
   * Cycle more blocks than FREED_POINTER_DELAY so the delayed-free list
   * drains and slots are genuinely reused.  Every fresh block must
   * report the family it was allocated with, never a stale identity.
   */
  for (int round = 0; round < 2; ++round) {
    for (int i = 0; i < 32; ++i) {
      blocks[i] = dmalloc_malloc(__FILE__, __LINE__, 64 + (i % 3) * 16,
				 (i & 1) ? DMALLOC_FUNC_MALLOC :
				 DMALLOC_FUNC_NEW_ARRAY, 0, 0);
      TASK_CHECK(blocks[i] != NULL);
    }
    for (int i = 0; i < 32; ++i) {
      unsigned family = (i & 1) ? DMALLOC_RELEASE_C : DMALLOC_RELEASE_ARRAY;
      TASK_CHECK(dmalloc_contract_query(blocks[i], family,
					64 + (i % 3) * 16, 0,
					DMALLOC_RELEASE_HAS_SIZE, &r) == 1);
      TASK_CHECK(r.allocation_family == family);
    }
    for (int i = 0; i < 32; ++i) {
      int func = (i & 1) ? DMALLOC_FUNC_FREE : DMALLOC_FUNC_DELETE_ARRAY;
      TASK_CHECK(dmalloc_free(__FILE__, __LINE__, blocks[i], func) ==
		 FREE_NOERROR);
    }
  }
  return 0;
}

static int
case_query_is_non_mutating(void)
{
  dmalloc_release_report r;
  void *p = dmalloc_malloc(__FILE__, __LINE__, 24, DMALLOC_FUNC_MALLOC, 0,
			   0);
  TASK_CHECK(p != NULL);
  std::memset(p, 0x5A, 24);

  int saved_errno = dmalloc_errno;
  TASK_CHECK(dmalloc_contract_query(static_cast<char *>(p) + 1,
				    DMALLOC_RELEASE_C, 0, 0, 0, &r) == 0);
  TASK_CHECK(dmalloc_contract_query(p, DMALLOC_RELEASE_SCALAR, 99, 0,
				    DMALLOC_RELEASE_HAS_SIZE, &r) == 0);
  /* query does not touch dmalloc_errno */
  TASK_CHECK(dmalloc_errno == saved_errno);

  /* query does not touch the user area */
  bool content_ok = true;
  for (int i = 0; i < 24; ++i) {
    if (static_cast<unsigned char *>(p)[i] != 0x5A) {
      content_ok = false;
    }
  }
  TASK_CHECK(content_ok);
  TASK_CHECK(dmalloc_verify(p) == DMALLOC_VERIFY_NOERROR);
  TASK_CHECK(dmalloc_free(__FILE__, __LINE__, p, DMALLOC_FUNC_FREE) ==
	     FREE_NOERROR);
  return 0;
}

/******************************** alignment *********************************/

static int
check_alignment_matrix(unsigned debug_flags)
{
  static const unsigned long aligns[] = {64, 4096, 16384, 32768, 65536};
  static const size_t sizes[] = {1, 5, 63, 100};
  dmalloc_release_report r;

  dmalloc_debug(debug_flags);

  for (unsigned long align : aligns) {
    for (size_t size : sizes) {
      void *p = dmalloc_malloc(__FILE__, __LINE__, size,
			       DMALLOC_FUNC_MEMALIGN, align, 0);
      TASK_CHECK(p != NULL);
      TASK_CHECK((reinterpret_cast<uintptr_t>(p) % align) == 0);
      TASK_CHECK(dmalloc_contract_query(p, DMALLOC_RELEASE_C, size, align,
					DMALLOC_RELEASE_HAS_SIZE |
					DMALLOC_RELEASE_HAS_ALIGNMENT, &r) == 1);
      TASK_CHECK(r.code == DMALLOC_RELEASE_OK);
      TASK_CHECK(r.requested_alignment == align);
      TASK_CHECK(r.requested_size == size);

      /* normal read/write across the whole requested region */
      unsigned char fill = static_cast<unsigned char>(size & 0xFF);
      std::memset(p, fill, size);
      unsigned char sum = 0;
      for (size_t i = 0; i < size; ++i) {
	sum += static_cast<unsigned char *>(p)[i];
      }
      TASK_CHECK(sum == static_cast<unsigned char>(fill * size));

      /* the fence layout stays interpretable */
      TASK_CHECK(dmalloc_verify(p) == DMALLOC_VERIFY_NOERROR);

      TASK_CHECK(dmalloc_contract_release(__FILE__, __LINE__, p,
					  DMALLOC_RELEASE_C, size, align,
					  DMALLOC_RELEASE_HAS_SIZE |
					  DMALLOC_RELEASE_HAS_ALIGNMENT,
					  &r) == 1);
    }
  }
  return 0;
}

static int
case_alignment_with_fences(void)
{
  return check_alignment_matrix(FENCE_FLAGS);
}

static int
case_alignment_without_fences(void)
{
  int rc = check_alignment_matrix(BLANK_ONLY_FLAGS);
  dmalloc_debug(FENCE_FLAGS);
  return rc;
}

static int
case_valloc_and_zero_size(void)
{
  dmalloc_release_report r;
  unsigned page = dmalloc_page_size();

  void *v = dmalloc_malloc(__FILE__, __LINE__, 50, DMALLOC_FUNC_VALLOC, 0,
			   0);
  TASK_CHECK(v != NULL);
  TASK_CHECK((reinterpret_cast<uintptr_t>(v) % page) == 0);
  TASK_CHECK(dmalloc_contract_query(v, DMALLOC_RELEASE_C, 50, page,
				    DMALLOC_RELEASE_HAS_SIZE |
				    DMALLOC_RELEASE_HAS_ALIGNMENT, &r) == 1);
  /* valloc records the page-alignment promise */
  TASK_CHECK(r.requested_alignment == page);
  TASK_CHECK(dmalloc_verify(v) == DMALLOC_VERIFY_NOERROR);
  TASK_CHECK(dmalloc_free(__FILE__, __LINE__, v, DMALLOC_FUNC_FREE) ==
	     FREE_NOERROR);

  /* zero-size new still yields releasable storage and keeps the zero ask */
  void *z = dmalloc_malloc(__FILE__, __LINE__, 0, DMALLOC_FUNC_NEW, 0, 0);
  TASK_CHECK(z != NULL);
  TASK_CHECK(dmalloc_contract_query(z, DMALLOC_RELEASE_SCALAR, 0, 0,
				    DMALLOC_RELEASE_HAS_SIZE, &r) == 1);
  TASK_CHECK(r.requested_size == 0);
  TASK_CHECK(r.allocation_family == DMALLOC_RELEASE_SCALAR);
  TASK_CHECK(dmalloc_free(__FILE__, __LINE__, z, DMALLOC_FUNC_DELETE) ==
	     FREE_NOERROR);

  /* out-of-range alignments are rejected before rounding */
  TASK_CHECK(dmalloc_malloc(__FILE__, __LINE__, 10, DMALLOC_FUNC_MEMALIGN,
			    3, 0) == NULL);
  TASK_CHECK(dmalloc_malloc(__FILE__, __LINE__, 10, DMALLOC_FUNC_MEMALIGN,
			    6, 0) == NULL);				/* not power of two */
  TASK_CHECK(dmalloc_malloc(__FILE__, __LINE__, 10, DMALLOC_FUNC_MEMALIGN,
			    (1u << 20) + 1, 0) == NULL);
  return 0;
}

/******************************** cpp_entry *********************************/

struct TrackedObject {
  static int alive;
  int value;
  TrackedObject() : value(1234) { ++alive; }
  ~TrackedObject() { --alive; }
};
int TrackedObject::alive = 0;

struct alignas(128) OverAlignedObject {
  int payload[8];
};

static int
case_cpp_scalar_and_array(void)
{
  dmalloc_release_report r;

  int *scalar = new int(11);
  TASK_CHECK(scalar != NULL && *scalar == 11);
  TASK_CHECK(dmalloc_contract_query(scalar, DMALLOC_RELEASE_SCALAR,
				    sizeof(int), 0,
				    DMALLOC_RELEASE_HAS_SIZE, &r) == 1);
  TASK_CHECK(r.allocation_family == DMALLOC_RELEASE_SCALAR);
  delete scalar;

  /*
   * A new[] expression for a type with a destructor hands back the
   * object pointer, which sits after the ABI cookie; the deallocation
   * function is later invoked with the true allocation base.  We
   * therefore verify construction/destruction counts and let delete[]
   * drive the real ARRAY-family release rather than guessing the
   * cookie or element count.
   */
  TrackedObject *arr = new TrackedObject[7];
  TASK_CHECK(TrackedObject::alive == 7);
  delete[] arr;
  TASK_CHECK(TrackedObject::alive == 0);

  /*
   * A direct operator new[] returns the allocation base, so its
   * ARRAY-family contract is directly queryable.
   */
  void *raw = ::operator new[](53);
  TASK_CHECK(raw != NULL);
  TASK_CHECK(dmalloc_contract_query(raw, DMALLOC_RELEASE_ARRAY, 53, 0,
				    DMALLOC_RELEASE_HAS_SIZE, &r) == 1);
  TASK_CHECK(r.allocation_family == DMALLOC_RELEASE_ARRAY);
  TASK_CHECK(r.requested_size == 53);
  ::operator delete[](raw);
  return 0;
}

static int
case_cpp_over_aligned(void)
{
  dmalloc_release_report r;

  OverAlignedObject *o = new OverAlignedObject;
  TASK_CHECK((reinterpret_cast<uintptr_t>(o) % 128) == 0);
  TASK_CHECK(dmalloc_contract_query(o, DMALLOC_RELEASE_SCALAR,
				    sizeof(OverAlignedObject), 128,
				    DMALLOC_RELEASE_HAS_SIZE |
				    DMALLOC_RELEASE_HAS_ALIGNMENT, &r) == 1);
  TASK_CHECK(r.requested_alignment == 128);
  TASK_CHECK(dmalloc_verify(o) == DMALLOC_VERIFY_NOERROR);
  delete o;

  void *ap = ::operator new[](96, std::align_val_t(256));
  TASK_CHECK((reinterpret_cast<uintptr_t>(ap) % 256) == 0);
  TASK_CHECK(dmalloc_contract_query(ap, DMALLOC_RELEASE_ARRAY, 96, 256,
				    DMALLOC_RELEASE_HAS_SIZE |
				    DMALLOC_RELEASE_HAS_ALIGNMENT, &r) == 1);
  std::memset(ap, 0x2B, 96);
  TASK_CHECK(dmalloc_verify(ap) == DMALLOC_VERIFY_NOERROR);
  ::operator delete[](ap, 96, std::align_val_t(256));
  return 0;
}

static int
case_cpp_sized_and_aligned_delete(void)
{
  dmalloc_release_report r;

  void *p = ::operator new(77);
  TASK_CHECK(p != NULL);
  TASK_CHECK(dmalloc_contract_query(p, DMALLOC_RELEASE_SCALAR, 77, 0,
				    DMALLOC_RELEASE_HAS_SIZE, &r) == 1);
  /* sized delete feeds the known size into the shared judgment */
  ::operator delete(p, std::size_t(77));
  TASK_CHECK(dmalloc_contract_query(p, DMALLOC_RELEASE_SCALAR, 0, 0, 0,
				    &r) == 0);
  TASK_CHECK(r.code == DMALLOC_RELEASE_NOT_LIVE);

  void *q = ::operator new(64, std::align_val_t(64));
  TASK_CHECK((reinterpret_cast<uintptr_t>(q) % 64) == 0);
  /* aligned delete with only the alignment known */
  ::operator delete(q, std::align_val_t(64));
  TASK_CHECK(dmalloc_contract_query(q, DMALLOC_RELEASE_SCALAR, 0, 0, 0,
				    &r) == 0);
  TASK_CHECK(r.code == DMALLOC_RELEASE_NOT_LIVE);

  /* NULL deletes are noexcept no-ops */
  ::operator delete(nullptr);
  ::operator delete(nullptr, std::size_t(0));
  ::operator delete[](nullptr, std::align_val_t(64));
  return 0;
}

static void *g_handler_held = NULL;
static int g_handler_calls = 0;

static void
reentrant_new_handler(void)
{
  ++g_handler_calls;
  /* reentrant release from inside the handler must not deadlock */
  ::operator delete(g_handler_held);
  g_handler_held = NULL;
  std::set_new_handler(NULL);
}

static int
case_cpp_handler_and_bad_alloc(void)
{
  g_handler_calls = 0;
  g_handler_held = ::operator new(24 * 1024);
  TASK_CHECK(g_handler_held != NULL);

  /* controlled capacity exhaustion */
  _dmalloc_memory_limit = 48u * 1024u;
  std::set_new_handler(reentrant_new_handler);
  void *p = ::operator new(24 * 1024);
  TASK_CHECK(p != NULL && g_handler_calls == 1);
  ::operator delete(p);
  _dmalloc_memory_limit = 0;

  /* with no handler, throwing new raises and nothrow new returns NULL */
  bool threw = false;
  try {
    (void)::operator new(static_cast<std::size_t>(-1));
  }
  catch (const std::bad_alloc &) {
    threw = true;
  }
  TASK_CHECK(threw);
  TASK_CHECK(::operator new(static_cast<std::size_t>(-1),
			    std::nothrow) == NULL);
  return 0;
}

static int
case_cpp_nothrow_and_cleanup_overload(void)
{
  void *p = ::operator new(40, std::nothrow);
  TASK_CHECK(p != NULL);
  /* matching nothrow failure-cleanup deallocator */
  ::operator delete(p, std::nothrow);

  void *a = ::operator new(40, std::align_val_t(128), std::nothrow);
  TASK_CHECK(a != NULL && (reinterpret_cast<uintptr_t>(a) % 128) == 0);
  ::operator delete(a, std::align_val_t(128), std::nothrow);

  void *arr = ::operator new[](40, std::nothrow);
  TASK_CHECK(arr != NULL);
  ::operator delete[](arr, std::nothrow);
  return 0;
}

/******************************** registration ******************************/

extern "C" const struct task_case task_cases[] = {
  {"classification", "three_families", case_three_families},
  {"classification", "all_three_mismatches", case_all_three_mismatches},
  {"classification", "interior_and_not_live", case_interior_and_not_live},
  {"classification", "null_and_bad_arguments", case_null_and_bad_arguments},
  {"classification", "refusal_keeps_content_and_examine",
   case_refusal_keeps_content_and_examine},
  {"classification", "realloc_in_place_and_move",
   case_realloc_in_place_and_move},
  {"classification", "reuse_metadata_cleanup", case_reuse_metadata_cleanup},
  {"classification", "query_is_non_mutating", case_query_is_non_mutating},

  {"alignment", "with_fences", case_alignment_with_fences},
  {"alignment", "without_fences", case_alignment_without_fences},
  {"alignment", "valloc_and_zero_size", case_valloc_and_zero_size},

  {"cpp_entry", "scalar_and_array", case_cpp_scalar_and_array},
  {"cpp_entry", "over_aligned", case_cpp_over_aligned},
  {"cpp_entry", "sized_and_aligned_delete",
   case_cpp_sized_and_aligned_delete},
  {"cpp_entry", "handler_and_bad_alloc", case_cpp_handler_and_bad_alloc},
  {"cpp_entry", "nothrow_and_cleanup_overload",
   case_cpp_nothrow_and_cleanup_overload},
};

extern "C" const size_t task_case_count =
  sizeof(task_cases) / sizeof(task_cases[0]);
