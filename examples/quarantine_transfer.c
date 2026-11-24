/* quarantine_transfer.c - legal allocations handed between threads
 *
 * The main thread configures a budgeted FIFO quarantine, frees a few
 * live allocations into it, and detaches the batch onto a ticket. A
 * worker thread then configures its own quarantine, adopts the ticket,
 * snapshots the merged queue and flushes it. Every pointer here is a
 * valid IsoAlloc allocation and is freed exactly once.
 */
#include "iso_alloc.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#define EXAMPLE_CHUNKS 4

static uint64_t example_ticket;

static void *adopter_main(void *arg) {
    (void) arg;

    if(iso_quarantine_configure(EXAMPLE_CHUNKS, 1u << 20) != 0) {
        fprintf(stderr, "worker configure failed\n");
        return (void *) 1;
    }

    iso_quarantine_stats before;

    if(iso_quarantine_snapshot(example_ticket, &before) != 0) {
        fprintf(stderr, "worker ticket snapshot failed\n");
        return (void *) 1;
    }

    printf("ticket holds %zu chunks / %zu bytes\n", before.pending_chunks, before.pending_bytes);

    if(iso_quarantine_adopt(example_ticket) != 0) {
        fprintf(stderr, "worker adopt failed\n");
        return (void *) 1;
    }

    iso_quarantine_stats merged;

    if(iso_quarantine_snapshot(0, &merged) != 0) {
        fprintf(stderr, "worker snapshot failed\n");
        return (void *) 1;
    }

    printf("worker queue holds %zu chunks / %zu bytes after adopt\n",
           merged.pending_chunks, merged.pending_bytes);

    if(merged.pending_chunks != EXAMPLE_CHUNKS) {
        fprintf(stderr, "unexpected adopted chunk count\n");
        return (void *) 1;
    }

    /* Return the adopted chunks to their zones */
    iso_flush_caches();

    iso_quarantine_stats flushed;

    if(iso_quarantine_snapshot(0, &flushed) != 0 || flushed.pending_chunks != 0 ||
       flushed.released_flush != EXAMPLE_CHUNKS) {
        fprintf(stderr, "flush accounting mismatch\n");
        return (void *) 1;
    }

    return NULL;
}

int main(void) {
    iso_alloc_initialize();

    if(iso_quarantine_configure(EXAMPLE_CHUNKS, 1u << 20) != 0) {
        fprintf(stderr, "main configure failed\n");
        return 1;
    }

    size_t expected_bytes = 0;

    for(int i = 0; i < EXAMPLE_CHUNKS; i++) {
        void *p = iso_alloc(64);

        if(p == NULL) {
            fprintf(stderr, "allocation failed\n");
            return 1;
        }

        expected_bytes += iso_chunksz(p);
        iso_free(p);
    }

    iso_quarantine_stats held;

    if(iso_quarantine_snapshot(0, &held) != 0 || held.pending_chunks != EXAMPLE_CHUNKS ||
       held.pending_bytes != expected_bytes) {
        fprintf(stderr, "main snapshot mismatch\n");
        return 1;
    }

    example_ticket = iso_quarantine_detach();

    if(example_ticket == 0) {
        fprintf(stderr, "detach failed\n");
        return 1;
    }

    pthread_t worker;

    if(pthread_create(&worker, NULL, adopter_main, NULL) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        return 1;
    }

    void *worker_status = NULL;

    if(pthread_join(worker, &worker_status) != 0 || worker_status != NULL) {
        fprintf(stderr, "worker failed\n");
        return 1;
    }

    /* The ticket was consumed by adoption so it must no longer resolve */
    iso_quarantine_stats stale;

    if(iso_quarantine_snapshot(example_ticket, &stale) != -1) {
        fprintf(stderr, "consumed ticket still valid\n");
        return 1;
    }

    puts("QUARANTINE_TRANSFER_EXAMPLE_OK");
    return 0;
}
