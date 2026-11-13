/*
**  gnu_extension_contract.c - defensive regression tests for the GNU
**  long-name / long-link extension read chain.
**
**  The archive images below are built independently of the library: the
**  test writes the 512-byte headers itself and computes the checksum
**  directly.  The only library entry points exercised while reading are
**  tar_fdopen(), th_read(), th_get_pathname(), th_get_linkname(),
**  tar_set_gnu_extension_limits(), and tar_close().
*/

#include "check.h"
#include "libtar.h"

#ifdef LIBTAR_FUZZ_STANDALONE
/*
** Standalone libFuzzer build: the driver's assertion sink is not linked
** in, so provide a trivial one.  Build with e.g.
**   cc -std=c99 -fsanitize=fuzzer,address,undefined \
**      -DLIBTAR_FUZZ_STANDALONE -I. -Itask_support -Ilib ... \
**      tests/gnu_extension_contract.c <library objects>
*/
int
task_check(int value, const char *expr, const char *file, int line)
{
	(void)expr;
	(void)file;
	(void)line;
	return value;
}
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* internal allocation hook, declared here rather than by including
   internal.h, so the test also works when only the public include path is
   available */
typedef void *(*libtar_malloc_func_t)(size_t);
typedef void (*libtar_free_func_t)(void *);
extern void libtar_set_alloc_hooks(libtar_malloc_func_t malloc_func,
				   libtar_free_func_t free_func);

#define BLK 512
#define MAX_EXT 12
#define MAX_LEN 2048

/* ------------------------------------------------------------------ */
/* growable byte buffer                                               */
/* ------------------------------------------------------------------ */

typedef struct
{
	unsigned char *p;
	size_t len;
	size_t cap;
} abuf_t;

static void
ab_reserve(abuf_t *a, size_t extra)
{
	size_t need = a->len + extra;
	size_t cap;

	if (need <= a->cap)
		return;
	cap = a->cap ? a->cap : 4096;
	while (cap < need)
		cap *= 2;
	a->p = (unsigned char *)realloc(a->p, cap);
	if (a->p == NULL)
		abort();
	a->cap = cap;
}

static void
ab_bytes(abuf_t *a, const void *src, size_t n)
{
	ab_reserve(a, n);
	memcpy(a->p + a->len, src, n);
	a->len += n;
}

static size_t
ab_zero_blocks(abuf_t *a, size_t nblocks)
{
	size_t off = a->len;

	ab_reserve(a, nblocks * BLK);
	memset(a->p + a->len, 0, nblocks * BLK);
	a->len += nblocks * BLK;
	return off;
}

/* ------------------------------------------------------------------ */
/* independent tar header construction                                */
/* ------------------------------------------------------------------ */

static void
put_octal(unsigned char *field, size_t width, unsigned long long value)
{
	char tmp[32];
	int n = snprintf(tmp, sizeof(tmp), "%0*llo", (int)(width - 1), value);

	if (n < 0)
		abort();
	if ((size_t)n > width - 1)
		n = (int)(width - 1);
	memcpy(field, tmp, (size_t)n);
	/* header is zero-filled already, so the terminator is implicit */
}

static void
hdr_checksum(unsigned char *b)
{
	unsigned int sum = 0;
	char tmp[16];
	size_t i;

	memset(b + 148, ' ', 8);
	for (i = 0; i < BLK; i++)
		sum += b[i];
	snprintf(tmp, sizeof(tmp), "%06o", sum & 0777777U);
	memcpy(b + 148, tmp, 6);
	b[154] = '\0';
	b[155] = ' ';
}

static void
hdr_base(unsigned char *b, char type, const char *name, const char *link)
{
	memset(b, 0, BLK);

	if (name != NULL)
		strncpy((char *)b, name, 99);
	put_octal(b + 100, 8, 0644);	/* mode */
	put_octal(b + 108, 8, 0);	/* uid */
	put_octal(b + 116, 8, 0);	/* gid */
	put_octal(b + 136, 12, 0);	/* mtime */
	b[156] = type;
	if (link != NULL)
		strncpy((char *)b + 157, link, 99);
	memcpy(b + 257, "ustar", 5);
	b[262] = '\0';
	memcpy(b + 263, "00", 2);
	strcpy((char *)b + 265, "user");
	strcpy((char *)b + 297, "group");
}

static void
hdr_ordinary(unsigned char b[BLK], char type, const char *name,
	     const char *link, unsigned long long size)
{
	hdr_base(b, type, name, link);
	put_octal(b + 124, 12, size);
	hdr_checksum(b);
}

/* GNU extension header with a caller-supplied raw 12-byte size field */
static void
hdr_extension_raw(unsigned char b[BLK], char type,
		  const unsigned char raw_size[12])
{
	hdr_base(b, type, "././@LongLink", "././@LongLink");
	memcpy(b + 124, raw_size, 12);
	/* the field immediately after size is deliberately non-octal: a
	   decoder that scans past its own 12 bytes will trip over it */
	memset(b + 136, 'Q', 12);
	hdr_checksum(b);
}

static void
raw_size_value(unsigned char raw[12], unsigned long long value)
{
	char tmp[16];
	int n = snprintf(tmp, sizeof(tmp), "%011llo", value);

	if (n < 0 || n != 11)
		abort();
	memcpy(raw, tmp, 11);
	raw[11] = '\0';
}

/* append an extension header + payload; returns payload offset via *poff */
static void
ab_extension(abuf_t *a, char type, const char *content, size_t n,
	     int trailing_nul, int nonzero_pad, size_t *poff)
{
	unsigned char raw[12];
	unsigned char hdr[BLK];
	size_t blocks = n / BLK + (n % BLK ? 1 : 0);
	size_t off;

	raw_size_value(raw, n);
	hdr_extension_raw(hdr, type, raw);
	ab_bytes(a, hdr, BLK);

	off = ab_zero_blocks(a, blocks);
	if (poff != NULL)
		*poff = off;
	if (trailing_nul)
	{
		memcpy(a->p + off, content, n - 1);
		a->p[off + n - 1] = '\0';
	}
	else
		memcpy(a->p + off, content, n);
	if (nonzero_pad && n % BLK != 0)
		memset(a->p + off + n, 0xff, blocks * BLK - n);
}

static size_t
ab_extension_with_raw(abuf_t *a, char type, const unsigned char raw[12],
		      const char *content, size_t n, size_t *poff)
{
	unsigned char hdr[BLK];
	size_t blocks = n / BLK + (n % BLK ? 1 : 0);
	size_t off;

	hdr_extension_raw(hdr, type, raw);
	ab_bytes(a, hdr, BLK);
	off = ab_zero_blocks(a, blocks);
	if (poff != NULL)
		*poff = off;
	memcpy(a->p + off, content, n);
	return off;
}

/* fill a safe relative name of exactly n characters (no NUL inside) */
static void
fill_name(char *dst, size_t n, char seed)
{
	size_t i;

	for (i = 0; i < n; i++)
		dst[i] = (char)('a' + (seed + i) % 26);
	dst[n] = '\0';
}

/* ------------------------------------------------------------------ */
/* in-memory tartype_t backend                                        */
/* ------------------------------------------------------------------ */

typedef struct
{
	unsigned char *data;
	size_t len;
	size_t pos;
	int reads;
	int closes;
	int fail_read;		/* 1-based read index returning -1 */
	int fail_errno;
	int short_read;		/* 1-based read index returning a short count */
} memio_t;

static memio_t *io_slots[8];

static int
io_attach(memio_t *m)
{
	int i;

	for (i = 0; i < 8; i++)
	{
		if (io_slots[i] == NULL)
		{
			io_slots[i] = m;
			return i;
		}
	}
	return -1;
}

static ssize_t
io_read(int fd, void *out, size_t n)
{
	memio_t *m = io_slots[fd];
	size_t take;

	m->reads++;

	if (m->fail_read == m->reads)
	{
		errno = m->fail_errno;
		return -1;
	}

	if (m->short_read == m->reads)
	{
		take = n > 1 ? n - 1 : 0;
		if (m->pos + take > m->len)
			take = m->len - m->pos;
		if (take > 0)
			memcpy(out, m->data + m->pos, take);
		m->pos += take;
		return (ssize_t)take;
	}

	if (m->pos >= m->len)
		return 0;
	take = m->len - m->pos;
	if (take > n)
		take = n;
	memcpy(out, m->data + m->pos, take);
	m->pos += take;
	return (ssize_t)take;
}

static int
io_close(int fd)
{
	memio_t *m = io_slots[fd];

	if (m != NULL)
	{
		m->closes++;
		io_slots[fd] = NULL;
	}
	return 0;
}

static tartype_t mem_type = { NULL, io_close, io_read, NULL };

static TAR *
open_mem(memio_t *m, int options)
{
	TAR *t = NULL;
	int fd;

	m->pos = 0;
	m->reads = 0;
	m->closes = 0;
	fd = io_attach(m);
	if (fd < 0)
		return NULL;
	if (tar_fdopen(&t, fd, "memory", &mem_type, O_RDONLY, 0, options) != 0)
	{
		io_slots[fd] = NULL;
		return NULL;
	}
	return t;
}

static void
mem_init(memio_t *m, const abuf_t *a)
{
	memset(m, 0, sizeof(*m));
	m->data = a->p;
	m->len = a->len;
}

/* ------------------------------------------------------------------ */
/* allocation accounting / failure hooks                              */
/* ------------------------------------------------------------------ */

static int hook_fail_at;	/* 1-based allocation index to fail, 0 = never */
static int hook_total;		/* allocations attempted in current read */
static int hook_live;		/* outstanding allocations */

static void *
hook_malloc(size_t n)
{
	void *p;

	hook_total++;
	if (hook_fail_at != 0 && hook_total == hook_fail_at)
		return NULL;
	p = malloc(n);
	if (p != NULL)
		hook_live++;
	return p;
}

static void
hook_free(void *p)
{
	if (p == NULL)
		return;
	hook_live--;
	free(p);
}

static void
hooks_reset(void)
{
	libtar_set_alloc_hooks(NULL, NULL);
	hook_fail_at = 0;
	hook_total = 0;
	hook_live = 0;
}

static void
hooks_account(void)
{
	libtar_set_alloc_hooks(hook_malloc, hook_free);
	hook_fail_at = 0;
	hook_total = 0;
	hook_live = 0;
}

static void
hooks_fail_at(int index)
{
	libtar_set_alloc_hooks(hook_malloc, hook_free);
	hook_fail_at = index;
	hook_total = 0;
	hook_live = 0;
}

/* ------------------------------------------------------------------ */
/* group: layout                                                      */
/* ------------------------------------------------------------------ */

static int
case_layout_l_plain(void)
{
	abuf_t a;
	memio_t m;
	TAR *t;
	unsigned char hdr[BLK];
	char name[160];
	size_t body_off;
	static const unsigned char body[7] = { 'B', 'O', 'D', 'Y', '0', '1', '2' };

	memset(&a, 0, sizeof(a));
	fill_name(name, 151, 3);

	ab_extension(&a, 'L', name, 151, 0, 0, NULL);
	body_off = a.len;
	hdr_ordinary(hdr, '0', "plain", NULL, sizeof(body));
	ab_bytes(&a, hdr, BLK);
	ab_bytes(&a, body, sizeof(body));

	mem_init(&m, &a);
	t = open_mem(&m, 0);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(t->th_buf.typeflag == '0');
	TASK_CHECK(th_get_size(t) == sizeof(body));
	TASK_CHECK(strcmp(th_get_pathname(t), name) == 0);
	TASK_CHECK(th_get_linkname(t)[0] == '\0');
	TASK_CHECK(m.pos == body_off + BLK);
	TASK_CHECK(memcmp(m.data + m.pos, body, sizeof(body)) == 0);
	TASK_CHECK(tar_close(t) == 0);
	TASK_CHECK(m.closes == 1);
	free(a.p);
	return 0;
}

static int
case_layout_k_only(void)
{
	abuf_t a;
	memio_t m;
	TAR *t;
	unsigned char hdr[BLK];
	char link[80];

	memset(&a, 0, sizeof(a));
	fill_name(link, 70, 7);

	ab_extension(&a, 'K', link, 70, 0, 0, NULL);
	hdr_ordinary(hdr, '1', "hard", NULL, 0);
	ab_bytes(&a, hdr, BLK);

	mem_init(&m, &a);
	t = open_mem(&m, 0);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(t->th_buf.typeflag == '1');
	TASK_CHECK(strcmp(th_get_pathname(t), "hard") == 0);
	TASK_CHECK(strcmp(th_get_linkname(t), link) == 0);
	TASK_CHECK(m.pos == a.len);
	TASK_CHECK(tar_close(t) == 0);
	free(a.p);
	return 0;
}

/* L-K and K-L; both names committed together */
static int
layout_pair(char first, char second)
{
	abuf_t a;
	memio_t m;
	TAR *t;
	unsigned char hdr[BLK];
	char lname[60];
	char llink[60];

	memset(&a, 0, sizeof(a));
	fill_name(lname, 41, 1);
	fill_name(llink, 51, 2);

	ab_extension(&a, first, first == 'L' ? lname : llink,
		     first == 'L' ? 41 : 51, 0, 0, NULL);
	ab_extension(&a, second, second == 'L' ? lname : llink,
		     second == 'L' ? 41 : 51, 0, 0, NULL);
	hdr_ordinary(hdr, '0', "plain", NULL, 0);
	ab_bytes(&a, hdr, BLK);

	mem_init(&m, &a);
	t = open_mem(&m, 0);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(strcmp(th_get_pathname(t), lname) == 0);
	TASK_CHECK(strcmp(th_get_linkname(t), llink) == 0);
	TASK_CHECK(m.pos == a.len);
	TASK_CHECK(tar_close(t) == 0);
	free(a.p);
	return 0;
}

static int
case_layout_l_k(void)
{
	return layout_pair('L', 'K');
}

static int
case_layout_k_l(void)
{
	return layout_pair('K', 'L');
}

static int
case_layout_l_k_l(void)
{
	abuf_t a;
	memio_t m;
	TAR *t;
	unsigned char hdr[BLK];
	char n1[40], n2[70], l1[50];

	memset(&a, 0, sizeof(a));
	fill_name(n1, 31, 1);
	fill_name(n2, 61, 9);
	fill_name(l1, 41, 5);

	ab_extension(&a, 'L', n1, 31, 0, 0, NULL);
	ab_extension(&a, 'K', l1, 41, 0, 0, NULL);
	ab_extension(&a, 'L', n2, 61, 0, 0, NULL);
	hdr_ordinary(hdr, '0', "plain", NULL, 0);
	ab_bytes(&a, hdr, BLK);

	mem_init(&m, &a);
	t = open_mem(&m, 0);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(strcmp(th_get_pathname(t), n2) == 0);
	TASK_CHECK(strcmp(th_get_linkname(t), l1) == 0);
	TASK_CHECK(tar_close(t) == 0);
	free(a.p);
	return 0;
}

/* same type repeated: last wins for both L and K */
static int
case_layout_override(void)
{
	abuf_t a;
	memio_t m;
	TAR *t;
	unsigned char hdr[BLK];
	char n1[20], n2[30], l1[20], l2[30];

	memset(&a, 0, sizeof(a));
	fill_name(n1, 11, 1);
	fill_name(n2, 21, 4);
	fill_name(l1, 11, 6);
	fill_name(l2, 21, 8);

	ab_extension(&a, 'L', n1, 11, 0, 0, NULL);
	ab_extension(&a, 'K', l1, 11, 0, 0, NULL);
	ab_extension(&a, 'L', n2, 21, 0, 0, NULL);
	ab_extension(&a, 'K', l2, 21, 0, 0, NULL);
	hdr_ordinary(hdr, '0', "plain", NULL, 0);
	ab_bytes(&a, hdr, BLK);

	mem_init(&m, &a);
	t = open_mem(&m, 0);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(strcmp(th_get_pathname(t), n2) == 0);
	TASK_CHECK(strcmp(th_get_linkname(t), l2) == 0);
	TASK_CHECK(strcmp(t->th_buf.gnu_longname, n2) == 0);
	TASK_CHECK(strcmp(t->th_buf.gnu_longlink, l2) == 0);
	TASK_CHECK(tar_close(t) == 0);
	free(a.p);
	return 0;
}

/* extensions apply to exactly one ordinary record */
static int
case_layout_next_record_clean(void)
{
	abuf_t a;
	memio_t m;
	TAR *t;
	unsigned char hdr[BLK];
	char lname[50];
	size_t second_end;

	memset(&a, 0, sizeof(a));
	fill_name(lname, 41, 2);

	ab_extension(&a, 'L', lname, 41, 0, 0, NULL);
	hdr_ordinary(hdr, '0', "first", NULL, 0);
	ab_bytes(&a, hdr, BLK);
	hdr_ordinary(hdr, '0', "second", NULL, 0);
	ab_bytes(&a, hdr, BLK);
	second_end = a.len;

	mem_init(&m, &a);
	t = open_mem(&m, 0);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(strcmp(th_get_pathname(t), lname) == 0);
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(strcmp(th_get_pathname(t), "second") == 0);
	TASK_CHECK(t->th_buf.gnu_longname == NULL);
	TASK_CHECK(t->th_buf.gnu_longlink == NULL);
	TASK_CHECK(m.pos == second_end);
	TASK_CHECK(tar_close(t) == 0);
	free(a.p);
	return 0;
}

/* declared lengths 1/511/512/513/1024, optional final NUL and padding */
static int
case_layout_lengths(void)
{
	static const size_t lens[] = { 1, 511, 512, 513, 1024 };
	size_t i;
	int nul, pad;

	for (i = 0; i < sizeof(lens) / sizeof(lens[0]); i++)
	{
		for (nul = 0; nul < 2; nul++)
		{
			abuf_t a;
			memio_t m;
			TAR *t;
			unsigned char hdr[BLK];
			char content[MAX_LEN + 1];
			char expect[MAX_LEN + 1];
			char llink[MAX_LEN + 1];
			size_t n = lens[i];
			size_t body_off;

			/* n == 1 with a final NUL would be an empty name;
			   keep the no-NUL form for that single case */
			if (n == 1 && nul)
				continue;

			memset(&a, 0, sizeof(a));
			fill_name(content, n, (char)(3 + i));
			if (nul)
			{
				memcpy(expect, content, n - 1);
				expect[n - 1] = '\0';
			}
			else
			{
				memcpy(expect, content, n);
				expect[n] = '\0';
			}
			fill_name(llink, n, (char)(11 + i));

			pad = (n % BLK != 0) && ((i + (size_t)nul) & 1);
			ab_extension(&a, 'L', content, n, nul, pad, NULL);
			ab_extension(&a, 'K', llink, n, 0, pad, NULL);
			hdr_ordinary(hdr, '0', "plain", NULL, 1);
			body_off = a.len + BLK;
			ab_bytes(&a, hdr, BLK);
			{
				static const unsigned char one = 0x5a;
				ab_bytes(&a, &one, 1);
			}

			mem_init(&m, &a);
			t = open_mem(&m, 0);
			TASK_CHECK(t != NULL);
			TASK_CHECK(th_read(t) == 0);
			TASK_CHECK(strcmp(th_get_pathname(t), expect) == 0);
			TASK_CHECK(strcmp(th_get_linkname(t), llink) == 0);
			TASK_CHECK(m.pos == body_off);
			TASK_CHECK(tar_close(t) == 0);
			free(a.p);
		}
	}
	return 0;
}

/* a NUL earlier than the final declared byte is rejected */
static int
case_layout_embedded_nul(void)
{
	abuf_t a;
	memio_t m;
	TAR *t;
	unsigned char hdr[BLK];
	char content[8] = { 'a', 'b', '\0', 'd', 'e', 'f', 'g', 'h' };

	memset(&a, 0, sizeof(a));
	/* declare 8 bytes, NUL at index 2 */
	ab_extension(&a, 'L', content, 8, 0, 0, NULL);
	hdr_ordinary(hdr, '0', "plain", NULL, 0);
	ab_bytes(&a, hdr, BLK);

	mem_init(&m, &a);
	t = open_mem(&m, 0);
	TASK_CHECK(t != NULL);
	errno = 0;
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == EINVAL);
	TASK_CHECK(t->th_buf.gnu_longname == NULL);
	TASK_CHECK(t->th_buf.gnu_longlink == NULL);
	TASK_CHECK(tar_close(t) == 0);
	free(a.p);
	return 0;
}

/* no extensions: ordinary validation, EOF and options unchanged */
static int
case_layout_plain_eof(void)
{
	abuf_t a;
	memio_t m;
	TAR *t;
	unsigned char hdr[BLK];
	size_t off;

	memset(&a, 0, sizeof(a));
	hdr_ordinary(hdr, '0', "plain", NULL, 0);
	ab_bytes(&a, hdr, BLK);
	off = ab_zero_blocks(&a, 2);

	mem_init(&m, &a);
	t = open_mem(&m, 0);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(strcmp(th_get_pathname(t), "plain") == 0);
	TASK_CHECK(m.pos == off);	/* only the ordinary header was read */
	TASK_CHECK(th_read(t) == 1);	/* EOF */
	TASK_CHECK(tar_close(t) == 0);

	/* bad checksum is rejected when the option asks for it */
	a.len = 0;
	hdr_ordinary(hdr, '0', "plain", NULL, 0);
	hdr[10] ^= 0x01;		/* corrupt without fixing checksum */
	ab_bytes(&a, hdr, BLK);
	mem_init(&m, &a);
	t = open_mem(&m, TAR_CHECK_MAGIC | TAR_CHECK_VERSION);
	TASK_CHECK(t != NULL);
	errno = 0;
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == EINVAL);
	TASK_CHECK(tar_close(t) == 0);
	free(a.p);
	return 0;
}

/* ------------------------------------------------------------------ */
/* group: limits                                                      */
/* ------------------------------------------------------------------ */

static int
case_limits_exact(void)
{
	abuf_t a;
	memio_t m;
	TAR *t;
	unsigned char hdr[BLK];
	char c1[301], c2[301];
	size_t poff2;

	memset(&a, 0, sizeof(a));
	fill_name(c1, 300, 1);
	fill_name(c2, 300, 2);

	ab_extension(&a, 'L', c1, 300, 0, 0, NULL);
	ab_extension(&a, 'K', c2, 300, 0, 0, NULL);
	hdr_ordinary(hdr, '0', "plain", NULL, 0);
	ab_bytes(&a, hdr, BLK);
	(void)poff2;

	mem_init(&m, &a);
	t = open_mem(&m, 0);
	TASK_CHECK(t != NULL);
	TASK_CHECK(tar_set_gnu_extension_limits(t, 600, 2) == 0);
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(strcmp(th_get_pathname(t), c1) == 0);
	TASK_CHECK(strcmp(th_get_linkname(t), c2) == 0);
	TASK_CHECK(tar_close(t) == 0);
	free(a.p);

	/* one byte below the exact total fails */
	memset(&a, 0, sizeof(a));
	ab_extension(&a, 'L', c1, 300, 0, 0, NULL);
	poff2 = a.len;
	ab_extension(&a, 'K', c2, 300, 0, 0, NULL);
	hdr_ordinary(hdr, '0', "plain", NULL, 0);
	ab_bytes(&a, hdr, BLK);

	mem_init(&m, &a);
	t = open_mem(&m, 0);
	TASK_CHECK(t != NULL);
	TASK_CHECK(tar_set_gnu_extension_limits(t, 599, 64) == 0);
	errno = 0;
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == E2BIG);
	/* the over-limit payload must not have been read: reading stops at
	   the end of the offending extension header */
	TASK_CHECK(m.pos == poff2 + BLK);
	TASK_CHECK(m.reads == 3);	/* first header, first payload, second header */
	TASK_CHECK(t->th_buf.gnu_longname == NULL);
	TASK_CHECK(t->th_buf.gnu_longlink == NULL);
	TASK_CHECK(tar_close(t) == 0);
	free(a.p);
	return 0;
}

/* superseded copies still count against the byte budget */
static int
case_limits_superseded_bytes(void)
{
	abuf_t a;
	memio_t m;
	TAR *t;
	unsigned char hdr[BLK];
	char c1[201], c2[201];
	size_t poff2;

	memset(&a, 0, sizeof(a));
	fill_name(c1, 200, 1);
	fill_name(c2, 200, 2);

	ab_extension(&a, 'L', c1, 200, 0, 0, NULL);
	poff2 = a.len;
	ab_extension(&a, 'L', c2, 200, 0, 0, NULL);
	hdr_ordinary(hdr, '0', "plain", NULL, 0);
	ab_bytes(&a, hdr, BLK);

	mem_init(&m, &a);
	t = open_mem(&m, 0);
	TASK_CHECK(t != NULL);
	/* total declared is 400 even though the final name is 200 bytes */
	TASK_CHECK(tar_set_gnu_extension_limits(t, 300, 64) == 0);
	errno = 0;
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == E2BIG);
	TASK_CHECK(m.pos == poff2 + BLK);
	TASK_CHECK(m.reads == 3);
	TASK_CHECK(tar_close(t) == 0);
	free(a.p);
	return 0;
}

/* header count is separate from the byte budget */
static int
case_limits_header_count(void)
{
	abuf_t a;
	memio_t m;
	TAR *t;
	unsigned char hdr[BLK];
	char c[4];
	size_t poff3;

	memset(&a, 0, sizeof(a));
	fill_name(c, 3, 1);

	ab_extension(&a, 'L', c, 3, 0, 0, NULL);
	ab_extension(&a, 'L', c, 3, 0, 0, NULL);
	poff3 = a.len;
	ab_extension(&a, 'K', c, 3, 0, 0, NULL);
	hdr_ordinary(hdr, '0', "plain", NULL, 0);
	ab_bytes(&a, hdr, BLK);

	mem_init(&m, &a);
	t = open_mem(&m, 0);
	TASK_CHECK(t != NULL);
	/* bytes are ample; only two extension headers are allowed */
	TASK_CHECK(tar_set_gnu_extension_limits(t, 1000, 2) == 0);
	errno = 0;
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == E2BIG);
	TASK_CHECK(m.pos == poff3 + BLK);
	TASK_CHECK(m.reads == 5);	/* hdr, payload, hdr, payload, third hdr */
	TASK_CHECK(tar_close(t) == 0);
	free(a.p);

	/* exactly two headers is fine */
	memset(&a, 0, sizeof(a));
	ab_extension(&a, 'L', c, 3, 0, 0, NULL);
	ab_extension(&a, 'K', c, 3, 0, 0, NULL);
	hdr_ordinary(hdr, '0', "plain", NULL, 0);
	ab_bytes(&a, hdr, BLK);
	mem_init(&m, &a);
	t = open_mem(&m, 0);
	TASK_CHECK(t != NULL);
	TASK_CHECK(tar_set_gnu_extension_limits(t, 1000, 2) == 0);
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(tar_close(t) == 0);
	free(a.p);
	return 0;
}

/* limits/accumulation are per-handle and reset on each record */
static int
case_limits_independent(void)
{
	abuf_t a;
	memio_t m1, m2;
	TAR *t1, *t2;
	unsigned char hdr[BLK];
	char c[201];

	memset(&a, 0, sizeof(a));
	fill_name(c, 200, 4);

	/* two records, each with a 200-byte extension */
	ab_extension(&a, 'L', c, 200, 0, 0, NULL);
	hdr_ordinary(hdr, '0', "one", NULL, 0);
	ab_bytes(&a, hdr, BLK);
	ab_extension(&a, 'L', c, 200, 0, 0, NULL);
	hdr_ordinary(hdr, '0', "two", NULL, 0);
	ab_bytes(&a, hdr, BLK);

	mem_init(&m1, &a);
	mem_init(&m2, &a);
	t1 = open_mem(&m1, 0);
	t2 = open_mem(&m2, 0);
	TASK_CHECK(t1 != NULL && t2 != NULL);

	/* t1 budget is per record (200 bytes) and must reset for record two */
	TASK_CHECK(tar_set_gnu_extension_limits(t1, 200, 1) == 0);
	/* t2 uses a larger budget and must be unaffected by t1's counters */
	TASK_CHECK(tar_set_gnu_extension_limits(t2, 4096, 64) == 0);

	TASK_CHECK(th_read(t1) == 0);
	TASK_CHECK(strcmp(th_get_pathname(t1), c) == 0);
	/* t2 has not accumulated anything from t1 */
	TASK_CHECK(th_read(t2) == 0);
	TASK_CHECK(strcmp(th_get_pathname(t2), c) == 0);

	/* second record of t1: budget starts over */
	TASK_CHECK(th_read(t1) == 0);
	TASK_CHECK(strcmp(th_get_pathname(t1), c) == 0);
	TASK_CHECK(th_read(t2) == 0);
	TASK_CHECK(strcmp(th_get_pathname(t2), c) == 0);

	TASK_CHECK(tar_close(t1) == 0);
	TASK_CHECK(tar_close(t2) == 0);
	free(a.p);
	return 0;
}

static int
case_limits_setter_args(void)
{
	abuf_t a;
	memio_t m;
	TAR *t;
	unsigned char hdr[BLK];

	memset(&a, 0, sizeof(a));
	hdr_ordinary(hdr, '0', "plain", NULL, 0);
	ab_bytes(&a, hdr, BLK);

	mem_init(&m, &a);
	t = open_mem(&m, 0);
	TASK_CHECK(t != NULL);
	TASK_CHECK(tar_set_gnu_extension_limits(t, 0, 1) == -1);
	TASK_CHECK(errno == EINVAL);
	TASK_CHECK(tar_set_gnu_extension_limits(t, 1, 0) == -1);
	TASK_CHECK(errno == EINVAL);
	TASK_CHECK(t->gnu_limit_bytes == TAR_GNU_DEFAULT_MAX_BYTES);
	TASK_CHECK(t->gnu_limit_headers == TAR_GNU_DEFAULT_MAX_HEADERS);
	TASK_CHECK(tar_set_gnu_extension_limits(t, 7, 3) == 0);
	TASK_CHECK(t->gnu_limit_bytes == 7);
	TASK_CHECK(t->gnu_limit_headers == 3);
	TASK_CHECK(tar_close(t) == 0);
	free(a.p);
	return 0;
}

/* strict 12-byte size decoding: accepted spellings */
static int
strict_accept_one(const unsigned char raw[12], size_t declared)
{
	abuf_t a;
	memio_t m;
	TAR *t;
	unsigned char hdr[BLK];
	char content[MAX_LEN + 1];
	char expect[MAX_LEN + 1];

	memset(&a, 0, sizeof(a));
	fill_name(content, declared, 5);
	memcpy(expect, content, declared);
	expect[declared] = '\0';

	ab_extension_with_raw(&a, 'L', raw, content, declared, NULL);
	/* adjacent mtime field contains non-octal garbage; only the 12-byte
	   size field may be examined */
	hdr_ordinary(hdr, '0', "plain", NULL, 0);
	memset(hdr + 136, 'Z', 12);
	hdr_checksum(hdr);
	ab_bytes(&a, hdr, BLK);

	mem_init(&m, &a);
	t = open_mem(&m, 0);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(strcmp(th_get_pathname(t), expect) == 0);
	TASK_CHECK(tar_close(t) == 0);
	free(a.p);
	return 0;
}

/* strict 12-byte size decoding: rejected spellings */
static int
strict_reject_one(const unsigned char raw[12])
{
	abuf_t a;
	memio_t m;
	TAR *t;
	unsigned char hdr[BLK];

	memset(&a, 0, sizeof(a));
	hdr_extension_raw(hdr, 'L', raw);
	ab_bytes(&a, hdr, BLK);
	/* a payload that a lax decoder might consume; it must not be read */
	ab_zero_blocks(&a, 1);
	hdr_ordinary(hdr, '0', "plain", NULL, 0);
	ab_bytes(&a, hdr, BLK);

	mem_init(&m, &a);
	t = open_mem(&m, 0);
	TASK_CHECK(t != NULL);
	hooks_account();
	errno = 0;
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == EINVAL);
	TASK_CHECK(hook_total == 0);	/* rejection precedes allocation */
	TASK_CHECK(m.reads == 1);	/* payload was not read */
	TASK_CHECK(t->th_buf.gnu_longname == NULL);
	TASK_CHECK(tar_close(t) == 0);
	TASK_CHECK(hook_live == 0);
	hooks_reset();
	free(a.p);
	return 0;
}

static int
case_limits_strict_size_accept(void)
{
	unsigned char raw[12];

	/* twelve octal digits, no terminator, and a non-zero mtime beside it */
	memcpy(raw, "000000000144", 12);
	if (strict_accept_one(raw, 100) != 0)
		return 1;

	/* leading spaces */
	memset(raw, ' ', 12);
	memcpy(raw + 4, "144", 3);
	if (strict_accept_one(raw, 100) != 0)
		return 1;

	/* leading NUL padding */
	memset(raw, 0, 12);
	memcpy(raw + 9, "144", 3);
	if (strict_accept_one(raw, 100) != 0)
		return 1;

	/* digits followed by NUL and then spaces */
	memset(raw, 0, 12);
	memcpy(raw, "144", 3);
	memset(raw + 3, ' ', 9);
	if (strict_accept_one(raw, 100) != 0)
		return 1;

	/* a full 12-digit field spelling 512 */
	memcpy(raw, "000000001000", 12);
	if (strict_accept_one(raw, 512) != 0)
		return 1;

	/* a full 12-digit field spelling 1024 */
	memcpy(raw, "000000002000", 12);
	if (strict_accept_one(raw, 1024) != 0)
		return 1;

	return 0;
}

static int
case_limits_strict_size_reject(void)
{
	unsigned char raw[12];

	/* internal space between digits */
	memcpy(raw, "1 44        ", 12);
	if (strict_reject_one(raw) != 0)
		return 1;

	/* sign */
	memcpy(raw, "-144        ", 12);
	if (strict_reject_one(raw) != 0)
		return 1;

	/* 8 and 9 are not octal */
	memcpy(raw, "891         ", 12);
	if (strict_reject_one(raw) != 0)
		return 1;

	/* base-256 encoding */
	memset(raw, 0, 12);
	raw[0] = 0x80;
	raw[11] = 1;
	if (strict_reject_one(raw) != 0)
		return 1;

	/* all spaces, no digits */
	memset(raw, ' ', 12);
	if (strict_reject_one(raw) != 0)
		return 1;

	/* all NUL, no digits */
	memset(raw, 0, 12);
	if (strict_reject_one(raw) != 0)
		return 1;

	/* zero length extension */
	memcpy(raw, "000000000000", 12);
	if (strict_reject_one(raw) != 0)
		return 1;

	/* digit followed by NUL and then another digit */
	memset(raw, 0, 12);
	memcpy(raw, "144", 3);
	raw[11] = '4';
	if (strict_reject_one(raw) != 0)
		return 1;

	/* trailing garbage */
	memcpy(raw, "144x00000000", 12);
	if (strict_reject_one(raw) != 0)
		return 1;

	/* padding followed by more digits */
	memset(raw, ' ', 12);
	memcpy(raw, "14", 2);
	raw[2] = ' ';
	raw[3] = '4';
	if (strict_reject_one(raw) != 0)
		return 1;

	/* an embedded NUL then digits */
	memset(raw, 0, 12);
	memcpy(raw, "14", 2);
	raw[2] = '\0';
	raw[3] = '4';
	if (strict_reject_one(raw) != 0)
		return 1;

	return 0;
}

/* values near the native size_t limit: representable ones must either read
   safely (when under budget) or fail with E2BIG before allocating;
   unrepresentable ones must be rejected with EINVAL, never truncated */
static int
case_limits_size_boundary(void)
{
	/* largest value writable in 12 octal digits */
	unsigned long long max12 = 0777777777777ULL;
	int fits = (max12 <= (unsigned long long)(size_t)-1);
	unsigned char raw[12];
	abuf_t a;
	memio_t m;
	TAR *t;
	unsigned char hdr[BLK];
	int want;

	/* 2^33: representable on 64-bit, not on 32-bit */
	memset(&a, 0, sizeof(a));
	memcpy(raw, "400000000000", 12);	/* 8^11 == 2^33 */
	hdr_extension_raw(hdr, 'L', raw);
	ab_bytes(&a, hdr, BLK);
	hdr_ordinary(hdr, '0', "plain", NULL, 0);
	ab_bytes(&a, hdr, BLK);

	mem_init(&m, &a);
	t = open_mem(&m, 0);
	TASK_CHECK(t != NULL);
	TASK_CHECK(tar_set_gnu_extension_limits(t, 1, 64) == 0);
	hooks_account();
	errno = 0;
	TASK_CHECK(th_read(t) == -1);
	want = (sizeof(size_t) < 5) ? EINVAL : E2BIG;
	TASK_CHECK(errno == want);
	/* no giant allocation was attempted */
	TASK_CHECK(hook_total == 0);
	TASK_CHECK(hook_live == 0);
	/* the payload was not read */
	TASK_CHECK(m.reads == 1);
	TASK_CHECK(tar_close(t) == 0);
	hooks_reset();
	free(a.p);

	/* maximum 12-digit value, same rule */
	memset(&a, 0, sizeof(a));
	memcpy(raw, "777777777777", 12);
	hdr_extension_raw(hdr, 'L', raw);
	ab_bytes(&a, hdr, BLK);
	hdr_ordinary(hdr, '0', "plain", NULL, 0);
	ab_bytes(&a, hdr, BLK);

	mem_init(&m, &a);
	t = open_mem(&m, 0);
	TASK_CHECK(t != NULL);
	TASK_CHECK(tar_set_gnu_extension_limits(t, 1, 64) == 0);
	hooks_account();
	errno = 0;
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == (fits ? E2BIG : EINVAL));
	TASK_CHECK(hook_total == 0);
	TASK_CHECK(hook_live == 0);
	TASK_CHECK(m.reads == 1);
	TASK_CHECK(tar_close(t) == 0);
	hooks_reset();
	free(a.p);

	return 0;
}

/* a size field whose 12 bytes are all digits next to a hostile neighbour
   must not scan past its own field */
static int
case_limits_no_overscan(void)
{
	abuf_t a;
	memio_t m;
	TAR *t;
	unsigned char raw[12];
	unsigned char hdr[BLK];
	char content[65];
	char expect[65];

	memset(&a, 0, sizeof(a));
	fill_name(content, 64, 3);
	memcpy(expect, content, 64);
	expect[64] = '\0';
	memcpy(raw, "000000000100", 12);	/* 64 */
	ab_extension_with_raw(&a, 'L', raw, content, 64, NULL);

	/* checksum field adjacent to size is fine; mtime after size is
	   deliberately full of non-octal bytes */
	hdr_ordinary(hdr, '0', "plain", NULL, 0);
	memset(hdr + 136, 0xfe, 12);
	hdr_checksum(hdr);
	ab_bytes(&a, hdr, BLK);

	mem_init(&m, &a);
	t = open_mem(&m, 0);
	TASK_CHECK(t != NULL);
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(strcmp(th_get_pathname(t), expect) == 0);
	TASK_CHECK(tar_close(t) == 0);
	free(a.p);
	return 0;
}

/* ------------------------------------------------------------------ */
/* group: failure                                                     */
/* ------------------------------------------------------------------ */

/*
** Build a small chain whose reads are numbered:
**   1: L header   2,3: L payload (600 bytes)   4: K header
**   5: K payload (100 bytes)   6: ordinary header
** and fail the k-th read either by returning a short count (mode 0) or
** by returning -1 with EIO (mode 1).
*/
static int
failure_at_read(int read_index, int mode)
{
	abuf_t a;
	memio_t m;
	TAR *t;
	unsigned char hdr[BLK];
	char name[601];
	char link[101];
	int reads_after_failure;
	int expected_errno = mode ? EIO : EINVAL;

	memset(&a, 0, sizeof(a));
	fill_name(name, 600, 2);
	fill_name(link, 100, 3);
	ab_extension(&a, 'L', name, 600, 0, 1, NULL);
	ab_extension(&a, 'K', link, 100, 0, 0, NULL);
	hdr_ordinary(hdr, '0', "plain", NULL, 0);
	ab_bytes(&a, hdr, BLK);

	mem_init(&m, &a);
	if (mode)
	{
		m.fail_read = read_index;
		m.fail_errno = EIO;
	}
	else
		m.short_read = read_index;
	t = open_mem(&m, 0);
	TASK_CHECK(t != NULL);
	hooks_account();
	errno = 0;
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == expected_errno);
	TASK_CHECK(t->gnu_read_error == 1);
	TASK_CHECK(t->th_buf.gnu_longname == NULL);
	TASK_CHECK(t->th_buf.gnu_longlink == NULL);
	TASK_CHECK(hook_live == 0);
	TASK_CHECK(m.closes == 0);

	/* a second read repeats the error without calling the reader */
	reads_after_failure = m.reads;
	errno = 0;
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == expected_errno);
	TASK_CHECK(m.reads == reads_after_failure);

	/* the setter must not clear the error state */
	errno = 0;
	TASK_CHECK(tar_set_gnu_extension_limits(t, 4096, 8) == -1);
	TASK_CHECK(errno == EINVAL);
	errno = 0;
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == expected_errno);
	TASK_CHECK(m.reads == reads_after_failure);

	/* close still releases everything and closes the backend */
	TASK_CHECK(tar_close(t) == 0);
	TASK_CHECK(m.closes == 1);
	TASK_CHECK(hook_live == 0);
	hooks_reset();
	free(a.p);
	return 0;
}

static int
case_failure_header_short(void)
{
	return failure_at_read(1, 0);
}

static int
case_failure_header_eio(void)
{
	return failure_at_read(1, 1);
}

static int
case_failure_payload_short(void)
{
	return failure_at_read(2, 0);
}

static int
case_failure_payload_eio(void)
{
	return failure_at_read(3, 1);
}

static int
case_failure_second_payload_eio(void)
{
	return failure_at_read(5, 1);
}

static int
case_failure_ordinary_header_short(void)
{
	return failure_at_read(6, 0);
}

static int
case_failure_ordinary_header_eio(void)
{
	return failure_at_read(6, 1);
}

/* each allocation site in one read is failed in turn */
static int
case_failure_allocation(void)
{
	abuf_t a;
	memio_t m;
	TAR *t;
	unsigned char hdr[BLK];
	char n1[100], l1[50], n2[201];
	int successes;
	int k;

	memset(&a, 0, sizeof(a));
	fill_name(n1, 99, 1);
	fill_name(l1, 49, 2);
	fill_name(n2, 200, 3);
	ab_extension(&a, 'L', n1, 99, 0, 0, NULL);
	ab_extension(&a, 'K', l1, 49, 0, 0, NULL);
	ab_extension(&a, 'L', n2, 200, 0, 0, NULL);
	hdr_ordinary(hdr, '0', "plain", NULL, 0);
	ab_bytes(&a, hdr, BLK);

	/* count allocations on a successful read */
	mem_init(&m, &a);
	t = open_mem(&m, 0);
	TASK_CHECK(t != NULL);
	hooks_account();
	TASK_CHECK(th_read(t) == 0);
	TASK_CHECK(strcmp(th_get_pathname(t), n2) == 0);
	TASK_CHECK(hook_total == 3);	/* one per extension */
	TASK_CHECK(hook_live == 2);	/* final name and link retained */
	TASK_CHECK(tar_close(t) == 0);
	TASK_CHECK(hook_live == 0);
	hooks_reset();

	successes = 0;
	for (k = 1; k <= 3; k++)
	{
		int expected_reads;

		mem_init(&m, &a);
		t = open_mem(&m, 0);
		TASK_CHECK(t != NULL);
		hooks_fail_at(k);
		errno = 0;
		TASK_CHECK(th_read(t) == -1);
		TASK_CHECK(errno == ENOMEM);
		TASK_CHECK(t->th_buf.gnu_longname == NULL);
		TASK_CHECK(t->th_buf.gnu_longlink == NULL);
		TASK_CHECK(hook_live == 0);	/* every pending name released */

		expected_reads = m.reads;
		errno = 0;
		TASK_CHECK(th_read(t) == -1);
		TASK_CHECK(errno == ENOMEM);
		TASK_CHECK(m.reads == expected_reads);
		TASK_CHECK(tar_close(t) == 0);
		TASK_CHECK(m.closes == 1);
		TASK_CHECK(hook_live == 0);
		hooks_reset();
		successes++;
	}
	TASK_CHECK(successes == 3);
	free(a.p);
	return 0;
}

/* extension followed by EOF, a termination block or a truncated header */
static int
dangling_variant(int variant)
{
	abuf_t a;
	memio_t m;
	TAR *t;
	unsigned char hdr[BLK];
	char name[64];

	memset(&a, 0, sizeof(a));
	fill_name(name, 63, 4);
	ab_extension(&a, 'L', name, 63, 0, 0, NULL);

	if (variant == 0)
	{
		/* EOF immediately */
	}
	else if (variant == 1)
	{
		/* zero termination block */
		ab_zero_blocks(&a, 1);
	}
	else if (variant == 2)
	{
		/* double zero termination */
		ab_zero_blocks(&a, 2);
	}
	else
	{
		/* truncated header */
		hdr_ordinary(hdr, '0', "plain", NULL, 0);
		ab_bytes(&a, hdr, 200);
	}

	mem_init(&m, &a);
	t = open_mem(&m, 0);
	TASK_CHECK(t != NULL);
	hooks_account();
	errno = 0;
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == EINVAL);
	TASK_CHECK(t->th_buf.gnu_longname == NULL);
	TASK_CHECK(t->th_buf.gnu_longlink == NULL);
	TASK_CHECK(hook_live == 0);
	{
		int reads = m.reads;
		TASK_CHECK(th_read(t) == -1);
		TASK_CHECK(errno == EINVAL);
		TASK_CHECK(m.reads == reads);
	}
	TASK_CHECK(tar_close(t) == 0);
	TASK_CHECK(hook_live == 0);
	hooks_reset();
	free(a.p);
	return 0;
}

static int
case_failure_dangling_eof(void)
{
	return dangling_variant(0);
}

static int
case_failure_dangling_zero_block(void)
{
	return dangling_variant(1);
}

static int
case_failure_dangling_two_zero_blocks(void)
{
	return dangling_variant(2);
}

static int
case_failure_dangling_truncated_header(void)
{
	return dangling_variant(3);
}

/* a malformed ordinary header after a valid extension is not committed */
static int
case_failure_bad_ordinary_crc(void)
{
	abuf_t a;
	memio_t m;
	TAR *t;
	unsigned char hdr[BLK];
	char name[64];

	memset(&a, 0, sizeof(a));
	fill_name(name, 63, 8);
	ab_extension(&a, 'L', name, 63, 0, 0, NULL);
	hdr_ordinary(hdr, '0', "plain", NULL, 0);
	hdr[0] ^= 0x01;		/* break the checksum */
	ab_bytes(&a, hdr, BLK);

	mem_init(&m, &a);
	t = open_mem(&m, 0);
	TASK_CHECK(t != NULL);
	hooks_account();
	errno = 0;
	TASK_CHECK(th_read(t) == -1);
	TASK_CHECK(errno == EINVAL);
	TASK_CHECK(t->th_buf.gnu_longname == NULL);
	TASK_CHECK(t->th_buf.gnu_longlink == NULL);
	TASK_CHECK(hook_live == 0);
	TASK_CHECK(tar_close(t) == 0);
	TASK_CHECK(hook_live == 0);
	hooks_reset();
	free(a.p);
	return 0;
}


/* ------------------------------------------------------------------ */
/* group: model                                                       */
/* ------------------------------------------------------------------ */

/*
** An independent model of the extension chain.  A chain is a list of
** extension descriptors followed by a termination form.  The model
** computes, without consulting the library, the expected outcome and the
** exact stream position at which the reader must stop.
*/

#define MODEL_MAX_EXT MAX_EXT

typedef struct
{
	char type;		/* 'L' or 'K' */
	size_t len;
	size_t payload_off;	/* filled while building */
	char content[MAX_LEN + 1];
} model_ext_t;

typedef struct
{
	int count;
	model_ext_t ext[MODEL_MAX_EXT];
	int term;		/* 0 ordinary, 1 EOF, 2 one zero blk,
				   3 two zero blks, 4 truncated */
	size_t limit_bytes;
	size_t limit_headers;
	char ordinary_name[32];
} model_chain_t;

/* deterministic generator; state is explicit so the fuzz entry can seed it */
typedef struct
{
	uint64_t s;
} prng_t;

static uint32_t
prng_next(prng_t *r)
{
	r->s = r->s * 6364136223846793005ULL + 1442695040888963407ULL;
	return (uint32_t)(r->s >> 33);
}

static uint32_t
prng_below(prng_t *r, uint32_t n)
{
	return n ? prng_next(r) % n : 0;
}

static void
model_generate(model_chain_t *c, prng_t *r)
{
	int i;

	memset(c, 0, sizeof(*c));
	c->count = 1 + (int)prng_below(r, MODEL_MAX_EXT);
	for (i = 0; i < c->count; i++)
	{
		int which = (int)prng_below(r, 8);

		c->ext[i].type = prng_below(r, 2) ? 'L' : 'K';
		/* mix random lengths with the blocksize boundaries */
		if (which == 0)
			c->ext[i].len = 1;
		else if (which == 1)
			c->ext[i].len = 511;
		else if (which == 2)
			c->ext[i].len = 512;
		else if (which == 3)
			c->ext[i].len = 513;
		else if (which == 4)
			c->ext[i].len = 1024;
		else if (which < 8)
			c->ext[i].len = 1 + (size_t)prng_below(r, MAX_LEN);
		fill_name(c->ext[i].content, c->ext[i].len,
			  (char)prng_below(r, 26));
	}
	c->term = (int)prng_below(r, 5);
	c->limit_bytes = 1 + (size_t)prng_below(r, 4096);
	c->limit_headers = 1 + (size_t)prng_below(r, MODEL_MAX_EXT + 2);
	snprintf(c->ordinary_name, sizeof(c->ordinary_name), "ord%u",
		 prng_below(r, 1000));
}

/* build an image from a model chain */
static void
model_build(model_chain_t *c, abuf_t *a)
{
	int i;
	unsigned char hdr[BLK];
	size_t hdr_start;

	memset(a, 0, sizeof(*a));
	for (i = 0; i < c->count; i++)
	{
		hdr_start = a->len;
		ab_extension(a, c->ext[i].type, c->ext[i].content,
			     c->ext[i].len, 0, 0, NULL);
		c->ext[i].payload_off = hdr_start + BLK;
	}

	switch (c->term)
	{
	case 0:
		hdr_ordinary(hdr, '0', c->ordinary_name, NULL, 0);
		ab_bytes(a, hdr, BLK);
		break;
	case 2:
		ab_zero_blocks(a, 1);
		break;
	case 3:
		ab_zero_blocks(a, 2);
		break;
	case 4:
		hdr_ordinary(hdr, '0', "trunc", NULL, 0);
		ab_bytes(a, hdr, 100 + (a->len % 300));
		break;
	default:
		/* EOF: nothing appended */
		break;
	}
}

typedef struct
{
	int rc;			/* 0 success, -1 failure, 1 EOF */
	int err;		/* expected errno when rc == -1 */
	size_t pos;		/* expected stream position after the call */
	const char *name;	/* expected pathname when rc == 0 */
	const char *link;	/* expected linkname when rc == 0 */
} model_expect_t;

static void
model_evaluate(const model_chain_t *c, const abuf_t *a, model_expect_t *e)
{
	size_t bytes = 0;
	size_t headers = 0;
	int i;
	const char *last_name = NULL;
	const char *last_link = NULL;
	size_t pos = 0;

	memset(e, 0, sizeof(*e));

	for (i = 0; i < c->count; i++)
	{
		size_t n = c->ext[i].len;
		size_t blocks = n / BLK + (n % BLK ? 1 : 0);

		pos = c->ext[i].payload_off;
		if (headers >= c->limit_headers
		    || n > c->limit_bytes - bytes)
		{
			e->rc = -1;
			e->err = E2BIG;
			e->pos = pos;	/* payload never read */
			return;
		}
		headers++;
		bytes += n;
		pos = c->ext[i].payload_off + blocks * BLK;
		if (c->ext[i].type == 'L')
			last_name = c->ext[i].content;
		else
			last_link = c->ext[i].content;
	}

	/* extensions all fit; the termination decides */
	switch (c->term)
	{
	case 0:
		pos += BLK;
		e->rc = 0;
		e->name = last_name ? last_name : c->ordinary_name;
		e->link = last_link ? last_link : "";
		e->pos = pos;
		break;
	case 2:
		pos += BLK;
		e->rc = -1;
		e->err = EINVAL;
		e->pos = pos;
		break;
	case 3:
		pos += BLK;
		e->rc = -1;
		e->err = EINVAL;
		e->pos = pos;
		break;
	case 4:
		e->rc = -1;
		e->err = EINVAL;
		e->pos = a->len;	/* short read consumes the rest */
		break;
	default:
		e->rc = -1;
		e->err = EINVAL;
		e->pos = a->len;
		break;
	}
}

static int
model_run_one(model_chain_t *c)
{
	abuf_t a;
	memio_t m;
	TAR *t;
	model_expect_t e;
	int rc;

	model_build(c, &a);
	model_evaluate(c, &a, &e);

	mem_init(&m, &a);
	t = open_mem(&m, 0);
	TASK_CHECK(t != NULL);
	TASK_CHECK(tar_set_gnu_extension_limits(t, c->limit_bytes,
						c->limit_headers) == 0);
	hooks_account();
	errno = 0;
	rc = th_read(t);

	if (e.rc == 0)
	{
		TASK_CHECK(rc == 0);
		TASK_CHECK(strcmp(th_get_pathname(t), e.name) == 0);
		TASK_CHECK(strcmp(th_get_linkname(t), e.link) == 0);
	}
	else
	{
		TASK_CHECK(rc == -1);
		TASK_CHECK(errno == e.err);
		TASK_CHECK(t->th_buf.gnu_longname == NULL);
		TASK_CHECK(t->th_buf.gnu_longlink == NULL);
	}
	TASK_CHECK(m.pos == e.pos);
	TASK_CHECK(hook_live == 0 || e.rc == 0);
	TASK_CHECK(tar_close(t) == 0);
	TASK_CHECK(hook_live == 0);
	hooks_reset();
	free(a.p);
	return 0;
}

static int
case_model_fixed_chains(void)
{
	prng_t r;
	int i;

	r.s = 0x5eed1234ULL;
	for (i = 0; i < 256; i++)
	{
		model_chain_t c;

		model_generate(&c, &r);
		if (model_run_one(&c) != 0)
			return 1;
	}
	return 0;
}

/* cover at least one chain of each order/termination shape explicitly */
static int
case_model_shapes(void)
{
	static const int terms[] = { 0, 1, 2, 3, 4 };
	size_t ti;
	int order;

	for (ti = 0; ti < sizeof(terms) / sizeof(terms[0]); ti++)
	{
		for (order = 0; order < 4; order++)
		{
			model_chain_t c;
			int i;

			memset(&c, 0, sizeof(c));
			c.count = 1 + order * 3;
			for (i = 0; i < c.count; i++)
			{
				c.ext[i].type = ((i + order) % 2) ? 'K' : 'L';
				c.ext[i].len = 1 + (size_t)((i * 511 + order)
							    % MAX_LEN);
				fill_name(c.ext[i].content, c.ext[i].len,
					  (char)((i + order) % 26));
			}
			c.term = terms[ti];
			c.limit_bytes = 4096;
			c.limit_headers = MODEL_MAX_EXT;
			strcpy(c.ordinary_name, "shape");
			if (model_run_one(&c) != 0)
				return 1;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* fuzz entry point                                                   */
/* ------------------------------------------------------------------ */

/*
** Reuses the bounded generator and checking core.  The input is consumed
** only in process memory; no file is ever created.  Sizes and counts are
** clamped so a fuzz input can never request a giant allocation.
*/
int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	prng_t r;
	model_chain_t c;
	size_t i;
	int n;

	if (data == NULL || size == 0)
		return 0;

	r.s = 0;
	for (i = 0; i < size && i < 64; i++)
		r.s = (r.s << 8) ^ data[i];

	memset(&c, 0, sizeof(c));
	c.count = 1 + (int)prng_below(&r, MODEL_MAX_EXT);
	for (n = 0; n < c.count; n++)
	{
		uint32_t b1, b2, b3;

		b1 = (n * 3 < (int)size) ? data[n * 3] : (uint32_t)n;
		b2 = (n * 3 + 1 < (int)size) ? data[n * 3 + 1] : 0;
		b3 = (n * 3 + 2 < (int)size) ? data[n * 3 + 2] : 0;
		c.ext[n].type = (b1 & 1) ? 'K' : 'L';
		c.ext[n].len = 1 + (size_t)(((uint32_t)b2 << 8 | b3) % MAX_LEN);
		fill_name(c.ext[n].content, c.ext[n].len, (char)(b1 % 26));
	}
	c.term = (int)prng_below(&r, 5);
	c.limit_bytes = 1 + prng_below(&r, 4096);
	c.limit_headers = 1 + prng_below(&r, MODEL_MAX_EXT + 2);
	strcpy(c.ordinary_name, "fuzz");

	/* model_run_one() aborts on impossible framework errors; a fuzz
	   input can only influence the chain, which the model mirrors */
	model_run_one(&c);
	return 0;
}

/* ------------------------------------------------------------------ */
/* registry                                                           */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
	/* layout */
	{ "layout", "l_plain", case_layout_l_plain },
	{ "layout", "k_only", case_layout_k_only },
	{ "layout", "l_then_k", case_layout_l_k },
	{ "layout", "k_then_l", case_layout_k_l },
	{ "layout", "l_k_l", case_layout_l_k_l },
	{ "layout", "override", case_layout_override },
	{ "layout", "next_record_clean", case_layout_next_record_clean },
	{ "layout", "lengths", case_layout_lengths },
	{ "layout", "embedded_nul", case_layout_embedded_nul },
	{ "layout", "plain_eof", case_layout_plain_eof },

	/* limits */
	{ "limits", "exact", case_limits_exact },
	{ "limits", "superseded_bytes", case_limits_superseded_bytes },
	{ "limits", "header_count", case_limits_header_count },
	{ "limits", "independent", case_limits_independent },
	{ "limits", "setter_args", case_limits_setter_args },
	{ "limits", "strict_size_accept", case_limits_strict_size_accept },
	{ "limits", "strict_size_reject", case_limits_strict_size_reject },
	{ "limits", "size_boundary", case_limits_size_boundary },
	{ "limits", "no_overscan", case_limits_no_overscan },

	/* failure */
	{ "failure", "header_short", case_failure_header_short },
	{ "failure", "header_eio", case_failure_header_eio },
	{ "failure", "payload_short", case_failure_payload_short },
	{ "failure", "payload_eio", case_failure_payload_eio },
	{ "failure", "second_payload_eio", case_failure_second_payload_eio },
	{ "failure", "ordinary_short", case_failure_ordinary_header_short },
	{ "failure", "ordinary_eio", case_failure_ordinary_header_eio },
	{ "failure", "allocation", case_failure_allocation },
	{ "failure", "dangling_eof", case_failure_dangling_eof },
	{ "failure", "dangling_zero_block", case_failure_dangling_zero_block },
	{ "failure", "dangling_two_zero_blocks",
	  case_failure_dangling_two_zero_blocks },
	{ "failure", "dangling_truncated_header",
	  case_failure_dangling_truncated_header },
	{ "failure", "bad_ordinary_crc", case_failure_bad_ordinary_crc },

	/* model */
	{ "model", "fixed_chains", case_model_fixed_chains },
	{ "model", "shapes", case_model_shapes },
};

const size_t task_case_count =
	sizeof(task_cases) / sizeof(task_cases[0]);
