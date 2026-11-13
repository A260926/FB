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
 * Strict mode helpers.  DMALLOC_FUNC_REJECT is implemented entirely on
 * top of the allocation records: a pointer is resolved to the live
 * user area which contains it before any byte is read from it, and no
 * unknown pointer is ever dereferenced.
 */

/*
 * Number of bytes between base and pnt.  Callers must only use this
 * after _dmalloc_chunk_user_area() has shown that pnt is at or after
 * base.
 */
static	DMALLOC_SIZE	strict_offset(const void *base, const void *pnt)
{
  return (DMALLOC_SIZE)((PNT_ARITH_TYPE)pnt - (PNT_ARITH_TYPE)base);
}

/*
 * Return 1 if the two byte ranges overlap else 0.  Both lengths have
 * already been checked against their user areas so the ends cannot
 * wrap around the address type.
 */
static	int	strict_overlap(const void *a, DMALLOC_SIZE a_len,
			       const void *b, DMALLOC_SIZE b_len)
{
  PNT_ARITH_TYPE	a_start = (PNT_ARITH_TYPE)a;
  PNT_ARITH_TYPE	b_start = (PNT_ARITH_TYPE)b;
  PNT_ARITH_TYPE	a_end = a_start + (PNT_ARITH_TYPE)a_len;
  PNT_ARITH_TYPE	b_end = b_start + (PNT_ARITH_TYPE)b_len;

  return (a_start < b_end && b_start < a_end);
}

/*
 * Scan a live user area for a terminator, reading at most max_len
 * bytes.  On success read_len is set to the number of bytes the
 * caller needs: through the terminator when one was found, otherwise
 * max_len.  Returns 0 if the area does not cover max_len bytes without
 * a terminator.  Every byte read lies inside the tracked user area.
 */
static	int	strict_scan(const char *str, DMALLOC_SIZE max_len,
			    const void *base, PNT_ARITH_TYPE size,
			    DMALLOC_SIZE *read_len_p, int *have_null_p)
{
  PNT_ARITH_TYPE	avail, scan_max;
  DMALLOC_SIZE		i;

  avail = size - (PNT_ARITH_TYPE)strict_offset(base, str);
  scan_max = ((PNT_ARITH_TYPE)max_len < avail)
    ? (PNT_ARITH_TYPE)max_len : avail;

  for (i = 0; i < scan_max; i++) {
    if (str[i] == '\0') {
      *read_len_p = i + 1;
      *have_null_p = 1;
      return 1;
    }
  }

  *have_null_p = 0;
  *read_len_p = scan_max;

  /* not enough readable bytes and no terminator: reject */
  return (avail >= (PNT_ARITH_TYPE)max_len);
}

/*
 * Find a terminator inside a live user area starting at str.  Returns
 * 1 and the offset of the terminator, or 0 when no terminator exists
 * in the area.  Every byte read lies inside the tracked user area.
 */
static	int	strict_find_null(const char *str, const void *base,
				 PNT_ARITH_TYPE size, PNT_ARITH_TYPE *offset_p)
{
  PNT_ARITH_TYPE	avail;
  DMALLOC_SIZE		i;

  avail = size - (PNT_ARITH_TYPE)strict_offset(base, str);
  for (i = 0; (PNT_ARITH_TYPE)i < avail; i++) {
    if (str[i] == '\0') {
      *offset_p = i;
      return 1;
    }
  }

  return 0;
}

/*
 * Reject a strict-mode operation and set the distinguishable error.
 */
static	void	strict_reject(const char *file, const int line,
			     const char *func, const void *pnt,
			     const int error, const char *reason)
{
  _dmalloc_chunk_func_error(file, line, func, pnt, error, reason);
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
  if (_dmalloc_func_check_policy == DMALLOC_FUNC_REJECT) {
    /*
     * A zero length copy never touches either pointer, so do not even
     * resolve them.
     */
    if (len == 0) {
      return to;
    }

    /* both complete ranges must fit inside their user areas */
    if ((! _dmalloc_chunk_user_area(file, line, "memcpy", to, len,
				    NULL, NULL))
	|| (! _dmalloc_chunk_user_area(file, line, "memcpy", from, len,
				       NULL, NULL))) {
      return NULL;
    }

    /* memcpy may not be used on overlapping ranges, unlike memmove */
    if (strict_overlap(to, len, from, len)) {
      strict_reject(file, line, "memcpy", to,
		    DMALLOC_ERROR_FUNC_OVERLAP,
		    "source and destination ranges overlap");
      return NULL;
    }

    return (void *)memcpy(to, from, len);
  }

  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
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
  if (_dmalloc_func_check_policy == DMALLOC_FUNC_REJECT) {
    /* a zero length move never touches either pointer */
    if (len == 0) {
      return to;
    }

    /*
     * Both complete ranges must fit.  Overlapping ranges are allowed:
     * that is the whole point of memmove.
     */
    if ((! _dmalloc_chunk_user_area(file, line, "memmove", to, len,
				    NULL, NULL))
	|| (! _dmalloc_chunk_user_area(file, line, "memmove", from, len,
				       NULL, NULL))) {
      return NULL;
    }

    return (void *)memmove(to, from, len);
  }

  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
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
  if (_dmalloc_func_check_policy == DMALLOC_FUNC_REJECT) {
    const void	*to_base, *from_base;
    PNT_ARITH_TYPE to_size, from_size, to_avail, dst_len;
    DMALLOC_SIZE read_len, append_len, remaining;
    int		have_null;

    if (! _dmalloc_chunk_user_area(file, line, "strncat", to, 0,
				   &to_base, &to_size)) {
      return NULL;
    }
    to_avail = to_size - (PNT_ARITH_TYPE)((PNT_ARITH_TYPE)to
					  - (PNT_ARITH_TYPE)to_base);

    /* the destination must already be terminated inside its area */
    if (! strict_find_null(to, to_base, to_size, &dst_len)) {
      strict_reject(file, line, "strncat", to, DMALLOC_ERROR_FUNC_NO_NULL,
		    "destination string is not null terminated");
      return NULL;
    }

    /* no source lookup and no source terminator check when len is 0 */
    if (len == 0) {
      return to;
    }

    if (! _dmalloc_chunk_user_area(file, line, "strncat", from, 0,
				   &from_base, &from_size)) {
      return NULL;
    }

    /* read at most len non-null source characters */
    if (! strict_scan(from, len, from_base, from_size,
		      &read_len, &have_null)) {
      strict_reject(file, line, "strncat", from,
		    DMALLOC_ERROR_FUNC_OUT_OF_BOUNDS,
		    "source range exceeds the user allocation");
      return NULL;
    }

    /*
     * The source copy stops at a terminator when one was seen, so the
     * appended length excludes that byte.  Otherwise it is the full
     * len characters.
     */
    append_len = have_null ? (read_len - 1) : len;

    /* the destination needs the append bytes plus the final \0 */
    if (dst_len >= to_avail) {
      strict_reject(file, line, "strncat", to,
		    DMALLOC_ERROR_FUNC_OUT_OF_BOUNDS,
		    "destination range exceeds the user allocation");
      return NULL;
    }
    remaining = to_avail - dst_len;
    if (append_len > remaining - 1) {
      strict_reject(file, line, "strncat", to,
		    DMALLOC_ERROR_FUNC_OUT_OF_BOUNDS,
		    "destination range exceeds the user allocation");
      return NULL;
    }

    /* the actual source read and destination write must not overlap */
    if (strict_overlap(from, read_len, (const char *)to + dst_len,
		       append_len + 1)) {
      strict_reject(file, line, "strncat", to,
		    DMALLOC_ERROR_FUNC_OVERLAP,
		    "source and destination ranges overlap");
      return NULL;
    }

    return (char *)strncat(to, from, len);
  }

  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
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
  if (_dmalloc_func_check_policy == DMALLOC_FUNC_REJECT) {
    const void	*to_base, *from_base;
    PNT_ARITH_TYPE to_size, from_size;
    DMALLOC_SIZE read_len;
    int		have_null;

    /* a zero length copy never reads the source */
    if (len == 0) {
      return to;
    }

    /*
     * strncpy always writes len bytes into the destination, including
     * any zero padding, so the whole destination range is required.
     */
    if (! _dmalloc_chunk_user_area(file, line, "strncpy", to, len,
				   &to_base, &to_size)) {
      return NULL;
    }

    if (! _dmalloc_chunk_user_area(file, line, "strncpy", from, 0,
				   &from_base, &from_size)) {
      return NULL;
    }

    /* the source only needs to cover through its terminator or len */
    if (! strict_scan(from, len, from_base, from_size,
		      &read_len, &have_null)) {
      strict_reject(file, line, "strncpy", from,
		    DMALLOC_ERROR_FUNC_OUT_OF_BOUNDS,
		    "source range exceeds the user allocation");
      return NULL;
    }

    /* the bytes actually read must not overlap the bytes written */
    if (strict_overlap(from, read_len, to, len)) {
      strict_reject(file, line, "strncpy", to,
		    DMALLOC_ERROR_FUNC_OVERLAP,
		    "source and destination ranges overlap");
      return NULL;
    }

    return (char *)strncpy(to, from, len);
  }

  if (BIT_IS_SET(_dmalloc_flags, DMALLOC_DEBUG_CHECK_FUNCS)) {
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
