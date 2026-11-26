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
#include <stdint.h>

#ifdef STDC_HEADERS
# include <string.h>
# include <stdlib.h>
#endif


#define BIT_ISSET(bitmask, bit) ((bitmask) & (bit))


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


/*
** Strict, bounded decode of the size field of a GNU 'L' (longname) or
** 'K' (longlink) extension header.
**
** The grammar, restricted to exactly the @len bytes of @field (always
** 12 for the on-disk header size field), is:
**
**	[' ' | NUL]*  [0-7]+  [' ' | NUL]*
**
** There must be at least one octal digit, padding bytes may only be
** spaces or NULs, and such padding may not appear in the middle of the
** digit run.  Signs, '8'/'9', base-256 markers and any other character
** are rejected.  The field is never inspected past @len bytes, so an
** unterminated 12-byte field cannot borrow a terminator from an
** adjacent header field.  The decoded value must be positive and fit
** in size_t.
**
** Returns 0 and stores the value in @value, or -1 with errno=EINVAL.
*/
static int
gnu_ext_decode_size(const char *field, size_t len, size_t *value)
{
	size_t i = 0;
	size_t acc = 0;
	int digits = 0;
	unsigned d;

	if (field == NULL || value == NULL || len == 0)
	{
		errno = EINVAL;
		return -1;
	}

	/* leading padding: spaces or NULs only */
	while (i < len && (field[i] == ' ' || field[i] == '\0'))
		i++;

	/* maximal octal digit run */
	while (i < len && field[i] >= '0' && field[i] <= '7')
	{
		d = (unsigned)((unsigned char)field[i] - '0');

		/* checked accumulation: acc * 8 + d must fit size_t */
		if (acc > (SIZE_MAX - (size_t)d) / 8)
		{
			errno = EINVAL;
			return -1;
		}
		acc = acc * 8 + d;
		digits = 1;
		i++;
	}

	if (!digits)
	{
		errno = EINVAL;
		return -1;
	}

	/* all remaining bytes must be spaces or NULs */
	while (i < len)
	{
		if (field[i] != ' ' && field[i] != '\0')
		{
			errno = EINVAL;
			return -1;
		}
		i++;
	}

	/* a zero-length extension is not legal */
	if (acc == 0)
	{
		errno = EINVAL;
		return -1;
	}

	*value = acc;
	return 0;
}


/*
** Transition to the sticky read-error state.  All uncommitted pending
** extension strings are released here.  Published strings live in
** t->th_buf and are owned by the TAR handle; on a failure path none
** have been published yet, so those pointers are NULL (clear them
** defensively as well).
*/
static int
th_read_fail(TAR *t, char *pending_name, char *pending_link, int error)
{
	free(pending_name);
	free(pending_link);

	t->th_buf.gnu_longname = NULL;
	t->th_buf.gnu_longlink = NULL;

	t->read_failed = 1;
	t->read_failure_errno = error;
	errno = error;
	return -1;
}


/*
** Read the next logical tar record.
**
** A logical record is zero or more GNU longname ('L') / longlink ('K')
** extension headers, each followed by the complete number of 512-byte
** data blocks declared by its strictly decoded size field, and finally
** an ordinary header.  Extensions may appear in any order and repeat;
** the last copy of each kind wins, only for the immediately following
** ordinary record.
**
** Returns 0 once the ordinary header has been read and validated, 1 on
** clean EOF, and -1 on error (errno set).  After any -1 the handle is
** in a sticky read-error state: further th_read() calls replay the
** original failure without invoking the reader, and only tar_close()
** remains valid.
*/
int
th_read(TAR *t)
{
	int i;
	size_t n, blocks, b;
	size_t payload_offset;
	size_t name_len;
	char *pending_name = NULL;
	char *pending_link = NULL;
	char *name = NULL;
	char block[T_BLOCKSIZE];

	if (t == NULL)
	{
		errno = EINVAL;
		return -1;
	}

	/* replay a sticky failure without touching the backend */
	if (t->read_failed)
	{
		errno = t->read_failure_errno;
		return -1;
	}

#ifdef DEBUG
	printf("==> th_read(t=0x%lx)\n", (unsigned long)t);
#endif

	/*
	** Invalidate the previous entry view: release the published
	** extension strings and clear the whole header buffer.
	*/
	free(t->th_buf.gnu_longname);
	free(t->th_buf.gnu_longlink);
	memset(&(t->th_buf), 0, sizeof(struct tar_header));

	/* extension budgets accumulate per logical record */
	t->gnu_used_bytes = 0;
	t->gnu_used_headers = 0;

	for (;;)
	{
		i = th_read_internal(t);

		if (i == 0)
		{
			if (t->gnu_used_headers == 0)
			{
				/* clean EOF / EOT at a record boundary */
				return 1;
			}

			/*
			** Extension header(s) consumed but no ordinary
			** header followed: EOF, zero terminator blocks or
			** a vanished next header makes the chain dangling.
			*/
			return th_read_fail(t, pending_name, pending_link,
					     EINVAL);
		}

		if (i != T_BLOCKSIZE)
		{
			/*
			** i == -1: the reader failed; errno belongs to it
			** and must be preserved.  i == -2 is magic/version/
			** checksum validation failure.  Any other value is
			** a short header read.  All non-reader cases EINVAL.
			*/
			int error = (i == -1) ? errno : EINVAL;
			return th_read_fail(t, pending_name, pending_link,
					     error);
		}

		/* the terminal ordinary header publishes pending names */
		if (!TH_ISLONGNAME(t) && !TH_ISLONGLINK(t))
		{
			t->th_buf.gnu_longname = pending_name;
			t->th_buf.gnu_longlink = pending_link;
			return 0;
		}

		/* ---- GNU 'L'/'K' extension header ---- */

		/*
		** Extension-header budget.  Every extension header counts,
		** including repeated kinds that override earlier copies.
		*/
		t->gnu_used_headers++;
		if (t->gnu_used_headers > t->gnu_limit_headers)
		{
			return th_read_fail(t, pending_name, pending_link,
					     E2BIG);
		}

		/* strict bounded decode of only the 12 size bytes */
		if (gnu_ext_decode_size(t->th_buf.size,
					sizeof(t->th_buf.size), &n) == -1)
		{
			return th_read_fail(t, pending_name, pending_link,
					     EINVAL);
		}

		/*
		** Declared-payload byte budget: every declaration counts,
		** even when a later copy overrides it.  The subtraction
		** form cannot overflow because gnu_used_bytes is kept
		** <= gnu_limit_bytes.
		*/
		if (n > t->gnu_limit_bytes - t->gnu_used_bytes)
		{
			return th_read_fail(t, pending_name, pending_link,
					     E2BIG);
		}
		t->gnu_used_bytes += n;

		/* representability of the extra terminator byte */
		if (n > SIZE_MAX - (size_t)1)
		{
			return th_read_fail(t, pending_name, pending_link,
					     EINVAL);
		}

		/* checked round-up to a whole number of T_BLOCKSIZE blocks */
		if (n > SIZE_MAX - (T_BLOCKSIZE - 1))
		{
			return th_read_fail(t, pending_name, pending_link,
					     EINVAL);
		}
		blocks = (n + (T_BLOCKSIZE - 1)) / T_BLOCKSIZE;

		/*
		** Library-owned normalized string: exactly the name bytes
		** plus one guaranteed terminator.
		*/
		name = (char *)malloc(n + 1);
		if (name == NULL)
		{
			return th_read_fail(t, pending_name, pending_link,
					     ENOMEM);
		}
		name_len = 0;

		/*
		** Consume exactly the ceil(n / T_BLOCKSIZE) complete data
		** blocks.  Blocks go through one staging buffer; only the
		** first n bytes of the declaration are string content, so
		** block padding is never scanned into the name.
		*/
		for (b = 0; b < blocks; b++)
		{
			size_t j;

			i = tar_block_read(t, block);
			if (i != T_BLOCKSIZE)
			{
				int error = (i == -1) ? errno : EINVAL;
				free(name);
				return th_read_fail(t, pending_name,
						    pending_link, error);
			}

			payload_offset = b * T_BLOCKSIZE;
			for (j = 0; j < T_BLOCKSIZE; j++)
			{
				size_t pos = payload_offset + j;
				unsigned char c;

				if (pos >= n)
					break;	/* remainder is padding */

				c = (unsigned char)block[j];
				if (c == '\0')
				{
					/*
					** A NUL is tolerated only as the
					** final declared byte; an earlier
					** NUL means trailing content would
					** be silently dropped.
					*/
					if (pos + 1 != n)
					{
						free(name);
						return th_read_fail(t,
							pending_name,
							pending_link, EINVAL);
					}
					/* terminal NUL: not part of name */
				}
				else
				{
					name[name_len++] = (char)c;
				}
			}
		}
		name[name_len] = '\0';

		/* commit into pending state; last copy of a kind wins */
		if (TH_ISLONGNAME(t))
		{
			free(pending_name);
			pending_name = name;
		}
		else
		{
			free(pending_link);
			pending_link = name;
		}
		name = NULL;
	}

	/* not reached */
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
#ifdef DEBUG
			printf("    th_write(): writing long linkname "
			       "(%d blocks left)\n", j);
#endif
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
#ifdef DEBUG
			printf("    th_write(): writing long filename "
			       "(%d blocks left)\n", j);
#endif
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
