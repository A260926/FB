/*
  This example demonstrates retrying a shared evbuffer snapshot
  (evbuffer_add_buffer_reference) after an allocation failure.

  The two fixed entry points, evbuffer_add_buffer_reference() and
  evbuffer_remove_buffer(), are atomic on failure: when memory is exhausted
  they return -1 and leave both the source and the destination exactly as
  they were on entry, so the same call can simply be retried once memory is
  available again.

  A controlled allocator is installed before any libevent object is created
  and programmed to fail a single allocation, proving that the failed share
  changes neither the bytes, lengths, reference relationships nor the
  change-callback accounting.  The retry then succeeds and every external
  block is cleaned up exactly once.

  This sample is single threaded and uses no networking.
*/

#include <event2/buffer.h>
#include <event2/event.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- controlled allocator: libc backed, one programmable failure ------ */

static int fail_at = -1;

static void
discard_log(int severity, const char *msg)
{
	/* The injected allocation failure is deliberate in this sample. */
	(void)severity;
	(void)msg;
}

static void *
sample_malloc(size_t n)
{
	if (fail_at > 0 && --fail_at == 0) {
		fail_at = -1;
		return NULL;
	}
	return malloc(n);
}

static void *
sample_realloc(void *p, size_t n)
{
	if (fail_at > 0 && --fail_at == 0) {
		fail_at = -1;
		return NULL;
	}
	return realloc(p, n);
}

/* ---- external blocks with an identity-checked cleanup ------------------ */

struct block {
	unsigned char *data;
	size_t len;
	int id;
	int cleanups;
};

static void
block_cleanup(const void *p, size_t n, void *arg)
{
	struct block *b = arg;
	if (p != (const void *)b->data || n != b->len)
		abort();
	++b->cleanups;
	free(b->data);
	b->data = NULL;
}

static void
block_make(struct block *b, int id, size_t len)
{
	size_t i;
	b->data = malloc(len);
	b->len = len;
	b->id = id;
	b->cleanups = 0;
	for (i = 0; i < len; ++i)
		b->data[i] = (unsigned char)('a' + (id + (int)i) % 26);
}

struct cb_count {
	size_t added;
	size_t deleted;
};

static void
change_cb(struct evbuffer *buf, const struct evbuffer_cb_info *info, void *arg)
{
	struct cb_count *c = arg;
	(void)buf;
	c->added += info->n_added;
	c->deleted += info->n_deleted;
}

#define NBLOCKS 3

int
main(void)
{
	static const size_t lens[NBLOCKS] = { 5, 7, 4 };
	struct block blocks[NBLOCKS];
	struct cb_count cb = { 0, 0 };
	struct evbuffer *src, *dst;
	char out[64];
	size_t src_len, dst_before;
	size_t i;
	int rc;

	event_set_mem_functions(sample_malloc, sample_realloc, free);
	event_set_log_callback(discard_log);

	src = evbuffer_new();
	dst = evbuffer_new();
	if (!src || !dst)
		return 1;
	evbuffer_add_cb(dst, change_cb, &cb);

	for (i = 0; i < NBLOCKS; ++i) {
		block_make(&blocks[i], (int)i + 1, lens[i]);
		if (evbuffer_add_reference(src, blocks[i].data, blocks[i].len,
		        block_cleanup, &blocks[i]) != 0)
			return 1;
	}
	if (evbuffer_add(dst, "PREFIX:", 7) != 0)
		return 1;

	src_len = evbuffer_get_length(src);
	dst_before = evbuffer_get_length(dst);
	/* Baseline the callback ledger after setup; only deltas caused by the
	 * share calls below matter. */
	{
		size_t added_base = cb.added;
		size_t deleted_base = cb.deleted;

		/* Fail one of the snapshot allocations.  Whichever one is hit,
		 * the call must report -1 and change nothing. */
		fail_at = 2;
		rc = evbuffer_add_buffer_reference(dst, src);
		if (rc != -1) {
			fprintf(stderr, "expected failure, got %d\n", rc);
			return 1;
		}
		if (evbuffer_get_length(dst) != dst_before ||
		    evbuffer_get_length(src) != src_len ||
		    cb.added != added_base || cb.deleted != deleted_base) {
			fprintf(stderr, "failed share mutated buffer state\n");
			return 1;
		}
		for (i = 0; i < NBLOCKS; ++i) {
			if (blocks[i].cleanups != 0)
				return 1;
		}
		puts("failed add_buffer_reference left both buffers untouched");

		/* Retry with memory available: same call now succeeds. */
		rc = evbuffer_add_buffer_reference(dst, src);
		if (rc != 0)
			return 1;
		if (evbuffer_get_length(dst) != dst_before + src_len ||
		    cb.added != added_base + src_len) {
			fprintf(stderr, "retry did not append the full snapshot\n");
			return 1;
		}
	}
	memset(out, 0, sizeof(out));
	if (evbuffer_copyout(dst, out, evbuffer_get_length(dst)) !=
	    (ev_ssize_t)evbuffer_get_length(dst))
		return 1;
	if (memcmp(out, "PREFIX:", 7) != 0)
		return 1;
	printf("retry succeeded, destination now %zu bytes: %s\n",
	    evbuffer_get_length(dst), out);

	/* The source can be released; the shared target keeps the bytes. */
	evbuffer_free(src);
	for (i = 0; i < NBLOCKS; ++i) {
		if (blocks[i].cleanups != 0)
			return 1;
	}
	evbuffer_free(dst);
	for (i = 0; i < NBLOCKS; ++i) {
		if (blocks[i].cleanups != 1)
			return 1;
	}

	puts("BUFFER_RETRY_SAMPLE_OK");
	return 0;
}
