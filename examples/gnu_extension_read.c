/*
**  gnu_extension_read.c - read GNU long names without extracting files
**
**  This example opens a tar archive with libtar's in-memory-agnostic
**  handle API, configures the GNU extension budget, reads one logical
**  record, prints the resulting names, and closes the handle.
**
**  It deliberately never calls into the extraction entry points: the
**  regular file body stays unread on the archive, and the next th_read()
**  call would skip it.
**
**  Build (from the top of the source tree):
**      cc -std=c99 -Ilib examples/gnu_extension_read.c \
**          .task-build/libtar.a -o gnu_extension_read
**  (use the same sanitizer flags, if any, that the library was built with)
**
**  Usage: gnu_extension_read ARCHIVE
*/

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include "libtar.h"

int
main(int argc, char **argv)
{
	TAR *t = NULL;
	int rc;

	if (argc != 2)
	{
		fprintf(stderr, "usage: %s ARCHIVE\n", argv[0]);
		return 2;
	}

	if (tar_open(&t, argv[1], NULL, O_RDONLY, 0, 0) != 0)
	{
		fprintf(stderr, "tar_open: %s\n", strerror(errno));
		return 1;
	}

	/* At most 64 KiB of declared extension payload and 8 extension
	   headers per logical record.  Both values must be positive. */
	if (tar_set_gnu_extension_limits(t, 64 * 1024, 8) != 0)
	{
		fprintf(stderr, "tar_set_gnu_extension_limits: %s\n",
			strerror(errno));
		tar_close(t);
		return 1;
	}

	rc = th_read(t);
	if (rc == 0)
	{
		/* The strings point into memory owned by the handle.  They
		   stay valid until the next th_read() or tar_close(). */
		printf("pathname: %s\n", th_get_pathname(t));
		printf("linkname: %s\n", th_get_linkname(t));
		printf("size:     %lu\n", (unsigned long)th_get_size(t));
	}
	else if (rc == 1)
	{
		/* Normal end of archive: the double zero block was seen. */
		puts("end of archive");
	}
	else if (errno == E2BIG)
	{
		/* The record declared more extension data than allowed.  The
		   chain was rejected before its payload was read.  The handle
		   is now in a read-error state: it can still be closed (and
		   the backend descriptor is closed exactly once), but
		   th_read() and the limit setter will keep failing. */
		fprintf(stderr, "extension budget exceeded: %s\n",
			strerror(errno));
	}
	else
	{
		/* Malformed or truncated chain, allocation failure, or a
		   reader error.  Only tar_close() remains usable. */
		fprintf(stderr, "th_read: %s\n", strerror(errno));
	}

	/* tar_close() releases the extension strings owned by the handle
	   and closes the underlying backend, including after a failure. */
	if (tar_close(t) != 0)
	{
		fprintf(stderr, "tar_close failed\n");
		return 1;
	}

	return (rc == 0 || rc == 1) ? 0 : 1;
}
