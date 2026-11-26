/*
** gnu_extension_contract.c
**
** Defensive regression contract for the libtar th_read() GNU
** longname ('L') / longlink ('K') extension chain.
**
** Everything is driven through tar_fdopen() with an in-process memory
** tartype_t: headers are constructed independently here (with our own
** checksum calculation), never by calling th_write()/th_finish().
**
** This file intentionally defines no main(): task_support/driver.c
** provides it and iterates the task_cases table.  The same bounded
** generator/check core also backs LLVMFuzzerTestOneInput.
**
** The shipping block.c/handle.c are textually included with their
** allocation entry points redirected through an observation/fault
** hook.  The exact production parser and close code still run; the
** hook only observes or replaces allocations, it never influences a
** parsing decision.
*/

#include "task_support/check.h"

#include <config.h>
#include <libtar.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(GNU_FUZZER_BUILD)
/* Standalone libFuzzer build does not link task_support/driver.c. */
int
task_check(int value, const char *expr, const char *file, int line)
{
	if (!value)
	{
		fprintf(stderr, "FUZZ CHECK FAIL %s:%d %s\n",
			file ? file : "?", line, expr ? expr : "?");
		abort();
	}
	return value;
}
#endif

/*
** ===================================================================
** Allocation observation / fault injection hook
** ===================================================================
*/

#define HOOK_MAX 8192

static void *hook_live_ptrs[HOOK_MAX];
static int hook_live_count;
static int hook_alloc_calls;
static int hook_fail_at;	/* 1-based allocation call to fail, 0 = off */

static int
hook_index_of(void *p)
{
	int i;

	for (i = 0; i < HOOK_MAX; i++)
	{
		if (hook_live_ptrs[i] == p)
			return i;
	}
	return -1;
}

static void
hook_track(void *p)
{
	int i;

	if (p == NULL)
		return;
	for (i = 0; i < HOOK_MAX; i++)
	{
		if (hook_live_ptrs[i] == NULL)
		{
			hook_live_ptrs[i] = p;
			hook_live_count++;
			return;
		}
	}
	/* table exhausted: the test suite is far below this bound */
	abort();
}

static void
hook_untrack(void *p)
{
	int i;

	if (p == NULL)
		return;
	i = hook_index_of(p);
	if (i >= 0)
	{
		hook_live_ptrs[i] = NULL;
		hook_live_count--;
	}
}

static void *
test_malloc(size_t n)
{
	void *p;

	hook_alloc_calls++;
	if (hook_fail_at != 0 && hook_alloc_calls == hook_fail_at)
	{
		errno = ENOMEM;
		return NULL;
	}
	p = malloc(n ? n : 1);
	if (p != NULL)
		hook_track(p);
	return p;
}

static void *
test_calloc(size_t a, size_t b)
{
	void *p;

	hook_alloc_calls++;
	if (hook_fail_at != 0 && hook_alloc_calls == hook_fail_at)
	{
		errno = ENOMEM;
		return NULL;
	}
	p = calloc(a, b);
	if (p != NULL)
		hook_track(p);
	return p;
}

static void
test_free(void *p)
{
	if (p == NULL)
		return;
	/*
	** Pointers allocated by other translation units (e.g. the
	** th_pathname cache) are not in the registry; untracked pointers
	** still get freed for real.
	*/
	hook_untrack(p);
	free(p);
}

/*
** Redirect allocation calls inside the production units to the hook,
** then include the actual implementation.  Header include guards mean
** the prototypes already processed above are reused.
**
** The pinned target driver compiles this file without -Icompat, so
** neutralize internal.h (whose only extra dependency is compat.h):
** config.h + libtar.h + the standard headers below already supply
** everything block.c/handle.c use here.
*/
#define INTERNAL_H

#define malloc(sz)		test_malloc(sz)
#define calloc(a, b)		test_calloc(a, b)
#define free(p)			test_free(p)

#include "../lib/block.c"
#include "../lib/handle.c"

#undef malloc
#undef calloc
#undef free

/*
** ===================================================================
** In-process memory tartype_t reader
** ===================================================================
*/

enum { IO_SLOTS = 16 };

#define FAIL_SHORT 1
#define FAIL_EIO   2

struct memio
{
	const unsigned char *data;
	size_t size;
	size_t pos;
	int reads;
	int closes;
	int close_rc;
	int fail_on_read;	/* absolute read invocation, 0 = off */
	int fail_kind;
	int short_ret;
	int io_errno;
};

static struct memio ios[IO_SLOTS];

static ssize_t
mem_read(int fd, void *out, size_t n)
{
	struct memio *io;
	size_t avail, take;

	if (fd < 0 || fd >= IO_SLOTS)
	{
		errno = EBADF;
		return -1;
	}
	io = &ios[fd];
	io->reads++;

	if (io->fail_on_read != 0 && io->reads == io->fail_on_read)
	{
		if (io->fail_kind == FAIL_EIO)
		{
			errno = io->io_errno ? io->io_errno : EIO;
			return -1;
		}
		/* positive short read */
		take = (size_t)io->short_ret;
		avail = (io->pos <= io->size) ? io->size - io->pos : 0;
		if (take > avail)
			take = avail;
		if (take > n)
			take = n;
		if (take != 0)
			memcpy(out, io->data + io->pos, take);
		io->pos += take;
		return (ssize_t)take;
	}

	avail = (io->pos <= io->size) ? io->size - io->pos : 0;
	take = (n < avail) ? n : avail;
	if (take != 0)
		memcpy(out, io->data + io->pos, take);
	io->pos += take;
	return (ssize_t)take;
}

static int
mem_close(int fd)
{
	if (fd < 0 || fd >= IO_SLOTS)
		return -1;
	ios[fd].closes++;
	return ios[fd].close_rc;
}

static tartype_t mem_type = { NULL, mem_close, mem_read, NULL };

static void
io_attach(int slot, const unsigned char *data, size_t size)
{
	memset(&ios[slot], 0, sizeof(ios[slot]));
	ios[slot].data = data;
	ios[slot].size = size;
}

static TAR *
mem_open(int slot, int options)
{
	TAR *t = NULL;

	if (tar_fdopen(&t, slot, "memory", &mem_type,
			O_RDONLY, 0, options) != 0)
		return NULL;
	return t;
}

/*
** ===================================================================
** Independent archive builder (raw 512-byte headers + own checksum)
** ===================================================================
*/

#define ARC_CAP (4u * 1024u * 1024u)

static unsigned char arc_buf[ARC_CAP];
static size_t arc_len;

/* standard ustar header field offsets */
enum
{
	H_NAME = 0,
	H_MODE = 100,
	H_UID = 108,
	H_GID = 116,
	H_SIZE = 124,
	H_MTIME = 136,
	H_CHKSUM = 148,
	H_TYPE = 156,
	H_LINK = 157,
	H_MAGIC = 257,
	H_VERSION = 263,
	H_UNAME = 265,
	H_GNAME = 297,
	H_DEVMAJ = 329,
	H_DEVMIN = 337,
	H_PREFIX = 345
};

#define HF_POSIX_MAGIC	0x1
#define HF_BAD_CRC	0x2

static void
arc_reset(void)
{
	arc_len = 0;
}

static int
arc_add(const void *p, size_t n)
{
	if (n > ARC_CAP - arc_len)
		return -1;
	if (n != 0)
		memcpy(arc_buf + arc_len, p, n);
	arc_len += n;
	return 0;
}

static void
hdr_rechecksum(unsigned char h[512])
{
	unsigned int sum = 0;
	int i;

	memset(h + H_CHKSUM, ' ', 8);
	for (i = 0; i < T_BLOCKSIZE; i++)
		sum += h[i];
	snprintf((char *)(h + H_CHKSUM), 7, "%06o", sum);
	h[H_CHKSUM + 6] = '\0';
	h[H_CHKSUM + 7] = ' ';
}

static void
make_hdr(unsigned char h[512], char type, const char *nm, const char *lnk,
	 size_t sz, const unsigned char sz12[12], unsigned flags)
{
	size_t l;

	memset(h, 0, T_BLOCKSIZE);

	if (nm != NULL)
	{
		l = strlen(nm);
		if (l > 100)
			l = 100;
		memcpy(h + H_NAME, nm, l);
	}

	memcpy(h + H_MODE, "0000644", 7);
	memcpy(h + H_UID, "0000000", 7);
	memcpy(h + H_GID, "0000000", 7);

	if (sz12 != NULL)
		memcpy(h + H_SIZE, sz12, 12);
	else
		snprintf((char *)(h + H_SIZE), 12, "%011lo",
			 (unsigned long)sz);

	memcpy(h + H_MTIME, "00000000000", 11);
	memset(h + H_CHKSUM, ' ', 8);
	h[H_TYPE] = (unsigned char)type;

	if (lnk != NULL)
	{
		l = strlen(lnk);
		if (l > 100)
			l = 100;
		memcpy(h + H_LINK, lnk, l);
	}

	if (flags & HF_POSIX_MAGIC)
	{
		memcpy(h + H_MAGIC, TMAGIC, TMAGLEN);
		memcpy(h + H_VERSION, TVERSION, TVERSLEN);
	}
	else
	{
		/* GNU-style magic, as th_finish(TAR_GNU) writes it */
		memcpy(h + H_MAGIC, "ustar ", 6);
		memcpy(h + H_VERSION, " ", 1);
	}

	hdr_rechecksum(h);

	if (flags & HF_BAD_CRC)
		h[H_CHKSUM] = 'Z';	/* unparsable checksum */
}

static int
arc_hdr(char type, const char *nm, const char *lnk, size_t sz,
	const unsigned char sz12[12], unsigned flags)
{
	unsigned char h[T_BLOCKSIZE];

	make_hdr(h, type, nm, lnk, sz, sz12, flags);
	return arc_add(h, T_BLOCKSIZE);
}

/*
** Append exactly n declared payload bytes from decl (or a generated
** pattern when decl is NULL), then pad to the 512 block boundary with
** pad.  n must not be a multiple overflowing the test arena.
*/
static int
arc_payload(const unsigned char *decl, size_t n, unsigned char pad)
{
	size_t blocks = (n + (T_BLOCKSIZE - 1)) / T_BLOCKSIZE;
	size_t total = blocks * T_BLOCKSIZE;
	size_t i;
	static const char alphabet[] =
		"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

	if (total > ARC_CAP - arc_len)
		return -1;

	for (i = 0; i < n; i++)
	{
		unsigned char c = decl ? decl[i]
				       : (unsigned char)alphabet[i % 62];
		arc_buf[arc_len + i] = c;
	}
	for (i = n; i < total; i++)
		arc_buf[arc_len + i] = pad;
	arc_len += total;
	return 0;
}

static int
arc_ext(char kind, const unsigned char *decl, size_t n, unsigned char pad,
	const unsigned char sz12[12])
{
	if (arc_hdr(kind, "././@LongLink", NULL, n, sz12, 0) != 0)
		return -1;
	return arc_payload(decl, n, pad);
}

static void
fill_safe(unsigned char *buf, size_t n, unsigned seed)
{
	static const char alphabet[] =
		"abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ__";
	size_t i;

	for (i = 0; i < n; i++)
		buf[i] = (unsigned char)alphabet[(seed + i * 17u) % 64];
}

/*
** ===================================================================
** Shared assertion helpers (each returns 1 on failed TASK_CHECK)
** ===================================================================
*/

static int
check_gnu_null(TAR *t)
{
	TASK_CHECK(t->th_buf.gnu_longname == NULL);
	TASK_CHECK(t->th_buf.gnu_longlink == NULL);
	return 0;
}

/* A sticky failure replays errno with zero additional reader calls. */
static int
check_replay_dead(int slot, TAR *t, int expected_errno)
{
	int reads_before = ios[slot].reads;
	int rc;

	errno = 0;
	rc = th_read(t);
	TASK_CHECK(rc == -1);
	TASK_CHECK(errno == expected_errno);
	TASK_CHECK(ios[slot].reads == reads_before);
	return 0;
}

static int
check_close_frees(int slot, TAR *t)
{
	TASK_CHECK(tar_close(t) == 0);
	TASK_CHECK(ios[slot].closes == 1);
	TASK_CHECK(hook_live_count == 0);
	return 0;
}

/* Open the global arc on slot 0 (most cases use slot 0). */
static TAR *
open_global_arc(int options)
{
	io_attach(0, arc_buf, arc_len);
	return mem_open(0, options);
}

/*
** ###################################################################
** LAYOUT GROUP
** ###################################################################
*/

/* A single L extension drives pathname; plain linkname stays inline. */
static int
case_layout_L_simple(void)
{
	const char *want = "a-safe-long-path-name-0123456789-abcdef";
	TAR *t;
	size_t want_len = strlen(want);
	size_t expect_pos;

	arc_reset();
	TASK_CHECK(arc_ext('L', (const unsigned char *)want, want_len,
			   0, NULL) == 0);
	TASK_CHECK(arc_hdr(REGTYPE, "ordinary.txt", "ord.lnk", 0,
			   NULL, 0) == 0);
	expect_pos = arc_len;

	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);

	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(t->th_buf.typeflag == REGTYPE);
	TASK_CHECK(strcmp(t->th_buf.name, "ordinary.txt") == 0);
	TASK_CHECK(t->th_buf.gnu_longname != NULL);
	TASK_CHECK(strcmp(t->th_buf.gnu_longname, want) == 0);
	TASK_CHECK(strcmp(th_get_pathname(t), want) == 0);
	TASK_CHECK(t->th_buf.gnu_longlink == NULL);
	TASK_CHECK(strcmp(th_get_linkname(t), "ord.lnk") == 0);
	TASK_CHECK(ios[0].pos == expect_pos);
	TASK_CHECK(ios[0].reads == 3);	/* L header, payload, ordinary */

	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* A single K extension drives linkname; plain pathname stays inline. */
static int
case_layout_K_simple(void)
{
	const char *want = "a-safe-long-link-target-value-9876543210";
	TAR *t;
	size_t want_len = strlen(want);

	arc_reset();
	TASK_CHECK(arc_ext('K', (const unsigned char *)want, want_len,
			   0, NULL) == 0);
	TASK_CHECK(arc_hdr(REGTYPE, "k-rec.txt", "inline.lnk", 0,
			   NULL, 0) == 0);

	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == 0);

	TASK_CHECK(t->th_buf.typeflag == REGTYPE);
	TASK_CHECK(strcmp(t->th_buf.name, "k-rec.txt") == 0);
	TASK_CHECK(t->th_buf.gnu_longlink != NULL);
	TASK_CHECK(strcmp(t->th_buf.gnu_longlink, want) == 0);
	TASK_CHECK(strcmp(th_get_linkname(t), want) == 0);
	TASK_CHECK(t->th_buf.gnu_longname == NULL);
	TASK_CHECK(strcmp(th_get_pathname(t), "k-rec.txt") == 0);

	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* L then K. */
static int
case_layout_LK(void)
{
	TAR *t;

	arc_reset();
	TASK_CHECK(arc_ext('L', (const unsigned char *)"lk-path", 7,
			   0, NULL) == 0);
	TASK_CHECK(arc_ext('K', (const unsigned char *)"lk-link", 7,
			   0, NULL) == 0);
	TASK_CHECK(arc_hdr(REGTYPE, "base", "base.lnk", 0, NULL, 0) == 0);

	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(strcmp(th_get_pathname(t), "lk-path") == 0);
	TASK_CHECK(strcmp(th_get_linkname(t), "lk-link") == 0);
	TASK_CHECK(ios[0].reads == 5);

	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* K then L: the order the old fixed sequence could not handle. */
static int
case_layout_KL(void)
{
	TAR *t;
	size_t expect_pos;

	arc_reset();
	TASK_CHECK(arc_ext('K', (const unsigned char *)"kl-link", 7,
			   0, NULL) == 0);
	TASK_CHECK(arc_ext('L', (const unsigned char *)"kl-path", 7,
			   'Z', NULL) == 0);
	TASK_CHECK(arc_hdr(REGTYPE, "base", "base.lnk", 0, NULL, 0) == 0);
	expect_pos = arc_len;

	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(strcmp(th_get_pathname(t), "kl-path") == 0);
	TASK_CHECK(strcmp(th_get_linkname(t), "kl-link") == 0);
	TASK_CHECK(ios[0].pos == expect_pos);
	TASK_CHECK(ios[0].reads == 5);

	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* L-K-L: arbitrary order plus a repeat. */
static int
case_layout_LKL(void)
{
	TAR *t;

	arc_reset();
	TASK_CHECK(arc_ext('L', (const unsigned char *)"first-path", 10,
			   0, NULL) == 0);
	TASK_CHECK(arc_ext('K', (const unsigned char *)"the-link", 8,
			   0, NULL) == 0);
	TASK_CHECK(arc_ext('L', (const unsigned char *)"second-path", 11,
			   0, NULL) == 0);
	TASK_CHECK(arc_hdr(REGTYPE, "base", NULL, 0, NULL, 0) == 0);

	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(strcmp(th_get_pathname(t), "second-path") == 0);
	TASK_CHECK(strcmp(th_get_linkname(t), "the-link") == 0);
	TASK_CHECK(t->gnu_used_headers == 3);
	TASK_CHECK(t->gnu_used_bytes == 10 + 8 + 11);

	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* Repeated copies of both kinds: the last copy overrides. */
static int
case_layout_repeat_override(void)
{
	TAR *t;

	arc_reset();
	TASK_CHECK(arc_ext('L', (const unsigned char *)"L-v1", 4,
			   0, NULL) == 0);
	TASK_CHECK(arc_ext('L', (const unsigned char *)"L-v2-longer", 11,
			   0, NULL) == 0);
	TASK_CHECK(arc_ext('K', (const unsigned char *)"K-v1", 4,
			   0, NULL) == 0);
	TASK_CHECK(arc_ext('K', (const unsigned char *)"K-v2-long", 9,
			   0, NULL) == 0);
	TASK_CHECK(arc_ext('L', (const unsigned char *)"L-v3", 4,
			   0, NULL) == 0);
	TASK_CHECK(arc_hdr(REGTYPE, "base", "x", 0, NULL, 0) == 0);

	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(strcmp(t->th_buf.gnu_longname, "L-v3") == 0);
	TASK_CHECK(strcmp(t->th_buf.gnu_longlink, "K-v2-long") == 0);
	TASK_CHECK(strcmp(th_get_pathname(t), "L-v3") == 0);
	TASK_CHECK(strcmp(th_get_linkname(t), "K-v2-long") == 0);
	/* every declaration accumulates, even overridden ones */
	TASK_CHECK(t->gnu_used_bytes == (size_t)(4 + 11 + 4 + 9 + 4));
	TASK_CHECK(t->gnu_used_headers == 5);

	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* Overrides apply only to the immediately following ordinary record. */
static int
case_layout_next_record_clean(void)
{
	TAR *t;
	size_t pos_after_first, pos_after_second;

	arc_reset();
	TASK_CHECK(arc_ext('L', (const unsigned char *)"only-first-path",
			   15, 0, NULL) == 0);
	TASK_CHECK(arc_ext('K', (const unsigned char *)"only-first-link",
			   15, 0, NULL) == 0);
	TASK_CHECK(arc_hdr(REGTYPE, "first.txt", "first.inline", 0,
			   NULL, 0) == 0);
	pos_after_first = arc_len;
	TASK_CHECK(arc_hdr(REGTYPE, "second.txt", "second.inline", 0,
			   NULL, 0) == 0);
	pos_after_second = arc_len;

	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);

	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(strcmp(th_get_pathname(t), "only-first-path") == 0);
	TASK_CHECK(strcmp(th_get_linkname(t), "only-first-link") == 0);
	TASK_CHECK(ios[0].pos == pos_after_first);

	/* old view is invalidated at the start of this th_read() */
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(t->th_buf.gnu_longname == NULL);
	TASK_CHECK(t->th_buf.gnu_longlink == NULL);
	TASK_CHECK(strcmp(th_get_pathname(t), "second.txt") == 0);
	TASK_CHECK(strcmp(th_get_linkname(t), "second.inline") == 0);
	TASK_CHECK(ios[0].pos == pos_after_second);
	/* per-record budget was recomputed */
	TASK_CHECK(t->gnu_used_headers == 0);
	TASK_CHECK(t->gnu_used_bytes == 0);

	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* Position before the unread regular-file body must be exact. */
static int
case_layout_body_position(void)
{
	const char *body = "BODY-DATA-1234567890";
	TAR *t;
	size_t body_len = strlen(body);
	size_t pos_before_body;

	arc_reset();
	TASK_CHECK(arc_ext('L', (const unsigned char *)"body-rec-path",
			   13, 0, NULL) == 0);
	TASK_CHECK(arc_hdr(REGTYPE, "body.txt", NULL, body_len,
			   NULL, 0) == 0);
	pos_before_body = arc_len;
	TASK_CHECK(arc_add(body, body_len) == 0);
	/* body padded by the producer to a block; add it here */
	{
		unsigned char pad[512];
		size_t z = (512 - (body_len % 512)) % 512;
		memset(pad, 0, sizeof(pad));
		TASK_CHECK(arc_add(pad, z) == 0);
	}

	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(strcmp(th_get_pathname(t), "body-rec-path") == 0);
	/* th_read must not consume any body bytes */
	TASK_CHECK(ios[0].pos == pos_before_body);
	TASK_CHECK(ios[0].pos == 3 * 512);
	/* and the body is intact for the normal read entry point */
	TASK_CHECK(memcmp(arc_buf + ios[0].pos, body, body_len) == 0);

	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* Declared lengths 1/511/512/513/1024, no terminal NUL, nonzero pad. */
static int
case_layout_lengths_plain(void)
{
	static const size_t lens[] = { 1, 511, 512, 513, 1024 };
	unsigned char name[1024];
	size_t k;

	for (k = 0; k < sizeof(lens) / sizeof(lens[0]); k++)
	{
		size_t n = lens[k];
		size_t payload_blocks = (n + 511) / 512;
		TAR *t;

		fill_safe(name, n, (unsigned)(k + 1) * 31u);

		arc_reset();
		TASK_CHECK(arc_ext('L', name, n, 0x5A, NULL) == 0);
		TASK_CHECK(arc_hdr(DIRTYPE, "len-rec", NULL, 0,
				   NULL, 0) == 0);

		t = open_global_arc(TAR_GNU);
		TASK_CHECK(t != NULL);
		TASK_CHECK(th_read(t) == 0);
		TASK_CHECK(t->th_buf.typeflag == DIRTYPE);
		TASK_CHECK(t->th_buf.gnu_longname != NULL);
		TASK_CHECK(strlen(t->th_buf.gnu_longname) == n);
		TASK_CHECK(memcmp(t->th_buf.gnu_longname, name, n) == 0);
		TASK_CHECK(t->th_buf.gnu_longname[n] == '\0');
		TASK_CHECK(ios[0].reads == (int)(2 + payload_blocks));
		TASK_CHECK(ios[0].pos ==
			   (size_t)(2 + payload_blocks) * 512);
		TASK_CHECK(check_close_frees(0, t) == 0);
	}
	return 0;
}

/* Same lengths with the optional single terminal NUL and zero pad. */
static int
case_layout_lengths_terminal_nul(void)
{
	static const size_t lens[] = { 1, 511, 512, 513, 1024 };
	unsigned char decl[1025];
	size_t k;

	for (k = 0; k < sizeof(lens) / sizeof(lens[0]); k++)
	{
		size_t n = lens[k];	/* declared length includes NUL */
		size_t name_len = n - 1;
		size_t payload_blocks = (n + 511) / 512;
		TAR *t;

		fill_safe(decl, name_len, (unsigned)(k + 7) * 13u);
		decl[name_len] = '\0';

		arc_reset();
		TASK_CHECK(arc_ext('K', decl, n, 0xA5, NULL) == 0);
		TASK_CHECK(arc_hdr(REGTYPE, "nul-rec", NULL, 0,
				   NULL, 0) == 0);

		t = open_global_arc(TAR_GNU);
		TASK_CHECK(t != NULL);
		TASK_CHECK(th_read(t) == 0);
		TASK_CHECK(t->th_buf.gnu_longlink != NULL);
		TASK_CHECK(strlen(t->th_buf.gnu_longlink) == name_len);
		if (name_len != 0)
			TASK_CHECK(memcmp(t->th_buf.gnu_longlink, decl,
					  name_len) == 0);
		TASK_CHECK(t->th_buf.gnu_longlink[name_len] == '\0');
		TASK_CHECK(ios[0].pos ==
			   (size_t)(2 + payload_blocks) * 512);
		TASK_CHECK(check_close_frees(0, t) == 0);
	}
	return 0;
}

/* No extensions: empty EOF, EOT blocks and normal next record. */
static int
case_layout_plain_eof(void)
{
	TAR *t;

	/* empty archive: clean EOF returns 1 */
	arc_reset();
	io_attach(0, arc_buf, arc_len);
	t = mem_open(0, TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == 1);
	TASK_CHECK(th_read(t) == 1);	/* EOF remains clean, not sticky */
	TASK_CHECK(check_close_frees(0, t) == 0);

	/* two zero blocks then EOF */
	arc_reset();
	{
		unsigned char zeros[1024];
		memset(zeros, 0, sizeof(zeros));
		TASK_CHECK(arc_add(zeros, sizeof(zeros)) == 0);
	}
	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == 1);
	TASK_CHECK(check_close_frees(0, t) == 0);

	/* one ordinary record then reader EOF */
	arc_reset();
	TASK_CHECK(arc_hdr(REGTYPE, "only.txt", NULL, 0, NULL, 0) == 0);
	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(strcmp(th_get_pathname(t), "only.txt") == 0);
	TASK_CHECK(th_read(t) == 1);
	TASK_CHECK(check_close_frees(0, t) == 0);

	return 0;
}

/* Header options still apply to ordinary headers (magic checked). */
static int
case_layout_magic_option(void)
{
	TAR *t;

	arc_reset();
	TASK_CHECK(arc_hdr(REGTYPE, "magic.txt", NULL, 0, NULL,
			   HF_POSIX_MAGIC) == 0);
	t = open_global_arc(TAR_GNU | TAR_CHECK_MAGIC |
			    TAR_CHECK_VERSION);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(strcmp(th_get_pathname(t), "magic.txt") == 0);
	TASK_CHECK(check_close_frees(0, t) == 0);

	/* bad magic under TAR_CHECK_MAGIC is EINVAL sticky */
	arc_reset();
	TASK_CHECK(arc_hdr(REGTYPE, "badmagic.txt", NULL, 0, NULL, 0) == 0);
	/* GNU magic "ustar " does not match TMAGIC "ustar\0" prefix? */
	t = open_global_arc(TAR_GNU | TAR_CHECK_MAGIC);
	TASK_CHECK(t != NULL);
	/*
	 * GNU "ustar " differs at byte 6; TMAGLEN-1 = 5 compares "ustar",
	 * so the 5-byte prefix actually matches.  Corrupt magic directly
	 * to force the validation failure and recompute not needed for
	 * magic check order (magic checked before checksum).
	 */
	memset(arc_buf + H_MAGIC, 'X', 5);
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == EINVAL);
	TASK_CHECK(check_replay_dead(0, t, EINVAL) == 0);
	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/*
** ###################################################################
** LIMITS AND BOUNDED SIZE DECODE GROUP
** ###################################################################
*/

/* Equality with the byte budget is legal; one byte more is E2BIG. */
static int
case_limits_exact_bytes(void)
{
	TAR *t;

	/* one extension of exactly 5 bytes, limit 5 */
	arc_reset();
	TASK_CHECK(arc_ext('L', (const unsigned char *)"12345", 5,
			   0, NULL) == 0);
	TASK_CHECK(arc_hdr(REGTYPE, "r", NULL, 0, NULL, 0) == 0);
	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(tar_set_gnu_extension_limits(t, 5, 8) == 0);
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(strcmp(th_get_pathname(t), "12345") == 0);
	TASK_CHECK(check_close_frees(0, t) == 0);

	/* two declarations totalling exactly 10 */
	arc_reset();
	TASK_CHECK(arc_ext('L', (const unsigned char *)"aaaa", 4,
			   0, NULL) == 0);
	TASK_CHECK(arc_ext('L', (const unsigned char *)"bbbbbb", 6,
			   0, NULL) == 0);
	TASK_CHECK(arc_hdr(REGTYPE, "r", NULL, 0, NULL, 0) == 0);
	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(tar_set_gnu_extension_limits(t, 10, 8) == 0);
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(strcmp(th_get_pathname(t), "bbbbbb") == 0);
	TASK_CHECK(check_close_frees(0, t) == 0);

	/* one byte over: rejected before payload read */
	arc_reset();
	TASK_CHECK(arc_hdr('L', "././@LongLink", NULL, 6, NULL, 0) == 0);
	TASK_CHECK(arc_payload((const unsigned char *)"abcdef", 6, 0) == 0);
	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(tar_set_gnu_extension_limits(t, 5, 8) == 0);
	{
		size_t pos_after_header = 512;
		TASK_CHECK(th_read(t) == -1);
		TASK_CHECK(errno == E2BIG);
		TASK_CHECK(ios[0].reads == 1);
		TASK_CHECK(ios[0].pos == pos_after_header);
		TASK_CHECK(check_replay_dead(0, t, E2BIG) == 0);
	}
	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* Overridden copies still consume byte budget. */
static int
case_limits_accumulates_overridden(void)
{
	TAR *t;

	/* budget 6: first copy consumes all; a 1-byte replacement fails */
	arc_reset();
	TASK_CHECK(arc_ext('L', (const unsigned char *)"aaaaaa", 6,
			   0, NULL) == 0);
	TASK_CHECK(arc_hdr('L', "././@LongLink", NULL, 1, NULL, 0) == 0);
	/* payload byte exists in archive but must NOT be read */
	TASK_CHECK(arc_payload((const unsigned char *)"b", 1, 0) == 0);
	TASK_CHECK(arc_hdr(REGTYPE, "r", NULL, 0, NULL, 0) == 0);

	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(tar_set_gnu_extension_limits(t, 6, 8) == 0);
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == E2BIG);
	/* L header, first payload, second L header only */
	TASK_CHECK(ios[0].reads == 3);
	TASK_CHECK(ios[0].pos == 3 * 512);
	TASK_CHECK(check_gnu_null(t) == 0);
	TASK_CHECK(check_replay_dead(0, t, E2BIG) == 0);
	TASK_CHECK(check_close_frees(0, t) == 0);

	/* same chain with budget 7 passes; last copy wins */
	io_attach(0, arc_buf, arc_len);
	t = mem_open(0, TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(tar_set_gnu_extension_limits(t, 7, 8) == 0);
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(strcmp(th_get_pathname(t), "b") == 0);
	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* Header count budget acts independently of byte budget. */
static int
case_limits_headers(void)
{
	TAR *t;
	int k;

	/* exactly 2 extension headers + ordinary record is legal */
	arc_reset();
	TASK_CHECK(arc_ext('L', (const unsigned char *)"p", 1, 0, NULL) == 0);
	TASK_CHECK(arc_ext('K', (const unsigned char *)"q", 1, 0, NULL) == 0);
	TASK_CHECK(arc_hdr(REGTYPE, "r", NULL, 0, NULL, 0) == 0);
	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(tar_set_gnu_extension_limits(t, 4096, 2) == 0);
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(check_close_frees(0, t) == 0);

	/* third extension header trips the header budget */
	arc_reset();
	TASK_CHECK(arc_ext('L', (const unsigned char *)"p", 1, 0, NULL) == 0);
	TASK_CHECK(arc_ext('K', (const unsigned char *)"q", 1, 0, NULL) == 0);
	TASK_CHECK(arc_hdr('L', "././@LongLink", NULL, 1, NULL, 0) == 0);
	TASK_CHECK(arc_payload((const unsigned char *)"s", 1, 0) == 0);
	TASK_CHECK(arc_hdr(REGTYPE, "r", NULL, 0, NULL, 0) == 0);
	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(tar_set_gnu_extension_limits(t, 4096, 2) == 0);
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == E2BIG);
	/* Lh Lp Kh Kp Lh: offending payload never read */
	TASK_CHECK(ios[0].reads == 5);
	TASK_CHECK(ios[0].pos == 5 * 512);
	TASK_CHECK(check_replay_dead(0, t, E2BIG) == 0);
	TASK_CHECK(check_close_frees(0, t) == 0);

	/* overrides do not refund header budget */
	arc_reset();
	for (k = 0; k < 3; k++)
		TASK_CHECK(arc_ext('L', (const unsigned char *)"x", 1,
				   0, NULL) == 0);
	TASK_CHECK(arc_hdr(REGTYPE, "r", NULL, 0, NULL, 0) == 0);
	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(tar_set_gnu_extension_limits(t, 4096, 2) == 0);
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == E2BIG);
	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* Built-in defaults: 1 MiB declared bytes, 64 extension headers. */
static int
case_limits_defaults(void)
{
	TAR *t;
	int k;

	/* 64 tiny extensions are exactly at the header limit */
	arc_reset();
	for (k = 0; k < 64; k++)
	{
		char kind = (k & 1) ? 'K' : 'L';
		TASK_CHECK(arc_ext(kind, (const unsigned char *)"x", 1,
				   0, NULL) == 0);
	}
	TASK_CHECK(arc_hdr(REGTYPE, "many", NULL, 0, NULL, 0) == 0);
	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(check_close_frees(0, t) == 0);

	/* 65th extension header exceeds it */
	arc_reset();
	for (k = 0; k < 65; k++)
		TASK_CHECK(arc_ext('L', (const unsigned char *)"x", 1,
				   0, NULL) == 0);
	TASK_CHECK(arc_hdr(REGTYPE, "many", NULL, 0, NULL, 0) == 0);
	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == E2BIG);
	TASK_CHECK(check_replay_dead(0, t, E2BIG) == 0);
	TASK_CHECK(check_close_frees(0, t) == 0);

	/* exactly 1 MiB declared content is legal by default */
	{
		size_t one_mib = (size_t)1024 * 1024;
		unsigned char *big = test_malloc(one_mib);
		TASK_CHECK(big != NULL);
		fill_safe(big, one_mib, 5);
		arc_reset();
		TASK_CHECK(arc_ext('L', big, one_mib, 0, NULL) == 0);
		TASK_CHECK(arc_hdr(REGTYPE, "big", NULL, 0, NULL, 0) == 0);
		/* arc_ext copied the bytes; release the tracked source */
		test_free(big);
		t = open_global_arc(TAR_GNU);
		TASK_CHECK(t != NULL);
		TASK_CHECK(th_read(t) == 0);
		TASK_CHECK(strlen(t->th_buf.gnu_longname) == one_mib);
		TASK_CHECK(check_close_frees(0, t) == 0);
	}

	/* 1 MiB + 1 is rejected before any payload allocation/read */
	arc_reset();
	{
		unsigned char f[12];
		/* encode 1048577 as 11 octal digits + NUL */
		snprintf((char *)f, 12, "%011lo", 1048577UL);
		TASK_CHECK(arc_hdr('L', "././@LongLink", NULL, 0, f, 0) == 0);
	}
	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == E2BIG);
	TASK_CHECK(ios[0].reads == 1);
	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* Two handles keep independent budgets and accumulation state. */
static int
case_limits_two_handles(void)
{
	TAR *a, *b;

	arc_reset();
	TASK_CHECK(arc_ext('L', (const unsigned char *)"12345", 5,
			   0, NULL) == 0);
	TASK_CHECK(arc_hdr(REGTYPE, "r", NULL, 0, NULL, 0) == 0);

	io_attach(3, arc_buf, arc_len);
	io_attach(4, arc_buf, arc_len);
	a = mem_open(3, TAR_GNU);
	b = mem_open(4, TAR_GNU);
	TASK_CHECK(a != NULL && b != NULL);

	TASK_CHECK(tar_set_gnu_extension_limits(a, 4, 8) == 0);
	TASK_CHECK(tar_set_gnu_extension_limits(b, 5, 8) == 0);

	TASK_CHECK(th_read(a) == -1);
	TASK_CHECK(errno == E2BIG);
	TASK_CHECK(th_read(b) == 0);
	TASK_CHECK(strcmp(th_get_pathname(b), "12345") == 0);

	/* A stays sticky; B is unaffected and still has its own state */
	TASK_CHECK(check_replay_dead(3, a, E2BIG) == 0);
	TASK_CHECK(b->gnu_used_bytes == 5);

	TASK_CHECK(tar_close(a) == 0);
	TASK_CHECK(ios[3].closes == 1);
	TASK_CHECK(tar_close(b) == 0);
	TASK_CHECK(ios[4].closes == 1);
	TASK_CHECK(hook_live_count == 0);
	return 0;
}

/* Setter argument validation preserves existing settings. */
static int
case_limits_setter_validation(void)
{
	TAR *t;

	TASK_CHECK(tar_set_gnu_extension_limits(NULL, 1, 1) == -1);
	TASK_CHECK(errno == EINVAL);

	arc_reset();
	TASK_CHECK(arc_ext('L', (const unsigned char *)"12345", 5,
			   0, NULL) == 0);
	TASK_CHECK(arc_hdr(REGTYPE, "r", NULL, 0, NULL, 0) == 0);
	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);

	TASK_CHECK(tar_set_gnu_extension_limits(t, 4, 2) == 0);

	errno = 0;
	TASK_CHECK(tar_set_gnu_extension_limits(t, 0, 2) == -1);
	TASK_CHECK(errno == EINVAL);
	TASK_CHECK(tar_set_gnu_extension_limits(t, 2, 0) == -1);
	TASK_CHECK(errno == EINVAL);
	TASK_CHECK(tar_set_gnu_extension_limits(t, 0, 0) == -1);
	TASK_CHECK(errno == EINVAL);

	/* old (4 bytes, 2 headers) settings still in force */
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == E2BIG);
	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* Strict 12-byte size grammar through the public th_read path. */
static int
case_limits_size_grammar(void)
{
	struct vec
	{
		const char *label;
		unsigned char f[12];
		int accept;	/* 1 decode to val, -1 EINVAL, 2 E2BIG */
		size_t val;
	} v[] = {
		{ "all-12-digits-1",
		  { '0','0','0','0','0','0','0','0','0','0','0','1' },
		  1, 1 },
		{ "leading-spaces",
		  { ' ',' ',' ',' ',' ',' ',' ',' ','0','0','0','5' },
		  1, 5 },
		{ "leading-NULs",
		  { '\0','\0','0','0','0','0','0','0','0','0','0','7' },
		  1, 7 },
		{ "trailing-spaces",
		  { '0','0','0','0','0','0','0','0','0','5',' ',' ' },
		  1, 5 },
		{ "trailing-NULs",
		  { '0','0','0','0','0','0','0','0','0','5','\0','\0' },
		  1, 5 },
		{ "mixed-padding",
		  { ' ','\0','0','0','0','0','0','0','0','3',' ','\0' },
		  1, 3 },
		{ "NUL-then-11-digits-literal-style",
		  { '0','0','0','0','0','0','0','0','0','0','5','\0' },
		  1, 5 },
		{ "no-digits-spaces",
		  { ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ' },
		  -1, 0 },
		{ "no-digits-NULs",
		  { '\0','\0','\0','\0','\0','\0','\0','\0','\0','\0','\0','\0' },
		  -1, 0 },
		{ "minus-sign",
		  { '-','0','0','0','0','0','0','0','0','0','0','1' },
		  -1, 0 },
		{ "plus-sign",
		  { '+','0','0','0','0','0','0','0','0','0','0','1' },
		  -1, 0 },
		{ "digit-8",
		  { '0','0','0','0','0','0','0','0','0','0','0','8' },
		  -1, 0 },
		{ "digit-9",
		  { '0','0','0','0','0','0','0','0','0','0','0','9' },
		  -1, 0 },
		{ "letter-X",
		  { '0','0','0','0','0','0','0','0','0','0','X','1' },
		  -1, 0 },
		{ "embedded-space",
		  { '0','0','0',' ','0','0','0','0','0','0','0','1' },
		  -1, 0 },
		{ "embedded-NUL",
		  { '0','0','0','\0','0','0','0','0','0','0','0','1' },
		  -1, 0 },
		{ "trailing-garbage",
		  { '0','0','0','0','0','0','0','0','0','1','X',' ' },
		  -1, 0 },
		{ "base256-marker-80",
		  { 0x80,'0','0','0','0','0','0','0','0','0','0','1' },
		  -1, 0 },
		{ "base256-marker-ff",
		  { 0xff,0,0,0,0,0,0,0,0,0,0,0 },
		  -1, 0 },
		{ "zero-value",
		  { '0','0','0','0','0','0','0','0','0','0','0','0' },
		  -1, 0 },
		{ "all-seven-huge",
		  { '7','7','7','7','7','7','7','7','7','7','7','7' },
		  2, 0 }
	};
	size_t i;

	for (i = 0; i < sizeof(v) / sizeof(v[0]); i++)
	{
		TAR *t;

		if (v[i].accept == 1)
		{
			unsigned char payload[16];
			fill_safe(payload, v[i].val, (unsigned)i + 3);

			arc_reset();
			TASK_CHECK(arc_hdr('L', "././@LongLink", NULL, 0,
					   v[i].f, 0) == 0);
			TASK_CHECK(arc_payload(payload, v[i].val, 0) == 0);
			TASK_CHECK(arc_hdr(REGTYPE, "g", NULL, 0,
					   NULL, 0) == 0);

			t = open_global_arc(TAR_GNU);
			TASK_CHECK(t != NULL);
			TASK_CHECK(tar_set_gnu_extension_limits(t, 64, 8) == 0);
			TASK_CHECK(th_read(t) == 0);
			TASK_CHECK(strlen(t->th_buf.gnu_longname) ==
				   v[i].val);
			TASK_CHECK(memcmp(t->th_buf.gnu_longname, payload,
					  v[i].val) == 0);
			TASK_CHECK(check_close_frees(0, t) == 0);
		}
		else if (v[i].accept == -1)
		{
			arc_reset();
			TASK_CHECK(arc_hdr('L', "././@LongLink", NULL, 0,
					   v[i].f, 0) == 0);
			/* trailing bytes exist but must never be consumed */
			TASK_CHECK(arc_payload((const unsigned char *)"xxxx",
					       4, 0) == 0);

			t = open_global_arc(TAR_GNU);
			TASK_CHECK(t != NULL);
			TASK_CHECK(tar_set_gnu_extension_limits(t, 64, 8) == 0);
			TASK_CHECK(th_read(t) == -1);
			TASK_CHECK(errno == EINVAL);
			TASK_CHECK(ios[0].reads == 1);
			TASK_CHECK(check_gnu_null(t) == 0);
			TASK_CHECK(check_replay_dead(0, t, EINVAL) == 0);
			TASK_CHECK(check_close_frees(0, t) == 0);
		}
		else
		{
			/* decodable but far above the tiny byte budget */
			arc_reset();
			TASK_CHECK(arc_hdr('L', "././@LongLink", NULL, 0,
					   v[i].f, 0) == 0);
			t = open_global_arc(TAR_GNU);
			TASK_CHECK(t != NULL);
			TASK_CHECK(tar_set_gnu_extension_limits(t, 16, 8) == 0);
			TASK_CHECK(th_read(t) == -1);
			TASK_CHECK(errno == E2BIG);
			TASK_CHECK(ios[0].reads == 1);
			TASK_CHECK(check_close_frees(0, t) == 0);
		}
	}

	/* nonzero adjacent header fields do not affect the 12 bytes */
	{
		unsigned char h[512];
		TAR *t2;

		make_hdr(h, 'L', "././@LongLink", NULL, 5, NULL, 0);
		memset(h + H_MTIME, 0xAB, 12);	/* immediately after size */
		memset(h + H_UID, 0xCD, 8);
		hdr_rechecksum(h);

		arc_reset();
		TASK_CHECK(arc_add(h, 512) == 0);
		TASK_CHECK(arc_payload((const unsigned char *)"adj01", 5,
				       0xEE) == 0);
		TASK_CHECK(arc_hdr(REGTYPE, "adj", NULL, 0, NULL, 0) == 0);
		t2 = open_global_arc(TAR_GNU);
		TASK_CHECK(t2 != NULL);
		TASK_CHECK(th_read(t2) == 0);
		TASK_CHECK(strcmp(th_get_pathname(t2), "adj01") == 0);
		TASK_CHECK(check_close_frees(0, t2) == 0);
	}
	return 0;
}

/* Direct bounded-decoder family crossing the native size_t boundary. */
static int
case_limits_size_boundary(void)
{
	size_t v = 0;
	char buf[64];
	unsigned char *exact12;

	/* the maximal 12-octal-digit field decodes exactly to 8^12 - 1 */
	TASK_CHECK(gnu_ext_decode_size(
		(const char *)((unsigned char[12]){
			'7','7','7','7','7','7','7','7','7','7','7','7' }),
		12, &v) == 0);
	TASK_CHECK(v == ((size_t)0xFFFFFFFFFU));

	/* exact 12-byte heap object: an over-read would trip ASan */
	exact12 = test_malloc(12);
	TASK_CHECK(exact12 != NULL);
	memset(exact12, '7', 12);
	v = 0;
	TASK_CHECK(gnu_ext_decode_size((char *)exact12, 12, &v) == 0);
	TASK_CHECK(v == ((size_t)0xFFFFFFFFFU));
	test_free(exact12);

	/* SIZE_MAX itself round-trips; appending a digit overflows */
	snprintf(buf, sizeof(buf), "%zo", SIZE_MAX);
	TASK_CHECK(gnu_ext_decode_size(buf, strlen(buf), &v) == 0);
	TASK_CHECK(v == SIZE_MAX);
	TASK_CHECK(strlen(buf) + 2 < sizeof(buf));
	buf[strlen(buf)] = '0';
	buf[strlen(buf) + 1] = '\0';
	TASK_CHECK(gnu_ext_decode_size(buf, strlen(buf), &v) == -1);
	TASK_CHECK(errno == EINVAL);

	/* a long run of 7s is unrepresentable */
	memset(buf, '7', sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	TASK_CHECK(gnu_ext_decode_size(buf, 40, &v) == -1);
	TASK_CHECK(errno == EINVAL);

	/* 32-bit size_t boundary pair, expectations adapt to platform */
	TASK_CHECK(gnu_ext_decode_size("37777777777", 11, &v) == 0);
	TASK_CHECK(v == (size_t)0xFFFFFFFFU);
	if ((unsigned long long)SIZE_MAX >= 0x100000000ULL)
	{
		TASK_CHECK(gnu_ext_decode_size("40000000000", 11, &v) == 0);
		TASK_CHECK(v == (size_t)0x100000000ULL);
	}
	else
	{
		TASK_CHECK(gnu_ext_decode_size("40000000000", 11, &v) == -1);
		TASK_CHECK(errno == EINVAL);
	}

	/* grammar helpers on an empty length are rejected */
	TASK_CHECK(gnu_ext_decode_size("", 0, &v) == -1);
	TASK_CHECK(errno == EINVAL);
	return 0;
}

/*
** ###################################################################
** FAILURE GROUP
** ###################################################################
*/

/* A positive short read on the very first header is EINVAL. */
static int
case_failure_first_header_short(void)
{
	TAR *t;

	arc_reset();
	TASK_CHECK(arc_add((const unsigned char *)"partial-header-bytes",
			   20) == 0);
	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(hook_live_count == 1);

	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == EINVAL);
	TASK_CHECK(check_replay_dead(0, t, EINVAL) == 0);
	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* Short read on a later extension header releases pending state. */
static int
case_failure_ext_header_short(void)
{
	unsigned char partial[200];
	TAR *t;

	memset(partial, 'H', sizeof(partial));
	arc_reset();
	TASK_CHECK(arc_ext('K', (const unsigned char *)"pending-link", 12,
			   0, NULL) == 0);
	TASK_CHECK(arc_add(partial, sizeof(partial)) == 0);

	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == EINVAL);
	TASK_CHECK(check_gnu_null(t) == 0);
	TASK_CHECK(check_replay_dead(0, t, EINVAL) == 0);
	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* EIO from an extension header reader is preserved. */
static int
case_failure_ext_header_eio(void)
{
	TAR *t;

	arc_reset();
	TASK_CHECK(arc_ext('K', (const unsigned char *)"pending-link", 12,
			   0, NULL) == 0);
	TASK_CHECK(arc_hdr('L', "././@LongLink", NULL, 1, NULL, 0) == 0);

	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	io_attach(0, arc_buf, arc_len);	/* reset counters, then arm */
	t->fd = 0;
	ios[0].fail_on_read = 3;	/* K hdr, K payload, L hdr */
	ios[0].fail_kind = FAIL_EIO;
	ios[0].io_errno = EIO;

	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == EIO);
	TASK_CHECK(check_gnu_null(t) == 0);
	TASK_CHECK(check_replay_dead(0, t, EIO) == 0);
	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* Short read of extension payload. */
static int
case_failure_payload_short(void)
{
	TAR *t;

	arc_reset();
	TASK_CHECK(arc_hdr('L', "././@LongLink", NULL, 513, NULL, 0) == 0);
	TASK_CHECK(arc_payload(NULL, 513, 0) == 0);

	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	ios[0].fail_on_read = 2;	/* first payload block */
	ios[0].fail_kind = FAIL_SHORT;
	ios[0].short_ret = 257;

	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == EINVAL);
	TASK_CHECK(check_gnu_null(t) == 0);
	TASK_CHECK(check_replay_dead(0, t, EINVAL) == 0);
	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* EIO on payload read preserves errno. */
static int
case_failure_payload_eio(void)
{
	TAR *t;

	arc_reset();
	TASK_CHECK(arc_hdr('K', "././@LongLink", NULL, 9, NULL, 0) == 0);
	TASK_CHECK(arc_payload((const unsigned char *)"linkvalue", 9, 0) == 0);

	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	ios[0].fail_on_read = 2;
	ios[0].fail_kind = FAIL_EIO;
	ios[0].io_errno = EIO;

	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == EIO);
	TASK_CHECK(check_replay_dead(0, t, EIO) == 0);
	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* EIO on the second block of a two-block payload. */
static int
case_failure_payload_second_block(void)
{
	TAR *t;

	arc_reset();
	TASK_CHECK(arc_hdr('L', "././@LongLink", NULL, 1024, NULL, 0) == 0);
	TASK_CHECK(arc_payload(NULL, 1024, 0) == 0);

	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	ios[0].fail_on_read = 3;	/* hdr, block 1, block 2 */
	ios[0].fail_kind = FAIL_EIO;
	ios[0].io_errno = EIO;

	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == EIO);
	TASK_CHECK(check_replay_dead(0, t, EIO) == 0);
	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* Short read of the terminal ordinary header. */
static int
case_failure_ordinary_short(void)
{
	TAR *t;

	arc_reset();
	TASK_CHECK(arc_ext('L', (const unsigned char *)"long-name-here",
			   14, 0, NULL) == 0);
	TASK_CHECK(arc_hdr(REGTYPE, "ord", NULL, 0, NULL, 0) == 0);

	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	ios[0].fail_on_read = 3;	/* L hdr, payload, ordinary */
	ios[0].fail_kind = FAIL_SHORT;
	ios[0].short_ret = 300;

	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == EINVAL);
	TASK_CHECK(check_gnu_null(t) == 0);
	TASK_CHECK(check_replay_dead(0, t, EINVAL) == 0);
	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* EIO on the terminal ordinary header. */
static int
case_failure_ordinary_eio(void)
{
	TAR *t;

	arc_reset();
	TASK_CHECK(arc_ext('K', (const unsigned char *)"long-link-here",
			   14, 0, NULL) == 0);
	TASK_CHECK(arc_hdr(REGTYPE, "ord", NULL, 0, NULL, 0) == 0);

	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	ios[0].fail_on_read = 3;
	ios[0].fail_kind = FAIL_EIO;
	ios[0].io_errno = EIO;

	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == EIO);
	TASK_CHECK(check_replay_dead(0, t, EIO) == 0);
	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* Extension followed immediately by clean EOF: dangling chain. */
static int
case_failure_dangling_eof(void)
{
	TAR *t;

	arc_reset();
	TASK_CHECK(arc_ext('L', (const unsigned char *)"orphan", 6,
			   0, NULL) == 0);
	/* arc ends right after the payload: reader returns 0 */

	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == EINVAL);
	TASK_CHECK(check_gnu_null(t) == 0);
	TASK_CHECK(check_replay_dead(0, t, EINVAL) == 0);
	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* Extension followed by the two zero terminator blocks. */
static int
case_failure_dangling_eot(void)
{
	unsigned char zeros[1024];
	TAR *t;

	memset(zeros, 0, sizeof(zeros));
	arc_reset();
	TASK_CHECK(arc_ext('K', (const unsigned char *)"orphan-link", 11,
			   0, NULL) == 0);
	TASK_CHECK(arc_add(zeros, sizeof(zeros)) == 0);

	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == EINVAL);
	TASK_CHECK(check_replay_dead(0, t, EINVAL) == 0);
	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* Extension followed by a truncated ordinary header. */
static int
case_failure_dangling_truncated(void)
{
	unsigned char bits[123];
	TAR *t;

	memset(bits, 0x41, sizeof(bits));
	arc_reset();
	TASK_CHECK(arc_ext('L', (const unsigned char *)"orphan2", 7,
			   0, NULL) == 0);
	TASK_CHECK(arc_add(bits, sizeof(bits)) == 0);

	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == EINVAL);
	TASK_CHECK(check_replay_dead(0, t, EINVAL) == 0);
	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* An early NUL inside declared content is rejected, not truncated. */
static int
case_failure_early_nul(void)
{
	unsigned char decl[8];
	TAR *t;

	memcpy(decl, "ab\0cde", 6);	/* NUL at index 2, n = 6 */
	arc_reset();
	TASK_CHECK(arc_ext('L', decl, 6, 0, NULL) == 0);
	TASK_CHECK(arc_hdr(REGTYPE, "never", NULL, 0, NULL, 0) == 0);

	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == EINVAL);
	TASK_CHECK(check_gnu_null(t) == 0);
	TASK_CHECK(check_replay_dead(0, t, EINVAL) == 0);
	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* Malformed 12-byte size field on an otherwise checksum-valid header. */
static int
case_failure_bad_size(void)
{
	static const unsigned char f[12] =
		{ '0','0','0','0','0','0','0','0','0','0','8',' ' };
	TAR *t;

	arc_reset();
	TASK_CHECK(arc_hdr('L', "././@LongLink", NULL, 0, f, 0) == 0);
	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == EINVAL);
	TASK_CHECK(ios[0].reads == 1);
	TASK_CHECK(check_replay_dead(0, t, EINVAL) == 0);
	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* Bad checksum on the ordinary header fails the logical record. */
static int
case_failure_bad_checksum(void)
{
	TAR *t;

	arc_reset();
	TASK_CHECK(arc_ext('L', (const unsigned char *)"ok-name", 7,
			   0, NULL) == 0);
	TASK_CHECK(arc_hdr(REGTYPE, "crc", NULL, 0, NULL, HF_BAD_CRC) == 0);

	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == EINVAL);
	TASK_CHECK(check_gnu_null(t) == 0);
	TASK_CHECK(check_replay_dead(0, t, EINVAL) == 0);
	TASK_CHECK(check_close_frees(0, t) == 0);

	/* and a bad extension header checksum fails likewise */
	arc_reset();
	TASK_CHECK(arc_hdr('L', "././@LongLink", NULL, 1, NULL,
			   HF_BAD_CRC) == 0);
	TASK_CHECK(arc_payload((const unsigned char *)"x", 1, 0) == 0);
	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == EINVAL);
	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* Setter cannot clear a sticky read failure. */
static int
case_failure_sticky_setter(void)
{
	TAR *t;

	arc_reset();
	TASK_CHECK(arc_hdr('L', "././@LongLink", NULL, 9, NULL, 0) == 0);
	TASK_CHECK(arc_payload((const unsigned char *)"123456789", 9, 0) == 0);
	TASK_CHECK(arc_hdr(REGTYPE, "r", NULL, 0, NULL, 0) == 0);
	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(tar_set_gnu_extension_limits(t, 4, 8) == 0);

	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == E2BIG);

	/* raising limits does not revive the handle */
	TASK_CHECK(tar_set_gnu_extension_limits(t, 1024 * 1024, 64) == 0);
	TASK_CHECK(check_replay_dead(0, t, E2BIG) == 0);

	/* invalid setter also leaves the sticky state untouched */
	TASK_CHECK(tar_set_gnu_extension_limits(t, 0, 0) == -1);
	TASK_CHECK(errno == EINVAL);
	TASK_CHECK(check_replay_dead(0, t, E2BIG) == 0);

	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

/* Every allocation invocation in a multi-extension read can fail. */
static int
case_failure_alloc_sites(void)
{
	int site;

	arc_reset();
	TASK_CHECK(arc_ext('L', (const unsigned char *)"alloc-path-1", 12,
			   0, NULL) == 0);
	TASK_CHECK(arc_ext('K', (const unsigned char *)"alloc-link", 10,
			   0, NULL) == 0);
	TASK_CHECK(arc_ext('L', (const unsigned char *)"alloc-path-3", 12,
			   0, NULL) == 0);
	TASK_CHECK(arc_hdr(REGTYPE, "alloc", NULL, 0, NULL, 0) == 0);

	for (site = 1; site <= 3; site++)
	{
		TAR *t;
		int reads_before;

		io_attach(0, arc_buf, arc_len);
		t = mem_open(0, TAR_GNU);
		TASK_CHECK(t != NULL);
		TASK_CHECK(hook_live_count == 1);	/* TAR handle */

		hook_alloc_calls = 0;
		hook_fail_at = site;
		errno = 0;
		TASK_CHECK(th_read(t) == -1);
		TASK_CHECK(errno == ENOMEM);
		hook_fail_at = 0;

		/* all pending allocations were rolled back */
		TASK_CHECK(hook_live_count == 1);
		TASK_CHECK(check_gnu_null(t) == 0);

		/* sticky ENOMEM replay: no allocations or reads */
		reads_before = ios[0].reads;
		TASK_CHECK(th_read(t) == -1);
		TASK_CHECK(errno == ENOMEM);
		TASK_CHECK(ios[0].reads == reads_before);
		TASK_CHECK(hook_live_count == 1);

		TASK_CHECK(tar_close(t) == 0);
		TASK_CHECK(hook_live_count == 0);
	}
	return 0;
}

/* A successful record's owned strings are actually released by close. */
static int
case_failure_close_releases_success(void)
{
	TAR *t;

	arc_reset();
	TASK_CHECK(arc_ext('L', (const unsigned char *)"close-path", 10,
			   0, NULL) == 0);
	TASK_CHECK(arc_ext('K', (const unsigned char *)"close-link", 10,
			   0, NULL) == 0);
	TASK_CHECK(arc_hdr(REGTYPE, "c", NULL, 0, NULL, 0) == 0);

	t = open_global_arc(TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(hook_live_count == 3);	/* TAR + two names */
	TASK_CHECK(tar_close(t) == 0);
	TASK_CHECK(ios[0].closes == 1);
	TASK_CHECK(hook_live_count == 0);
	return 0;
}

/*
** ###################################################################
** MODEL GROUP: independent override/budget/termination model
** ###################################################################
*/

#define MODEL_MAX_EXT 12
#define MODEL_MAX_NAME 2048

struct m_ext
{
	char kind;
	size_t n;		/* declared length */
	size_t name_len;	/* non-NUL bytes */
	int nul_terminated;
	unsigned char bytes[MODEL_MAX_NAME + 1];
	unsigned char pad;
};

struct rng
{
	const unsigned char *fuzz;
	size_t fuzz_n;
	uint64_t state;
};

static uint64_t
splitmix64(uint64_t *x)
{
	uint64_t z;

	*x += 0x9e3779b97f4a7c15ULL;
	z = *x;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

static uint32_t
rng_u32(struct rng *r)
{
	uint32_t v;

	if (r->fuzz_n >= 4)
	{
		v = (uint32_t)r->fuzz[0]
		  | ((uint32_t)r->fuzz[1] << 8)
		  | ((uint32_t)r->fuzz[2] << 16)
		  | ((uint32_t)r->fuzz[3] << 24);
		r->fuzz += 4;
		r->fuzz_n -= 4;
		return v;
	}
	return (uint32_t)(splitmix64(&r->state) >> 32);
}

static uint32_t
rng_below(struct rng *r, uint32_t bound)
{
	if (bound == 0)
		return 0;
	return rng_u32(r) % bound;
}

/*
** Build one bounded pseudo-random chain and compare the library result
** with an independently simulated model.  Returns non-zero only when a
** TASK_CHECK fails.
*/
static int
model_run(struct rng *r)
{
	static const size_t interesting[] = {
		1, 2, 3, 16, 63, 64, 100, 150, 255, 510, 511, 512, 513,
		514, 1023, 1024, 1025, 1500, 2047, 2048
	};
	struct m_ext exts[MODEL_MAX_EXT];
	int next_count;
	int i;
	size_t byte_limit, header_limit;
	int limit_choice;
	int ending;
	size_t model_bytes = 0;
	size_t model_headers = 0;
	int fail_index = -1;
	int fail_errno = 0;
	int model_reads = 0;
	size_t model_pos = 0;
	const unsigned char *want_l = NULL;
	size_t want_l_len = 0;
	const unsigned char *want_k = NULL;
	size_t want_k_len = 0;
	int library_rc;
	TAR *t;

	TASK_CHECK(hook_live_count == 0);

	next_count = (int)rng_below(r, MODEL_MAX_EXT + 1); /* 0..12 */

	for (i = 0; i < next_count; i++)
	{
		struct m_ext *e = &exts[i];
		size_t n;

		memset(e, 0, sizeof(*e));
		e->kind = rng_below(r, 2) ? 'K' : 'L';

		if (rng_below(r, 2) == 0)
			n = interesting[rng_below(r,
				(uint32_t)(sizeof(interesting) /
					   sizeof(interesting[0])))];
		else
			n = 1 + rng_below(r, 70);

		e->nul_terminated = (int)rng_below(r, 2);
		if (n > MODEL_MAX_NAME)
			n = MODEL_MAX_NAME;
		if (e->nul_terminated && n < 2)
			e->nul_terminated = 0;	/* keep a non-empty name */
		e->n = n;
		e->name_len = n - (e->nul_terminated ? 1 : 0);
		fill_safe(e->bytes, e->name_len, rng_u32(r));
		e->bytes[e->name_len] = '\0';
		if (e->nul_terminated)
			e->bytes[n - 1] = '\0';
		e->pad = rng_below(r, 3) == 0 ? (unsigned char)'Z' : 0;
	}

	/* choose the byte budget around the simulated total */
	{
		size_t total = 0;
		for (i = 0; i < next_count; i++)
			total += exts[i].n;

		limit_choice = (int)rng_below(r, 5);
		switch (limit_choice)
		{
		case 0: byte_limit = total > 0 ? total : 1; break;
		case 1: byte_limit = total + 1; break;
		case 2: byte_limit = total > 1 ? total - 1 : 1; break;
		case 3:
		{
			static const size_t smalls[] =
				{ 1, 2, 7, 16, 511, 512, 513, 1024 };
			byte_limit = smalls[rng_below(r, 8)];
			break;
		}
		default: byte_limit = (size_t)1 << 20; break;
		}

		header_limit = 1 + rng_below(r, (uint32_t)next_count + 2);
		if (rng_below(r, 6) == 0)
			header_limit = 64;
	}

	/* independent simulation of rejection point and final value */
	for (i = 0; i < next_count; i++)
	{
		struct m_ext *e = &exts[i];
		size_t blocks = (e->n + 511) / 512;

		model_reads++;		/* extension header */
		model_pos += 512;
		model_headers++;

		if (model_headers > header_limit)
		{
			fail_index = i;
			fail_errno = E2BIG;
			break;
		}
		if (e->n > byte_limit - model_bytes)
		{
			fail_index = i;
			fail_errno = E2BIG;
			break;
		}
		model_bytes += e->n;
		model_reads += (int)blocks;
		model_pos += blocks * 512;

		if (e->kind == 'L')
		{
			want_l = e->bytes;
			want_l_len = e->name_len;
		}
		else
		{
			want_k = e->bytes;
			want_k_len = e->name_len;
		}
	}

	/* perturb how the chain terminates */
	ending = (int)rng_below(r, 5);	/* 0 eof,1 eot,2 trunc,3/4 ordinary */

	arc_reset();
	for (i = 0; i < next_count; i++)
	{
		TASK_CHECK(arc_ext(exts[i].kind, exts[i].bytes, exts[i].n,
				   exts[i].pad, NULL) == 0);
	}

	if (fail_index < 0)
	{
		if (ending == 0)
		{
			/* clean EOF: no more bytes */
			model_reads++;
		}
		else if (ending == 1)
		{
			unsigned char zeros[1024];
			memset(zeros, 0, sizeof(zeros));
			TASK_CHECK(arc_add(zeros, sizeof(zeros)) == 0);
			model_reads += 2;
			model_pos += 1024;
		}
		else if (ending == 2)
		{
			unsigned char bits[137];
			memset(bits, 0x55, sizeof(bits));
			TASK_CHECK(arc_add(bits, sizeof(bits)) == 0);
			model_reads++;
		}
		else
		{
			TASK_CHECK(arc_hdr(REGTYPE, "model-rec", "model-lnk",
					   0, NULL, 0) == 0);
			model_reads++;
			model_pos += 512;
			/* second plain record to test non-inheritance */
			TASK_CHECK(arc_hdr(REGTYPE, "model-second", NULL,
					   0, NULL, 0) == 0);
		}
	}

	io_attach(0, arc_buf, arc_len);
	t = mem_open(0, TAR_GNU);
	TASK_CHECK(t != NULL);
	TASK_CHECK(tar_set_gnu_extension_limits(t, byte_limit,
					       header_limit) == 0);

	errno = 0;
	library_rc = th_read(t);

	if (fail_index >= 0)
	{
		TASK_CHECK(library_rc == -1);
		TASK_CHECK(errno == fail_errno);
		TASK_CHECK(ios[0].reads == model_reads);
		TASK_CHECK(ios[0].pos == model_pos);
		TASK_CHECK(check_replay_dead(0, t, fail_errno) == 0);
		TASK_CHECK(check_close_frees(0, t) == 0);
		return 0;
	}

	if (ending == 0 || ending == 1 || ending == 2)
	{
		/*
		** With no extension consumed yet, clean EOF/EOT is a normal
		** EOF (return 1); a positive short header read is EINVAL.
		** With one or more extensions, every such ending makes the
		** chain dangling and is EINVAL + sticky.
		*/
		if (next_count == 0 && ending != 2)
		{
			TASK_CHECK(library_rc == 1);
			TASK_CHECK(check_close_frees(0, t) == 0);
			return 0;
		}

		TASK_CHECK(library_rc == -1);
		TASK_CHECK(errno == EINVAL);
		TASK_CHECK(check_replay_dead(0, t, EINVAL) == 0);
		TASK_CHECK(check_close_frees(0, t) == 0);
		return 0;
	}

	/* ordinary header: successful logical record */
	TASK_CHECK(library_rc == 0);
	TASK_CHECK(t->th_buf.typeflag == REGTYPE);

	if (want_l != NULL)
	{
		TASK_CHECK(t->th_buf.gnu_longname != NULL);
		TASK_CHECK(strlen(t->th_buf.gnu_longname) == want_l_len);
		TASK_CHECK(memcmp(t->th_buf.gnu_longname, want_l,
				  want_l_len) == 0);
		TASK_CHECK(strcmp(th_get_pathname(t),
				  (const char *)want_l) == 0);
	}
	else
	{
		TASK_CHECK(t->th_buf.gnu_longname == NULL);
		TASK_CHECK(strcmp(th_get_pathname(t), "model-rec") == 0);
	}

	if (want_k != NULL)
	{
		TASK_CHECK(t->th_buf.gnu_longlink != NULL);
		TASK_CHECK(strlen(t->th_buf.gnu_longlink) == want_k_len);
		TASK_CHECK(memcmp(t->th_buf.gnu_longlink, want_k,
				  want_k_len) == 0);
		TASK_CHECK(strcmp(th_get_linkname(t),
				  (const char *)want_k) == 0);
	}
	else
	{
		TASK_CHECK(t->th_buf.gnu_longlink == NULL);
		TASK_CHECK(strcmp(th_get_linkname(t), "model-lnk") == 0);
	}

	TASK_CHECK(ios[0].pos == model_pos);

	/* next record does not inherit either extension */
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(t->th_buf.gnu_longname == NULL);
	TASK_CHECK(t->th_buf.gnu_longlink == NULL);
	TASK_CHECK(strcmp(th_get_pathname(t), "model-second") == 0);

	/* and then the in-memory archive ends cleanly */
	TASK_CHECK(th_read(t) == 1);

	TASK_CHECK(check_close_frees(0, t) == 0);
	return 0;
}

static int
model_run_seed(uint64_t seed)
{
	struct rng r;

	memset(&r, 0, sizeof(r));
	r.fuzz = NULL;
	r.fuzz_n = 0;
	r.state = seed ? seed : 1;
	return model_run(&r);
}

static int
case_model_seeds_1_80(void)
{
	uint64_t s;

	for (s = 1; s <= 80; s++)
		TASK_CHECK(model_run_seed(s) == 0);
	return 0;
}

static int
case_model_seeds_81_160(void)
{
	uint64_t s;

	for (s = 81; s <= 160; s++)
		TASK_CHECK(model_run_seed(s) == 0);
	return 0;
}

static int
case_model_seeds_161_240(void)
{
	uint64_t s;

	for (s = 161; s <= 240; s++)
		TASK_CHECK(model_run_seed(s) == 0);
	return 0;
}

static int
case_model_seeds_241_320(void)
{
	uint64_t s;

	for (s = 241; s <= 320; s++)
		TASK_CHECK(model_run_seed(s) == 0);
	return 0;
}

/*
** ===================================================================
** Fuzzer entry: reuse the bounded generator/core plus a raw in-memory
** parse of the exact input bytes.  Nothing touches the filesystem.
** ===================================================================
*/

static void
raw_parse_memory(const unsigned char *data, size_t size)
{
	TAR *t;
	int rounds;

	io_attach(15, data, size);
	t = mem_open(15, TAR_GNU);
	if (t == NULL)
		return;

	for (rounds = 0; rounds < 4096; rounds++)
	{
		int rc = th_read(t);
		if (rc == 0)
		{
			/* exercise path decoding on the owned/bounded name */
			(void)th_get_pathname(t);
			continue;
		}
		break;	/* 1 EOF or -1 sticky error: either is fine */
	}
	tar_close(t);
}

int
LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
	if (size != 0 && data != NULL)
	{
		struct rng r;

		memset(&r, 0, sizeof(r));
		r.fuzz = data;
		r.fuzz_n = size;
		r.state = 0xd1b54a32d192ed03ULL;
		(void)model_run(&r);
	}
	raw_parse_memory(data, size);
	return 0;
}

/*
** ===================================================================
** Task case table (consumed by task_support/driver.c)
** ===================================================================
*/

const struct task_case task_cases[] = {
	/* layout */
	{ "layout", "L-simple", case_layout_L_simple },
	{ "layout", "K-simple", case_layout_K_simple },
	{ "layout", "L-K-order", case_layout_LK },
	{ "layout", "K-L-order", case_layout_KL },
	{ "layout", "L-K-L-repeat", case_layout_LKL },
	{ "layout", "repeated-override", case_layout_repeat_override },
	{ "layout", "next-record-clean", case_layout_next_record_clean },
	{ "layout", "body-position", case_layout_body_position },
	{ "layout", "lengths-plain", case_layout_lengths_plain },
	{ "layout", "lengths-terminal-NUL", case_layout_lengths_terminal_nul },
	{ "layout", "plain-and-EOF", case_layout_plain_eof },
	{ "layout", "magic-option", case_layout_magic_option },

	/* limits + bounded decode */
	{ "limits", "exact-bytes", case_limits_exact_bytes },
	{ "limits", "accumulates-overridden",
	  case_limits_accumulates_overridden },
	{ "limits", "headers-independent", case_limits_headers },
	{ "limits", "defaults", case_limits_defaults },
	{ "limits", "two-handles-independent", case_limits_two_handles },
	{ "limits", "setter-validation", case_limits_setter_validation },
	{ "limits", "size-grammar-12-bytes", case_limits_size_grammar },
	{ "limits", "size-boundary-family", case_limits_size_boundary },

	/* failure */
	{ "failure", "first-header-short", case_failure_first_header_short },
	{ "failure", "ext-header-short", case_failure_ext_header_short },
	{ "failure", "ext-header-EIO", case_failure_ext_header_eio },
	{ "failure", "payload-short", case_failure_payload_short },
	{ "failure", "payload-EIO", case_failure_payload_eio },
	{ "failure", "payload-second-block-EIO",
	  case_failure_payload_second_block },
	{ "failure", "ordinary-header-short", case_failure_ordinary_short },
	{ "failure", "ordinary-header-EIO", case_failure_ordinary_eio },
	{ "failure", "dangling-EOF", case_failure_dangling_eof },
	{ "failure", "dangling-EOT", case_failure_dangling_eot },
	{ "failure", "dangling-truncated", case_failure_dangling_truncated },
	{ "failure", "early-NUL", case_failure_early_nul },
	{ "failure", "bad-size", case_failure_bad_size },
	{ "failure", "bad-checksum", case_failure_bad_checksum },
	{ "failure", "sticky-setter", case_failure_sticky_setter },
	{ "failure", "alloc-sites", case_failure_alloc_sites },
	{ "failure", "close-releases-success",
	  case_failure_close_releases_success },

	/* model */
	{ "model", "fixed-seeds-1-80", case_model_seeds_1_80 },
	{ "model", "fixed-seeds-81-160", case_model_seeds_81_160 },
	{ "model", "fixed-seeds-161-240", case_model_seeds_161_240 },
	{ "model", "fixed-seeds-241-320", case_model_seeds_241_320 }
};

const size_t task_case_count =
	sizeof(task_cases) / sizeof(task_cases[0]);
