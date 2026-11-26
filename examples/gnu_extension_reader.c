/*
** gnu_extension_reader.c
**
** Small C99 example for the bounded GNU longname/longlink read chain.
**
** It opens a tar archive named on the command line, optionally adjusts
** the per-record GNU extension budgets, and prints each entry's pathname
** and linkname (including normalized GNU 'L'/'K' values) before closing
** the handle.  Regular-file bodies are skipped through the ordinary
** tar_skip_regfile() entry point; th_read() itself never consumes body
** data.
**
** Usage:
**   gnu_extension_reader ARCHIVE.tar [BYTE_LIMIT HEADER_LIMIT]
**
** Budget model (see tar_set_gnu_extension_limits()):
**   * Every handle starts with a default budget of 1 MiB of declared
**     extension payload bytes and 64 GNU L/K extension headers per
**     logical record.
**   * Both limits are per logical record and reset on the next
**     th_read().  Every declaration counts towards the byte total,
**     even when a later copy of the same kind overrides it, and every
**     extension header counts towards the header total.
**   * Reaching a limit exactly is allowed; exceeding it fails
**     th_read() with -1/errno=E2BIG before the offending payload is
**     allocated or read.
**
** Lifetime:
**   The pointers returned by th_get_pathname() and th_get_linkname(),
**   and t->th_buf.gnu_longname/gnu_longlink, refer to library storage
**   that is released or overwritten at the START of the next
**   th_read() (and at tar_close()).  Copy a name if it must survive
**   the next th_read() call.
**
** After a read error:
**   The TAR handle enters a sticky read-error state.  Later th_read()
**   calls return -1 with the original errno and perform no backend
**   reads; tar_set_gnu_extension_limits() cannot clear the state.
**   The only productive operation left is tar_close(), which releases
**   all memory still owned by the handle and calls the backend close
**   function as usual.
*/

#include <libtar.h>

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>

int
main(int argc, char **argv)
{
	TAR *t = NULL;
	int rc;
	int exit_status = 0;

	if (argc < 2 || argc > 4)
	{
		fprintf(stderr,
			"usage: %s ARCHIVE.tar [BYTE_LIMIT HEADER_LIMIT]\n",
			argv[0]);
		return 2;
	}

	if (tar_open(&t, argv[1], NULL, O_RDONLY, 0, TAR_GNU) == -1)
	{
		fprintf(stderr, "tar_open(\"%s\"): %s\n",
			argv[1], strerror(errno));
		return 1;
	}

	/* Optional: replace the default 1 MiB / 64 extension budget. */
	if (argc == 4)
	{
		size_t byte_limit = (size_t)strtoul(argv[2], NULL, 10);
		size_t header_limit = (size_t)strtoul(argv[3], NULL, 10);

		if (tar_set_gnu_extension_limits(t, byte_limit,
						 header_limit) == -1)
		{
			fprintf(stderr,
				"tar_set_gnu_extension_limits: %s\n"
				"(both limits must be positive; "
				"previous settings unchanged)\n",
				strerror(errno));
			tar_close(t);
			return 2;
		}
		printf("# extension limits: %s bytes, %s headers\n",
		       argv[2], argv[3]);
	}
	else
	{
		printf("# extension limits: 1048576 bytes, 64 headers "
		       "(defaults)\n");
	}

	for (;;)
	{
		rc = th_read(t);

		if (rc == 1)
		{
			/* normal end of archive */
			break;
		}
		if (rc == -1)
		{
			/*
			** Format/truncation/dangling-chain problems are
			** EINVAL, allocation failure is ENOMEM, budget
			** overflow is E2BIG, and a backend reader failure
			** preserves the reader's own errno (often EIO).
			*/
			fprintf(stderr, "th_read: %s\n", strerror(errno));
			fputs("# handle is now in a sticky read-error "
			      "state; only tar_close() is useful\n", stderr);

			/* This replay performs no backend reads. */
			errno = 0;
			if (th_read(t) != -1)
				fputs("warning: sticky error not replayed\n",
				      stderr);
			else
				fprintf(stderr,
					"# repeated th_read replays: %s\n",
					strerror(errno));
			exit_status = 1;
			break;
		}

		/* rc == 0: a complete, validated logical record */
		printf("pathname: %s\n", th_get_pathname(t));
		printf("linkname: %s\n", th_get_linkname(t));

		/*
		** Names are only valid until the next th_read(); we do
		** not retain the pointers here.  Consume a regular file
		** body so the next header is aligned, without extracting.
		*/
		if (TH_ISREG(t) && tar_skip_regfile(t) == -1)
		{
			fprintf(stderr, "tar_skip_regfile: %s\n",
				strerror(errno));
			exit_status = 1;
			break;
		}
	}

	/* Always close: frees owned extension strings and the TAR. */
	if (tar_close(t) == -1)
	{
		fprintf(stderr, "tar_close: %s\n", strerror(errno));
		return 1;
	}
	return exit_status;
}
