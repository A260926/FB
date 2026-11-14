#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>

#include "event2/util.h"
#include "event2/event.h"
#include "tinytest.h"
#include "tinytest_macros.h"
#include "regress.h"

/* Local replacement for regress_main.c's regress_make_tmpfile that keeps the
 * temporary file inside the writable build directory rather than /tmp. */
int
regress_make_tmpfile(const void *data, size_t datalen, char **filename_out)
{
	char path[] = ".task-build/eventtmp.XXXXXX";
	int fd = mkstemp(path);
	if (fd == -1)
		return -1;
	if (write(fd, data, datalen) != (int)datalen) {
		close(fd);
		return -1;
	}
	lseek(fd, 0, SEEK_SET);
	unlink(path);
	if (filename_out)
		*filename_out = NULL;
	return fd;
}

/* Minimal stand-in for regress_main.c's basic setup: evbuffer tests need at
 * most an event_base, never the socketpair/DNS machinery. */
static void *
local_basic_setup(const struct testcase_t *tc)
{
	struct basic_test_data *d = calloc(1, sizeof(*d));
	if (!d)
		return NULL;
	d->pair[0] = d->pair[1] = -1;
	if (tc->flags & TT_NEED_SOCKETPAIR) {
		if (evutil_socketpair(AF_UNIX, SOCK_STREAM, 0, d->pair) == -1) {
			free(d);
			return NULL;
		}
		evutil_make_socket_nonblocking(d->pair[0]);
		evutil_make_socket_nonblocking(d->pair[1]);
	}
	if (tc->flags & TT_NEED_BASE)
		d->base = event_base_new();
	d->setup_data = tc->setup_data;
	return d;
}

static int
local_basic_cleanup(const struct testcase_t *tc, void *ptr)
{
	struct basic_test_data *d = ptr;
	if (d) {
		if (d->base)
			event_base_free(d->base);
		if (d->pair[0] != -1)
			evutil_closesocket(d->pair[0]);
		if (d->pair[1] != -1)
			evutil_closesocket(d->pair[1]);
		free(d);
	}
	(void)tc;
	return 1;
}

const struct testcase_setup_t basic_setup = {
	local_basic_setup, local_basic_cleanup
};

extern struct testcase_t evbuffer_testcases[];

static struct testgroup_t groups[] = {
	{ "evbuffer/", evbuffer_testcases },
	END_OF_GROUPS
};

int
main(int argc, const char **argv)
{
	return tinytest_main(argc, argv, groups);
}
