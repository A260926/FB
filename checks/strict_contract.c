/*
 * Contract tests for the strict function checking mode.
 *
 * The four wrappers (memcpy, memmove, strncpy, strncat) are exercised
 * both through their direct underscore entry points and through the
 * public DMALLOC_FUNC_CHECK macros.  A normal group covers internal
 * pointers, overlapping memmove, zero padding from a short source, and
 * truncation of an unterminated source.  A failure group mixes
 * undersized destinations, undersized sources, unterminated
 * destinations, overlapping ranges, unknown objects, and lengths over
 * INT_MAX.
 *
 * Every failure is checked for the correct dmalloc_errno category and
 * for the absence of any partial copy: the destination user area, the
 * source user area, and the picket fences are compared before and
 * after the call.  Unknown sources are also probed from a forked child
 * using a PROT_NONE page so that an implementation which scans first
 * and translates the fault afterwards would crash the child.
 */

#if defined(__APPLE__)
# define _DARWIN_C_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
# define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>

/* have the public header install its function macros for this file */
#define DMALLOC_FUNC_CHECK

#include "conf.h"
#include "dmalloc.h"
#include "chunk.h"
#include "debug_tok.h"
#include "error_val.h"

#define MAX_BUF		256

static int		failures = 0;
static unsigned char	snap_dst[MAX_BUF], snap_src[MAX_BUF];

/*
 * Report a failed assertion.
 */
static	void	fail(const char *what)
{
  (void)printf("STRICT-CONTRACT: FAIL: %s\n", what);
  failures++;
}

/*
 * Assert that a condition holds.
 */
static	void	check(const int cond, const char *what)
{
  if (! cond) {
    fail(what);
  }
}

/*
 * Fill a buffer with a known byte, without using any checked function.
 */
static	void	fill(void *pnt, const DMALLOC_SIZE size, const unsigned char ch)
{
  unsigned char	*p = (unsigned char *)pnt;
  DMALLOC_SIZE	i;

  for (i = 0; i < size; i++) {
    p[i] = ch;
  }
}

/*
 * Copy a snapshot of a buffer, without using any checked function.
 */
static	void	record(const void *pnt, const DMALLOC_SIZE size,
		       unsigned char *snap)
{
  const unsigned char	*p = (const unsigned char *)pnt;
  DMALLOC_SIZE		i;

  for (i = 0; i < size; i++) {
    snap[i] = p[i];
  }
}

/*
 * Return 1 if the buffer still matches the snapshot.
 */
static	int	same(const void *pnt, const DMALLOC_SIZE size,
		     const unsigned char *snap)
{
  const unsigned char	*p = (const unsigned char *)pnt;
  DMALLOC_SIZE		i;

  for (i = 0; i < size; i++) {
    if (p[i] != snap[i]) {
      return 0;
    }
  }

  return 1;
}

/*
 * Return 1 if the buffer's bytes match the expected bytes.
 */
static	int	match(const void *pnt, const DMALLOC_SIZE size,
		      const char *expected, const DMALLOC_SIZE expected_size)
{
  const unsigned char	*p = (const unsigned char *)pnt;
  DMALLOC_SIZE		i;

  if (size != expected_size) {
    return 0;
  }
  for (i = 0; i < size; i++) {
    if (p[i] != (unsigned char)expected[i]) {
      return 0;
    }
  }

  return 1;
}

/*
 * Return 1 if the fences and administrative record of a live pointer
 * are intact.
 */
static	int	fence_ok(const void *pnt)
{
  return dmalloc_verify((DMALLOC_PNT)pnt) == DMALLOC_VERIFY_NOERROR;
}

/*
 * Return the requested user size of a live pointer or 0 on failure.
 */
static	unsigned int	recorded_size(const void *pnt)
{
  unsigned int	size = 0;

  if (! _dmalloc_chunk_read_info(pnt, "strict-contract", &size, NULL, NULL,
				 NULL, NULL, NULL, NULL, NULL, NULL)) {
    return 0;
  }

  return size;
}

/*
 * Allocate a tracked block using the library directly.
 */
static	char	*alloc_block(const DMALLOC_SIZE size)
{
  return dmalloc_malloc(__FILE__, __LINE__, size, DMALLOC_FUNC_MALLOC,
			0, 0);
}

/*
 * Open and close a page which cannot be read.
 */
static	void	*unreadable_page(void)
{
#ifdef MAP_ANONYMOUS
  int	flags = MAP_ANONYMOUS | MAP_PRIVATE;
#else
  int	flags = MAP_ANON | MAP_PRIVATE;
#endif
  void	*page;

  page = mmap(NULL, 4096, PROT_NONE, flags, -1, 0);
  if (page == MAP_FAILED) {
    return NULL;
  }

  return page;
}

/*
 * Confirm from a child process that an untracked source pointer is
 * rejected without ever being read.
 *
 * mode 0 -> reject a PROT_NONE source page,
 * mode 1 -> a zero length copy must return the destination without
 *           touching either PROT_NONE pointer.
 */
static	void	child_unreadable(const void *dst, const void *page, const int mode)
{
  pid_t	pid;
  int	status;

  pid = fork();
  if (pid < 0) {
    fail("fork failed for the unreadable source test");
    return;
  }

  if (pid == 0) {
    void	*ret;
    int		err;

    dmalloc_errno = DMALLOC_ERROR_NONE;
    if (mode == 0) {
      ret = _dmalloc_memcpy(__FILE__, __LINE__, (void *)dst, page, 4);
    }
    else {
      ret = _dmalloc_memcpy(__FILE__, __LINE__, (void *)page, page, 0);
    }
    err = dmalloc_errno;

    if (mode == 0) {
      /* rejected as an unknown object, and nothing was read */
      _exit(! (ret == NULL && err == DMALLOC_ERROR_FUNC_BAD_OBJECT));
    }
    else {
      /* returned the destination untouched, no lookup of either pointer */
      _exit(! (ret == (void *)page && err == DMALLOC_ERROR_NONE));
    }
  }

  if (waitpid(pid, &status, 0) < 0) {
    fail("waitpid failed for the unreadable source test");
    return;
  }

  if (! WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    if (WIFSIGNALED(status)) {
      fail("unreadable source test crashed with a signal");
    }
    else {
      fail("unreadable source test rejected the wrong way");
    }
  }
}

/*
 * Failures which only involve one live object and one bad pointer.
 * The live object and its fence must be untouched.
 */
static	void	check_bad_object(const char *what, void *dst,
				 const void *bad_from, const DMALLOC_SIZE len)
{
  void		*ret;
  int		err;

  fill(dst, 8, 'D');
  record(dst, 8, snap_dst);
  dmalloc_errno = DMALLOC_ERROR_NONE;

  ret = _dmalloc_memcpy(__FILE__, __LINE__, dst, bad_from, len);
  err = dmalloc_errno;

  check(ret == NULL && err == DMALLOC_ERROR_FUNC_BAD_OBJECT, what);
  check(same(dst, 8, snap_dst) && fence_ok(dst), what);
  check(recorded_size(dst) == 8, what);
}

/*
 * Exercise the normal group.  Returns 1 on success.
 */
static	int	run_normal_group(void)
{
  char		*dst, *src, *buf;
  void		*ret;
  int		err;

  /* memcpy with internal pointers on both sides */
  dst = alloc_block(32);
  src = alloc_block(32);
  if (dst == NULL || src == NULL) {
    return 0;
  }
  fill(dst, 32, 'D');
  fill(src, 32, 'Q');

  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_memcpy(__FILE__, __LINE__, dst + 4, src + 6, 8);
  err = dmalloc_errno;
  check(ret == dst + 4 && err == DMALLOC_ERROR_NONE,
	"memcpy internal pointers");
  check(match(dst + 4, 8, "QQQQQQQQ", 8), "memcpy internal result");
  check(fence_ok(dst) && fence_ok(src), "memcpy internal fences");

  /* memmove of an overlapping range must move correctly */
  buf = alloc_block(16);
  if (buf == NULL) {
    return 0;
  }
  fill(buf, 16, 'D');
  buf[0] = 'a';
  buf[1] = 'b';
  buf[2] = 'c';
  dmalloc_errno = DMALLOC_ERROR_NONE;
  (void)_dmalloc_memmove(__FILE__, __LINE__, buf + 1, buf, 4);
  err = dmalloc_errno;
  check(err == DMALLOC_ERROR_NONE, "memmove overlap error");
  check(match(buf + 1, 4, "abcD", 4), "memmove overlap result");
  check(fence_ok(buf), "memmove overlap fence");

  /* strncpy from a short source must zero-fill the whole length */
  dst = alloc_block(16);
  src = alloc_block(4);
  if (dst == NULL || src == NULL) {
    return 0;
  }
  fill(dst, 16, 'D');
  src[0] = 'a';
  src[1] = 'b';
  src[2] = '\0';
  src[3] = 'X';
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_strncpy(__FILE__, __LINE__, dst, src, 6);
  err = dmalloc_errno;
  check(ret == dst && err == DMALLOC_ERROR_NONE, "strncpy short source");
  check(match(dst, 6, "ab\0\0\0\0", 6), "strncpy short source pad");
  check(fence_ok(dst) && fence_ok(src), "strncpy short source fence");

  /* strncpy of an unterminated source of exactly len bytes succeeds */
  dst = alloc_block(16);
  src = alloc_block(4);
  if (dst == NULL || src == NULL) {
    return 0;
  }
  fill(dst, 16, 'D');
  src[0] = 'w';
  src[1] = 'x';
  src[2] = 'y';
  src[3] = 'z';
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_strncpy(__FILE__, __LINE__, dst, src, 4);
  err = dmalloc_errno;
  check(ret == dst && err == DMALLOC_ERROR_NONE,
	"strncpy unterminated source");
  check(match(dst, 4, "wxyz", 4), "strncpy unterminated result");

  /* strncat appends a string and always writes a terminator */
  dst = alloc_block(16);
  src = alloc_block(4);
  if (dst == NULL || src == NULL) {
    return 0;
  }
  fill(dst, 16, 'D');
  dst[0] = 'a';
  dst[1] = 'b';
  dst[2] = '\0';
  src[0] = 'c';
  src[1] = 'd';
  src[2] = '\0';
  src[3] = 'X';
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_strncat(__FILE__, __LINE__, dst, src, 4);
  err = dmalloc_errno;
  check(ret == dst && err == DMALLOC_ERROR_NONE, "strncat basic");
  check(match(dst, 5, "abcd\0", 5), "strncat basic result");
  check(fence_ok(dst) && fence_ok(src), "strncat basic fence");

  /* strncat truncates an unterminated source without padding */
  dst = alloc_block(16);
  src = alloc_block(4);
  if (dst == NULL || src == NULL) {
    return 0;
  }
  fill(dst, 16, 'D');
  dst[0] = 'a';
  dst[1] = 'b';
  dst[2] = '\0';
  src[0] = 'w';
  src[1] = 'x';
  src[2] = 'y';
  src[3] = 'z';
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_strncat(__FILE__, __LINE__, dst, src, 2);
  err = dmalloc_errno;
  check(ret == dst && err == DMALLOC_ERROR_NONE,
	"strncat unterminated source");
  check(match(dst, 5, "abwx\0", 5), "strncat unterminated result");

  /* strncat with len 0 only validates the destination terminator */
  dst = alloc_block(8);
  if (dst == NULL) {
    return 0;
  }
  dst[0] = 'a';
  dst[1] = '\0';
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_strncat(__FILE__, __LINE__, dst, NULL, 0);
  err = dmalloc_errno;
  check(ret == dst && err == DMALLOC_ERROR_NONE, "strncat len 0");

  /* all four wrappers accept an internal pointer as a destination */
  dst = alloc_block(16);
  src = alloc_block(16);
  if (dst == NULL || src == NULL) {
    return 0;
  }
  fill(dst, 16, 'D');
  fill(src, 16, 'Q');
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_memmove(__FILE__, __LINE__, dst + 8, src + 8, 4);
  check(ret == dst + 8 && dmalloc_errno == DMALLOC_ERROR_NONE,
	"memmove internal pointers");

  /* strncpy and strncat likewise accept internal source pointers */
  fill(src, 16, 'Q');
  src[7] = 'a';
  src[8] = 'b';
  src[9] = '\0';
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_strncpy(__FILE__, __LINE__, dst + 4, src + 7, 3);
  check(ret == dst + 4 && dmalloc_errno == DMALLOC_ERROR_NONE,
	"strncpy internal source");
  check(match(dst + 4, 3, "ab\0", 3), "strncpy internal source result");

  fill(dst, 16, 'D');
  dst[0] = 'x';
  dst[1] = '\0';
  dst[2] = '\0';
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_strncat(__FILE__, __LINE__, dst + 2, src + 7, 3);
  check(ret == dst + 2 && dmalloc_errno == DMALLOC_ERROR_NONE,
	"strncat internal pointers");
  check(match(dst + 2, 3, "ab\0", 3), "strncat internal pointers result");

  dmalloc_free(__FILE__, __LINE__, dst, DMALLOC_FUNC_FREE);
  dmalloc_free(__FILE__, __LINE__, src, DMALLOC_FUNC_FREE);

  return 1;
}

/*
 * Exercise the failure group.  Returns 1 on success.
 */
static	int	run_failure_group(void)
{
  char		*dst, *src, *bad_dst;
  void		*ret;
  int		err;
  DMALLOC_SIZE	huge = (DMALLOC_SIZE)INT_MAX + 64;

  /* memcpy: destination smaller than len */
  dst = alloc_block(8);
  src = alloc_block(16);
  if (dst == NULL || src == NULL) {
    return 0;
  }
  fill(dst, 8, 'D');
  fill(src, 16, 'Q');
  record(dst, 8, snap_dst);
  record(src, 16, snap_src);
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_memcpy(__FILE__, __LINE__, dst, src, 12);
  err = dmalloc_errno;
  check(ret == NULL && err == DMALLOC_ERROR_FUNC_OUT_OF_BOUNDS,
	"memcpy destination too small");
  check(same(dst, 8, snap_dst) && same(src, 16, snap_src)
	&& fence_ok(dst) && fence_ok(src)
	&& recorded_size(dst) == 8 && recorded_size(src) == 16,
	"memcpy destination too small untouched");

  /* memcpy: source smaller than len */
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_memcpy(__FILE__, __LINE__, src, dst, 12);
  err = dmalloc_errno;
  check(ret == NULL && err == DMALLOC_ERROR_FUNC_OUT_OF_BOUNDS,
	"memcpy source too small");
  check(same(dst, 8, snap_dst) && same(src, 16, snap_src)
	&& fence_ok(dst) && fence_ok(src),
	"memcpy source too small untouched");

  /* memcpy: overlapping ranges are refused */
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_memcpy(__FILE__, __LINE__, src + 2, src, 8);
  err = dmalloc_errno;
  check(ret == NULL && err == DMALLOC_ERROR_FUNC_OVERLAP,
	"memcpy overlap");
  check(same(src, 16, snap_src) && fence_ok(src), "memcpy overlap untouched");

  /* memcpy: length beyond INT_MAX must not be truncated */
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_memcpy(__FILE__, __LINE__, dst, src, huge);
  err = dmalloc_errno;
  check(ret == NULL && err == DMALLOC_ERROR_FUNC_OUT_OF_BOUNDS,
	"memcpy length over INT_MAX");
  check(same(dst, 8, snap_dst) && same(src, 16, snap_src),
	"memcpy over INT_MAX untouched");

  /* memcpy: a one-past-end pointer has no room for a nonzero range */
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_memcpy(__FILE__, __LINE__, dst + 8, src, 4);
  err = dmalloc_errno;
  check(ret == NULL && err == DMALLOC_ERROR_FUNC_OUT_OF_BOUNDS,
	"memcpy one past user area");
  check(same(dst, 8, snap_dst) && same(src, 16, snap_src),
	"memcpy one past user area untouched");

  /* memcpy: a pointer into the rounded-out space past the user area */
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_memcpy(__FILE__, __LINE__, dst + 9, src, 4);
  err = dmalloc_errno;
  check(ret == NULL && err == DMALLOC_ERROR_FUNC_BAD_OBJECT,
	"memcpy into top fence");
  check(same(dst, 8, snap_dst) && same(src, 16, snap_src),
	"memcpy into top fence untouched");

  /* memcpy: a pointer into the bottom fence is not user capacity */
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_memcpy(__FILE__, __LINE__, dst - 1, src, 4);
  err = dmalloc_errno;
  check(ret == NULL && err == DMALLOC_ERROR_FUNC_BAD_OBJECT,
	"memcpy into bottom fence");
  check(same(dst, 8, snap_dst) && same(src, 16, snap_src),
	"memcpy into bottom fence untouched");

  /* memcpy: a source pointer past its requested size is not capacity */
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_memcpy(__FILE__, __LINE__, dst, src + 14, 4);
  err = dmalloc_errno;
  check(ret == NULL
	&& (err == DMALLOC_ERROR_FUNC_BAD_OBJECT
	    || err == DMALLOC_ERROR_FUNC_OUT_OF_BOUNDS),
	"memcpy source past user area");
  check(same(dst, 8, snap_dst) && same(src, 16, snap_src),
	"memcpy source past user area untouched");

  /*
   * A large allocation has rounded-out block space past the requested
   * size.  That space is not capacity: writing just past the requested
   * bytes must be rejected.
   */
  {
    char	*big = alloc_block(100);

    if (big == NULL) {
      return 0;
    }
    fill(big, 100, 'B');
    record(big, 100, snap_dst);
    dmalloc_errno = DMALLOC_ERROR_NONE;
    ret = _dmalloc_memcpy(__FILE__, __LINE__, big, big, 101);
    err = dmalloc_errno;
    check(ret == NULL && err == DMALLOC_ERROR_FUNC_OUT_OF_BOUNDS,
	  "memcpy over requested size of large block");
    check(same(big, 100, snap_dst) && fence_ok(big),
	  "memcpy over requested size of large block untouched");
    dmalloc_free(__FILE__, __LINE__, big, DMALLOC_FUNC_FREE);
  }

  /* unknown, freed, and stack pointers are rejected without access */
  bad_dst = alloc_block(8);
  if (bad_dst == NULL) {
    return 0;
  }
  fill(bad_dst, 8, 'Z');
  dmalloc_free(__FILE__, __LINE__, bad_dst, DMALLOC_FUNC_FREE);
  check_bad_object("memcpy freed source", dst, bad_dst, 4);
  check_bad_object("memcpy stack source", dst, snap_src, 4);
  check_bad_object("memcpy null source", dst, NULL, 4);

  /* memmove: both complete ranges are checked, overlap is allowed */
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_memmove(__FILE__, __LINE__, dst, src, 12);
  err = dmalloc_errno;
  check(ret == NULL && err == DMALLOC_ERROR_FUNC_OUT_OF_BOUNDS,
	"memmove destination too small");
  check(same(dst, 8, snap_dst) && same(src, 16, snap_src),
	"memmove destination too small untouched");

  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_memmove(__FILE__, __LINE__, src, dst, 12);
  err = dmalloc_errno;
  check(ret == NULL && err == DMALLOC_ERROR_FUNC_OUT_OF_BOUNDS,
	"memmove source too small");
  check(same(dst, 8, snap_dst) && same(src, 16, snap_src),
	"memmove source too small untouched");

  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_memmove(__FILE__, __LINE__, src, src, huge);
  err = dmalloc_errno;
  check(ret == NULL && err == DMALLOC_ERROR_FUNC_OUT_OF_BOUNDS,
	"memmove length over INT_MAX");
  check(same(src, 16, snap_src), "memmove over INT_MAX untouched");

  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_memmove(__FILE__, __LINE__, NULL, src, 4);
  err = dmalloc_errno;
  check(ret == NULL && err == DMALLOC_ERROR_FUNC_BAD_OBJECT,
	"memmove null destination");
  check(same(src, 16, snap_src), "memmove null destination untouched");

  /* strncpy: destination must hold the whole len, including padding */
  dst = alloc_block(4);
  src = alloc_block(8);
  if (dst == NULL || src == NULL) {
    return 0;
  }
  fill(dst, 4, 'D');
  fill(src, 8, 'Q');
  record(dst, 4, snap_dst);
  record(src, 8, snap_src);

  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_strncpy(__FILE__, __LINE__, dst, src, 6);
  err = dmalloc_errno;
  check(ret == NULL && err == DMALLOC_ERROR_FUNC_OUT_OF_BOUNDS,
	"strncpy destination too small");
  check(same(dst, 4, snap_dst) && same(src, 8, snap_src)
	&& fence_ok(dst) && fence_ok(src), "strncpy destination untouched");

  /* strncpy: source with no NUL and not enough bytes */
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_strncpy(__FILE__, __LINE__, src, dst, 6);
  err = dmalloc_errno;
  check(ret == NULL && err == DMALLOC_ERROR_FUNC_OUT_OF_BOUNDS,
	"strncpy source too small");
  check(same(src, 8, snap_src) && same(dst, 4, snap_dst),
	"strncpy source untouched");

  /* strncpy: the read range and the write range must not overlap */
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_strncpy(__FILE__, __LINE__, src + 2, src, 4);
  err = dmalloc_errno;
  check(ret == NULL && err == DMALLOC_ERROR_FUNC_OVERLAP,
	"strncpy overlap");
  check(same(src, 8, snap_src) && fence_ok(src), "strncpy overlap untouched");

  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_strncpy(__FILE__, __LINE__, dst, src, huge);
  err = dmalloc_errno;
  check(ret == NULL && err == DMALLOC_ERROR_FUNC_OUT_OF_BOUNDS,
	"strncpy length over INT_MAX");
  check(same(dst, 4, snap_dst) && same(src, 8, snap_src),
	"strncpy over INT_MAX untouched");

  check_bad_object("strncpy null source", src, NULL, 4);

  /* strncat: destination must have room for the append and the \0 */
  dst = alloc_block(4);
  src = alloc_block(16);
  if (dst == NULL || src == NULL) {
    return 0;
  }
  fill(dst, 4, 'D');
  dst[0] = 'a';
  dst[1] = '\0';
  fill(src, 16, 'Q');
  src[16 - 1] = '\0';
  record(dst, 4, snap_dst);
  record(src, 16, snap_src);

  /* 4 bytes of source after "a" plus the \0 do not fit into 4 */
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_strncat(__FILE__, __LINE__, dst, src, 6);
  err = dmalloc_errno;
  check(ret == NULL && err == DMALLOC_ERROR_FUNC_OUT_OF_BOUNDS,
	"strncat destination too small");
  check(same(dst, 4, snap_dst) && same(src, 16, snap_src)
	&& fence_ok(dst) && fence_ok(src), "strncat destination untouched");

  /* source with no terminator and fewer than len readable bytes */
  dst = alloc_block(16);
  src = alloc_block(3);
  if (dst == NULL || src == NULL) {
    return 0;
  }
  fill(dst, 16, 'D');
  dst[0] = 'a';
  dst[1] = '\0';
  fill(src, 3, 'Q');
  record(dst, 16, snap_dst);
  record(src, 3, snap_src);
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_strncat(__FILE__, __LINE__, dst, src, 5);
  err = dmalloc_errno;
  check(ret == NULL && err == DMALLOC_ERROR_FUNC_OUT_OF_BOUNDS,
	"strncat source too small");
  check(same(dst, 16, snap_dst) && same(src, 3, snap_src)
	&& fence_ok(dst) && fence_ok(src), "strncat source untouched");

  /* unterminated destination is refused, even with len 0 */
  fill(dst, 16, 'D');
  record(dst, 16, snap_dst);
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_strncat(__FILE__, __LINE__, dst, src, 4);
  err = dmalloc_errno;
  check(ret == NULL && err == DMALLOC_ERROR_FUNC_NO_NULL,
	"strncat unterminated destination");
  check(same(dst, 16, snap_dst) && fence_ok(dst),
	"strncat unterminated destination untouched");

  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_strncat(__FILE__, __LINE__, dst, NULL, 0);
  err = dmalloc_errno;
  check(ret == NULL && err == DMALLOC_ERROR_FUNC_NO_NULL,
	"strncat len 0 unterminated destination");

  /* source overlapping the destination write range */
  fill(dst, 16, 'D');
  dst[0] = 'a';
  dst[1] = 'b';
  dst[2] = '\0';
  dst[4] = 'x';
  dst[5] = 'y';
  dst[6] = 'z';
  dst[7] = '\0';
  record(dst, 16, snap_dst);
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_strncat(__FILE__, __LINE__, dst, dst + 4, 3);
  err = dmalloc_errno;
  check(ret == NULL && err == DMALLOC_ERROR_FUNC_OVERLAP,
	"strncat overlap");
  check(same(dst, 16, snap_dst) && fence_ok(dst), "strncat overlap untouched");

  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_strncat(__FILE__, __LINE__, dst, dst + 4, huge);
  err = dmalloc_errno;
  check(ret == NULL
	&& (err == DMALLOC_ERROR_FUNC_OUT_OF_BOUNDS
	    || err == DMALLOC_ERROR_FUNC_OVERLAP),
	"strncat length over INT_MAX");
  check(same(dst, 16, snap_dst), "strncat over INT_MAX untouched");

  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_strncat(__FILE__, __LINE__, dst, NULL, 4);
  err = dmalloc_errno;
  check(ret == NULL && err == DMALLOC_ERROR_FUNC_BAD_OBJECT,
	"strncat null source");
  check(same(dst, 16, snap_dst) && fence_ok(dst),
	"strncat null source untouched");

  /* zero length calls never resolve anything they do not need */
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_strncpy(__FILE__, __LINE__, NULL, NULL, 0);
  check(ret == NULL && dmalloc_errno == DMALLOC_ERROR_NONE,
	"strncpy len 0 returns destination");
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_memmove(__FILE__, __LINE__, NULL, NULL, 0);
  check(ret == NULL && dmalloc_errno == DMALLOC_ERROR_NONE,
	"memmove len 0 returns destination");
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_memcpy(__FILE__, __LINE__, NULL, NULL, 0);
  check(ret == NULL && dmalloc_errno == DMALLOC_ERROR_NONE,
	"memcpy len 0 returns destination");

  return 1;
}

/*
 * Exercise the public macros from dmalloc.h.
 */
static	int	run_macro_group(void)
{
  char		*dst, *src;
  char		*ret;

  dst = alloc_block(16);
  src = alloc_block(16);
  if (dst == NULL || src == NULL) {
    return 0;
  }
  fill(dst, 16, 'D');
  fill(src, 16, 'Q');

  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = memcpy(dst, src, 6);
  check(ret == dst && dmalloc_errno == DMALLOC_ERROR_NONE,
	"macro memcpy succeeds for tracked objects");
  check(match(dst, 6, "QQQQQQ", 6), "macro memcpy result");

  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = memmove(dst + 1, dst, 4);
  check(ret == dst + 1 && dmalloc_errno == DMALLOC_ERROR_NONE,
	"macro memmove overlapping succeeds");

  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = strncpy(dst, src, 6);
  check(ret == dst && dmalloc_errno == DMALLOC_ERROR_NONE,
	"macro strncpy succeeds");

  dst[0] = 'a';
  dst[1] = '\0';
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = strncat(dst, src, 6);
  check(ret == dst && dmalloc_errno == DMALLOC_ERROR_NONE,
	"macro strncat succeeds");

  /* and one rejection through the macros */
  record(dst, 16, snap_dst);
  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = memcpy(dst, src, 20);
  check(ret == NULL && dmalloc_errno == DMALLOC_ERROR_FUNC_OUT_OF_BOUNDS,
	"macro memcpy rejects an undersized destination");
  check(same(dst, 16, snap_dst) && fence_ok(dst) && fence_ok(src),
	"macro memcpy rejection untouched");

  return 1;
}

/*
 * Confirm that strict mode does not depend on the check-funcs token.
 */
static	int	run_check_funcs_independence(void)
{
  char		*dst, *src;
  void		*ret;

  dst = alloc_block(4);
  src = alloc_block(8);
  if (dst == NULL || src == NULL) {
    return 0;
  }
  fill(dst, 4, 'D');
  fill(src, 8, 'Q');
  record(dst, 4, snap_dst);

  /* enable check-funcs and repeat a rejection */
  (void)dmalloc_debug(DMALLOC_DEBUG_CHECK_FENCE | DMALLOC_DEBUG_CHECK_FUNCS);

  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_memcpy(__FILE__, __LINE__, dst, src, 6);
  check(ret == NULL && dmalloc_errno == DMALLOC_ERROR_FUNC_OUT_OF_BOUNDS,
	"strict rejection with check-funcs enabled");
  check(same(dst, 4, snap_dst) && fence_ok(dst) && fence_ok(src),
	"strict rejection with check-funcs untouched");

  return 1;
}

/*
 * Confirm that REPORT keeps the historical report-then-continue
 * behavior once strict mode is switched off.
 */
static	int	run_report_continuation(void)
{
  char		*dst, *src;
  void		*ret;
  int		err;

  dst = alloc_block(8);
  src = alloc_block(9);
  if (dst == NULL || src == NULL) {
    return 0;
  }
  fill(dst, 8, 'D');
  fill(src, 9, 'Q');

  dmalloc_errno = DMALLOC_ERROR_NONE;
  ret = _dmalloc_memcpy(__FILE__, __LINE__, dst, src, 9);
  err = dmalloc_errno;

  check(ret == dst && err == DMALLOC_ERROR_WOULD_OVERWRITE,
	"REPORT reports and still copies");
  check(dst[0] == 'Q', "REPORT copied the destination bytes");

  return 1;
}

int	main(void)
{
  int		old_policy, ret;
  char		*dst;
  void		*page;

  (void)dmalloc_debug(DMALLOC_DEBUG_CHECK_FENCE);

  /* policy API: default is REPORT, invalid leaves it unchanged */
  old_policy = dmalloc_set_func_check_policy(DMALLOC_FUNC_REPORT);
  check(old_policy == DMALLOC_FUNC_REPORT, "default policy is REPORT");

  ret = dmalloc_set_func_check_policy(99);
  check(ret == -1, "invalid policy returns -1");
  ret = dmalloc_set_func_check_policy(DMALLOC_FUNC_REPORT);
  check(ret == DMALLOC_FUNC_REPORT, "invalid policy leaves policy unchanged");

  old_policy = dmalloc_set_func_check_policy(DMALLOC_FUNC_REJECT);
  check(old_policy == DMALLOC_FUNC_REPORT, "setting REJECT returns previous");

  if (! run_normal_group()) {
    fail("could not allocate for the normal group");
  }
  if (! run_failure_group()) {
    fail("could not allocate for the failure group");
  }
  if (! run_macro_group()) {
    fail("could not allocate for the macro group");
  }

  /* unknown sources must never be read: probe from a child process */
  dst = alloc_block(8);
  page = unreadable_page();
  if (dst == NULL || page == NULL) {
    fail("could not prepare the unreadable source test");
  }
  else {
    fill(dst, 8, 'D');
    record(dst, 8, snap_dst);
    child_unreadable(dst, page, 0);
    check(same(dst, 8, snap_dst) && fence_ok(dst),
	  "unreadable source rejection untouched");

    /* zero length copies must not query their pointers at all */
    child_unreadable(dst, page, 1);
    (void)munmap(page, 4096);
  }

  /* strict mode still works with check-funcs enabled */
  if (! run_check_funcs_independence()) {
    fail("could not allocate for the check-funcs test");
  }

  /* switching back to REPORT must restore report-and-continue */
  old_policy = dmalloc_set_func_check_policy(DMALLOC_FUNC_REPORT);
  check(old_policy == DMALLOC_FUNC_REJECT, "switching to REPORT returns REJECT");
  (void)dmalloc_debug(DMALLOC_DEBUG_CHECK_FENCE | DMALLOC_DEBUG_CHECK_FUNCS);
  if (! run_report_continuation()) {
    fail("could not allocate for the REPORT continuation test");
  }

  if (failures == 0) {
    (void)printf("DMALLOC strict=contract passed policy=REJECT/REPORT\n");
    return 0;
  }

  (void)printf("DMALLOC strict=contract failures=%d\n", failures);
  return 1;
}
