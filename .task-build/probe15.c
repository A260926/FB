#include <event2/buffer.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void show(const char *t, struct evbuffer *b) {
    char out[80];
    size_t n = evbuffer_get_length(b);
    memset(out, 0, sizeof out);
    evbuffer_copyout(b, out, n < 79 ? n : 79);
    printf("%s len=%zu data=%s\n", t, n, out);
}

int main(void) {
    struct evbuffer *src = evbuffer_new();
    struct evbuffer *a = evbuffer_new();
    struct evbuffer *b = evbuffer_new();

    evbuffer_add(src, "AAAA", 4);
    evbuffer_add(src, "BBBB", 4);
    evbuffer_add(src, "CCCC", 4);
    evbuffer_add_buffer_reference(a, src);
    evbuffer_add_buffer_reference(b, src);

    /* drain a prefix in b so the shared chains are misaligned, then pullup */
    evbuffer_drain(b, 3);
    show("a before", a);
    show("b before", b);
    unsigned char *p = evbuffer_pullup(b, 9);
    printf("pullup(b,9)=%p\n", (void*)p);
    show("b after ", b);
    show("a after ", a);

    int ok = evbuffer_get_length(a) == 12;
    char expect[16]; memset(expect, 0, sizeof expect);
    evbuffer_copyout(a, expect, 12);
    ok = ok && memcmp(expect, "AAAABBBBCCCC", 12) == 0;
    printf(ok ? "PULLUP_DRAIN_OK\n" : "PULLUP_DRAIN_CORRUPT\n");

    evbuffer_free(src); evbuffer_free(a); evbuffer_free(b);
    return ok ? 0 : 1;
}
