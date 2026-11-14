#include <stdio.h>
#include <event2/buffer.h>
#include "event2/buffer_compat.h"
#include "buffer_atomic_contract.c"
#include "evbuffer-internal.h"

int task_check(int value, const char *expr, const char *file, int line) {
    (void)expr; (void)file; (void)line;
    return value;
}

static void dump_chain_sources(const struct model_state *ms) {
    size_t b;
    for (b = 0; b < MODEL_NBUFS; ++b) {
        struct evbuffer_chain *c;
        printf("buf%zu refcnt=%d mcast=%02x\n", b,
            ms->real[b]->refcnt, ms->mcast_src[b]);
        for (c = ms->real[b]->first; c; c = c->next) {
            if (c->flags & EVBUFFER_MULTICAST) {
                struct evbuffer_multicast_parent *x =
                    EVBUFFER_CHAIN_EXTRA(struct evbuffer_multicast_parent, c);
                printf("   MC off=%zu source=%p (", c->off, (void*)x->source);
                size_t s;
                for (s = 0; s < MODEL_NBUFS; ++s)
                    if (ms->real[s] == x->source) printf("buf%zu", s);
                printf(") parent=%p\n", (void*)x->parent);
            }
        }
    }
}

int main(void) {
    ensure_allocator();
    /* replicate run up to 46 but intercept via a second run is not possible;
       instead run full and dump at end via a hook below. */
    struct buffer_model_result r = buffer_model_run(83, 46);
    printf("failed=%d field=%s live=%zu\n", r.failed,
        r.failed ? r.field : "-", buffer_test_live_allocations());
    return 0;
}
