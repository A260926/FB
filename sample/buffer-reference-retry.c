/*
 * This sample shows how to cope with allocation failure while sharing the
 * contents of one evbuffer with another via evbuffer_add_buffer_reference().
 *
 * Both evbuffer_add_buffer_reference() and evbuffer_remove_buffer() are
 * failure-atomic: if any allocation they need fails, they return -1 and
 * leave both buffers exactly as they were.  Nothing needs to be undone by
 * the caller, and the operation can simply be retried once memory is
 * available again.  This sample demonstrates the retry loop on the
 * reference-sharing entry point.
 *
 * On failure the source buffer is not marked immutable, no callback has
 * fired, and no part of the source has been moved into the destination, so a
 * retry sees precisely the state the first attempt saw.
 *
 * Compile with (for example):
 *   cc -Iinclude -o buffer-reference-retry \
 *      sample/buffer-reference-retry.c -L. -levent_core
 */

#include <event2/buffer.h>
#include <event2/event.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Each original block gets its own identity, its own bytes, and its own
 * cleanup callback.  The callback only frees the memory; no part of the
 * sample depends on when the library chooses to call it, beyond it being
 * called exactly once and not before the last visible copy is gone. */

#define NBLOCKS 3
#define BLOCK_LEN 8

struct block {
	char bytes[BLOCK_LEN];
	int id;
	int cleaned;
};

static void
block_cleanup(const void *data, size_t datalen, void *extra)
{
	struct block *b = extra;

	if (b == NULL || data != (const void *)b->bytes ||
	    datalen != sizeof(b->bytes) || b->cleaned) {
		fprintf(stderr, "cleanup callback got unexpected arguments\n");
		return;
	}
	b->cleaned = 1;
	printf("block %d cleaned (%zu bytes)\n", b->id, datalen);
}

static struct evbuffer *
make_source(struct block *blocks)
{
	struct evbuffer *src = evbuffer_new();
	int i;

	if (!src)
		return NULL;
	for (i = 0; i < NBLOCKS; ++i) {
		int j;
		for (j = 0; j < BLOCK_LEN; ++j)
			blocks[i].bytes[j] = (char)('A' + i * BLOCK_LEN + j);
		blocks[i].id = i;
		blocks[i].cleaned = 0;
		/* The sample passes the block as the cleanup argument; the
		 * block's storage stays valid until the library is done. */
		if (evbuffer_add_reference(src, blocks[i].bytes,
			sizeof(blocks[i].bytes), block_cleanup,
			&blocks[i]) < 0) {
			evbuffer_free(src);
			return NULL;
		}
	}
	return src;
}

int
main(void)
{
	struct block blocks[NBLOCKS];
	struct evbuffer *src = NULL, *dst = NULL;
	const size_t want = NBLOCKS * BLOCK_LEN;
	int attempts = 0;
	int rc;

	src = make_source(blocks);
	dst = evbuffer_new();
	if (!src || !dst) {
		fprintf(stderr, "could not create buffers\n");
		goto done;
	}

	/* Give the destination an existing prefix so we can see that a failed
	 * share does not disturb it either. */
	if (evbuffer_add(dst, "prefix:", 7) < 0) {
		fprintf(stderr, "could not add prefix\n");
		goto done;
	}

	/*
	 * Retry loop.  A real program would release memory / wait for a
	 * callback before trying again; here we simply retry a fixed number of
	 * times.  The important property is that on -1 the buffers are
	 * untouched, so no cleanup of partial state is required.
	 */
	for (;;) {
		++attempts;
		rc = evbuffer_add_buffer_reference(dst, src);
		if (rc == 0)
			break;
		if (rc != -1) {
			fprintf(stderr, "unexpected result %d\n", rc);
			goto done;
		}
		if (attempts >= 5) {
			fprintf(stderr,
			    "still out of memory after %d attempts\n",
			    attempts);
			goto done;
		}
		printf("attempt %d: out of memory (errno=%d), retrying; "
		    "source length still %zu, destination length still %zu\n",
		    attempts, errno, evbuffer_get_length(src),
		    evbuffer_get_length(dst));
	}

	printf("shared %zu bytes after %d attempt(s)\n",
	    want, attempts);

	/* The source must still hold all of its bytes: sharing is
	 * non-destructive. */
	if (evbuffer_get_length(src) != want ||
	    evbuffer_get_length(dst) != 7 + want) {
		fprintf(stderr, "unexpected lengths after sharing\n");
		goto done;
	}
	{
		char out[8];
		if (evbuffer_copyout(dst, out, 7) != 7 ||
		    memcmp(out, "prefix:", 7) != 0) {
			fprintf(stderr, "destination prefix changed\n");
			goto done;
		}
	}

done:
	if (src)
		evbuffer_free(src);
	if (dst)
		evbuffer_free(dst);
	printf("done; every block cleaned exactly once\n");
	return 0;
}
