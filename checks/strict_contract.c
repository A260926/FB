/*
 * checks/strict_contract.c
 *
 * Contract tests for the strict function-check policy
 * (dmalloc_set_func_check_policy / DMALLOC_FUNC_REJECT) applied to the
 * _dmalloc_memcpy, _dmalloc_memmove, _dmalloc_strncpy, and
 * _dmalloc_strncat wrappers and their public macro entry points.
 *
 * Strict mode only accepts pointers into active dmalloc-tracked heap
 * user allocations.  Every rejected call must leave the user space,
 * the fence-posts, and the allocation records untouched, and unknown
 * pointers must be rejected without being read.
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define DMALLOC_FUNC_CHECK
#include "dmalloc.h"

#include "conf.h"		/* for FENCE_BOTTOM_SIZE and FENCE_TOP_SIZE */
#include "debug_tok.h"
#include "error_val.h"

static	int	fail_c = 0;

#define CHECK(cond, msg) \
  do { \
    if (! (cond)) { \
      (void)printf("FAIL(%d): %s\n", __LINE__, msg); \
      fail_c++; \
    } \
  } while (0)

/* address which is never readable, used to prove no scanning happens */
#define BOGUS_PNT	((void *)0xdead0000)

/*
 * Fill a buffer with a deterministic non-zero pattern.
 */
static	void	fill_bytes(char *pnt, const size_t len, const unsigned int seed)
{
  size_t	i;

  for (i = 0; i < len; i++) {
    pnt[i] = (char)(seed + i);
  }
}

/*
 * Fill a buffer with a constant byte.
 */
static	void	fill_const(char *pnt, const size_t len, const char value)
{
  size_t	i;

  for (i = 0; i < len; i++) {
    pnt[i] = value;
  }
}

/*
 * Compare two ranges byte for byte.
 */
static	int	range_equal(const char *pnt1, const char *pnt2, const size_t len)
{
  size_t	i;

  for (i = 0; i < len; i++) {
    if (pnt1[i] != pnt2[i]) {
      return 0;
    }
  }
  return 1;
}

/*
 * Check that a range holds a constant byte.
 */
static	int	range_is(const char *pnt, const size_t len, const char value)
{
  size_t	i;

  for (i = 0; i < len; i++) {
    if (pnt[i] != value) {
      return 0;
    }
  }
  return 1;
}

#define SNAP_MAX_USER	64

/*
 * A record of everything a rejected call must not disturb: the user
 * space, the surrounding fence-posts, and the allocation record.
 */
typedef struct {
  unsigned char	fence_bottom[FENCE_BOTTOM_SIZE];
  unsigned char	user[SNAP_MAX_USER];
  unsigned char	fence_top[FENCE_TOP_SIZE];
  size_t	user_size;
  DMALLOC_SIZE	rec_user_size;
  DMALLOC_SIZE	rec_total_size;
  int		examine_ok;
} snapshot_t;

/*
 * Snapshot the user space, fence-posts, and allocation record of a
 * dmalloc pointer.  The pointer must be the start of the allocation
 * and user_size must be the requested size.
 */
static	void	snap_take(snapshot_t *snap, const char *pnt,
			  const size_t user_size)
{
  size_t	i;

  snap->user_size = user_size;
  for (i = 0; i < FENCE_BOTTOM_SIZE; i++) {
    snap->fence_bottom[i] = (unsigned char)(pnt - FENCE_BOTTOM_SIZE)[i];
  }
  for (i = 0; i < user_size; i++) {
    snap->user[i] = (unsigned char)pnt[i];
  }
  for (i = 0; i < FENCE_TOP_SIZE; i++) {
    snap->fence_top[i] = (unsigned char)(pnt + user_size)[i];
  }
  /* dmalloc_examine also validates the fence-posts of the pointer */
  snap->examine_ok = dmalloc_examine((DMALLOC_PNT)pnt, &snap->rec_user_size,
				     &snap->rec_total_size, NULL, NULL,
				     NULL, NULL, NULL);
}

/*
 * Compare two snapshots byte for byte.
 */
static	int	snap_same(const snapshot_t *snap1, const snapshot_t *snap2)
{
  size_t	i;

  if (snap1->user_size != snap2->user_size
      || snap1->rec_user_size != snap2->rec_user_size
      || snap1->rec_total_size != snap2->rec_total_size
      || snap1->examine_ok != snap2->examine_ok) {
    return 0;
  }
  for (i = 0; i < FENCE_BOTTOM_SIZE; i++) {
    if (snap1->fence_bottom[i] != snap2->fence_bottom[i]) {
      return 0;
    }
  }
  for (i = 0; i < snap1->user_size; i++) {
    if (snap1->user[i] != snap2->user[i]) {
      return 0;
    }
  }
  for (i = 0; i < FENCE_TOP_SIZE; i++) {
    if (snap1->fence_top[i] != snap2->fence_top[i]) {
      return 0;
    }
  }
  return 1;
}

/*
 * Allocate a dmalloc-tracked buffer of the given user size.
 */
static	char	*alloc_user(const size_t size)
{
  char	*pnt;

  pnt = dmalloc_malloc(__FILE__, __LINE__, size, DMALLOC_FUNC_MALLOC, 0, 0);
  if (pnt == NULL) {
    (void)printf("FAIL(%d): out of dmalloc memory\n", __LINE__);
    exit(1);
  }
  return pnt;
}

int	main(void)
{
  int		prev_policy;

  /* fence-posts on for the snapshot checks, check-funcs for report mode */
  (void)dmalloc_debug(DMALLOC_DEBUG_CHECK_FUNCS | DMALLOC_DEBUG_CHECK_FENCE);

  /******************** policy setter contract ********************/

  prev_policy = dmalloc_set_func_check_policy(DMALLOC_FUNC_REJECT);
  CHECK(prev_policy == DMALLOC_FUNC_REPORT, "default policy is REPORT");
  CHECK(dmalloc_set_func_check_policy(999) == -1,
	"invalid policy value returns -1");
  prev_policy = dmalloc_set_func_check_policy(DMALLOC_FUNC_REJECT);
  CHECK(prev_policy == DMALLOC_FUNC_REJECT,
	"invalid policy value left the active policy unchanged");

  /******************** strict-mode normal group ********************/

  /* memcpy between interior pointers */
  {
    char	*d = alloc_user(32);
    char	*s = alloc_user(32);
    char	exp[32];
    void	*r;

    fill_bytes(s, 32, 1);
    fill_const(d, 32, 'D');
    fill_const(exp, 32, 'D');
    fill_bytes(exp + 4, 10, 1 + 6);

    dmalloc_errno = 0;
    r = _dmalloc_memcpy(__FILE__, __LINE__, d + 4, s + 6, 10);
    CHECK(r == d + 4, "memcpy interior returns destination");
    CHECK(dmalloc_errno == 0, "memcpy interior sets no error");
    CHECK(range_equal(d, exp, 32), "memcpy interior copies correct bytes");
  }

  /* memmove with overlapping ranges is still allowed */
  {
    char	*b = alloc_user(32);
    char	exp[32];
    void	*r;
    int		i;

    fill_bytes(b, 32, 0);
    fill_bytes(exp, 32, 0);
    for (i = 15; i >= 0; i--) {
      exp[i + 4] = exp[i];
    }

    dmalloc_errno = 0;
    r = _dmalloc_memmove(__FILE__, __LINE__, b + 4, b, 16);
    CHECK(r == b + 4, "memmove overlap returns destination");
    CHECK(dmalloc_errno == 0, "memmove overlap sets no error");
    CHECK(range_equal(b, exp, 32), "memmove overlap moves correct bytes");
  }

  /* strncpy with a short source zero-pads the destination */
  {
    char	*d = alloc_user(16);
    char	*s = alloc_user(8);
    char	exp[16];
    void	*r;

    fill_bytes(s, 8, 0x60);
    s[0] = 'h';
    s[1] = 'i';
    s[2] = '\0';
    fill_const(d, 16, 'X');
    fill_const(exp, 16, 'X');
    exp[0] = 'h';
    exp[1] = 'i';
    fill_const(exp + 2, 6, '\0');

    dmalloc_errno = 0;
    r = _dmalloc_strncpy(__FILE__, __LINE__, d, s, 8);
    CHECK(r == d, "strncpy padding returns destination");
    CHECK(dmalloc_errno == 0, "strncpy padding sets no error");
    CHECK(range_equal(d, exp, 16), "strncpy short source zero-pads");
  }

  /* strncpy truncating a source with no terminator */
  {
    char	*d = alloc_user(4);
    char	*s = alloc_user(8);
    void	*r;
    int		i;

    for (i = 0; i < 8; i++) {
      s[i] = (char)('a' + i);
    }
    fill_const(d, 4, 'X');

    dmalloc_errno = 0;
    r = _dmalloc_strncpy(__FILE__, __LINE__, d, s, 4);
    CHECK(r == d, "strncpy truncate returns destination");
    CHECK(dmalloc_errno == 0, "strncpy truncate sets no error");
    CHECK(range_equal(d, "abcd", 4),
	  "strncpy truncate copies without terminator");
  }

  /* strncat appending a short source */
  {
    char	*d = alloc_user(16);
    char	*s = alloc_user(8);
    char	exp[16];
    void	*r;

    fill_bytes(d, 16, 0x40);
    d[0] = 'a';
    d[1] = 'b';
    d[2] = '\0';
    fill_bytes(s, 8, 0x50);
    s[0] = 'X';
    s[1] = 'Y';
    s[2] = '\0';
    fill_bytes(exp, 16, 0x40);
    exp[0] = 'a';
    exp[1] = 'b';
    exp[2] = 'X';
    exp[3] = 'Y';
    exp[4] = '\0';

    dmalloc_errno = 0;
    r = _dmalloc_strncat(__FILE__, __LINE__, d, s, 4);
    CHECK(r == d, "strncat returns destination");
    CHECK(dmalloc_errno == 0, "strncat sets no error");
    CHECK(range_equal(d, exp, 16), "strncat appends and terminates");
  }

  /* strncat with len 0 verifies the destination but ignores the source */
  {
    char	*d = alloc_user(8);
    char	exp[8];
    void	*r;

    fill_bytes(d, 8, 0x30);
    d[0] = 'a';
    d[1] = 'b';
    d[2] = '\0';
    fill_bytes(exp, 8, 0x30);
    exp[0] = 'a';
    exp[1] = 'b';
    exp[2] = '\0';

    dmalloc_errno = 0;
    r = _dmalloc_strncat(__FILE__, __LINE__, d, (const char *)BOGUS_PNT, 0);
    CHECK(r == d, "strncat len 0 returns destination");
    CHECK(dmalloc_errno == 0, "strncat len 0 does not query the source");
    CHECK(range_equal(d, exp, 8), "strncat len 0 leaves destination alone");
  }

  /* zero-length memory and string copies query no pointers at all */
  {
    char	*d = alloc_user(8);
    void	*r1, *r2, *r3;

    fill_const(d, 8, 'D');
    dmalloc_errno = 0;
    r1 = _dmalloc_memcpy(__FILE__, __LINE__, d, BOGUS_PNT, 0);
    r2 = _dmalloc_memmove(__FILE__, __LINE__, d, BOGUS_PNT, 0);
    r3 = _dmalloc_strncpy(__FILE__, __LINE__, d, (const char *)BOGUS_PNT, 0);
    CHECK(r1 == d && r2 == d && r3 == d,
	  "zero-length copies return the destination");
    CHECK(dmalloc_errno == 0, "zero-length copies query no pointers");
  }

  /* memcpy ranges which are exactly adjacent do not count as overlap */
  {
    char	*b = alloc_user(16);
    char	exp[16];
    void	*r;

    fill_bytes(b, 16, 0x21);
    fill_bytes(exp, 16, 0x21);
    fill_bytes(exp + 8, 8, 0x21);

    dmalloc_errno = 0;
    r = _dmalloc_memcpy(__FILE__, __LINE__, b + 8, b, 8);
    CHECK(r == b + 8, "memcpy adjacent returns destination");
    CHECK(dmalloc_errno == 0, "memcpy adjacent sets no error");
    CHECK(range_equal(b, exp, 16), "memcpy adjacent copies correct bytes");
  }

  /* strncat inside one buffer with disjoint read and write ranges */
  {
    char	*b = alloc_user(16);
    char	exp[16];
    void	*r;

    fill_const(b, 16, '\0');
    b[0] = 'a';
    b[1] = 'b';
    b[8] = 'x';
    b[9] = 'y';
    fill_const(exp, 16, '\0');
    exp[0] = 'a';
    exp[1] = 'b';
    exp[2] = 'x';
    exp[3] = 'y';
    exp[8] = 'x';
    exp[9] = 'y';

    dmalloc_errno = 0;
    r = _dmalloc_strncat(__FILE__, __LINE__, b, b + 8, 2);
    CHECK(r == b, "strncat same-buffer returns destination");
    CHECK(dmalloc_errno == 0, "strncat same-buffer sets no error");
    CHECK(range_equal(b, exp, 16), "strncat same-buffer appends correctly");
  }

  /* memcpy which exactly fills the destination user space */
  {
    char	*d = alloc_user(16);
    char	*s = alloc_user(16);
    void	*r;

    fill_bytes(s, 16, 0x61);
    fill_const(d, 16, 'D');

    dmalloc_errno = 0;
    r = _dmalloc_memcpy(__FILE__, __LINE__, d, s, 16);
    CHECK(r == d, "memcpy exact fit returns destination");
    CHECK(dmalloc_errno == 0, "memcpy exact fit sets no error");
    CHECK(range_equal(d, s, 16), "memcpy exact fit copies correct bytes");
  }

  /* strncat with a huge len still stops at the source terminator */
  {
    char	*d = alloc_user(16);
    char	*s = alloc_user(4);
    char	exp[16];
    void	*r;

    fill_const(d, 16, '\0');
    d[0] = 'a';
    d[1] = 'b';
    fill_bytes(s, 4, 'x');
    s[0] = 'x';
    s[1] = 'y';
    s[2] = '\0';
    fill_const(exp, 16, '\0');
    exp[0] = 'a';
    exp[1] = 'b';
    exp[2] = 'x';
    exp[3] = 'y';

    dmalloc_errno = 0;
    r = _dmalloc_strncat(__FILE__, __LINE__, d, s, (DMALLOC_SIZE)INT_MAX + 1);
    CHECK(r == d, "strncat huge len with terminator returns destination");
    CHECK(dmalloc_errno == 0, "strncat huge len with terminator is fine");
    CHECK(range_equal(d, exp, 16), "strncat huge len stops at terminator");
  }

  /******************** public macro entry points ********************/

  {
    char	*md = alloc_user(16);
    char	*ms = alloc_user(16);
    char	exp[16];
    void	*r;
    int		err;

    /* the macros must reach the same strict machinery */
    fill_bytes(ms, 16, 0x71);
    fill_const(md, 16, 'M');
    fill_const(exp, 16, 'M');
    fill_bytes(exp, 12, 0x71);

    dmalloc_errno = 0;
    r = memcpy(md, ms, 12);
    CHECK(r == md && dmalloc_errno == 0 && range_equal(md, exp, 16),
	  "memcpy macro works in strict mode");

    fill_bytes(md, 16, 0x41);
    fill_bytes(exp, 16, 0x41);
    {
      int	i;
      for (i = 7; i >= 0; i--) {
	exp[i + 2] = exp[i];
      }
    }
    dmalloc_errno = 0;
    r = memmove(md + 2, md, 8);
    CHECK(r == md + 2 && dmalloc_errno == 0 && range_equal(md, exp, 16),
	  "memmove macro works in strict mode");

    fill_bytes(ms, 16, 0x61);
    ms[0] = 'h';
    ms[1] = 'i';
    ms[2] = '\0';
    fill_const(md, 16, 'M');
    fill_const(exp, 16, 'M');
    exp[0] = 'h';
    exp[1] = 'i';
    fill_const(exp + 2, 4, '\0');
    dmalloc_errno = 0;
    r = strncpy(md, ms, 6);
    CHECK(r == md && dmalloc_errno == 0 && range_equal(md, exp, 16),
	  "strncpy macro works in strict mode");

    fill_const(md, 16, '\0');
    md[0] = 'a';
    fill_const(exp, 16, '\0');
    exp[0] = 'a';
    exp[1] = 'h';
    exp[2] = 'i';
    dmalloc_errno = 0;
    r = strncat(md, ms, 2);
    CHECK(r == md && dmalloc_errno == 0 && range_equal(md, exp, 16),
	  "strncat macro works in strict mode");

    /* and a macro call must be rejected the same way */
    fill_bytes(md, 16, 0x55);
    dmalloc_errno = 0;
    r = memcpy(md + 1, md, 8);
    err = dmalloc_errno;
    CHECK(r == NULL && err == DMALLOC_ERROR_OVERLAP,
	  "memcpy macro rejects overlap in strict mode");
  }

  /******************** strict-mode failure group ********************/

  /* memcpy with a destination which is too small */
  {
    char	*d = alloc_user(8);
    char	*s = alloc_user(16);
    snapshot_t	d_before, d_after, s_before, s_after;
    void	*r;
    int		err;

    fill_const(d, 8, 'D');
    fill_bytes(s, 16, 1);
    snap_take(&d_before, d, 8);
    snap_take(&s_before, s, 16);

    dmalloc_errno = 0;
    r = _dmalloc_memcpy(__FILE__, __LINE__, d, s, 16);
    err = dmalloc_errno;
    CHECK(r == NULL, "memcpy small destination returns NULL");
    CHECK(err == DMALLOC_ERROR_WOULD_OVERWRITE,
	  "memcpy small destination sets WOULD_OVERWRITE");

    snap_take(&d_after, d, 8);
    snap_take(&s_after, s, 16);
    CHECK(snap_same(&d_before, &d_after), "memcpy reject leaves dest alone");
    CHECK(snap_same(&s_before, &s_after), "memcpy reject leaves source alone");
  }

  /* memcpy with a source which is too small */
  {
    char	*d = alloc_user(16);
    char	*s = alloc_user(8);
    snapshot_t	d_before, d_after, s_before, s_after;
    void	*r;
    int		err;

    fill_const(d, 16, 'D');
    fill_bytes(s, 8, 1);
    snap_take(&d_before, d, 16);
    snap_take(&s_before, s, 8);

    dmalloc_errno = 0;
    r = _dmalloc_memcpy(__FILE__, __LINE__, d, s, 16);
    err = dmalloc_errno;
    CHECK(r == NULL, "memcpy small source returns NULL");
    CHECK(err == DMALLOC_ERROR_WOULD_OVERWRITE,
	  "memcpy small source sets WOULD_OVERWRITE");

    snap_take(&d_after, d, 16);
    snap_take(&s_after, s, 8);
    CHECK(snap_same(&d_before, &d_after), "memcpy reject leaves dest alone 2");
    CHECK(snap_same(&s_before, &s_after),
	  "memcpy reject leaves source alone 2");
  }

  /* memmove with a length beyond INT_MAX must not be truncated to int */
  {
    char	*d = alloc_user(16);
    char	*s = alloc_user(16);
    snapshot_t	d_before, d_after, s_before, s_after;
    void	*r;
    int		err;

    fill_const(d, 16, 'D');
    fill_bytes(s, 16, 1);
    snap_take(&d_before, d, 16);
    snap_take(&s_before, s, 16);

    dmalloc_errno = 0;
    r = _dmalloc_memmove(__FILE__, __LINE__, d, s,
			 (DMALLOC_SIZE)INT_MAX + 1);
    err = dmalloc_errno;
    CHECK(r == NULL, "memmove huge length returns NULL");
    CHECK(err == DMALLOC_ERROR_WOULD_OVERWRITE,
	  "memmove huge length sets WOULD_OVERWRITE");

    snap_take(&d_after, d, 16);
    snap_take(&s_after, s, 16);
    CHECK(snap_same(&d_before, &d_after), "memmove reject leaves dest alone");
    CHECK(snap_same(&s_before, &s_after),
	  "memmove reject leaves source alone");
  }

  /* strncpy with a destination which is too small for the full len */
  {
    char	*d = alloc_user(4);
    char	*s = alloc_user(8);
    snapshot_t	d_before, d_after, s_before, s_after;
    void	*r;
    int		err;
    int		i;

    fill_const(d, 4, 'D');
    for (i = 0; i < 8; i++) {
      s[i] = (char)('a' + i);
    }
    snap_take(&d_before, d, 4);
    snap_take(&s_before, s, 8);

    /* the source is 8 bytes but the destination must take all 8 too */
    dmalloc_errno = 0;
    r = _dmalloc_strncpy(__FILE__, __LINE__, d, s, 8);
    err = dmalloc_errno;
    CHECK(r == NULL, "strncpy small destination returns NULL");
    CHECK(err == DMALLOC_ERROR_WOULD_OVERWRITE,
	  "strncpy small destination sets WOULD_OVERWRITE");

    snap_take(&d_after, d, 4);
    snap_take(&s_after, s, 8);
    CHECK(snap_same(&d_before, &d_after),
	  "strncpy reject leaves dest alone");
    CHECK(snap_same(&s_before, &s_after),
	  "strncpy reject leaves source alone");
  }

  /* strncpy with an unterminated source shorter than len */
  {
    char	*d = alloc_user(8);
    char	*s = alloc_user(4);
    snapshot_t	d_before, d_after, s_before, s_after;
    void	*r;
    int		err;

    fill_const(d, 8, 'D');
    fill_bytes(s, 4, 'w');	/* no null inside the source allocation */
    snap_take(&d_before, d, 8);
    snap_take(&s_before, s, 4);

    dmalloc_errno = 0;
    r = _dmalloc_strncpy(__FILE__, __LINE__, d, s, 8);
    err = dmalloc_errno;
    CHECK(r == NULL, "strncpy short source returns NULL");
    CHECK(err == DMALLOC_ERROR_WOULD_OVERWRITE,
	  "strncpy short source sets WOULD_OVERWRITE");

    snap_take(&d_after, d, 8);
    snap_take(&s_after, s, 4);
    CHECK(snap_same(&d_before, &d_after),
	  "strncpy reject leaves dest alone 2");
    CHECK(snap_same(&s_before, &s_after),
	  "strncpy reject leaves source alone 2");
  }

  /* strncpy with a length beyond INT_MAX */
  {
    char	*d = alloc_user(16);
    char	*s = alloc_user(16);
    snapshot_t	d_before, d_after;
    void	*r;
    int		err;

    fill_const(d, 16, 'D');
    fill_bytes(s, 16, 1);
    snap_take(&d_before, d, 16);

    dmalloc_errno = 0;
    r = _dmalloc_strncpy(__FILE__, __LINE__, d, s, (DMALLOC_SIZE)INT_MAX + 1);
    err = dmalloc_errno;
    CHECK(r == NULL, "strncpy huge length returns NULL");
    CHECK(err == DMALLOC_ERROR_WOULD_OVERWRITE,
	  "strncpy huge length sets WOULD_OVERWRITE");

    snap_take(&d_after, d, 16);
    CHECK(snap_same(&d_before, &d_after),
	  "strncpy huge reject leaves dest alone");
  }

  /* strncat with a destination string which is not terminated */
  {
    char	*d = alloc_user(4);
    char	*s = alloc_user(4);
    snapshot_t	d_before, d_after, s_before, s_after;
    void	*r;
    int		err;

    fill_const(d, 4, 'x');	/* no null inside the destination */
    fill_bytes(s, 4, 'a');
    s[2] = '\0';
    snap_take(&d_before, d, 4);
    snap_take(&s_before, s, 4);

    dmalloc_errno = 0;
    r = _dmalloc_strncat(__FILE__, __LINE__, d, s, 2);
    err = dmalloc_errno;
    CHECK(r == NULL, "strncat unterminated destination returns NULL");
    CHECK(err == DMALLOC_ERROR_NOT_TERMINATED,
	  "strncat unterminated destination sets NOT_TERMINATED");

    snap_take(&d_after, d, 4);
    snap_take(&s_after, s, 4);
    CHECK(snap_same(&d_before, &d_after),
	  "strncat reject leaves dest alone");
    CHECK(snap_same(&s_before, &s_after),
	  "strncat reject leaves source alone");
  }

  /* strncat with len 0 still requires a terminated destination */
  {
    char	*d = alloc_user(4);
    snapshot_t	d_before, d_after;
    void	*r;
    int		err;

    fill_const(d, 4, 'x');
    snap_take(&d_before, d, 4);

    dmalloc_errno = 0;
    r = _dmalloc_strncat(__FILE__, __LINE__, d, (const char *)BOGUS_PNT, 0);
    err = dmalloc_errno;
    CHECK(r == NULL, "strncat len 0 unterminated destination returns NULL");
    CHECK(err == DMALLOC_ERROR_NOT_TERMINATED,
	  "strncat len 0 unterminated destination sets NOT_TERMINATED");

    snap_take(&d_after, d, 4);
    CHECK(snap_same(&d_before, &d_after),
	  "strncat len 0 reject leaves dest alone");
  }

  /* strncat with a source which runs out before len and has no null */
  {
    char	*d = alloc_user(8);
    char	*s = alloc_user(2);
    snapshot_t	d_before, d_after, s_before, s_after;
    void	*r;
    int		err;

    fill_const(d, 8, '\0');
    d[0] = 'a';
    d[1] = 'b';
    fill_bytes(s, 2, 'c');	/* no null inside the source allocation */
    snap_take(&d_before, d, 8);
    snap_take(&s_before, s, 2);

    dmalloc_errno = 0;
    r = _dmalloc_strncat(__FILE__, __LINE__, d, s, 4);
    err = dmalloc_errno;
    CHECK(r == NULL, "strncat short source returns NULL");
    CHECK(err == DMALLOC_ERROR_WOULD_OVERWRITE,
	  "strncat short source sets WOULD_OVERWRITE");

    snap_take(&d_after, d, 8);
    snap_take(&s_after, s, 2);
    CHECK(snap_same(&d_before, &d_after),
	  "strncat reject leaves dest alone 2");
    CHECK(snap_same(&s_before, &s_after),
	  "strncat reject leaves source alone 2");
  }

  /* strncat with a destination too small for the result */
  {
    char	*d = alloc_user(4);
    char	*s = alloc_user(4);
    snapshot_t	d_before, d_after, s_before, s_after;
    void	*r;
    int		err;

    fill_const(d, 4, '\0');
    d[0] = 'a';
    d[1] = 'b';
    d[2] = 'c';
    fill_bytes(s, 4, 'x');
    s[1] = '\0';
    snap_take(&d_before, d, 4);
    snap_take(&s_before, s, 4);

    dmalloc_errno = 0;
    r = _dmalloc_strncat(__FILE__, __LINE__, d, s, 2);
    err = dmalloc_errno;
    CHECK(r == NULL, "strncat small destination returns NULL");
    CHECK(err == DMALLOC_ERROR_WOULD_OVERWRITE,
	  "strncat small destination sets WOULD_OVERWRITE");

    snap_take(&d_after, d, 4);
    snap_take(&s_after, s, 4);
    CHECK(snap_same(&d_before, &d_after),
	  "strncat reject leaves dest alone 3");
    CHECK(snap_same(&s_before, &s_after),
	  "strncat reject leaves source alone 3");
  }

  /* strncat with a huge length and an unterminated short source */
  {
    char	*d = alloc_user(8);
    char	*s = alloc_user(4);
    snapshot_t	d_before, d_after, s_before, s_after;
    void	*r;
    int		err;

    fill_const(d, 8, '\0');
    d[0] = 'a';
    fill_bytes(s, 4, 'w');
    snap_take(&d_before, d, 8);
    snap_take(&s_before, s, 4);

    dmalloc_errno = 0;
    r = _dmalloc_strncat(__FILE__, __LINE__, d, s, (DMALLOC_SIZE)INT_MAX + 1);
    err = dmalloc_errno;
    CHECK(r == NULL, "strncat huge length returns NULL");
    CHECK(err == DMALLOC_ERROR_WOULD_OVERWRITE,
	  "strncat huge length sets WOULD_OVERWRITE");

    snap_take(&d_after, d, 8);
    snap_take(&s_after, s, 4);
    CHECK(snap_same(&d_before, &d_after),
	  "strncat huge reject leaves dest alone");
    CHECK(snap_same(&s_before, &s_after),
	  "strncat huge reject leaves source alone");
  }

  /* memcpy with overlapping read and write ranges */
  {
    char	*b = alloc_user(16);
    snapshot_t	b_before, b_after;
    void	*r;
    int		err;

    fill_bytes(b, 16, 1);
    snap_take(&b_before, b, 16);

    dmalloc_errno = 0;
    r = _dmalloc_memcpy(__FILE__, __LINE__, b + 2, b, 8);
    err = dmalloc_errno;
    CHECK(r == NULL, "memcpy overlap returns NULL");
    CHECK(err == DMALLOC_ERROR_OVERLAP, "memcpy overlap sets OVERLAP");

    snap_take(&b_after, b, 16);
    CHECK(snap_same(&b_before, &b_after),
	  "memcpy overlap reject leaves buffer alone");
  }

  /* strncpy with overlapping actual read and write ranges */
  {
    char	*b = alloc_user(16);
    snapshot_t	b_before, b_after;
    void	*r;
    int		err;

    fill_const(b, 16, '\0');
    b[0] = 'a';
    b[1] = 'b';
    snap_take(&b_before, b, 16);

    /* reads [b, b+3) and writes [b+1, b+5) which intersect */
    dmalloc_errno = 0;
    r = _dmalloc_strncpy(__FILE__, __LINE__, b + 1, b, 4);
    err = dmalloc_errno;
    CHECK(r == NULL, "strncpy overlap returns NULL");
    CHECK(err == DMALLOC_ERROR_OVERLAP, "strncpy overlap sets OVERLAP");

    snap_take(&b_after, b, 16);
    CHECK(snap_same(&b_before, &b_after),
	  "strncpy overlap reject leaves buffer alone");
  }

  /* strncat with overlapping actual read and write ranges */
  {
    char	*b = alloc_user(16);
    snapshot_t	b_before, b_after;
    void	*r;
    int		err;

    fill_const(b, 16, '\0');
    b[0] = 'a';
    b[1] = 'b';
    snap_take(&b_before, b, 16);

    /* reads [b+1, b+3) and writes [b+2, b+4) which intersect */
    dmalloc_errno = 0;
    r = _dmalloc_strncat(__FILE__, __LINE__, b, b + 1, 2);
    err = dmalloc_errno;
    CHECK(r == NULL, "strncat overlap returns NULL");
    CHECK(err == DMALLOC_ERROR_OVERLAP, "strncat overlap sets OVERLAP");

    snap_take(&b_after, b, 16);
    CHECK(snap_same(&b_before, &b_after),
	  "strncat overlap reject leaves buffer alone");
  }

  /* unknown, freed, fence, and round-up pointers are invalid objects */
  {
    char		*d = alloc_user(8);
    char		*s = alloc_user(8);
    char		*freed = alloc_user(8);
    char		*shorty = alloc_user(6);
    snapshot_t	d_before, d_after;
    void		*r1, *r2, *r3, *r4, *r5;
    int			e1, e2, e3, e4, e5;

    fill_const(d, 8, 'D');
    fill_bytes(s, 8, 1);
    (void)dmalloc_free(__FILE__, __LINE__, freed, DMALLOC_FUNC_FREE);
    snap_take(&d_before, d, 8);

    dmalloc_errno = 0;
    r1 = _dmalloc_memcpy(__FILE__, __LINE__, d, BOGUS_PNT, 4);
    e1 = dmalloc_errno;
    dmalloc_errno = 0;
    r2 = _dmalloc_memcpy(__FILE__, __LINE__, BOGUS_PNT, s, 4);
    e2 = dmalloc_errno;
    dmalloc_errno = 0;
    r3 = _dmalloc_memcpy(__FILE__, __LINE__, d, freed, 4);
    e3 = dmalloc_errno;
    dmalloc_errno = 0;
    r4 = _dmalloc_memcpy(__FILE__, __LINE__, d, s - FENCE_BOTTOM_SIZE, 4);
    e4 = dmalloc_errno;
    dmalloc_errno = 0;
    r5 = _dmalloc_memcpy(__FILE__, __LINE__, d, shorty + 6, 2);
    e5 = dmalloc_errno;

    CHECK(r1 == NULL && e1 == DMALLOC_ERROR_NOT_FOUND,
	  "unknown source rejected as NOT_FOUND");
    CHECK(r2 == NULL && e2 == DMALLOC_ERROR_NOT_FOUND,
	  "unknown destination rejected as NOT_FOUND");
    CHECK(r3 == NULL && e3 == DMALLOC_ERROR_NOT_FOUND,
	  "freed source rejected as NOT_FOUND");
    CHECK(r4 == NULL && e4 == DMALLOC_ERROR_NOT_FOUND,
	  "fence pointer rejected as NOT_FOUND");
    CHECK(r5 == NULL && e5 == DMALLOC_ERROR_NOT_FOUND,
	  "round-up pointer rejected as NOT_FOUND");

    snap_take(&d_after, d, 8);
    CHECK(snap_same(&d_before, &d_after),
	  "invalid-object rejects leave dest alone");
  }

  /******************** strict mode without check-funcs ********************/

  {
    unsigned int	saved_flags;
    char		*d = alloc_user(8);
    char		*s = alloc_user(16);
    void		*r;
    int			err;

    fill_const(d, 8, 'D');
    fill_bytes(s, 16, 1);

    /* REJECT must work even when the check-funcs token is disabled */
    saved_flags = dmalloc_debug(DMALLOC_DEBUG_CHECK_FENCE);
    dmalloc_errno = 0;
    r = _dmalloc_memcpy(__FILE__, __LINE__, d, s, 16);
    err = dmalloc_errno;
    (void)dmalloc_debug(saved_flags);

    CHECK(r == NULL, "strict mode works without check-funcs");
    CHECK(err == DMALLOC_ERROR_WOULD_OVERWRITE,
	  "strict mode sets errno without check-funcs");
    CHECK(range_is(d, 8, 'D'), "strict reject without check-funcs is clean");
  }

  /******************** unknown sources are never scanned ********************/

  {
    char	*d = alloc_user(16);
    char	*s = alloc_user(4);
    pid_t	pid;
    int		status;

    fill_const(d, 16, 'D');
    fill_bytes(s, 4, 'a');
    s[2] = '\0';

    /*
     * In a child process, pass an unreadable address to each wrapper.
     * If the library scanned the pointer before rejecting it, the
     * child would die with a signal instead of exiting cleanly.
     */
    pid = fork();
    if (pid == 0) {
      void	*r1, *r2, *r3, *r4;
      int	e1, e2, e3, e4;

      dmalloc_errno = 0;
      r1 = _dmalloc_memcpy(__FILE__, __LINE__, d, BOGUS_PNT, 8);
      e1 = dmalloc_errno;
      dmalloc_errno = 0;
      r2 = _dmalloc_memmove(__FILE__, __LINE__, d, BOGUS_PNT, 8);
      e2 = dmalloc_errno;
      dmalloc_errno = 0;
      r3 = _dmalloc_strncpy(__FILE__, __LINE__, d, (const char *)BOGUS_PNT, 8);
      e3 = dmalloc_errno;
      dmalloc_errno = 0;
      r4 = _dmalloc_strncat(__FILE__, __LINE__, (char *)BOGUS_PNT, s, 2);
      e4 = dmalloc_errno;

      if (r1 == NULL && e1 == DMALLOC_ERROR_NOT_FOUND
	  && r2 == NULL && e2 == DMALLOC_ERROR_NOT_FOUND
	  && r3 == NULL && e3 == DMALLOC_ERROR_NOT_FOUND
	  && r4 == NULL && e4 == DMALLOC_ERROR_NOT_FOUND) {
	_exit(0);
      }
      _exit(1);
    }
    CHECK(pid > 0, "fork for unknown-source test succeeded");
    if (pid > 0) {
      CHECK(waitpid(pid, &status, 0) == pid, "waitpid succeeded");
      CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	    "unreadable pointers rejected without being scanned");
    }
  }

  /******************** other wrappers are not extended ********************/

  {
    char	*d = alloc_user(4);
    char	*s = alloc_user(8);
    void	*r;
    int		err;
    int		i;

    fill_const(d, 4, 'D');
    for (i = 0; i < 7; i++) {
      s[i] = (char)('a' + i);
    }
    s[7] = '\0';

    /* strcpy keeps the old report-and-continue behavior in strict mode */
    dmalloc_errno = 0;
    r = _dmalloc_strcpy(__FILE__, __LINE__, d, s);
    err = dmalloc_errno;
    CHECK(r == d, "strcpy is not strict-rejected");
    CHECK(d[0] == 'a', "strcpy still performs the copy");
    CHECK(err != 0, "strcpy still reports the problem");
  }

  /******************** back to report mode ********************/

  prev_policy = dmalloc_set_func_check_policy(DMALLOC_FUNC_REPORT);
  CHECK(prev_policy == DMALLOC_FUNC_REJECT, "previous policy was REJECT");

  {
    char	*d = alloc_user(8);
    char	*s = alloc_user(9);
    void	*r;
    int		err;

    fill_const(d, 8, 'D');
    fill_const(s, 9, 'Q');

    /* the historic behavior: report the error but still call libc */
    dmalloc_errno = 0;
    r = _dmalloc_memcpy(__FILE__, __LINE__, d, s, 9);
    err = dmalloc_errno;
    CHECK(r == d, "report mode memcpy returns destination");
    CHECK(d[0] == 'Q', "report mode still performs the copy");
    CHECK(err == DMALLOC_ERROR_WOULD_OVERWRITE,
	  "report mode still reports the error");
  }

  /******************** summary ********************/

  if (fail_c == 0) {
    (void)printf("strict-contract: all tests passed\n");
    return 0;
  }
  (void)printf("strict-contract: %d check(s) failed\n", fail_c);
  return 1;
}
