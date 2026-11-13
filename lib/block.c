/*
**  Copyright 1998-2003 University of Illinois Board of Trustees
**  Copyright 1998-2003 Mark D. Roth
**  All rights reserved.
**
**  block.c - libtar code to handle tar archive header blocks
**
**  Mark D. Roth <roth@uiuc.edu>
**  Campus Information Technologies and Educational Services
**  University of Illinois at Urbana-Champaign
*/

#include <internal.h>

#include <errno.h>

#ifdef STDC_HEADERS
# include <string.h>
# include <stdlib.h>
#endif


#define BIT_ISSET(bitmask, bit) ((bitmask) & (bit))


/*
** Allocation hooks.  These exist so a regression harness can observe or
** fail every allocation made while a GNU extension chain is being read.
** They only replace the allocator; they never alter a parse decision.
*/
static libtar_malloc_func_t libtar_malloc_hook;
static libtar_free_func_t libtar_free_hook;

void
libtar_set_alloc_hooks(libtar_malloc_func_t malloc_func,
		       libtar_free_func_t free_func)
{
	libtar_malloc_hook = malloc_func;
	libtar_free_hook = free_func;
}

void *
tar_gnu_alloc(size_t len)
{
	if (libtar_malloc_hook != NULL)
		return (*libtar_malloc_hook)(len);
	return malloc(len);
}

void
tar_gnu_free(void *ptr)
{
	if (ptr == NULL)
		return;
	if (libtar_free_hook != NULL)
		(*libtar_free_hook)(ptr);
	else
		free(ptr);
}


/*
** Strictly decode the 12-byte GNU extension size field.  Only this field
** is examined; no adjacent terminator is consulted.  Leading and trailing
** spaces or NULs are allowed, at least one octal digit is required, and
** digits must be contiguous.  Anything else (signs, 8/9, base-256, other
** characters) is rejected.  Values that cannot be represented in size_t
** are rejected rather than truncated.
**
** Returns 0 on success and stores the value in *out; -1 on any malformed
** or unrepresentable field (errno is not set here).
*/
static int
gnu_decode_size(const char *field, size_t *out)
{
	size_t val = 0;
	size_t max = (size_t)-1;
	int digits = 0;
	int saw_padding = 0;
	int i;

	for (i = 0; i < 12; i++)
	{
		unsigned char c = (unsigned char)field[i];

		if (c >= '0' && c <= '7')
		{
			if (saw_padding)
				return -1;
			if (val > max / 8)
				return -1;
			val *= 8;
			if (val > max - (size_t)(c - '0'))
				return -1;
			val += (size_t)(c - '0');
			digits++;
		}
		else if (c == ' ' || c == '\0')
		{
			if (digits)
				saw_padding = 1;
		}
		else
			return -1;
	}

	if (digits == 0)
		return -1;

	*out = val;
	return 0;
}


/* Validate magic, version and checksum for the block in t->th_buf,
** honoring the same options as th_read_internal().  Returns 0 if valid,
** -1 otherwise (errno is not set here).
*/
static int
th_validate_header(TAR *t)
{
	if (BIT_ISSET(t->options, TAR_CHECK_MAGIC)
	    && strncmp(t->th_buf.magic, TMAGIC, TMAGLEN - 1) != 0)
		return -1;

	if (BIT_ISSET(t->options, TAR_CHECK_VERSION)
	    && strncmp(t->th_buf.version, TVERSION, TVERSLEN) != 0)
		return -1;

	if (!BIT_ISSET(t->options, TAR_IGNORE_CRC) && !th_crc_ok(t))
		return -1;

	return 0;
}


/* Read the header block that must follow a GNU extension payload.
** Unlike the initial th_read_internal() call, EOF, a termination block or
** a truncated block here is a dangling extension and therefore EINVAL.
** Returns 0 on success, -1 with errno set on failure.
*/
static int
th_read_chain_header(TAR *t)
{
	ssize_t i;

	i = tar_block_read(t, &(t->th_buf));
	if (i == -1)
		return -1;
	if (i != T_BLOCKSIZE || t->th_buf.name[0] == '\0')
	{
		errno = EINVAL;
		return -1;
	}
	if (th_validate_header(t) != 0)
	{
		errno = EINVAL;
		return -1;
	}
	return 0;
}


/* Read one GNU extension payload of declared length n and normalize it
** into a library-owned NUL-terminated string.  The declared bytes may all
** be non-NUL, or the final declared byte may be NUL; a NUL anywhere
** earlier is rejected.  Block padding is never scanned.
**
** Returns 0 on success with *out set, -1 on failure with errno set
** (ENOMEM, EINVAL, or the reader's errno).
*/
static int
th_read_gnu_payload(TAR *t, size_t n, char **out)
{
	size_t blocks, j, left;
	char *name;

	if (n == 0)
	{
		errno = EINVAL;
		return -1;
	}
	if (n == (size_t)-1)
	{
		/* n + 1 below cannot be represented */
		errno = E2BIG;
		return -1;
	}

	blocks = n / T_BLOCKSIZE + (n % T_BLOCKSIZE ? 1 : 0);

	name = (char *)tar_gnu_alloc(n + 1);
	if (name == NULL)
	{
		errno = ENOMEM;
		return -1;
	}

	left = n;
	for (j = 0; j < blocks; j++)
	{
		char block[T_BLOCKSIZE];
		size_t take = left < T_BLOCKSIZE ? left : T_BLOCKSIZE;
		ssize_t i = tar_block_read(t, block);

		if (i == -1)
		{
			int saved = errno;
			tar_gnu_free(name);
			errno = saved;
			return -1;
		}
		if (i != T_BLOCKSIZE)
		{
			tar_gnu_free(name);
			errno = EINVAL;
			return -1;
		}
		memcpy(name + j * T_BLOCKSIZE, block, take);
		left -= take;
	}

	for (j = 0; j < n; j++)
	{
		if (name[j] == '\0' && j != n - 1)
		{
			tar_gnu_free(name);
			errno = EINVAL;
			return -1;
		}
	}
	name[n] = '\0';

	*out = name;
	return 0;
}


/* read a header block */
/* FIXME: the return value of this function should match the return value
	  of tar_block_read(), which is a macro which references a prototype
	  that returns a ssize_t.  So far, this is safe, since tar_block_read()
	  only ever reads 512 (T_BLOCKSIZE) bytes at a time, so any difference
	  in size of ssize_t and int is of negligible risk.  BUT, if
	  T_BLOCKSIZE ever changes, or ever becomes a variable parameter
	  controllable by the user, all the code that calls it,
	  including this function and all code that calls it, should be
	  fixed for security reasons.
	  Thanks to Chris Palmer for the critique.
*/
int
th_read_internal(TAR *t)
{
	int i;
	int num_zero_blocks = 0;

#ifdef DEBUG
	printf("==> th_read_internal(TAR=\"%s\")\n", t->pathname);
#endif

	while ((i = tar_block_read(t, &(t->th_buf))) == T_BLOCKSIZE)
	{
		/* two all-zero blocks mark EOF */
		if (t->th_buf.name[0] == '\0')
		{
			num_zero_blocks++;
			if (!BIT_ISSET(t->options, TAR_IGNORE_EOT)
			    && num_zero_blocks >= 2)
				return 0;	/* EOF */
			else
				continue;
		}

		/* verify magic and version */
		if (BIT_ISSET(t->options, TAR_CHECK_MAGIC)
		    && strncmp(t->th_buf.magic, TMAGIC, TMAGLEN - 1) != 0)
		{
#ifdef DEBUG
			puts("!!! unknown magic value in tar header");
#endif
			return -2;
		}

		if (BIT_ISSET(t->options, TAR_CHECK_VERSION)
		    && strncmp(t->th_buf.version, TVERSION, TVERSLEN) != 0)
		{
#ifdef DEBUG
			puts("!!! unknown version value in tar header");
#endif
			return -2;
		}

		/* check chksum */
		if (!BIT_ISSET(t->options, TAR_IGNORE_CRC)
		    && !th_crc_ok(t))
		{
#ifdef DEBUG
			puts("!!! tar header checksum error");
#endif
			return -2;
		}

		break;
	}

#ifdef DEBUG
	printf("<== th_read_internal(): returning %d\n", i);
#endif
	return i;
}


/* wrapper function for th_read_internal() to handle GNU extensions */
int
th_read(TAR *t)
{
	int i;
	size_t bytes_used = 0;
	size_t headers_used = 0;
	char *pending_name = NULL;
	char *pending_link = NULL;

#ifdef DEBUG
	printf("==> th_read(t=0x%lx)\n", t);
#endif

	/* A handle that suffered a read failure may only be closed.  Repeat
	   the original error without touching the reader. */
	if (t->gnu_read_error)
	{
		errno = t->gnu_read_error_errno;
		return -1;
	}

	/* Invalidate the previous entry view before doing anything else. */
	if (t->th_buf.gnu_longname != NULL)
		tar_gnu_free(t->th_buf.gnu_longname);
	if (t->th_buf.gnu_longlink != NULL)
		tar_gnu_free(t->th_buf.gnu_longlink);
	memset(&(t->th_buf), 0, sizeof(struct tar_header));

	i = th_read_internal(t);
	if (i == 0)
		return 1;
	else if (i != T_BLOCKSIZE)
	{
		/* Any failure of the first header is sticky, exactly like a
		   failure later in an extension chain.  A normal EOF (i == 0)
		   above is not a failure. */
		if (i != -1)
			errno = EINVAL;
		t->gnu_read_error = 1;
		t->gnu_read_error_errno = errno;
		return -1;
	}

	/* Consume zero or more GNU extension records in any order.  Names
	   are held as pending state and committed only once the ordinary
	   header has been read and validated. */
	while (TH_ISLONGNAME(t) || TH_ISLONGLINK(t))
	{
		size_t n;
		char *value;
		int is_name = TH_ISLONGNAME(t);

		if (gnu_decode_size(t->th_buf.size, &n) != 0)
		{
			errno = EINVAL;
			goto fail;
		}

		/* Enforce both limits before allocating or reading any of
		   this extension's payload. */
		if (headers_used >= t->gnu_limit_headers
		    || n > t->gnu_limit_bytes - bytes_used)
		{
			errno = E2BIG;
			goto fail;
		}

		if (th_read_gnu_payload(t, n, &value) != 0)
			goto fail;

		headers_used++;
		bytes_used += n;

		/* Same type repeated: the last one wins. */
		if (is_name)
		{
			tar_gnu_free(pending_name);
			pending_name = value;
		}
		else
		{
			tar_gnu_free(pending_link);
			pending_link = value;
		}

		if (th_read_chain_header(t) != 0)
			goto fail;
	}

	/* Commit pending names with the validated ordinary header. */
	t->th_buf.gnu_longname = pending_name;
	t->th_buf.gnu_longlink = pending_link;
	pending_name = NULL;
	pending_link = NULL;

#ifdef DEBUG
	if (t->th_buf.gnu_longname != NULL)
		printf("    th_read(): t->th_buf.gnu_longname == \"%s\"\n",
		       t->th_buf.gnu_longname);
	if (t->th_buf.gnu_longlink != NULL)
		printf("    th_read(): t->th_buf.gnu_longlink == \"%s\"\n",
		       t->th_buf.gnu_longlink);
#endif

#if 0
	/*
	** work-around for old archive files with broken typeflag fields
	** NOTE: I fixed this in the TH_IS*() macros instead
	*/

	/*
	** (directories are signified with a trailing '/')
	*/
	if (t->th_buf.typeflag == AREGTYPE
	    && t->th_buf.name[strlen(t->th_buf.name) - 1] == '/')
		t->th_buf.typeflag = DIRTYPE;

	/*
	** fallback to using mode bits
	*/
	if (t->th_buf.typeflag == AREGTYPE)
	{
		mode = (mode_t)oct_to_int(t->th_buf.mode);

		if (S_ISREG(mode))
			t->th_buf.typeflag = REGTYPE;
		else if (S_ISDIR(mode))
			t->th_buf.typeflag = DIRTYPE;
		else if (S_ISFIFO(mode))
			t->th_buf.typeflag = FIFOTYPE;
		else if (S_ISCHR(mode))
			t->th_buf.typeflag = CHRTYPE;
		else if (S_ISBLK(mode))
			t->th_buf.typeflag = BLKTYPE;
		else if (S_ISLNK(mode))
			t->th_buf.typeflag = SYMTYPE;
	}
#endif

	return 0;

fail:
	{
		int saved = errno;

		tar_gnu_free(pending_name);
		tar_gnu_free(pending_link);
		t->th_buf.gnu_longname = NULL;
		t->th_buf.gnu_longlink = NULL;
		t->gnu_read_error = 1;
		t->gnu_read_error_errno = saved;
		errno = saved;
	}
	return -1;
}


/* Set GNU extension limits for a handle. */
int
tar_set_gnu_extension_limits(TAR *t, size_t bytes, size_t headers)
{
	if (t == NULL || bytes == 0 || headers == 0)
	{
		errno = EINVAL;
		return -1;
	}

	/* A handle in a read-error state cannot be reconfigured; leaving the
	   error state intact keeps the failure sticky. */
	if (t->gnu_read_error)
	{
		errno = EINVAL;
		return -1;
	}

	t->gnu_limit_bytes = bytes;
	t->gnu_limit_headers = headers;
	return 0;
}


/* write a header block */
int
th_write(TAR *t)
{
	int i, j;
	char type2;
	size_t sz, sz2;
	char *ptr;
	char buf[T_BLOCKSIZE];

#ifdef DEBUG
	printf("==> th_write(TAR=\"%s\")\n", t->pathname);
	th_print(t);
#endif

	if ((t->options & TAR_GNU) && t->th_buf.gnu_longlink != NULL)
	{
#ifdef DEBUG
		printf("th_write(): using gnu_longlink (\"%s\")\n",
		       t->th_buf.gnu_longlink);
#endif
		/* save old size and type */
		type2 = t->th_buf.typeflag;
		sz2 = th_get_size(t);

		/* write out initial header block with fake size and type */
		t->th_buf.typeflag = GNU_LONGLINK_TYPE;
		sz = strlen(t->th_buf.gnu_longlink);
		th_set_size(t, sz);
		th_finish(t);
		i = tar_block_write(t, &(t->th_buf));
		if (i != T_BLOCKSIZE)
		{
			if (i != -1)
				errno = EINVAL;
			return -1;
		}

		/* write out extra blocks containing long name */
		for (j = (sz / T_BLOCKSIZE) + (sz % T_BLOCKSIZE ? 1 : 0),
		     ptr = t->th_buf.gnu_longlink; j > 1;
		     j--, ptr += T_BLOCKSIZE)
		{
			i = tar_block_write(t, ptr);
			if (i != T_BLOCKSIZE)
			{
				if (i != -1)
					errno = EINVAL;
				return -1;
			}
		}
		memset(buf, 0, T_BLOCKSIZE);
		strncpy(buf, ptr, T_BLOCKSIZE);
		i = tar_block_write(t, &buf);
		if (i != T_BLOCKSIZE)
		{
			if (i != -1)
				errno = EINVAL;
			return -1;
		}

		/* reset type and size to original values */
		t->th_buf.typeflag = type2;
		th_set_size(t, sz2);
	}

	if ((t->options & TAR_GNU) && t->th_buf.gnu_longname != NULL)
	{
#ifdef DEBUG
		printf("th_write(): using gnu_longname (\"%s\")\n",
		       t->th_buf.gnu_longname);
#endif
		/* save old size and type */
		type2 = t->th_buf.typeflag;
		sz2 = th_get_size(t);

		/* write out initial header block with fake size and type */
		t->th_buf.typeflag = GNU_LONGNAME_TYPE;
		sz = strlen(t->th_buf.gnu_longname);
		th_set_size(t, sz);
		th_finish(t);
		i = tar_block_write(t, &(t->th_buf));
		if (i != T_BLOCKSIZE)
		{
			if (i != -1)
				errno = EINVAL;
			return -1;
		}

		/* write out extra blocks containing long name */
		for (j = (sz / T_BLOCKSIZE) + (sz % T_BLOCKSIZE ? 1 : 0),
		     ptr = t->th_buf.gnu_longname; j > 1;
		     j--, ptr += T_BLOCKSIZE)
		{
			i = tar_block_write(t, ptr);
			if (i != T_BLOCKSIZE)
			{
				if (i != -1)
					errno = EINVAL;
				return -1;
			}
		}
		memset(buf, 0, T_BLOCKSIZE);
		strncpy(buf, ptr, T_BLOCKSIZE);
		i = tar_block_write(t, &buf);
		if (i != T_BLOCKSIZE)
		{
			if (i != -1)
				errno = EINVAL;
			return -1;
		}

		/* reset type and size to original values */
		t->th_buf.typeflag = type2;
		th_set_size(t, sz2);
	}

	th_finish(t);

#ifdef DEBUG
	/* print tar header */
	th_print(t);
#endif

	i = tar_block_write(t, &(t->th_buf));
	if (i != T_BLOCKSIZE)
	{
		if (i != -1)
			errno = EINVAL;
		return -1;
	}

#ifdef DEBUG
	puts("th_write(): returning 0");
#endif
	return 0;
}


