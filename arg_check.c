/*
 * Functions for testing of string routines arguments.
 *
 * Copyright 2020 by Gray Watson
 *
 * This file is part of the dmalloc package.
 *
 * Permission to use, copy, modify, and distribute this software for
 * any purpose and without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies, and that the name of Gray Watson not be used in advertising
 * or publicity pertaining to distribution of the document or software
 * without specific, written prior permission.
 *
 * Gray Watson makes no representations about the suitability of the
 * software described herein for any purpose.  It is provided "as is"
 * without express or implied warranty.
 *
 * The author may be contacted via https://dmalloc.com/
 */

/*
 * This file contains functions to be used to verify the arguments of
 * string functions.   If enabled these can discover problems with
 * heap-based strings (such as fence errors) much closer to the error.
 */

#define DMALLOC_DISABLE

#if HAVE_STRING_H
# include <string.h>
#endif
#if HAVE_STRINGS_H
# include <strings.h>
#endif
#if HAVE_STDLIB_H
# include <stdlib.h>
#endif

#include "conf.h"
#include "dmalloc.h"

#include "chunk.h"
#include "debug_tok.h"
#include "error.h"
#include "error_val.h"
#include "dmalloc_loc.h"
#include "arg_check.h"

/*
 * Current policy for the memcpy, memmove, strncpy, and strncat
 * function-checking wrappers.  See dmalloc_set_func_check_policy.
 */
static	int	func_check_policy = DMALLOC_FUNC_REPORT;

/*
 * int dmalloc_set_func_check_policy
 *
 * Set the policy used by the dmalloc memcpy, memmove, strncpy, and
 * strncat function-checking wrappers when they detect a dangerous
 * copy.  DMALLOC_FUNC_REPORT (the default) reports problems but still
 * calls the libc routine.  DMALLOC_FUNC_REJECT refuses dangerous
 * copies between dmalloc-tracked user allocations before any memory is
 * accessed.
 *
 * Returns the previous valid policy or -1 if policy is invalid, in
 * which case the active policy is left unchanged.
 *
 * ARGUMENTS:
 *
 * policy -> DMALLOC_FUNC_REPORT or DMALLOC_FUNC_REJECT.
 */
int	dmalloc_set_func_check_policy(const int policy)
{
  int	prev_policy = func_check_policy;

  if (policy != DMALLOC_FUNC_REPORT && policy != DMALLOC_FUNC_REJECT) {
    return -1;
  }
  func_check_policy = policy;

  return prev_policy;
}

/************************** strict-mode helpers ****************************/

/*
 * static void strict_reject
 *
 * Report a strict-mode rejection through the normal dmalloc error
 * machinery, keeping the caller's file and line information in the
 * logs.  dmalloc_errno must already be set by the caller.
 *
 * ARGUMENTS:
 *
 * file -> File-name or return-address of the caller.
 *
 * line -> Line-number of the caller.
 *
 * func -> Function string which is rejecting the operation.
 *
 * reason -> Description of why the operation was rejected.
 */
static	void	strict_reject(const char *file, const int line,
			      const char *func, const char *reason)
{
  char	where_buf[MAX_FILE_LENGTH + 64];

  dmalloc_message("strict %s rejected from '%s': %s", func,
		  _dmalloc_chunk_desc_pnt(where_buf, sizeof(where_buf),
					  file, line),
		  reason);
  dmalloc_error(func);
}

/*
 * static int strict_lookup
 *
 * Look up the active dmalloc user allocation containing a pointer and
 * return the byte just past its requested user space.  Unknown,
 * freed, fence-post, or otherwise untracked pointers are rejected
 * without being dereferenced.
 *
 * Returns 1 if the pointer is inside an active user allocation or 0 if
 * the operation must be rejected (dmalloc_errno set).
 *
 * ARGUMENTS:
 *
 * file -> File-name or return-address of the caller.
 *
 * line -> Line-number of the caller.
 *
 * func -> Function string which is checking the pointer.
 *
 * pnt -> Pointer we are looking up.
 *
 * bounds_p <- Pointer to a character pointer which, if not NULL, will
 * be set to the byte just past the end of the user allocation.
 */
static	int	strict_lookup(const char *file, const int line,
			      const char *func, const void *pnt,
			      const char **bounds_p)
{
  const char	*bounds;

  if (! _dmalloc_user_range(file, line, pnt, NULL, &bounds)) {
    /* dmalloc_errno set in the chunk code */
    strict_reject(file, line, func,
		  "pointer not in an active user allocation");
    return 0;
  }

  SET_POINTER(bounds_p, bounds);
  return 1;
}

/*
 * static int strict_len_check
 *
 * Make sure that len bytes starting at a pointer fit inside of the
 * requested user space of its active dmalloc allocation.  The length
 * is compared against the remaining user bytes so huge lengths cannot
 * wrap the address arithmetic.
 *
 * Returns 1 if the range is valid or 0 if the operation must be
 * rejected (dmalloc_errno set).
 *
 * ARGUMENTS: see strict_lookup, plus
 *
 * len -> Number of bytes which must be available from pnt.
 */
static	int	strict_len_check(const char *file, const int line,
				 const char *func, const void *pnt,
				 const DMALLOC_SIZE len,
				 const char **bounds_p)
{
  const char	*bounds;

  if (! strict_lookup(file, line, func, pnt, &bounds)) {
    return 0;
  }

  /* compare against the remaining user bytes, never compute pnt + len */
  if (len > (DMALLOC_SIZE)(bounds - (const char *)pnt)) {
    dmalloc_errno = DMALLOC_ERROR_WOULD_OVERWRITE;
    strict_reject(file, line, func, "range exceeds user allocation");
    return 0;
  }

  SET_POINTER(bounds_p, bounds);
  return 1;
}

/*
 * static int strict_overlap
 *
 * Test whether two address ranges intersect.  The lengths must already
 * have been verified to fit inside of user allocations so the end
 * addresses cannot wrap around.
 *
 * Returns 1 if the ranges overlap or 0 if not.
 *
 * ARGUMENTS:
 *
 * pnt1 -> Start of the first range.
 *
 * len1 -> Length of the first range.
 *
 * pnt2 -> Start of the second range.
 *
 * len2 -> Length of the second range.
 */
static	int	strict_overlap(const void *pnt1, const DMALLOC_SIZE len1,
			       const void *pnt2, const DMALLOC_SIZE len2)
{
  PNT_ARITH_TYPE	addr1 = (PNT_ARITH_TYPE)pnt1;
  PNT_ARITH_TYPE	addr2 = (PNT_ARITH_TYPE)pnt2;

  return (addr1 < addr2 + len2 && addr2 < addr1 + len1);
}

/*
 * static int strict_copy_check
 *
 * Strict-mode validation for _dmalloc_memcpy and _dmalloc_memmove.
 * Both complete len ranges must lie inside of active dmalloc user
 * allocations.  If overlap_bad_b is set then the read and write ranges
 * must not intersect.
 *
 * Returns 1 if the copy may proceed or 0 if it must be rejected
 * (dmalloc_errno set).
 *
 * ARGUMENTS:
 *
 * file -> File-name or return-address of the caller.
 *
 * line -> Line-number of the caller.
 *
 * func -> Function string which is checking the copy.
 *
 * to -> Destination pointer.
 *
 * from -> Source pointer.
 *
 * len -> Number of bytes to copy.
 *
 * overlap_bad_b -> Set to 1 to reject overlapping ranges.
 */
static	int	strict_copy_check(const char *file, const int line,
				  const char *func, const void *to,
				  const void *from, const DMALLOC_SIZE len,
				  const int overlap_bad_b)
{
  if (! strict_len_check(file, line, func, to, len, NULL)) {
    return 0;
  }
  if (! strict_len_check(file, line, func, from, len, NULL)) {
    return 0;
  }

  if (overlap_bad_b && strict_overlap(to, len, from, len)) {
    dmalloc_errno = DMALLOC_ERROR_OVERLAP;
    strict_reject(file, line, func, "copy ranges overlap");
    return 0;
  }

  return 1;
}

/*
 * static int strict_strncpy_check
 *
 * Strict-mode validation for _dmalloc_strncpy.  The destination always
 * receives all len bytes (the copy plus zero padding) so the complete
 * len range must fit.  The source only needs to cover the bytes up to
 * and including its first null, or len bytes if it has no null in the
 * first len bytes.  The actual read and write ranges must not
 * intersect.
 *
 * Returns 1 if the copy may proceed or 0 if it must be rejected
 * (dmalloc_errno set).
 *
 * ARGUMENTS: see strict_copy_check.
 */
static	int	strict_strncpy_check(const char *file, const int line,
				     const char *to, const char *from,
				     const DMALLOC_SIZE len)
{
  const char	*from_bounds, *scan_p;
  DMALLOC_SIZE	avail, scan, read_len;

  /* the destination gets all len bytes, even if the source is shorter */
  if (! strict_len_check(file, line, "strncpy", to, len, NULL)) {
    return 0;
  }

  if (! strict_lookup(file, line, "strncpy", from, &from_bounds)) {
    return 0;
  }

  /*
   * The source is known-valid user memory so we can scan it for the
   * first null to find the number of bytes which will actually be
   * read.  We never scan past the end of the source allocation.
   */
  avail = (DMALLOC_SIZE)(from_bounds - from);
  scan = MIN(len, avail);
  read_len = 0;
  for (scan_p = from; scan_p < from + scan; scan_p++) {
    if (*scan_p == '\0') {
      read_len = (DMALLOC_SIZE)(scan_p - from) + 1;
      break;
    }
  }
  if (read_len == 0) {
    if (avail < len) {
      /* no null and not enough readable source bytes for len */
      dmalloc_errno = DMALLOC_ERROR_WOULD_OVERWRITE;
      strict_reject(file, line, "strncpy",
		    "source not terminated within readable range");
      return 0;
    }
    read_len = len;
  }

  if (strict_overlap(to, len, from, read_len)) {
    dmalloc_errno = DMALLOC_ERROR_OVERLAP;
    strict_reject(file, line, "strncpy", "read and write ranges overlap");
    return 0;
  }

  return 1;
}

/*
 * static int strict_strncat_check
 *
 * Strict-mode validation for _dmalloc_strncat.  The destination string
 * must be null-terminated inside of its user allocation (verified even
 * when len is 0, in which case the source is not consulted).  The
 * source must provide up to len non-null characters, stopping at its
 * first null.  The copy plus its terminating null must fit past the
 * destination null, and the actual read and write ranges must not
 * intersect.
 *
 * Returns 1 if the concatenation may proceed or 0 if it must be
 * rejected (dmalloc_errno set).
 *
 * ARGUMENTS: see strict_copy_check.
 */
static	int	strict_strncat_check(const char *file, const int line,
				     const char *to, const char *from,
				     const DMALLOC_SIZE len)
{
  const char	*to_bounds, *from_bounds, *scan_p;
  DMALLOC_SIZE	dest_len, remaining, avail, scan, copy_len, read_len;

  if (! strict_lookup(file, line, "strncat", to, &to_bounds)) {
    return 0;
  }

  /* find the terminating null inside of the destination user space */
  for (scan_p = to; scan_p < to_bounds; scan_p++) {
    if (*scan_p == '\0') {
      break;
    }
  }
  if (scan_p >= to_bounds) {
    dmalloc_errno = DMALLOC_ERROR_NOT_TERMINATED;
    strict_reject(file, line, "strncat",
		  "destination string not terminated within allocation");
    return 0;
  }
  dest_len = (DMALLOC_SIZE)(scan_p - to);

  /* with len == 0 only the destination termination above is verified */
  if (len == 0) {
    return 1;
  }

  if (! strict_lookup(file, line, "strncat", from, &from_bounds)) {
    return 0;
  }

  /* see strict_strncpy_check for why this scan is safe */
  avail = (DMALLOC_SIZE)(from_bounds - from);
  scan = MIN(len, avail);
  copy_len = 0;
  read_len = 0;
  for (scan_p = from; scan_p < from + scan; scan_p++) {
    if (*scan_p == '\0') {
      copy_len = (DMALLOC_SIZE)(scan_p - from);
      read_len = copy_len + 1;
      break;
    }
  }
  if (read_len == 0) {
    if (avail < len) {
      /* ran out of readable source before len characters or a null */
      dmalloc_errno = DMALLOC_ERROR_WOULD_OVERWRITE;
      strict_reject(file, line, "strncat",
		    "source not terminated within readable range");
      return 0;
    }
    copy_len = len;
    read_len = len;
  }

  /*
   * The copy plus its terminating null must fit past the destination
   * null.  remaining >= 1 because the null was found inside the user
   * space; comparing with < avoids overflowing copy_len + 1.
   */
  remaining = (DMALLOC_SIZE)(to_bounds - to) - dest_len;
  if (copy_len >= remaining) {
    dmalloc_errno = DMALLOC_ERROR_WOULD_OVERWRITE;
    strict_reject(file, line, "strncat",
		  "concatenation exceeds destination allocation");
    return 0;
  }

  if (strict_overlap(to + dest_len, copy_len + 1, from, read_len)) {
    dmalloc_errno = DMALLOC_ERROR_OVERLAP;
    strict_reject(file, line, "strncat", "read and write ranges overlap");
    return 0;
  }

  return 1;
}

/*
 * Dummy function for checking strlen's arguments.
 */
static	int	loc_strlen(const char *file, const int line,
			   const char *func, const char *str)
{
  int		len = 0;
  const	char	*str_p;
  
  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    if (! dmalloc_verify_pnt(file, line, func, str, 0 /* not exact */, -1)) {
      dmalloc_message("bad pointer argument found in %s", func);
    }
  }
  
  for (str_p = str; *str_p != '\0'; str_p++) {
    len++;
  }
  
  return len;
}

#if HAVE_ATOI
/*
 * Dummy function for checking atoi's arguments.
 */
int	_dmalloc_atoi(const char *file, const int line, const char *str)
{
  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    if (! dmalloc_verify_pnt(file, line, "atoi", str,
			     0 /* not exact */, -1)) {
      dmalloc_message("bad pointer argument found in atoi");
    }
  }
  return atoi(str);
}
#endif /* HAVE_ATOI */

#if HAVE_ATOL
/*
 * Dummy function for checking atol's arguments.
 */
long	_dmalloc_atol(const char *file, const int line, const char *str)
{
  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    if (! dmalloc_verify_pnt(file, line, "atol", str,
			     0 /* not exact */, -1)) {
      dmalloc_message("bad pointer argument found in atol");
    }
  }
  return atol(str);
}
#endif /* HAVE_ATOL */

#if HAVE_BCMP
/*
 * Dummy function for checking bcmp's arguments.
 */
int	_dmalloc_bcmp(const char *file, const int line,
		      const void *b1, const void *b2, const DMALLOC_SIZE len)
{
  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    if ((! dmalloc_verify_pnt(file, line, "bcmp", b1,
			      0 /* not exact */, len))
	|| (! dmalloc_verify_pnt(file, line, "bcmp", b2,
				 0 /* not exact */, len))) {
      dmalloc_message("bad pointer argument found in bcmp");
    }
  }
  return bcmp(b1, b2, len);
}
#endif /* HAVE_BCMP */

#if HAVE_BCOPY
/*
 * Dummy function for checking bcopy's arguments.
 */
void	_dmalloc_bcopy(const char *file, const int line,
		       const void *from, void *to, const DMALLOC_SIZE len)
{
  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    if ((! dmalloc_verify_pnt(file, line, "bcopy", from,
			      0 /* not exact */, len))
	|| (! dmalloc_verify_pnt(file, line, "bcopy", to,
				 0 /* not exact */, len))) {
      dmalloc_message("bad pointer argument found in bcopy");
    }
  }
  bcopy(from, to, len);
}
#endif /* HAVE_BCOPY */

#if HAVE_BZERO
/*
 * Dummy function for checking bzero's arguments.
 */
void	_dmalloc_bzero(const char *file, const int line,
		       void *buf, const DMALLOC_SIZE len)
{
  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    if (! dmalloc_verify_pnt(file, line, "bzero", buf,
			     0 /* not exact */, len)) {
      dmalloc_message("bad pointer argument found in bzero");
    }
  }
  bzero(buf, len);
}
#endif /* HAVE_BZERO */

#if HAVE_INDEX
/*
 * Dummy function for checking index's arguments.
 */
char	*_dmalloc_index(const char *file, const int line,
			const char *str, const char ch)
{
  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    if (! dmalloc_verify_pnt(file, line, "index", str,
			     0 /* not exact */, -1)) {
      dmalloc_message("bad pointer argument found in index");
    }
  }
  return (char *)index(str, ch);
}
#endif /* HAVE_INDEX */

#if HAVE_MEMCCPY
/*
 * Dummy function for checking memccpy's arguments.
 */
void	*_dmalloc_memccpy(const char *file, const int line,
			  void *dest, const void *src, const int ch,
			  const DMALLOC_SIZE len)
{
  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    const char	*src_p;
    int		min_size;
    
    /* so we have to figure out the max length of the buffer directly here */
    for (src_p = (char *)src; src_p < (char *)src + len; src_p++) {
      if (*src_p == ch) {
	src_p++;
	break;
      }
    }
    min_size = src_p - (char *)src;
    
    /* maybe len maybe first ch */
    if ((! dmalloc_verify_pnt(file, line, "memccpy", dest,
			      0 /* not exact */, min_size))
	|| (! dmalloc_verify_pnt(file, line, "memccpy", src,
				 0 /* not exact */, min_size))) {
      dmalloc_message("bad pointer argument found in memccpy");
    }
  }
  return (void *)memccpy(dest, src, ch, len);
}
#endif /* HAVE_MEMCCPY */

#if HAVE_MEMCHR
/*
 * Dummy function for checking memchr's arguments.
 */
void	*_dmalloc_memchr(const char *file, const int line,
			 const void *s1, const int ch, const DMALLOC_SIZE len)
{
  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    if (! dmalloc_verify_pnt(file, line, "memchr", s1,
			     0 /* not exact */, len)) {
      dmalloc_message("bad pointer argument found in memchr");
    }
  }
  return (void *)memchr(s1, ch, len);
}
#endif /* HAVE_MEMCHR */

#if HAVE_MEMCMP
/*
 * Dummy function for checking memcmp's arguments.
 */
int	_dmalloc_memcmp(const char *file, const int line,
			const void *b1, const void *b2, const DMALLOC_SIZE len)
{
  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    if ((! dmalloc_verify_pnt(file, line, "memcmp", b1,
			      0 /* not exact */, len))
	|| (! dmalloc_verify_pnt(file, line, "memcmp", b2,
				 0 /* not exact */, len))) {
      dmalloc_message("bad pointer argument found in memcmp");
    }
  }
  return memcmp(b1, b2, len);
}
#endif /* HAVE_MEMCMP */

#if HAVE_MEMCPY
/*
 * Dummy function for checking memcpy's arguments.
 */
void	*_dmalloc_memcpy(const char *file, const int line,
			 void *to, const void *from, const DMALLOC_SIZE len)
{
  if (func_check_policy == DMALLOC_FUNC_REJECT) {
    /* zero-length copies access no memory and query no pointers */
    if (len == 0) {
      return to;
    }
    if (! strict_copy_check(file, line, "memcpy", to, from, len,
			    1 /* no overlap */)) {
      return NULL;
    }
  }
  else if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    if ((! dmalloc_verify_pnt(file, line, "memcpy", to,
			      0 /* not exact */, len))
	|| (! dmalloc_verify_pnt(file, line, "memcpy", from,
				 0 /* not exact */, len))) {
      dmalloc_message("bad pointer argument found in memcpy");
    }

#if HAVE_MEMMOVE
    /*
     * If memmove exists, dump out a warning if memcpy is being used
     * for overlapping memory segments.
     */
    if (((char *)from < (char *)to && (char *)from + len > (char *)to)
	|| ((char *)to < (char *)from && (char *)to + len > (char *)from)) {
      dmalloc_message("%s:%d: WARNING: memory overlap in memcpy, should use memmove",
		      file, line);
    }
#endif
  }
  return (void *)memcpy(to, from, len);
}
#endif /* HAVE_MEMCPY */

#if HAVE_MEMMOVE
/*
 * Dummy function for checking memmove's arguments.
 */
void	*_dmalloc_memmove(const char *file, const int line,
			  void *to, const void *from, const DMALLOC_SIZE len)
{
  if (func_check_policy == DMALLOC_FUNC_REJECT) {
    /* zero-length moves access no memory and query no pointers */
    if (len == 0) {
      return to;
    }
    if (! strict_copy_check(file, line, "memmove", to, from, len,
			    0 /* overlap okay */)) {
      return NULL;
    }
  }
  else if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    if ((! dmalloc_verify_pnt(file, line, "memmove", to,
			      0 /* not exact */, len))
	|| (! dmalloc_verify_pnt(file, line, "memmove", from,
				 0 /* not exact */, len))) {
      dmalloc_message("bad pointer argument found in memmove");
    }
  }
  return (void *)memmove(to, from, len);
}
#endif /* HAVE_MEMMOVE */

#if HAVE_MEMSET
/*
 * Dummy function for checking memset's arguments.
 */
void	*_dmalloc_memset(const char *file, const int line, void *buf,
			 const int ch, const DMALLOC_SIZE len)
{
  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    if (! dmalloc_verify_pnt(file, line, "memset", buf,
			     0 /* not exact */, len)) {
      dmalloc_message("bad pointer argument found in memset");
    }
  }
  return (void *)memset(buf, ch, len);
}
#endif /* HAVE_MEMSET */

#if HAVE_RINDEX
/*
 * Dummy function for checking rindex's arguments.
 */
char	*_dmalloc_rindex(const char *file, const int line,
			 const char *str, const char ch)
{
  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    if (! dmalloc_verify_pnt(file, line, "rindex", str,
			     0 /* not exact */, -1)) {
      dmalloc_message("bad pointer argument found in rindex");
    }
  }
  return (char *)rindex(str, ch);
}
#endif /* HAVE_RINDEX */

#if HAVE_STRCASECMP
/*
 * Dummy function for checking strcasecmp's arguments.
 */
int	_dmalloc_strcasecmp(const char *file, const int line,
			    const char *s1, const char *s2)
{
  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    if ((! dmalloc_verify_pnt(file, line, "strcasecmp", s1,
			      0 /* not exact */, -1))
	|| (! dmalloc_verify_pnt(file, line, "strcasecmp", s2,
				 0 /* not exact */, -1))) {
      dmalloc_message("bad pointer argument found in strcasecmp");
    }
  }
  return strcasecmp(s1, s2);
}
#endif /* HAVE_STRCASECMP */

#if HAVE_STRCAT
/*
 * Dummy function for checking strcat's arguments.
 */
char	*_dmalloc_strcat(const char *file, const int line,
			 char *to, const char *from)
{
  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    if ((! dmalloc_verify_pnt(file, line, "strcat", to,
			      0 /* not exact */,
			      loc_strlen(file, line, "strcat", to)
			      + loc_strlen(file, line, "strcat", from) + 1))
	|| (! dmalloc_verify_pnt(file, line, "strcat", from,
				 0 /* not exact */, -1))) {
      dmalloc_message("bad pointer argument found in strcat");
    }
  }
  return (char *)strcat(to, from);
}
#endif /* HAVE_STRCAT */

#if HAVE_STRCHR
/*
 * Dummy function for checking strchr's arguments.
 */
char	*_dmalloc_strchr(const char *file, const int line,
			 const char *str, const int ch)
{
  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    if (! dmalloc_verify_pnt(file, line, "strchr", str,
			     0 /* not exact */, -1)) {
      dmalloc_message("bad pointer argument found in strchr");
    }
  }
  return (char *)strchr(str, ch);
}
#endif /* HAVE_STRCHR */

#if HAVE_STRCMP
/*
 * Dummy function for checking strcmp's arguments.
 */
int	_dmalloc_strcmp(const char *file, const int line,
			const char *s1, const char *s2)
{
  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    if ((! dmalloc_verify_pnt(file, line, "strcmp", s1,
			      0 /* not exact */, -1))
	|| (! dmalloc_verify_pnt(file, line, "strcmp", s2,
				 0 /* not exact */, -1))) {
      dmalloc_message("bad pointer argument found in strcmp");
    }
  }
  return strcmp(s1, s2);
}
#endif /* HAVE_STRCMP */

#if HAVE_STRCPY
/*
 * Dummy function for checking strcpy's arguments.
 */
char	*_dmalloc_strcpy(const char *file, const int line,
			 char *to, const char *from)
{
  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    if ((! dmalloc_verify_pnt(file, line, "strcpy", to,
			      0 /* not exact */,
			      loc_strlen(file, line, "strcpy", from) + 1))
	|| (! dmalloc_verify_pnt(file, line, "strcpy", from,
				 0 /* not exact */, -1))) {
      dmalloc_message("bad pointer argument found in strcpy");
    }
  }
  return (char *)strcpy(to, from);
}
#endif /* HAVE_STRCPY */

#if HAVE_STRCSPN
/*
 * Dummy function for checking strcspn's arguments.
 */
int	_dmalloc_strcspn(const char *file, const int line,
			 const char *str, const char *list)
{
  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    if ((! dmalloc_verify_pnt(file, line, "strcspn", str,
			      0 /* not exact */, -1))
	|| (! dmalloc_verify_pnt(file, line, "strcspn", list,
				 0 /* not exact */, -1))) {
      dmalloc_message("bad pointer argument found in strcspn");
    }
  }
  return strcspn(str, list);
}
#endif /* HAVE_STRCSPN */

#if HAVE_STRLEN
/*
 * Dummy function for checking strlen's arguments.
 */
DMALLOC_SIZE	_dmalloc_strlen(const char *file, const int line,
				const char *str)
{
  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    if (! dmalloc_verify_pnt(file, line, "strlen", str,
			     0 /* not exact */, -1)) {
      dmalloc_message("bad pointer argument found in strlen");
    }
  }
  
  return loc_strlen(file, line, "strlen", str);
}
#endif

#if HAVE_STRNCASECMP
/*
 * Dummy function for checking strncasecmp's arguments.
 */
int	_dmalloc_strncasecmp(const char *file, const int line,
			     const char *s1, const char *s2,
			     const DMALLOC_SIZE len)
{
  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    const char	*s1_p, *s2_p;
    int		min_size;
    
    /* we go through both pointers up to the length characters */
    for (s1_p = s1, s2_p = s2; s1_p < s1 + len; s1_p++, s2_p++) {
      if (*s1_p == '\0' || *s2_p == '\0') {
	s1_p++;
 	break;
      }
    }
    min_size = s1_p - s1;
    
    if ((! dmalloc_verify_pnt(file, line, "strncasecmp", s1,
			      0 /* not exact */, min_size))
	|| (! dmalloc_verify_pnt(file, line, "strncasecmp", s2,
				 0 /* not exact */, min_size))) {
      dmalloc_message("bad pointer argument found in strncasecmp");
    }
  }
  return strncasecmp(s1, s2, len);
}
#endif /* HAVE_STRNCASECMP */

#if HAVE_STRNCAT
/*
 * Dummy function for checking strncat's arguments.
 */
char	*_dmalloc_strncat(const char *file, const int line,
			  char *to, const char *from, const DMALLOC_SIZE len)
{
  if (func_check_policy == DMALLOC_FUNC_REJECT) {
    if (! strict_strncat_check(file, line, to, from, len)) {
      return NULL;
    }
  }
  else if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    const char	*from_p;
    int		min_size;

    /* so we have to figure out the max length of the buffers directly here */
    for (from_p = from; from_p < from + len; from_p++) {
      if (*from_p == '\0') {
	/* no need to do ++ here because we +1 for the \0 anyway */
	break;
      }
    }
    min_size = from_p - from;

    /* either len or nullc */
    if ((! dmalloc_verify_pnt(file, line, "strncat", to,
			      0 /* not exact */,
			      loc_strlen(file, line, "strncat", to) + min_size
			      + 1))
	|| (! dmalloc_verify_pnt(file, line, "strncat", from,
				 0 /* not exact */, min_size))) {
      dmalloc_message("bad pointer argument found in strncat");
    }
  }
  return (char *)strncat(to, from, len);
}
#endif /* HAVE_STRNCAT */

#if HAVE_STRNCMP
/*
 * Dummy function for checking strncmp's arguments.
 */
int	_dmalloc_strncmp(const char *file, const int line,
			 const char *s1, const char *s2,
			 const DMALLOC_SIZE len)
{
  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    const char	*s1_p, *s2_p;
    int		min_size;
    
    /* so we have to figure out the max length of the buffers directly here */
    for (s1_p = s1, s2_p = s2; s1_p < s1 + len; s1_p++, s2_p++) {
      if (*s1_p == '\0' || *s2_p == '\0') {
	s1_p++;
	break;
      }
    }
    min_size = s1_p - s1;
    
    /* either len or nullc */
    if ((! dmalloc_verify_pnt(file, line, "strncmp", s1,
			      0 /* not exact */, min_size))
	|| (! dmalloc_verify_pnt(file, line, "strncmp", s2,
				 0 /* not exact */, min_size))) {
      dmalloc_message("bad pointer argument found in strncmp");
    }
  }
  return strncmp(s1, s2, len);
}
#endif /* HAVE_STRNCMP */

#if HAVE_STRNCPY
/*
 * Dummy function for checking strncpy's arguments.
 */
char	*_dmalloc_strncpy(const char *file, const int line,
			  char *to, const char *from, const DMALLOC_SIZE len)
{
  if (func_check_policy == DMALLOC_FUNC_REJECT) {
    /* zero-length copies access no memory and query no pointers */
    if (len == 0) {
      return to;
    }
    if (! strict_strncpy_check(file, line, to, from, len)) {
      return NULL;
    }
  }
  else if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    const char	*from_p;
    int		min_size;

    /* so we have to figure out the max length of the buffers directly here */
    for (from_p = from; from_p < from + len; from_p++) {
      if (*from_p == '\0') {
	from_p++;
	break;
      }
    }
    min_size = from_p - from;

    /* len or until nullc */
    if ((! dmalloc_verify_pnt(file, line, "strncpy", to,
			      0 /* not exact */, min_size))
	|| (! dmalloc_verify_pnt(file, line, "strncpy", from,
				 0 /* not exact */, min_size))) {
      dmalloc_message("bad pointer argument found in strncpy");
    }
  }
  return (char *)strncpy(to, from, len);
}
#endif /* HAVE_STRNCPY */

#if HAVE_STRPBRK
/*
 * Dummy function for checking strpbrk's arguments.
 */
char	*_dmalloc_strpbrk(const char *file, const int line,
			  const char *str, const char *list)
{
  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    if ((! dmalloc_verify_pnt(file, line, "strpbrk", str,
			      0 /* not exact */, -1))
	|| (! dmalloc_verify_pnt(file, line, "strpbrk", list,
				 0 /* not exact */, -1))) {
      dmalloc_message("bad pointer argument found in strpbrk");
    }
  }
  return (char *)strpbrk(str, list);
}
#endif /* HAVE_STRPBRK */

#if HAVE_STRRCHR
/*
 * Dummy function for checking strrchr's arguments.
 */
char	*_dmalloc_strrchr(const char *file, const int line,
			  const char *str, const int ch)
{
  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    if (! dmalloc_verify_pnt(file, line, "strrchr", str,
			     0 /* not exact */, -1)) {
      dmalloc_message("bad pointer argument found in strrchr");
    }
  }
  return (char *)strrchr(str, ch);
}
#endif /* HAVE_STRRCHR */

#if HAVE_STRSPN
/*
 * Dummy function for checking strspn's arguments.
 */
int	_dmalloc_strspn(const char *file, const int line,
			const char *str, const char *list)
{
  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    if ((! dmalloc_verify_pnt(file, line, "strspn", str,
			      0 /* not exact */, -1))
	|| (! dmalloc_verify_pnt(file, line, "strspn", list,
				 0 /* not exact */, -1))) {
      dmalloc_message("bad pointer argument found in strspn");
    }
  }
  return strspn(str, list);
}
#endif /* HAVE_STRSPN */

#if HAVE_STRSTR
/*
 * Dummy function for checking strstr's arguments.
 */
char	*_dmalloc_strstr(const char *file, const int line,
			 const char *str, const char *pat)
{
  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    if ((! dmalloc_verify_pnt(file, line, "strstr", str,
			      0 /* not exact */, -1))
	|| (! dmalloc_verify_pnt(file, line, "strstr", pat,
				 0 /* not exact */, -1))) {
      dmalloc_message("bad pointer argument found in strstr");
    }
  }
  return (char *)strstr(str, pat);
}
#endif /* HAVE_STRSTR */

#if HAVE_STRTOK
/*
 * Dummy function for checking strtok's arguments.
 */
char	*_dmalloc_strtok(const char *file, const int line,
			 char *str, const char *sep)
{
  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
    if ((str != NULL
	 && (! dmalloc_verify_pnt(file, line, "strtok", str,
				  0 /* not exact */, -1)))
	|| (! dmalloc_verify_pnt(file, line, "strtok", sep,
				 0 /* not exact */, -1))) {
      dmalloc_message("bad pointer argument found in strtok");
    }
  }
  return (char *)strtok(str, sep);
}
#endif /* HAVE_STRTOK */
