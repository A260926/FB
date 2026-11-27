#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tlsf.h"

#if defined(__cplusplus)
#define tlsf_decl inline
#else
#define tlsf_decl static
#endif

/*
** Architecture-specific bit manipulation routines.
**
** TLSF achieves O(1) cost for malloc and free operations by limiting
** the search for a free block to a free list of guaranteed size
** adequate to fulfill the request, combined with efficient free list
** queries using bitmasks and architecture-specific bit-manipulation
** routines.
**
** Most modern processors provide instructions to count leading zeroes
** in a word, find the lowest and highest set bit, etc. These
** specific implementations will be used when available, falling back
** to a reasonably efficient generic implementation.
**
** NOTE: TLSF spec relies on ffs/fls returning value 0..31.
** ffs/fls return 1-32 by default, returning 0 for error.
*/

/*
** Detect whether or not we are building for a 32- or 64-bit (LP/LLP)
** architecture. There is no reliable portable method at compile-time.
*/
#if defined (__alpha__) || defined (__ia64__) || defined (__x86_64__) \
	|| defined (_WIN64) || defined (__LP64__) || defined (__LLP64__)
#define TLSF_64BIT
#endif

/*
** gcc 3.4 and above have builtin support, specialized for architecture.
** Some compilers masquerade as gcc; patchlevel test filters them out.
*/
#if defined (__GNUC__) && (__GNUC__ > 3 || (__GNUC__ == 3 && __GNUC_MINOR__ >= 4)) \
	&& defined (__GNUC_PATCHLEVEL__)

#if defined (__SNC__)
/* SNC for Playstation 3. */

tlsf_decl int tlsf_ffs(unsigned int word)
{
	const unsigned int reverse = word & (~word + 1);
	const int bit = 32 - __builtin_clz(reverse);
	return bit - 1;
}

#else

tlsf_decl int tlsf_ffs(unsigned int word)
{
	return __builtin_ffs(word) - 1;
}

#endif

tlsf_decl int tlsf_fls(unsigned int word)
{
	const int bit = word ? 32 - __builtin_clz(word) : 0;
	return bit - 1;
}

#elif defined (_MSC_VER) && (_MSC_VER >= 1400) && (defined (_M_IX86) || defined (_M_X64))
/* Microsoft Visual C++ support on x86/X64 architectures. */

#include <intrin.h>

#pragma intrinsic(_BitScanReverse)
#pragma intrinsic(_BitScanForward)

tlsf_decl int tlsf_fls(unsigned int word)
{
	unsigned long index;
	return _BitScanReverse(&index, word) ? index : -1;
}

tlsf_decl int tlsf_ffs(unsigned int word)
{
	unsigned long index;
	return _BitScanForward(&index, word) ? index : -1;
}

#elif defined (_MSC_VER) && defined (_M_PPC)
/* Microsoft Visual C++ support on PowerPC architectures. */

#include <ppcintrinsics.h>

tlsf_decl int tlsf_fls(unsigned int word)
{
	const int bit = 32 - _CountLeadingZeros(word);
	return bit - 1;
}

tlsf_decl int tlsf_ffs(unsigned int word)
{
	const unsigned int reverse = word & (~word + 1);
	const int bit = 32 - _CountLeadingZeros(reverse);
	return bit - 1;
}

#elif defined (__ARMCC_VERSION)
/* RealView Compilation Tools for ARM */

tlsf_decl int tlsf_ffs(unsigned int word)
{
	const unsigned int reverse = word & (~word + 1);
	const int bit = 32 - __clz(reverse);
	return bit - 1;
}

tlsf_decl int tlsf_fls(unsigned int word)
{
	const int bit = word ? 32 - __clz(word) : 0;
	return bit - 1;
}

#elif defined (__ghs__)
/* Green Hills support for PowerPC */

#include <ppc_ghs.h>

tlsf_decl int tlsf_ffs(unsigned int word)
{
	const unsigned int reverse = word & (~word + 1);
	const int bit = 32 - __CLZ32(reverse);
	return bit - 1;
}

tlsf_decl int tlsf_fls(unsigned int word)
{
	const int bit = word ? 32 - __CLZ32(word) : 0;
	return bit - 1;
}

#else
/* Fall back to generic implementation. */

tlsf_decl int tlsf_fls_generic(unsigned int word)
{
	int bit = 32;

	if (!word) bit -= 1;
	if (!(word & 0xffff0000)) { word <<= 16; bit -= 16; }
	if (!(word & 0xff000000)) { word <<= 8; bit -= 8; }
	if (!(word & 0xf0000000)) { word <<= 4; bit -= 4; }
	if (!(word & 0xc0000000)) { word <<= 2; bit -= 2; }
	if (!(word & 0x80000000)) { word <<= 1; bit -= 1; }

	return bit;
}

/* Implement ffs in terms of fls. */
tlsf_decl int tlsf_ffs(unsigned int word)
{
	return tlsf_fls_generic(word & (~word + 1)) - 1;
}

tlsf_decl int tlsf_fls(unsigned int word)
{
	return tlsf_fls_generic(word) - 1;
}

#endif

/* Possibly 64-bit version of tlsf_fls. */
#if defined (TLSF_64BIT)
tlsf_decl int tlsf_fls_sizet(size_t size)
{
	int high = (int)(size >> 32);
	int bits = 0;
	if (high)
	{
		bits = 32 + tlsf_fls(high);
	}
	else
	{
		bits = tlsf_fls((int)size & 0xffffffff);

	}
	return bits;
}
#else
#define tlsf_fls_sizet tlsf_fls
#endif

#undef tlsf_decl

/*
** Constants.
*/

/* Public constants: may be modified. */
enum tlsf_public
{
	/* log2 of number of linear subdivisions of block sizes. Larger
	** values require more memory in the control structure. Values of
	** 4 or 5 are typical.
	*/
	SL_INDEX_COUNT_LOG2 = 5,
};

/* Private constants: do not modify. */
enum tlsf_private
{
#if defined (TLSF_64BIT)
	/* All allocation sizes and addresses are aligned to 8 bytes. */
	ALIGN_SIZE_LOG2 = 3,
#else
	/* All allocation sizes and addresses are aligned to 4 bytes. */
	ALIGN_SIZE_LOG2 = 2,
#endif
	ALIGN_SIZE = (1 << ALIGN_SIZE_LOG2),

	/*
	** We support allocations of sizes up to (1 << FL_INDEX_MAX) bits.
	** However, because we linearly subdivide the second-level lists, and
	** our minimum size granularity is 4 bytes, it doesn't make sense to
	** create first-level lists for sizes smaller than SL_INDEX_COUNT * 4,
	** or (1 << (SL_INDEX_COUNT_LOG2 + 2)) bytes, as there we will be
	** trying to split size ranges into more slots than we have available.
	** Instead, we calculate the minimum threshold size, and place all
	** blocks below that size into the 0th first-level list.
	*/

#if defined (TLSF_64BIT)
	/*
	** TODO: We can increase this to support larger sizes, at the expense
	** of more overhead in the TLSF structure.
	*/
	FL_INDEX_MAX = 32,
#else
	FL_INDEX_MAX = 30,
#endif
	SL_INDEX_COUNT = (1 << SL_INDEX_COUNT_LOG2),
	FL_INDEX_SHIFT = (SL_INDEX_COUNT_LOG2 + ALIGN_SIZE_LOG2),
	FL_INDEX_COUNT = (FL_INDEX_MAX - FL_INDEX_SHIFT + 1),

	SMALL_BLOCK_SIZE = (1 << FL_INDEX_SHIFT),
};

/*
** Cast and min/max macros.
*/

#define tlsf_cast(t, exp)	((t) (exp))
#define tlsf_min(a, b)		((a) < (b) ? (a) : (b))
#define tlsf_max(a, b)		((a) > (b) ? (a) : (b))

/*
** Set assert macro, if it has not been provided by the user.
*/
#if !defined (tlsf_assert)
#define tlsf_assert assert
#endif

/*
** Static assertion mechanism.
*/

#define _tlsf_glue2(x, y) x ## y
#define _tlsf_glue(x, y) _tlsf_glue2(x, y)
#define tlsf_static_assert(exp) \
	typedef char _tlsf_glue(static_assert, __LINE__) [(exp) ? 1 : -1]

/* This code has been tested on 32- and 64-bit (LP/LLP) architectures. */
tlsf_static_assert(sizeof(int) * CHAR_BIT == 32);
tlsf_static_assert(sizeof(size_t) * CHAR_BIT >= 32);
tlsf_static_assert(sizeof(size_t) * CHAR_BIT <= 64);

/* SL_INDEX_COUNT must be <= number of bits in sl_bitmap's storage type. */
tlsf_static_assert(sizeof(unsigned int) * CHAR_BIT >= SL_INDEX_COUNT);

/* Ensure we've properly tuned our sizes. */
tlsf_static_assert(ALIGN_SIZE == SMALL_BLOCK_SIZE / SL_INDEX_COUNT);

/*
** Data structures and associated constants.
*/

/*
** Block header structure.
**
** There are several implementation subtleties involved:
** - The prev_phys_block field is only valid if the previous block is free.
** - The prev_phys_block field is actually stored at the end of the
**   previous block. It appears at the beginning of this structure only to
**   simplify the implementation.
** - The next_free / prev_free fields are only valid if the block is free.
*/
typedef struct block_header_t
{
	/* Points to the previous physical block. */
	struct block_header_t* prev_phys_block;

	/* The size of this block, excluding the block header. */
	size_t size;

	/* Next and previous free blocks. */
	struct block_header_t* next_free;
	struct block_header_t* prev_free;
} block_header_t;

/*
** Since block sizes are always at least a multiple of 4, the two least
** significant bits of the size field are used to store the block status:
** - bit 0: whether block is busy or free
** - bit 1: whether previous block is busy or free
*/
static const size_t block_header_free_bit = 1 << 0;
static const size_t block_header_prev_free_bit = 1 << 1;

/*
** The size of the block header exposed to used blocks is the size field.
** The prev_phys_block field is stored *inside* the previous free block.
*/
static const size_t block_header_overhead = sizeof(size_t);

/* User data starts directly after the size field in a used block. */
static const size_t block_start_offset =
	offsetof(block_header_t, size) + sizeof(size_t);

/*
** A free block must be large enough to store its header minus the size of
** the prev_phys_block field, and no larger than the number of addressable
** bits for FL_INDEX.
*/
static const size_t block_size_min = 
	sizeof(block_header_t) - sizeof(block_header_t*);
static const size_t block_size_max = tlsf_cast(size_t, 1) << FL_INDEX_MAX;


/* The TLSF control structure. */
typedef struct control_t
{
	/* Empty lists point at this block to indicate they are free. */
	block_header_t block_null;

	/* Bitmaps for free lists. */
	unsigned int fl_bitmap;
	unsigned int sl_bitmap[FL_INDEX_COUNT];

	/* Head of free lists. */
	block_header_t* blocks[FL_INDEX_COUNT][SL_INDEX_COUNT];
} control_t;

/* A type used for casting when doing pointer arithmetic. */
typedef ptrdiff_t tlsfptr_t;

/*
** block_header_t member functions.
*/

static size_t block_size(const block_header_t* block)
{
	return block->size & ~(block_header_free_bit | block_header_prev_free_bit);
}

static void block_set_size(block_header_t* block, size_t size)
{
	const size_t oldsize = block->size;
	block->size = size | (oldsize & (block_header_free_bit | block_header_prev_free_bit));
}

static int block_is_last(const block_header_t* block)
{
	return block_size(block) == 0;
}

static int block_is_free(const block_header_t* block)
{
	return tlsf_cast(int, block->size & block_header_free_bit);
}

static void block_set_free(block_header_t* block)
{
	block->size |= block_header_free_bit;
}

static void block_set_used(block_header_t* block)
{
	block->size &= ~block_header_free_bit;
}

static int block_is_prev_free(const block_header_t* block)
{
	return tlsf_cast(int, block->size & block_header_prev_free_bit);
}

static void block_set_prev_free(block_header_t* block)
{
	block->size |= block_header_prev_free_bit;
}

static void block_set_prev_used(block_header_t* block)
{
	block->size &= ~block_header_prev_free_bit;
}

static block_header_t* block_from_ptr(const void* ptr)
{
	return tlsf_cast(block_header_t*,
		tlsf_cast(unsigned char*, ptr) - block_start_offset);
}

static void* block_to_ptr(const block_header_t* block)
{
	return tlsf_cast(void*,
		tlsf_cast(unsigned char*, block) + block_start_offset);
}

/* Return location of next block after block of given size. */
static block_header_t* offset_to_block(const void* ptr, size_t size)
{
	return tlsf_cast(block_header_t*, tlsf_cast(tlsfptr_t, ptr) + size);
}

/* Return location of previous block. */
static block_header_t* block_prev(const block_header_t* block)
{
	tlsf_assert(block_is_prev_free(block) && "previous block must be free");
	return block->prev_phys_block;
}

/* Return location of next existing block. */
static block_header_t* block_next(const block_header_t* block)
{
	block_header_t* next = offset_to_block(block_to_ptr(block),
		block_size(block) - block_header_overhead);
	tlsf_assert(!block_is_last(block));
	return next;
}

/* Link a new block with its physical neighbor, return the neighbor. */
static block_header_t* block_link_next(block_header_t* block)
{
	block_header_t* next = block_next(block);
	next->prev_phys_block = block;
	return next;
}

static void block_mark_as_free(block_header_t* block)
{
	/* Link the block to the next block, first. */
	block_header_t* next = block_link_next(block);
	block_set_prev_free(next);
	block_set_free(block);
}

static void block_mark_as_used(block_header_t* block)
{
	block_header_t* next = block_next(block);
	block_set_prev_used(next);
	block_set_used(block);
}

static size_t align_up(size_t x, size_t align)
{
	tlsf_assert(0 == (align & (align - 1)) && "must align to a power of two");
	return (x + (align - 1)) & ~(align - 1);
}

static size_t align_down(size_t x, size_t align)
{
	tlsf_assert(0 == (align & (align - 1)) && "must align to a power of two");
	return x - (x & (align - 1));
}

static void* align_ptr(const void* ptr, size_t align)
{
	const tlsfptr_t aligned =
		(tlsf_cast(tlsfptr_t, ptr) + (align - 1)) & ~(align - 1);
	tlsf_assert(0 == (align & (align - 1)) && "must align to a power of two");
	return tlsf_cast(void*, aligned);
}

/*
** Adjust an allocation size to be aligned to word size, and no smaller
** than internal minimum.
*/
static size_t adjust_request_size(size_t size, size_t align)
{
	size_t adjust = 0;
	if (size)
	{
		const size_t aligned = align_up(size, align);

		/* aligned sized must not exceed block_size_max or we'll go out of bounds on sl_bitmap */
		if (aligned < block_size_max) 
		{
			adjust = tlsf_max(aligned, block_size_min);
		}
	}
	return adjust;
}

/*
** TLSF utility functions. In most cases, these are direct translations of
** the documentation found in the white paper.
*/

static void mapping_insert(size_t size, int* fli, int* sli)
{
	int fl, sl;
	if (size < SMALL_BLOCK_SIZE)
	{
		/* Store small blocks in first list. */
		fl = 0;
		sl = tlsf_cast(int, size) / (SMALL_BLOCK_SIZE / SL_INDEX_COUNT);
	}
	else
	{
		fl = tlsf_fls_sizet(size);
		sl = tlsf_cast(int, size >> (fl - SL_INDEX_COUNT_LOG2)) ^ (1 << SL_INDEX_COUNT_LOG2);
		fl -= (FL_INDEX_SHIFT - 1);
	}
	*fli = fl;
	*sli = sl;
}

/* This version rounds up to the next block size (for allocations) */
static void mapping_search(size_t size, int* fli, int* sli)
{
	if (size >= SMALL_BLOCK_SIZE)
	{
		const size_t round = (1 << (tlsf_fls_sizet(size) - SL_INDEX_COUNT_LOG2)) - 1;
		size += round;
	}
	mapping_insert(size, fli, sli);
}

static block_header_t* search_suitable_block(control_t* control, int* fli, int* sli)
{
	int fl = *fli;
	int sl = *sli;

	/*
	** First, search for a block in the list associated with the given
	** fl/sl index.
	*/
	unsigned int sl_map = control->sl_bitmap[fl] & (~0U << sl);
	if (!sl_map)
	{
		/* No block exists. Search in the next largest first-level list. */
		const unsigned int fl_map = control->fl_bitmap & (~0U << (fl + 1));
		if (!fl_map)
		{
			/* No free blocks available, memory has been exhausted. */
			return 0;
		}

		fl = tlsf_ffs(fl_map);
		*fli = fl;
		sl_map = control->sl_bitmap[fl];
	}
	tlsf_assert(sl_map && "internal error - second level bitmap is null");
	sl = tlsf_ffs(sl_map);
	*sli = sl;

	/* Return the first block in the free list. */
	return control->blocks[fl][sl];
}

/* Remove a free block from the free list.*/
static void remove_free_block(control_t* control, block_header_t* block, int fl, int sl)
{
	block_header_t* prev = block->prev_free;
	block_header_t* next = block->next_free;
	tlsf_assert(prev && "prev_free field can not be null");
	tlsf_assert(next && "next_free field can not be null");
	next->prev_free = prev;
	prev->next_free = next;

	/* If this block is the head of the free list, set new head. */
	if (control->blocks[fl][sl] == block)
	{
		control->blocks[fl][sl] = next;

		/* If the new head is null, clear the bitmap. */
		if (next == &control->block_null)
		{
			control->sl_bitmap[fl] &= ~(1U << sl);

			/* If the second bitmap is now empty, clear the fl bitmap. */
			if (!control->sl_bitmap[fl])
			{
				control->fl_bitmap &= ~(1U << fl);
			}
		}
	}
}

/* Insert a free block into the free block list. */
static void insert_free_block(control_t* control, block_header_t* block, int fl, int sl)
{
	block_header_t* current = control->blocks[fl][sl];
	tlsf_assert(current && "free list cannot have a null entry");
	tlsf_assert(block && "cannot insert a null entry into the free list");
	block->next_free = current;
	block->prev_free = &control->block_null;
	current->prev_free = block;

	tlsf_assert(block_to_ptr(block) == align_ptr(block_to_ptr(block), ALIGN_SIZE)
		&& "block not aligned properly");
	/*
	** Insert the new block at the head of the list, and mark the first-
	** and second-level bitmaps appropriately.
	*/
	control->blocks[fl][sl] = block;
	control->fl_bitmap |= (1U << fl);
	control->sl_bitmap[fl] |= (1U << sl);
}

/* Remove a given block from the free list. */
static void block_remove(control_t* control, block_header_t* block)
{
	int fl, sl;
	mapping_insert(block_size(block), &fl, &sl);
	remove_free_block(control, block, fl, sl);
}

/* Insert a given block into the free list. */
static void block_insert(control_t* control, block_header_t* block)
{
	int fl, sl;
	mapping_insert(block_size(block), &fl, &sl);
	insert_free_block(control, block, fl, sl);
}

static int block_can_split(block_header_t* block, size_t size)
{
	return block_size(block) >= sizeof(block_header_t) + size;
}

/* Split a block into two, the second of which is free. */
static block_header_t* block_split(block_header_t* block, size_t size)
{
	/* Calculate the amount of space left in the remaining block. */
	block_header_t* remaining =
		offset_to_block(block_to_ptr(block), size - block_header_overhead);

	const size_t remain_size = block_size(block) - (size + block_header_overhead);

	tlsf_assert(block_to_ptr(remaining) == align_ptr(block_to_ptr(remaining), ALIGN_SIZE)
		&& "remaining block not aligned properly");

	tlsf_assert(block_size(block) == remain_size + size + block_header_overhead);
	block_set_size(remaining, remain_size);
	tlsf_assert(block_size(remaining) >= block_size_min && "block split with invalid size");

	block_set_size(block, size);
	block_mark_as_free(remaining);

	return remaining;
}

/* Absorb a free block's storage into an adjacent previous free block. */
static block_header_t* block_absorb(block_header_t* prev, block_header_t* block)
{
	tlsf_assert(!block_is_last(prev) && "previous block can't be last");
	/* Note: Leaves flags untouched. */
	prev->size += block_size(block) + block_header_overhead;
	block_link_next(prev);
	return prev;
}

/* Merge a just-freed block with an adjacent previous free block. */
static block_header_t* block_merge_prev(control_t* control, block_header_t* block)
{
	if (block_is_prev_free(block))
	{
		block_header_t* prev = block_prev(block);
		tlsf_assert(prev && "prev physical block can't be null");
		tlsf_assert(block_is_free(prev) && "prev block is not free though marked as such");
		block_remove(control, prev);
		block = block_absorb(prev, block);
	}

	return block;
}

/* Merge a just-freed block with an adjacent free block. */
static block_header_t* block_merge_next(control_t* control, block_header_t* block)
{
	block_header_t* next = block_next(block);
	tlsf_assert(next && "next physical block can't be null");

	if (block_is_free(next))
	{
		tlsf_assert(!block_is_last(block) && "previous block can't be last");
		block_remove(control, next);
		block = block_absorb(block, next);
	}

	return block;
}

/* Trim any trailing block space off the end of a block, return to pool. */
static void block_trim_free(control_t* control, block_header_t* block, size_t size)
{
	tlsf_assert(block_is_free(block) && "block must be free");
	if (block_can_split(block, size))
	{
		block_header_t* remaining_block = block_split(block, size);
		block_link_next(block);
		block_set_prev_free(remaining_block);
		block_insert(control, remaining_block);
	}
}

/* Trim any trailing block space off the end of a used block, return to pool. */
static void block_trim_used(control_t* control, block_header_t* block, size_t size)
{
	tlsf_assert(!block_is_free(block) && "block must be used");
	if (block_can_split(block, size))
	{
		/* If the next block is free, we must coalesce. */
		block_header_t* remaining_block = block_split(block, size);
		block_set_prev_used(remaining_block);

		remaining_block = block_merge_next(control, remaining_block);
		block_insert(control, remaining_block);
	}
}

static block_header_t* block_trim_free_leading(control_t* control, block_header_t* block, size_t size)
{
	block_header_t* remaining_block = block;
	if (block_can_split(block, size))
	{
		/* We want the 2nd block. */
		remaining_block = block_split(block, size - block_header_overhead);
		block_set_prev_free(remaining_block);

		block_link_next(block);
		block_insert(control, block);
	}

	return remaining_block;
}

static block_header_t* block_locate_free(control_t* control, size_t size)
{
	int fl = 0, sl = 0;
	block_header_t* block = 0;

	if (size)
	{
		mapping_search(size, &fl, &sl);
		
		/*
		** mapping_search can futz with the size, so for excessively large sizes it can sometimes wind up 
		** with indices that are off the end of the block array.
		** So, we protect against that here, since this is the only callsite of mapping_search.
		** Note that we don't need to check sl, since it comes from a modulo operation that guarantees it's always in range.
		*/
		if (fl < FL_INDEX_COUNT)
		{
			block = search_suitable_block(control, &fl, &sl);
		}
	}

	if (block)
	{
		tlsf_assert(block_size(block) >= size);
		remove_free_block(control, block, fl, sl);
	}

	return block;
}

static void* block_prepare_used(control_t* control, block_header_t* block, size_t size)
{
	void* p = 0;
	if (block)
	{
		tlsf_assert(size && "size must be non-zero");
		block_trim_free(control, block, size);
		block_mark_as_used(block);
		p = block_to_ptr(block);
	}
	return p;
}

/* Clear structure and point all empty lists at the null block. */
static void control_construct(control_t* control)
{
	int i, j;

	control->block_null.next_free = &control->block_null;
	control->block_null.prev_free = &control->block_null;

	control->fl_bitmap = 0;
	for (i = 0; i < FL_INDEX_COUNT; ++i)
	{
		control->sl_bitmap[i] = 0;
		for (j = 0; j < SL_INDEX_COUNT; ++j)
		{
			control->blocks[i][j] = &control->block_null;
		}
	}
}

/*
** Debugging utilities.
*/

typedef struct integrity_t
{
	int prev_status;
	int status;
} integrity_t;

#define tlsf_insist(x) { tlsf_assert(x); if (!(x)) { status--; } }

static void integrity_walker(void* ptr, size_t size, int used, void* user)
{
	block_header_t* block = block_from_ptr(ptr);
	integrity_t* integ = tlsf_cast(integrity_t*, user);
	const int this_prev_status = block_is_prev_free(block) ? 1 : 0;
	const int this_status = block_is_free(block) ? 1 : 0;
	const size_t this_block_size = block_size(block);

	int status = 0;
	(void)used;
	tlsf_insist(integ->prev_status == this_prev_status && "prev status incorrect");
	tlsf_insist(size == this_block_size && "block size incorrect");

	integ->prev_status = this_status;
	integ->status += status;
}

int tlsf_check(tlsf_t tlsf)
{
	int i, j;

	control_t* control = tlsf_cast(control_t*, tlsf);
	int status = 0;

	/* Check that the free lists and bitmaps are accurate. */
	for (i = 0; i < FL_INDEX_COUNT; ++i)
	{
		for (j = 0; j < SL_INDEX_COUNT; ++j)
		{
			const int fl_map = control->fl_bitmap & (1U << i);
			const int sl_list = control->sl_bitmap[i];
			const int sl_map = sl_list & (1U << j);
			const block_header_t* block = control->blocks[i][j];

			/* Check that first- and second-level lists agree. */
			if (!fl_map)
			{
				tlsf_insist(!sl_map && "second-level map must be null");
			}

			if (!sl_map)
			{
				tlsf_insist(block == &control->block_null && "block list must be null");
				continue;
			}

			/* Check that there is at least one free block. */
			tlsf_insist(sl_list && "no free blocks in second-level map");
			tlsf_insist(block != &control->block_null && "block should not be null");

			while (block != &control->block_null)
			{
				int fli, sli;
				tlsf_insist(block_is_free(block) && "block should be free");
				tlsf_insist(!block_is_prev_free(block) && "blocks should have coalesced");
				tlsf_insist(!block_is_free(block_next(block)) && "blocks should have coalesced");
				tlsf_insist(block_is_prev_free(block_next(block)) && "block should be free");
				tlsf_insist(block_size(block) >= block_size_min && "block not minimum size");

				mapping_insert(block_size(block), &fli, &sli);
				tlsf_insist(fli == i && sli == j && "block size indexed in wrong list");
				block = block->next_free;
			}
		}
	}

	return status;
}

#undef tlsf_insist

static void default_walker(void* ptr, size_t size, int used, void* user)
{
	(void)user;
	printf("\t%p %s size: %x (%p)\n", ptr, used ? "used" : "free", (unsigned int)size, block_from_ptr(ptr));
}

void tlsf_walk_pool(pool_t pool, tlsf_walker walker, void* user)
{
	tlsf_walker pool_walker = walker ? walker : default_walker;
	block_header_t* block =
		offset_to_block(pool, -(int)block_header_overhead);

	while (block && !block_is_last(block))
	{
		pool_walker(
			block_to_ptr(block),
			block_size(block),
			!block_is_free(block),
			user);
		block = block_next(block);
	}
}

size_t tlsf_block_size(void* ptr)
{
	size_t size = 0;
	if (ptr)
	{
		const block_header_t* block = block_from_ptr(ptr);
		size = block_size(block);
	}
	return size;
}

int tlsf_check_pool(pool_t pool)
{
	/* Check that the blocks are physically correct. */
	integrity_t integ = { 0, 0 };
	tlsf_walk_pool(pool, integrity_walker, &integ);

	return integ.status;
}

/*
** Size of the TLSF structures in a given memory block passed to
** tlsf_create, equal to the size of a control_t
*/
size_t tlsf_size(void)
{
	return sizeof(control_t);
}

size_t tlsf_align_size(void)
{
	return ALIGN_SIZE;
}

size_t tlsf_block_size_min(void)
{
	return block_size_min;
}

size_t tlsf_block_size_max(void)
{
	return block_size_max;
}

/*
** Overhead of the TLSF structures in a given memory block passed to
** tlsf_add_pool, equal to the overhead of a free block and the
** sentinel block.
*/
size_t tlsf_pool_overhead(void)
{
	return 2 * block_header_overhead;
}

size_t tlsf_alloc_overhead(void)
{
	return block_header_overhead;
}

pool_t tlsf_add_pool(tlsf_t tlsf, void* mem, size_t bytes)
{
	block_header_t* block;
	block_header_t* next;

	const size_t pool_overhead = tlsf_pool_overhead();
	const size_t pool_bytes = align_down(bytes - pool_overhead, ALIGN_SIZE);

	if (((ptrdiff_t)mem % ALIGN_SIZE) != 0)
	{
		printf("tlsf_add_pool: Memory must be aligned by %u bytes.\n",
			(unsigned int)ALIGN_SIZE);
		return 0;
	}

	if (pool_bytes < block_size_min || pool_bytes > block_size_max)
	{
#if defined (TLSF_64BIT)
		printf("tlsf_add_pool: Memory size must be between 0x%x and 0x%x00 bytes.\n", 
			(unsigned int)(pool_overhead + block_size_min),
			(unsigned int)((pool_overhead + block_size_max) / 256));
#else
		printf("tlsf_add_pool: Memory size must be between %u and %u bytes.\n", 
			(unsigned int)(pool_overhead + block_size_min),
			(unsigned int)(pool_overhead + block_size_max));
#endif
		return 0;
	}

	/*
	** Create the main free block. Offset the start of the block slightly
	** so that the prev_phys_block field falls outside of the pool -
	** it will never be used.
	*/
	block = offset_to_block(mem, -(tlsfptr_t)block_header_overhead);
	block_set_size(block, pool_bytes);
	block_set_free(block);
	block_set_prev_used(block);
	block_insert(tlsf_cast(control_t*, tlsf), block);

	/* Split the block to create a zero-size sentinel block. */
	next = block_link_next(block);
	block_set_size(next, 0);
	block_set_used(next);
	block_set_prev_free(next);

	return mem;
}

void tlsf_remove_pool(tlsf_t tlsf, pool_t pool)
{
	control_t* control = tlsf_cast(control_t*, tlsf);
	block_header_t* block = offset_to_block(pool, -(int)block_header_overhead);

	int fl = 0, sl = 0;

	tlsf_assert(block_is_free(block) && "block should be free");
	tlsf_assert(!block_is_free(block_next(block)) && "next block should not be free");
	tlsf_assert(block_size(block_next(block)) == 0 && "next block size should be zero");

	mapping_insert(block_size(block), &fl, &sl);
	remove_free_block(control, block, fl, sl);
}

/*
** TLSF main interface.
*/

#if _DEBUG
int test_ffs_fls()
{
	/* Verify ffs/fls work properly. */
	int rv = 0;
	rv += (tlsf_ffs(0) == -1) ? 0 : 0x1;
	rv += (tlsf_fls(0) == -1) ? 0 : 0x2;
	rv += (tlsf_ffs(1) == 0) ? 0 : 0x4;
	rv += (tlsf_fls(1) == 0) ? 0 : 0x8;
	rv += (tlsf_ffs(0x80000000) == 31) ? 0 : 0x10;
	rv += (tlsf_ffs(0x80008000) == 15) ? 0 : 0x20;
	rv += (tlsf_fls(0x80000008) == 31) ? 0 : 0x40;
	rv += (tlsf_fls(0x7FFFFFFF) == 30) ? 0 : 0x80;

#if defined (TLSF_64BIT)
	rv += (tlsf_fls_sizet(0x80000000) == 31) ? 0 : 0x100;
	rv += (tlsf_fls_sizet(0x100000000) == 32) ? 0 : 0x200;
	rv += (tlsf_fls_sizet(0xffffffffffffffff) == 63) ? 0 : 0x400;
#endif

	if (rv)
	{
		printf("test_ffs_fls: %x ffs/fls tests failed.\n", rv);
	}
	return rv;
}
#endif

tlsf_t tlsf_create(void* mem)
{
#if _DEBUG
	if (test_ffs_fls())
	{
		return 0;
	}
#endif

	if (((tlsfptr_t)mem % ALIGN_SIZE) != 0)
	{
		printf("tlsf_create: Memory must be aligned to %u bytes.\n",
			(unsigned int)ALIGN_SIZE);
		return 0;
	}

	control_construct(tlsf_cast(control_t*, mem));

	return tlsf_cast(tlsf_t, mem);
}

tlsf_t tlsf_create_with_pool(void* mem, size_t bytes)
{
	tlsf_t tlsf = tlsf_create(mem);
	tlsf_add_pool(tlsf, (char*)mem + tlsf_size(), bytes - tlsf_size());
	return tlsf;
}

void tlsf_destroy(tlsf_t tlsf)
{
	/* Nothing to do. */
	(void)tlsf;
}

pool_t tlsf_get_pool(tlsf_t tlsf)
{
	return tlsf_cast(pool_t, (char*)tlsf + tlsf_size());
}

void* tlsf_malloc(tlsf_t tlsf, size_t size)
{
	control_t* control = tlsf_cast(control_t*, tlsf);
	const size_t adjust = adjust_request_size(size, ALIGN_SIZE);
	block_header_t* block = block_locate_free(control, adjust);
	return block_prepare_used(control, block, adjust);
}

void* tlsf_memalign(tlsf_t tlsf, size_t align, size_t size)
{
	control_t* control = tlsf_cast(control_t*, tlsf);
	const size_t adjust = adjust_request_size(size, ALIGN_SIZE);

	/*
	** We must allocate an additional minimum block size bytes so that if
	** our free block will leave an alignment gap which is smaller, we can
	** trim a leading free block and release it back to the pool. We must
	** do this because the previous physical block is in use, therefore
	** the prev_phys_block field is not valid, and we can't simply adjust
	** the size of that block.
	*/
	const size_t gap_minimum = sizeof(block_header_t);
	const size_t size_with_gap = adjust_request_size(adjust + align + gap_minimum, align);

	/*
	** If alignment is less than or equals base alignment, we're done.
	** If we requested 0 bytes, return null, as tlsf_malloc(0) does.
	*/
	const size_t aligned_size = (adjust && align > ALIGN_SIZE) ? size_with_gap : adjust;

	block_header_t* block = block_locate_free(control, aligned_size);

	/* This can't be a static assert. */
	tlsf_assert(sizeof(block_header_t) == block_size_min + block_header_overhead);

	if (block)
	{
		void* ptr = block_to_ptr(block);
		void* aligned = align_ptr(ptr, align);
		size_t gap = tlsf_cast(size_t,
			tlsf_cast(tlsfptr_t, aligned) - tlsf_cast(tlsfptr_t, ptr));

		/* If gap size is too small, offset to next aligned boundary. */
		if (gap && gap < gap_minimum)
		{
			const size_t gap_remain = gap_minimum - gap;
			const size_t offset = tlsf_max(gap_remain, align);
			const void* next_aligned = tlsf_cast(void*,
				tlsf_cast(tlsfptr_t, aligned) + offset);

			aligned = align_ptr(next_aligned, align);
			gap = tlsf_cast(size_t,
				tlsf_cast(tlsfptr_t, aligned) - tlsf_cast(tlsfptr_t, ptr));
		}

		if (gap)
		{
			tlsf_assert(gap >= gap_minimum && "gap size too small");
			block = block_trim_free_leading(control, block, gap);
		}
	}

	return block_prepare_used(control, block, adjust);
}

void tlsf_free(tlsf_t tlsf, void* ptr)
{
	/* Don't attempt to free a NULL pointer. */
	if (ptr)
	{
		control_t* control = tlsf_cast(control_t*, tlsf);
		block_header_t* block = block_from_ptr(ptr);
		tlsf_assert(!block_is_free(block) && "block already marked as free");
		block_mark_as_free(block);
		block = block_merge_prev(control, block);
		block = block_merge_next(control, block);
		block_insert(control, block);
	}
}

/*
** The TLSF block information provides us with enough information to
** provide a reasonably intelligent implementation of realloc, growing or
** shrinking the currently allocated block as required.
**
** This routine handles the somewhat esoteric edge cases of realloc:
** - a non-zero size with a null pointer will behave like malloc
** - a zero size with a non-null pointer will behave like free
** - a request that cannot be satisfied will leave the original buffer
**   untouched
** - an extended buffer size will leave the newly-allocated area with
**   contents undefined
*/
void* tlsf_realloc(tlsf_t tlsf, void* ptr, size_t size)
{
	control_t* control = tlsf_cast(control_t*, tlsf);
	void* p = 0;

	/* Zero-size requests are treated as free. */
	if (ptr && size == 0)
	{
		tlsf_free(tlsf, ptr);
	}
	/* Requests with NULL pointers are treated as malloc. */
	else if (!ptr)
	{
		p = tlsf_malloc(tlsf, size);
	}
	else
	{
		block_header_t* block = block_from_ptr(ptr);
		block_header_t* next = block_next(block);

		const size_t cursize = block_size(block);
		const size_t combined = cursize + block_size(next) + block_header_overhead;
		const size_t adjust = adjust_request_size(size, ALIGN_SIZE);

		tlsf_assert(!block_is_free(block) && "block already marked as free");

		/*
		** If the next block is used, or when combined with the current
		** block, does not offer enough space, we must reallocate and copy.
		*/
		if (adjust > cursize && (!block_is_free(next) || adjust > combined))
		{
			p = tlsf_malloc(tlsf, size);
			if (p)
			{
				const size_t minsize = tlsf_min(cursize, size);
				memcpy(p, ptr, minsize);
				tlsf_free(tlsf, ptr);
			}
		}
		else
		{
			/* Do we need to expand to the next block? */
			if (adjust > cursize)
			{
				block_merge_next(control, block);
				block_mark_as_used(block);
			}

			/* Trim the resulting block and return the original pointer. */
			block_trim_used(control, block, adjust);
			p = ptr;
		}
	}

	return p;
}


/*
** Bounded, read-only heap audit.
**
** Everything below works from caller-supplied ranges rather than from any
** global heap state. A pointer found in corrupted memory is never dereferenced
** until its address has been proven to lie fully inside one of the ranges;
** all pointer/offset arithmetic is checked for size_t wrap. The routine never
** writes through any pointer into the audited memory.
*/

/* A fully-validated, half-open byte range [lo, hi) inside the caller's map. */
typedef struct audit_range_t
{
	unsigned char* lo;
	unsigned char* hi;
} audit_range_t;

/* Per-pool audit state. `map` is exactly the readable range the caller named
** ([pool_base, pool_base + bytes)); first_header is pool_base minus one word,
** the address TLSF uses for the first block whose prev_phys field lives
** outside the mapping and is never dereferenced. */
typedef struct audit_pool_state_t
{
	audit_range_t map;                 /* exact caller-supplied readable bytes */
	unsigned char* first_header;       /* pool_base - block_header_overhead */
	unsigned char* expected_sentinel;  /* derived only from trusted descriptors */
	size_t free_count;
} audit_pool_state_t;

typedef struct audit_ctx_t
{
	audit_range_t control;
	const block_header_t* null_node;
	audit_pool_state_t* pools;
	size_t pool_count;
	/* Flat free-block table and visit table, carved from caller workspace.
	** Visit values: 0 = unseen, 1 = seen in an earlier bucket,
	** 2 = on the current bucket's traversal chain. */
	block_header_t** free_blocks;
	size_t free_count;
	size_t free_cap;
	unsigned char* visited;
	tlsf_audit_result_t* out;
} audit_ctx_t;

static void audit_fail(tlsf_audit_result_t* out,
	tlsf_audit_corruption_t corruption, tlsf_audit_region_t region,
	int pool_index, size_t offset, const char* detail)
{
	if (out)
	{
		out->status = TLSF_AUDIT_CORRUPT;
		out->corruption = corruption;
		out->region = region;
		out->pool_index = pool_index;
		out->offset = offset;
		out->detail = detail;
	}
}

static int audit_add_wraps(size_t a, size_t b)
{
	return a > (size_t)-1 - b;
}

static int audit_sub_wraps(size_t a, size_t b)
{
	return b > a;
}

/* True exactly when [lo, lo+len) is a sub-range of r, with wrap checks. */
static int audit_range_contains(const audit_range_t* r,
	const unsigned char* lo, size_t len)
{
	size_t off;
	if (lo < r->lo || lo >= r->hi)
	{
		return 0;
	}
	off = tlsf_cast(size_t, lo - r->lo);
	if (audit_add_wraps(off, len) ||
		off + len > tlsf_cast(size_t, r->hi - r->lo))
	{
		return 0;
	}
	return 1;
}

/* True when two validated ranges share any byte. Touching endpoints is fine. */
static int audit_ranges_overlap(const audit_range_t* a, const audit_range_t* b)
{
	return a->lo < b->hi && b->lo < a->hi;
}

/* Validate a raw (base, bytes) pair into a half-open range. */
static int audit_make_range(audit_range_t* r, const void* base, size_t bytes)
{
	unsigned char* lo = tlsf_cast(unsigned char*, tlsf_cast(uintptr_t, base));
	if (!base || bytes == 0)
	{
		return 0;
	}
	if (tlsf_cast(uintptr_t, lo) + bytes < tlsf_cast(uintptr_t, lo))
	{
		return 0;
	}
	r->lo = lo;
	r->hi = lo + bytes;
	return 1;
}

static int audit_ptr_aligned(const void* p, size_t align)
{
	return (tlsf_cast(uintptr_t, p) & (align - 1)) == 0;
}

/*
** Validate a free-list target. The shared null node is the only permitted
** control-area target; every other target must be a pool block whose size and
** link fields (everything after the prev_phys word) lie fully inside that
** pool's mapped bytes. The prev_phys word of the first block sits outside the
** mapping and is never part of this window. Returns -1 for the null node, -2
** when the target is not fully readable, otherwise the owning pool index.
*/
static int audit_locate_block(const audit_ctx_t* ctx, const block_header_t* block)
{
	const unsigned char* fields;
	size_t i;
	if (block == ctx->null_node)
	{
		return -1;
	}
	/* Readable fields are size/next_free/prev_free, starting after prev_phys. */
	fields = tlsf_cast(const unsigned char*, block) + sizeof(void*);
	for (i = 0; i < ctx->pool_count; ++i)
	{
		if (audit_range_contains(&ctx->pools[i].map, fields,
			sizeof(block_header_t) - sizeof(void*)))
		{
			return tlsf_cast(int, i);
		}
	}
	return -2;
}

/*
** Look a free-list link target up in the blocks the physical scan proved
** free. The shared null node is the only permitted control-area target; any
** other aligned control address is rejected as an impostor.
*/
static int audit_find_free_block(const audit_ctx_t* ctx, const block_header_t* block)
{
	size_t i;
	if (block == ctx->null_node)
	{
		return -1;
	}
	for (i = 0; i < ctx->free_count; ++i)
	{
		if (ctx->free_blocks[i] == block)
		{
			return tlsf_cast(int, i);
		}
	}
	return -2;
}

static size_t audit_pool_block_offset(const audit_pool_state_t* ps,
	const block_header_t* block)
{
	return tlsf_cast(size_t,
		tlsf_cast(const unsigned char*, block) - ps->first_header);
}

/*
** Physical sweep of one pool. Walks block headers using only validated,
** wrap-checked advancement, verifies flags/back-links/coalescing/sentinel,
** and records every free block in the context-wide table for list checks.
*/
static tlsf_audit_status_t audit_scan_pool(audit_ctx_t* ctx, size_t pool_index)
{
	audit_pool_state_t* ps = &ctx->pools[pool_index];
	const unsigned char* base = ps->map.lo;
	block_header_t* block =
		tlsf_cast(block_header_t*, tlsf_cast(void*, ps->first_header));
	block_header_t* prev = 0;
	int prev_free = 0;
	int is_first = 1;

	/*
	** The first block's header pointer is base minus one word. Its
	** prev_phys_block field occupies that out-of-pool word and is never read;
	** its size word begins exactly at base, inside the mapped bytes.
	*/
	if (!audit_range_contains(&ps->map, base, sizeof(size_t)))
	{
		audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_TRUNCATED,
			TLSF_AUDIT_REGION_POOL, tlsf_cast(int, pool_index), 0,
			"first block size word is outside the supplied pool range");
		return TLSF_AUDIT_CORRUPT;
	}
	if (block_is_prev_free(block))
	{
		audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_PREV_FLAG,
			TLSF_AUDIT_REGION_POOL, tlsf_cast(int, pool_index), 0,
			"first pool block must mark its predecessor as used");
		return TLSF_AUDIT_CORRUPT;
	}

	for (;;)
	{
		size_t raw_size;
		size_t size;
		size_t block_off;
		block_header_t* next;
		size_t step;
		/* The size word always sits one word past the header pointer. */
		unsigned char* size_word =
			tlsf_cast(unsigned char*, block) + sizeof(void*);

		if (!audit_ptr_aligned(block, ALIGN_SIZE))
		{
			audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_BLOCK_ALIGN,
				TLSF_AUDIT_REGION_POOL, tlsf_cast(int, pool_index),
				tlsf_cast(size_t,
					tlsf_cast(unsigned char*, block) - ps->first_header),
				"block header is not aligned to tlsf_align_size()");
			return TLSF_AUDIT_CORRUPT;
		}
		block_off = tlsf_cast(size_t,
			tlsf_cast(unsigned char*, block) - ps->first_header);

		/* The size word must be inside the mapped range before it is read. */
		if (!audit_range_contains(&ps->map, size_word, sizeof(size_t)))
		{
			audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_TRUNCATED,
				TLSF_AUDIT_REGION_POOL, tlsf_cast(int, pool_index), block_off,
				"block size word falls outside the supplied pool range");
			return TLSF_AUDIT_CORRUPT;
		}

		raw_size = block->size;
		size = block_size(block);

		if (size == 0)
		{
			/*
			** Sentinel: a zero-size used block. Its position is computed from
			** the trusted descriptor, never from corrupted contents, so a bad
			** chain cannot make us accept a sentinel in the middle of a pool.
			*/
			if (tlsf_cast(unsigned char*, block) != ps->expected_sentinel)
			{
				audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_SENTINEL,
					TLSF_AUDIT_REGION_POOL, tlsf_cast(int, pool_index),
					block_off,
					"zero-size block is not at the expected end sentinel position");
				return TLSF_AUDIT_CORRUPT;
			}
			if (block_is_free(block))
			{
				audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_SENTINEL,
					TLSF_AUDIT_REGION_POOL, tlsf_cast(int, pool_index),
					block_off, "sentinel block must be marked used");
				return TLSF_AUDIT_CORRUPT;
			}
			if (!block_is_prev_free(block))
			{
				audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_SENTINEL,
					TLSF_AUDIT_REGION_POOL, tlsf_cast(int, pool_index),
					block_off, "sentinel block must mark its predecessor free");
				return TLSF_AUDIT_CORRUPT;
			}
			if (ps->free_count == 0)
			{
				audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_SENTINEL,
					TLSF_AUDIT_REGION_POOL, tlsf_cast(int, pool_index),
					block_off, "pool has no free block before the sentinel");
				return TLSF_AUDIT_CORRUPT;
			}
			break;
		}

		/* Sizes are multiples of ALIGN_SIZE; the other low bits are reserved
		** for the two documented status flags. */
		if ((raw_size & (ALIGN_SIZE - 1)) &
			~(block_header_free_bit | block_header_prev_free_bit))
		{
			audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_BLOCK_SIZE,
				TLSF_AUDIT_REGION_POOL, tlsf_cast(int, pool_index),
				block_off, "block size has reserved low bits set");
			return TLSF_AUDIT_CORRUPT;
		}
		if (size < block_size_min)
		{
			audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_BLOCK_SIZE,
				TLSF_AUDIT_REGION_POOL, tlsf_cast(int, pool_index),
				block_off, "block size is below the minimum block size");
			return TLSF_AUDIT_CORRUPT;
		}
		if (size >= block_size_max)
		{
			audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_BLOCK_SIZE,
				TLSF_AUDIT_REGION_POOL, tlsf_cast(int, pool_index),
				block_off, "block size reaches or exceeds the maximum block size");
			return TLSF_AUDIT_CORRUPT;
		}

		/* Advance: next = block + block_start_offset + (size - overhead). */
		if (audit_sub_wraps(size, block_header_overhead))
		{
			audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_BLOCK_SIZE,
				TLSF_AUDIT_REGION_POOL, tlsf_cast(int, pool_index),
				block_off, "block size wraps while computing the next block");
			return TLSF_AUDIT_CORRUPT;
		}
		step = block_start_offset + (size - block_header_overhead);
		if (audit_add_wraps(block_off, step))
		{
			audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_BLOCK_SIZE,
				TLSF_AUDIT_REGION_POOL, tlsf_cast(int, pool_index),
				block_off, "next block offset wraps the address space");
			return TLSF_AUDIT_CORRUPT;
		}
		next = tlsf_cast(block_header_t*,
			tlsf_cast(unsigned char*, block) + step);

		/* The successor size word must be readable before it is trusted. */
		if (!audit_range_contains(&ps->map,
			tlsf_cast(unsigned char*, next) + sizeof(void*), sizeof(size_t)))
		{
			audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_TRUNCATED,
				TLSF_AUDIT_REGION_POOL, tlsf_cast(int, pool_index),
				block_off, "block chain runs past the end of the pool range");
			return TLSF_AUDIT_CORRUPT;
		}

		/* Prev-free flag must agree with the predecessor's actual state.
		** block_is_prev_free() yields the raw bit value (0 or 2), so compare
		** its truthiness against the normalized predecessor state. */
		if (!!block_is_prev_free(block) != prev_free)
		{
			audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_PREV_FLAG,
				TLSF_AUDIT_REGION_POOL, tlsf_cast(int, pool_index),
				block_off,
				"prev-free flag disagrees with the predecessor block state");
			return TLSF_AUDIT_CORRUPT;
		}

		/* A free predecessor leaves its back-link in this block's header.
		** The first block's predecessor is fictitious and its back-link word
		** is outside the mapping, so it is skipped explicitly. For every other
		** block the word is proven mapped before it is dereferenced. */
		if (prev_free && !is_first)
		{
			if (!audit_range_contains(&ps->map,
				tlsf_cast(const unsigned char*, block), sizeof(void*)))
			{
				audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_TRUNCATED,
					TLSF_AUDIT_REGION_POOL, tlsf_cast(int, pool_index),
					block_off,
					"prev_phys_block word falls outside the pool range");
				return TLSF_AUDIT_CORRUPT;
			}
			if (block->prev_phys_block != prev)
			{
				audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_PREV_POINTER,
					TLSF_AUDIT_REGION_POOL, tlsf_cast(int, pool_index),
					block_off,
					"prev_phys_block does not point to the previous free block");
				return TLSF_AUDIT_CORRUPT;
			}
		}

		/* Adjacent free blocks must have been coalesced. */
		if (prev_free && block_is_free(block))
		{
			audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_COALESCE,
				TLSF_AUDIT_REGION_POOL, tlsf_cast(int, pool_index),
				block_off, "two adjacent free blocks were not coalesced");
			return TLSF_AUDIT_CORRUPT;
		}

		if (block_is_free(block))
		{
			/* The table was sized from the validated ranges; fail closed. */
			if (ctx->free_count >= ctx->free_cap)
			{
				audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_TRUNCATED,
					TLSF_AUDIT_REGION_POOL, tlsf_cast(int, pool_index),
					block_off, "free block table exhausted");
				return TLSF_AUDIT_CORRUPT;
			}
			ctx->free_blocks[ctx->free_count++] = block;
			ps->free_count++;
		}

		prev = block;
		prev_free = block_is_free(block);
		block = next;
		is_first = 0;
	}

	return TLSF_AUDIT_OK;
}

/*
** Free-list and bitmap checks. Every link target is range- and alignment-
** checked before it is read, must be either the shared null node or a block
** proven free by the physical scan, and traversal is bounded by the number of
** physically free blocks so a cycle can never loop forever.
*/
static tlsf_audit_status_t audit_check_lists(audit_ctx_t* ctx)
{
	const control_t* control =
		tlsf_cast(const control_t*, ctx->control.lo);
	const size_t total_free = ctx->free_count;
	size_t i, j;

	/*
	** The null node's identity is the single address &control->block_null:
	** empty buckets must hold it and every list terminates at it. Its own
	** next_free/prev_free fields are scratch space reused while lists are
	** edited (they need not point at itself, and prev_free is shared across
	** every bucket rather than marking any bucket's tail), so they are not
	** treated as an invariant here.
	*/

	/* Stray bits above the implemented first-level indexes are always wrong. */
	if (control->fl_bitmap & ~((1U << FL_INDEX_COUNT) - 1))
	{
		audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_BITMAP,
			TLSF_AUDIT_REGION_CONTROL, -1, offsetof(control_t, fl_bitmap),
			"first-level bitmap has bits outside the implemented index range");
		return TLSF_AUDIT_CORRUPT;
	}

	for (i = 0; i < FL_INDEX_COUNT; ++i)
	{
		const unsigned int fl_bit = control->fl_bitmap & (1U << i);
		const unsigned int sl_map = control->sl_bitmap[i];

		if (!fl_bit && sl_map != 0)
		{
			audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_BITMAP,
				TLSF_AUDIT_REGION_CONTROL, -1,
				offsetof(control_t, sl_bitmap) + i * sizeof(unsigned int),
				"second-level bitmap set while first-level bit is clear");
			return TLSF_AUDIT_CORRUPT;
		}
		if (fl_bit && sl_map == 0)
		{
			audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_BITMAP,
				TLSF_AUDIT_REGION_CONTROL, -1,
				offsetof(control_t, fl_bitmap),
				"first-level bitmap set with no second-level entries");
			return TLSF_AUDIT_CORRUPT;
		}

		for (j = 0; j < SL_INDEX_COUNT; ++j)
		{
			const unsigned int sl_bit = sl_map & (1U << j);
			const block_header_t* head = control->blocks[i][j];
			const block_header_t* node;
			size_t steps = 0;

			if (!sl_bit)
			{
				if (head != ctx->null_node)
				{
					audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_BITMAP,
						TLSF_AUDIT_REGION_CONTROL, -1,
						offsetof(control_t, blocks)
							+ (i * SL_INDEX_COUNT + j) * sizeof(void*),
						"empty bucket must point at the shared null node");
					return TLSF_AUDIT_CORRUPT;
				}
				continue;
			}
			if (head == ctx->null_node)
			{
				audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_BITMAP,
					TLSF_AUDIT_REGION_CONTROL, -1,
					offsetof(control_t, blocks)
						+ (i * SL_INDEX_COUNT + j) * sizeof(void*),
					"bitmap claims a bucket that points at the null node");
				return TLSF_AUDIT_CORRUPT;
			}

			/* Retire the previous bucket's chain marks so a repeat inside
			** this bucket reads as a cycle, not a cross-bucket duplicate. */
			for (size_t k = 0; k < total_free; ++k)
			{
				if (ctx->visited[k] == 2)
				{
					ctx->visited[k] = 1;
				}
			}

			node = head;
			while (node != ctx->null_node)
			{
				int idx;
				int fli, sli;
				const block_header_t* next_link;
				const block_header_t* prev_link;
				int pool_idx;
				size_t node_off;
				tlsf_audit_region_t node_region;

				if (++steps > total_free)
				{
					audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_LIST_CYCLE,
						TLSF_AUDIT_REGION_CONTROL, -1,
						offsetof(control_t, blocks)
							+ (i * SL_INDEX_COUNT + j) * sizeof(void*),
						"free-list traversal exceeded the number of free blocks");
					return TLSF_AUDIT_CORRUPT;
				}

				/* Prove the target is aligned and fully mapped before reading. */
				if (!audit_ptr_aligned(node, ALIGN_SIZE))
				{
					audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_LIST_ALIGN,
						TLSF_AUDIT_REGION_CONTROL, -1,
						offsetof(control_t, blocks)
							+ (i * SL_INDEX_COUNT + j) * sizeof(void*),
						"free-list target is not aligned to tlsf_align_size()");
					return TLSF_AUDIT_CORRUPT;
				}
				pool_idx = audit_locate_block(ctx, node);
				if (pool_idx < -1)
				{
					audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_LIST_TARGET,
						TLSF_AUDIT_REGION_CONTROL, -1,
						offsetof(control_t, blocks)
							+ (i * SL_INDEX_COUNT + j) * sizeof(void*),
						"free-list target lies outside every supplied range");
					return TLSF_AUDIT_CORRUPT;
				}
				idx = audit_find_free_block(ctx, node);
				if (idx == -2)
				{
					audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_LIST_TARGET,
						TLSF_AUDIT_REGION_CONTROL, -1,
						offsetof(control_t, blocks)
							+ (i * SL_INDEX_COUNT + j) * sizeof(void*),
						"free-list target is not a proven free block "
						"or is a control-area null impostor");
					return TLSF_AUDIT_CORRUPT;
				}
				if (ctx->visited[idx] == 2)
				{
					/* Reached again inside this same bucket chain: a cycle. */
					audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_LIST_CYCLE,
						TLSF_AUDIT_REGION_CONTROL, -1,
						offsetof(control_t, blocks)
							+ (i * SL_INDEX_COUNT + j) * sizeof(void*),
						"free list loops back to a block already on its chain");
					return TLSF_AUDIT_CORRUPT;
				}
				if (ctx->visited[idx] == 1)
				{
					audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_LIST_DUPLICATE,
						TLSF_AUDIT_REGION_CONTROL, -1,
						offsetof(control_t, blocks)
							+ (i * SL_INDEX_COUNT + j) * sizeof(void*),
						"free block appears more than once in the free lists");
					return TLSF_AUDIT_CORRUPT;
				}
				ctx->visited[idx] = 2;

				/* Resolve reporting coordinates for this mapped node. */
				if (pool_idx >= 0)
				{
					node_region = TLSF_AUDIT_REGION_POOL;
					node_off = audit_pool_block_offset(&ctx->pools[pool_idx], node);
				}
				else
				{
					node_region = TLSF_AUDIT_REGION_CONTROL;
					node_off = tlsf_cast(size_t,
						tlsf_cast(const unsigned char*, node) - ctx->control.lo);
				}

				/* The block size must map to the bucket it was found in.
				** Physical validation already bounded the size, so the index
				** arithmetic used by the allocator stays in range. */
				mapping_insert(block_size(node), &fli, &sli);
				if (fli != tlsf_cast(int, i) || sli != tlsf_cast(int, j))
				{
					audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_WRONG_BUCKET,
						node_region, pool_idx >= 0 ? pool_idx : -1, node_off,
						"free block size does not map to its free-list bucket");
					return TLSF_AUDIT_CORRUPT;
				}

				/* Links are read only after the node itself is proven. */
				next_link = node->next_free;
				prev_link = node->prev_free;

				if (!audit_ptr_aligned(next_link, ALIGN_SIZE) ||
					!audit_ptr_aligned(prev_link, ALIGN_SIZE))
				{
					audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_LIST_ALIGN,
						node_region, pool_idx >= 0 ? pool_idx : -1,
						node_off + offsetof(block_header_t, next_free),
						"free-list link value is misaligned");
					return TLSF_AUDIT_CORRUPT;
				}
				if (audit_locate_block(ctx, next_link) < -1 ||
					audit_locate_block(ctx, prev_link) < -1)
				{
					audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_LIST_TARGET,
						node_region, pool_idx >= 0 ? pool_idx : -1,
						node_off + offsetof(block_header_t, next_free),
						"free-list link points outside every supplied range");
					return TLSF_AUDIT_CORRUPT;
				}
				if (audit_find_free_block(ctx, next_link) == -2 ||
					audit_find_free_block(ctx, prev_link) == -2)
				{
					audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_LIST_TARGET,
						node_region, pool_idx >= 0 ? pool_idx : -1,
						node_off + offsetof(block_header_t, next_free),
						"free-list link does not target a free block or the null node");
					return TLSF_AUDIT_CORRUPT;
				}

				/* A successor already on this chain closes a cycle. Detect it
				** before the reciprocity checks, which would otherwise mislabel
				** the same corruption as a generic broken link. */
				if (next_link != ctx->null_node)
				{
					int next_idx = audit_find_free_block(ctx, next_link);
					if (next_idx >= 0 && ctx->visited[next_idx] == 2)
					{
						audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_LIST_CYCLE,
							node_region, pool_idx >= 0 ? pool_idx : -1,
							node_off + offsetof(block_header_t, next_free),
							"free list next link loops back onto its own chain");
						return TLSF_AUDIT_CORRUPT;
					}
				}

				/* Head nodes carry the shared null node as their prev link. */
				if (node == head && prev_link != ctx->null_node)
				{
					audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_LIST_LINK,
						node_region, pool_idx >= 0 ? pool_idx : -1,
						node_off + offsetof(block_header_t, prev_free),
						"free-list head must have the null node as its prev link");
					return TLSF_AUDIT_CORRUPT;
				}
				/* Interior nodes must link back to a real predecessor. */
				if (node != head && prev_link == ctx->null_node)
				{
					audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_LIST_LINK,
						node_region, pool_idx >= 0 ? pool_idx : -1,
						node_off + offsetof(block_header_t, prev_free),
						"interior free block must not use the null node as prev");
					return TLSF_AUDIT_CORRUPT;
				}
				/* Reciprocal links for real successors and interior nodes. */
				if (next_link != ctx->null_node &&
					next_link->prev_free != node)
				{
					audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_LIST_LINK,
						node_region, pool_idx >= 0 ? pool_idx : -1,
						node_off + offsetof(block_header_t, next_free),
						"next free block does not link back to this block");
					return TLSF_AUDIT_CORRUPT;
				}
				if (node != head && prev_link != ctx->null_node &&
					prev_link->next_free != node)
				{
					audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_LIST_LINK,
						node_region, pool_idx >= 0 ? pool_idx : -1,
						node_off + offsetof(block_header_t, prev_free),
						"previous free block does not link forward to this block");
					return TLSF_AUDIT_CORRUPT;
				}

				node = next_link;
			}
		}
	}

	/* Every physically free block must have been visited exactly once. */
	for (i = 0; i < total_free; ++i)
	{
		const block_header_t* block = ctx->free_blocks[i];
		if (!ctx->visited[i])
		{
			int pool_idx = audit_locate_block(ctx, block);
			if (pool_idx >= 0)
			{
				audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_LIST_MISSING,
					TLSF_AUDIT_REGION_POOL, pool_idx,
					audit_pool_block_offset(&ctx->pools[pool_idx], block),
					"physically free block is missing from every free list");
			}
			else
			{
				audit_fail(ctx->out, TLSF_AUDIT_CORRUPT_LIST_MISSING,
					TLSF_AUDIT_REGION_CONTROL, -1,
					tlsf_cast(size_t, tlsf_cast(const unsigned char*, block)
						- ctx->control.lo),
					"physically free block is missing from every free list");
			}
			return TLSF_AUDIT_CORRUPT;
		}
	}

	return TLSF_AUDIT_OK;
}

size_t tlsf_audit_workspace_size(size_t pool_count, size_t total_pool_bytes)
{
	/*
	** Upper bound on physical blocks: each real block advances at least
	** block_start_offset + block_size_min bytes, and every pool contributes
	** one sentinel, so total/per_block + pool_count rows always suffice.
	*/
	const size_t per_block = block_start_offset + block_size_min;
	size_t max_blocks;
	size_t bytes;

	if (pool_count == 0)
	{
		return sizeof(audit_pool_state_t);
	}
	max_blocks = (total_pool_bytes / per_block) + pool_count;
	bytes = pool_count * sizeof(audit_pool_state_t);
	if (audit_add_wraps(bytes, max_blocks * sizeof(block_header_t*)) ||
		audit_add_wraps(bytes, max_blocks * sizeof(unsigned char)))
	{
		return (size_t)-1;
	}
	bytes += max_blocks * sizeof(block_header_t*);
	bytes += max_blocks * sizeof(unsigned char);
	return bytes;
}

tlsf_audit_status_t tlsf_audit(
	const void* control, size_t control_bytes,
	const tlsf_audit_pool_t* pools, size_t pool_count,
	void* workspace, size_t workspace_bytes,
	tlsf_audit_result_t* result)
{
	audit_ctx_t ctx;
	audit_pool_state_t* states;
	block_header_t** free_table;
	unsigned char* visited;
	size_t i, cursor, total_pool_bytes, needed, max_blocks, per_block;
	unsigned char* control_lo;

	if (result)
	{
		result->status = TLSF_AUDIT_OK;
		result->corruption = TLSF_AUDIT_CORRUPT_NONE;
		result->region = TLSF_AUDIT_REGION_CONTROL;
		result->pool_index = -1;
		result->offset = 0;
		result->detail = "audit passed";
	}

	/* Descriptor validation: nothing in the heap is trusted at this point. */
	if (!audit_make_range(&ctx.control, control, control_bytes) ||
		!audit_ptr_aligned(control, ALIGN_SIZE) ||
		control_bytes < sizeof(control_t))
	{
		if (result)
		{
			result->status = TLSF_AUDIT_EINVAL;
			result->detail = "control range is null, misaligned, or too small";
		}
		return TLSF_AUDIT_EINVAL;
	}
	control_lo = tlsf_cast(unsigned char*, tlsf_cast(void*, control));
	ctx.control.lo = control_lo;
	ctx.control.hi = control_lo + control_bytes;

	if (pool_count > 0 && !pools)
	{
		if (result)
		{
			result->status = TLSF_AUDIT_EINVAL;
			result->detail = "pool descriptor array is null";
		}
		return TLSF_AUDIT_EINVAL;
	}
	if (pool_count > (size_t)-1 / sizeof(audit_pool_state_t))
	{
		if (result)
		{
			result->status = TLSF_AUDIT_EINVAL;
			result->detail = "pool count is implausibly large";
		}
		return TLSF_AUDIT_EINVAL;
	}
	if (workspace_bytes > 0 && !workspace)
	{
		if (result)
		{
			result->status = TLSF_AUDIT_EINVAL;
			result->detail = "workspace size given but workspace is null";
		}
		return TLSF_AUDIT_EINVAL;
	}

	total_pool_bytes = 0;
	for (i = 0; i < pool_count; ++i)
	{
		if (!pools[i].base || pools[i].bytes == 0)
		{
			if (result)
			{
				result->status = TLSF_AUDIT_EINVAL;
				result->region = TLSF_AUDIT_REGION_POOL;
				result->pool_index = tlsf_cast(int, i);
				result->detail = "pool base is null or pool size is zero";
			}
			return TLSF_AUDIT_EINVAL;
		}
		if (!audit_ptr_aligned(pools[i].base, ALIGN_SIZE))
		{
			if (result)
			{
				result->status = TLSF_AUDIT_EINVAL;
				result->region = TLSF_AUDIT_REGION_POOL;
				result->pool_index = tlsf_cast(int, i);
				result->detail = "pool base is not aligned to tlsf_align_size()";
			}
			return TLSF_AUDIT_EINVAL;
		}
		if (pools[i].bytes < tlsf_pool_overhead() + block_size_min)
		{
			if (result)
			{
				result->status = TLSF_AUDIT_EINVAL;
				result->region = TLSF_AUDIT_REGION_POOL;
				result->pool_index = tlsf_cast(int, i);
				result->detail = "pool range is too small for one minimum block";
			}
			return TLSF_AUDIT_EINVAL;
		}
		if (audit_add_wraps(total_pool_bytes, pools[i].bytes))
		{
			if (result)
			{
				result->status = TLSF_AUDIT_EINVAL;
				result->detail = "total pool size wraps size_t";
			}
			return TLSF_AUDIT_EINVAL;
		}
		total_pool_bytes += pools[i].bytes;
	}

	/* Carve the caller workspace: states, flat free table, visit table. */
	per_block = block_start_offset + block_size_min;
	max_blocks = (total_pool_bytes / per_block) + pool_count;
	needed = pool_count * sizeof(audit_pool_state_t);
	if (audit_add_wraps(needed, max_blocks * sizeof(block_header_t*)) ||
		audit_add_wraps(needed, max_blocks * sizeof(unsigned char)))
	{
		if (result)
		{
			result->status = TLSF_AUDIT_ENOSPC;
			result->detail = "workspace requirement overflows size_t";
		}
		return TLSF_AUDIT_ENOSPC;
	}
	needed += max_blocks * sizeof(block_header_t*);
	needed += max_blocks * sizeof(unsigned char);
	if (workspace_bytes < needed)
	{
		if (result)
		{
			result->status = TLSF_AUDIT_ENOSPC;
			result->detail = "workspace too small; see tlsf_audit_workspace_size()";
		}
		return TLSF_AUDIT_ENOSPC;
	}

	states = tlsf_cast(audit_pool_state_t*, workspace);
	cursor = pool_count * sizeof(audit_pool_state_t);
	free_table = tlsf_cast(block_header_t**,
		tlsf_cast(unsigned char*, workspace) + cursor);
	cursor += max_blocks * sizeof(block_header_t*);
	visited = tlsf_cast(unsigned char*, workspace) + cursor;
	/* The visit table is pure scratch; clear stale marks from earlier runs. */
	if (max_blocks > 0)
	{
		memset(visited, 0, max_blocks);
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.control.lo = control_lo;
	ctx.control.hi = control_lo + control_bytes;
	ctx.null_node =
		&(tlsf_cast(control_t*, tlsf_cast(void*, control))->block_null);
	ctx.pools = states;
	ctx.pool_count = pool_count;
	ctx.free_blocks = free_table;
	ctx.free_count = 0;
	ctx.free_cap = max_blocks;
	ctx.visited = visited;
	ctx.out = result;

	/* Build the exact per-pool readable ranges. The one-word gap before each
	** base (the first block's prev_phys field) is deliberately excluded. */
	for (i = 0; i < pool_count; ++i)
	{
		unsigned char* base =
			tlsf_cast(unsigned char*, tlsf_cast(void*, pools[i].base));
		audit_pool_state_t* ps = &states[i];
		size_t user_bytes = pools[i].bytes;
		size_t pool_bytes;

		if (!audit_make_range(&ps->map, base, user_bytes))
		{
			if (result)
			{
				result->status = TLSF_AUDIT_EINVAL;
				result->detail = "pool range end wraps the address space";
			}
			return TLSF_AUDIT_EINVAL;
		}
		/* first_header = base - one word; reject an underflowing address. */
		if (tlsf_cast(uintptr_t, base) < block_header_overhead)
		{
			if (result)
			{
				result->status = TLSF_AUDIT_EINVAL;
				result->detail = "pool header word would wrap below address zero";
			}
			return TLSF_AUDIT_EINVAL;
		}
		ps->first_header = base - block_header_overhead;
		ps->free_count = 0;

		/* Expected sentinel position, mirroring tlsf_add_pool's layout. */
		pool_bytes = (user_bytes - tlsf_pool_overhead()) & ~(ALIGN_SIZE - 1);
		ps->expected_sentinel = base + pool_bytes;
	}

	/* Ranges must not overlap; adjacent ranges are allowed. */
	for (i = 0; i < pool_count; ++i)
	{
		size_t k;
		if (audit_ranges_overlap(&ctx.control, &states[i].map))
		{
			if (result)
			{
				result->status = TLSF_AUDIT_EINVAL;
				result->detail = "pool range overlaps the control range";
			}
			return TLSF_AUDIT_EINVAL;
		}
		for (k = i + 1; k < pool_count; ++k)
		{
			if (audit_ranges_overlap(&states[i].map, &states[k].map))
			{
				if (result)
				{
					result->status = TLSF_AUDIT_EINVAL;
					result->region = TLSF_AUDIT_REGION_POOL;
					result->pool_index = tlsf_cast(int, k);
					result->detail = "pool ranges overlap";
				}
				return TLSF_AUDIT_EINVAL;
			}
		}
	}

	/* Physical sweeps first, in the caller's pool order. */
	for (i = 0; i < pool_count; ++i)
	{
		tlsf_audit_status_t st = audit_scan_pool(&ctx, i);
		if (st != TLSF_AUDIT_OK)
		{
			return st;
		}
	}

	/* Bitmap and free-list checks in fl/sl index order. */
	return audit_check_lists(&ctx);
}
