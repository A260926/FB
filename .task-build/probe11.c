#include <event2/buffer.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(void) {
    struct evbuffer *src = evbuffer_new();
    struct evbuffer *a = evbuffer_new();
    struct evbuffer *b = evbuffer_new();
    char out[64];

    evbuffer_add(src, "ABCDEFGHIJKLMNOP", 16);
    /* two independent snapshots of the same source => shared storage */
    if (evbuffer_add_buffer_reference(a, src) != 0) return 2;
    if (evbuffer_add_buffer_reference(b, src) != 0) return 2;

    memset(out, 0, sizeof out);
    evbuffer_copyout(a, out, 16);
    printf("before: a=%.16s\n", out);

    unsigned char *p = evbuffer_pullup(b, 16);
    printf("pullup(b) -> %p\n", (void*)p);

    memset(out, 0, sizeof out);
    evbuffer_copyout(a, out, 16);
    printf("after:  a=%.16s (expect ABCDEFGHIJKLMNOP)\n", out);
    int ok = memcmp(out, "ABCDEFGHIJKLMNOP", 16) == 0;
    printf(ok ? "PULLUP_SHARED_OK\n" : "PULLUP_SHARED_CORRUPT\n");

    evbuffer_free(src); evbuffer_free(a); evbuffer_free(b);
    return ok ? 0 : 1;
}
