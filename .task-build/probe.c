#include <event2/buffer.h>
#include <event2/event.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fail_at = -1;
static void *allocfn(size_t n) { if (fail_at > 0 && --fail_at == 0) { fail_at = -1; return NULL; } return malloc(n); }
static void *reallocfn(void *p, size_t n) { if (fail_at > 0 && --fail_at == 0) { fail_at = -1; return NULL; } return realloc(p, n); }
static void cleanup(const void *p, size_t n, void *a) { (void)p; (void)n; (void)a; }

#define STEP(s) do { write(2, "STEP " s "\n", 6 + sizeof(s)); } while (0)

int main(void) {
    struct evbuffer *s, *d;
    const char *blocks[] = {"ABCD", "EFGH", "IJKL"};
    char out[32] = {0};
    event_set_mem_functions(allocfn, reallocfn, free);
    STEP("new");
    s = evbuffer_new(); d = evbuffer_new();
    if (!s || !d) return 10;
    STEP("add_reference x3");
    for (int i = 0; i < 3; i++)
        if (evbuffer_add_reference(s, blocks[i], 4, cleanup, NULL) != 0) return 11;
    STEP("add D");
    if (evbuffer_add(d, "D", 1) != 0) return 12;
    STEP("add_buffer_reference");
    if (evbuffer_add_buffer_reference(d, s) != 0) return 13;
    STEP("length check");
    if (evbuffer_get_length(d) != 13) return 14;
    STEP("free s");
    evbuffer_free(s);
    STEP("copyout");
    if (evbuffer_copyout(d, out, 13) != 13) return 15;
    if (memcmp(out, "DABCDEFGHIJKL", 13) != 0) return 16;
    STEP("free d");
    evbuffer_free(d);
    STEP("done");
    puts("PROBE_OK");
    return 0;
}
