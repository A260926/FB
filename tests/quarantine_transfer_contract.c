/* quarantine_transfer_contract.c
 *
 * Contract cases for the budgeted, transferable FIFO quarantine.
 * Mounted into task_support/driver.c through task_cases / task_case_count
 * and the TASK_CHECK macro. Every operation here is a legal IsoAlloc
 * allocation, free or lifecycle call; no freed address is dereferenced.
 *
 * Groups: budget, handoff, lifecycle.
 */
#include "task_support/check.h"

#include "iso_alloc.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

/* Drains the calling thread, clears its cumulative counters via a
 * detach/discard pair and leaves it in the unconfigured state */
static void reset_main(void) {
    iso_alloc_initialize();
    iso_quarantine_configure(0, 0);
    iso_quarantine_configure(1, 1);
    uint64_t t = iso_quarantine_detach();

    if(t != 0) {
        iso_quarantine_discard(t);
    }
}

/* ---------------------------------- budget ---------------------------------- */

static int case_config_validation(void) {
    reset_main();

    TASK_CHECK(iso_quarantine_configure(0, 0) == 0);

    /* Only (0,0) or 1..64 chunks with positive bytes are accepted */
    TASK_CHECK(iso_quarantine_configure(0, 4096) == -1);
    TASK_CHECK(iso_quarantine_configure(0, 1) == -1);
    TASK_CHECK(iso_quarantine_configure(1, 0) == -1);
    TASK_CHECK(iso_quarantine_configure(65, 4096) == -1);
    TASK_CHECK(iso_quarantine_configure(100000, 4096) == -1);

    /* Boundary values are valid, including a one byte budget */
    TASK_CHECK(iso_quarantine_configure(1, 1) == 0);
    TASK_CHECK(iso_quarantine_configure(64, 1) == 0);
    TASK_CHECK(iso_quarantine_configure(64, 1u << 20) == 0);

    /* A rejected configure leaves the accepted state intact */
    iso_quarantine_stats s;
    TASK_CHECK(iso_quarantine_configure(99, 7) == -1);
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0);
    TASK_CHECK(s.max_chunks == 64);
    TASK_CHECK(s.max_bytes == (1u << 20));

    TASK_CHECK(iso_quarantine_configure(2, 7777) == 0);
    TASK_CHECK(iso_quarantine_configure(0, 0) == 0);
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0);
    TASK_CHECK(s.max_chunks == 0 && s.max_bytes == 0 && s.pending_chunks == 0);
    return 0;
}

static int case_count_byte_accounting(void) {
    reset_main();
    TASK_CHECK(iso_quarantine_configure(4, 1u << 20) == 0);

    void *p[3];
    size_t bytes = 0;

    for(int i = 0; i < 3; i++) {
        p[i] = iso_alloc(96);
        TASK_CHECK(p[i] != NULL);
        bytes += iso_chunksz(p[i]);
    }

    for(int i = 0; i < 3; i++) {
        iso_free(p[i]);
    }

    iso_quarantine_stats s;
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0);
    TASK_CHECK(s.pending_chunks == 3);
    TASK_CHECK(s.pending_bytes == bytes);
    TASK_CHECK(s.max_chunks == 4 && s.max_bytes == (1u << 20));
    TASK_CHECK(s.released_budget == 0 && s.released_flush == 0 && s.released_zone == 0);

    iso_flush_caches();
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0);
    TASK_CHECK(s.pending_chunks == 0 && s.pending_bytes == 0);
    TASK_CHECK(s.released_flush == 3);
    return 0;
}

static int case_count_eviction(void) {
    reset_main();
    TASK_CHECK(iso_quarantine_configure(2, 1u << 20) == 0);

    void *p[3];

    for(int i = 0; i < 3; i++) {
        p[i] = iso_alloc(64);
        TASK_CHECK(p[i] != NULL);
    }

    iso_quarantine_stats s;

    iso_free(p[0]);
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0 && s.pending_chunks == 1 && s.released_budget == 0);
    iso_free(p[1]);
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0 && s.pending_chunks == 2 && s.released_budget == 0);
    iso_free(p[2]);
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0 && s.pending_chunks == 2);
    TASK_CHECK(s.released_budget == 1);

    /* A looser count cap keeps everything up to the cap */
    iso_flush_caches();
    TASK_CHECK(iso_quarantine_configure(3, 1u << 20) == 0);

    void *q[5];

    for(int i = 0; i < 5; i++) {
        q[i] = iso_alloc(64);
        TASK_CHECK(q[i] != NULL);
        iso_free(q[i]);
    }

    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0);
    TASK_CHECK(s.pending_chunks == 3);
    /* One from p[2] era plus two here */
    TASK_CHECK(s.released_budget >= 3);

    iso_flush_caches();
    return 0;
}

static int case_max_chunks_capacity(void) {
    reset_main();
    TASK_CHECK(iso_quarantine_configure(64, 1u << 20) == 0);

    enum { N = 65 };
    void *p[N];

    for(int i = 0; i < N; i++) {
        p[i] = iso_alloc(64);
        TASK_CHECK(p[i] != NULL);
    }

    for(int i = 0; i < N; i++) {
        iso_free(p[i]);
    }

    iso_quarantine_stats s;
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0);
    TASK_CHECK(s.pending_chunks == 64);
    TASK_CHECK(s.released_budget == 1);

    iso_flush_caches();
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0 && s.pending_chunks == 0);
    return 0;
}

static int case_budget_tightening(void) {
    reset_main();

    /* Measure one size class */
    void *measure = iso_alloc(64);
    TASK_CHECK(measure != NULL);
    const size_t z = iso_chunksz(measure);
    iso_free_permanently(measure);

    TASK_CHECK(iso_quarantine_configure(8, 4 * z) == 0);

    void *p[5];

    for(int i = 0; i < 5; i++) {
        p[i] = iso_alloc(64);
        TASK_CHECK(p[i] != NULL);
        iso_free(p[i]);
    }

    iso_quarantine_stats s;
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0);
    TASK_CHECK(s.pending_chunks == 4 && s.pending_bytes == 4 * z);
    TASK_CHECK(s.released_budget == 1);

    /* Tighten bytes: oldest prefix of two must go */
    TASK_CHECK(iso_quarantine_configure(8, 2 * z) == 0);
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0);
    TASK_CHECK(s.pending_chunks == 2 && s.pending_bytes == 2 * z);
    TASK_CHECK(s.released_budget == 3);

    /* Tighten count to one */
    TASK_CHECK(iso_quarantine_configure(1, 1u << 20) == 0);
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0);
    TASK_CHECK(s.pending_chunks == 1 && s.pending_bytes == z);
    TASK_CHECK(s.released_budget == 4);

    iso_flush_caches();
    return 0;
}

static int case_oversize_immediate(void) {
    reset_main();

    void *small = iso_alloc(16);
    TASK_CHECK(small != NULL);
    const size_t z = iso_chunksz(small);

    TASK_CHECK(iso_quarantine_configure(8, 2 * z) == 0);
    iso_free(small);

    iso_quarantine_stats s;
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0 && s.pending_chunks == 1);

    /* Far above 2*z no matter how the small class rounds */
    void *huge = iso_alloc(16384);
    TASK_CHECK(huge != NULL);
    TASK_CHECK(iso_chunksz(huge) > 2 * z);
    iso_free(huge);

    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0);
    TASK_CHECK(s.pending_chunks == 1);
    TASK_CHECK(s.released_budget == 1);

    iso_flush_caches();
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0 && s.pending_chunks == 0);
    return 0;
}

static int case_zero_config_drains_and_disables(void) {
    reset_main();
    TASK_CHECK(iso_quarantine_configure(4, 1u << 20) == 0);

    void *p[2];

    for(int i = 0; i < 2; i++) {
        p[i] = iso_alloc(32);
        TASK_CHECK(p[i] != NULL);
        iso_free(p[i]);
    }

    iso_quarantine_stats s;
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0 && s.pending_chunks == 2);

    /* (0,0) drains as an explicit flush and switches isolation off */
    TASK_CHECK(iso_quarantine_configure(0, 0) == 0);
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0);
    TASK_CHECK(s.pending_chunks == 0 && s.max_chunks == 0 && s.max_bytes == 0);
    TASK_CHECK(s.released_flush == 2);

    /* While off, ordinary frees return immediately */
    void *q = iso_alloc(32);
    TASK_CHECK(q != NULL);
    iso_free(q);
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0 && s.pending_chunks == 0);
    return 0;
}

static int case_null_and_zero_do_not_count(void) {
    reset_main();
    TASK_CHECK(iso_quarantine_configure(4, 1u << 20) == 0);

    iso_free(NULL);
    iso_free_size(NULL, 0);

    void *z = iso_alloc(0);
    iso_free(z);

    iso_quarantine_stats s;
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0 && s.pending_chunks == 0 && s.pending_bytes == 0);
    return 0;
}

static int case_free_entry_points(void) {
    reset_main();
    TASK_CHECK(iso_quarantine_configure(8, 1u << 20) == 0);

    size_t expected_bytes = 0;

    /* sized free */
    void *p = iso_alloc(200);
    TASK_CHECK(p != NULL);
    size_t pz = iso_chunksz(p);
    expected_bytes += pz;
    iso_free_size(p, 200);

    iso_quarantine_stats s;
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0);
    TASK_CHECK(s.pending_chunks == 1 && s.pending_bytes == expected_bytes);

    /* zone free */
    iso_alloc_zone_handle *zone = iso_alloc_new_zone(64);
    TASK_CHECK(zone != NULL);
    void *x = iso_alloc_from_zone(zone);
    TASK_CHECK(x != NULL);
    size_t xz = iso_chunksz(x);
    expected_bytes += xz;
    iso_free_from_zone(x, zone);
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0);
    TASK_CHECK(s.pending_chunks == 2 && s.pending_bytes == expected_bytes);

    /* moving realloc leaves the old block quarantined at its old chunksz */
    void *q = iso_alloc(48);
    TASK_CHECK(q != NULL);
    size_t qz = iso_chunksz(q);
    memset(q, 0x5a, 48);
    void *r = iso_realloc(q, 4096);
    TASK_CHECK(r != NULL);

    for(int i = 0; i < 48; i++) {
        TASK_CHECK(((unsigned char *) r)[i] == 0x5a);
    }

    expected_bytes += qz;
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0);
    TASK_CHECK(s.pending_chunks == 3 && s.pending_bytes == expected_bytes);

    /* The relocated block is a live allocation; free it too */
    size_t rz = iso_chunksz(r);
    expected_bytes += rz;
    iso_free(r);
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0 && s.pending_chunks == 4);
    TASK_CHECK(s.pending_bytes == expected_bytes);

    iso_flush_caches();
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0);
    TASK_CHECK(s.pending_chunks == 0 && s.released_flush == 4);

    iso_alloc_destroy_zone(zone);
    return 0;
}

/* ---------------------------------- handoff ---------------------------------- */

static int case_detach_empty(void) {
    reset_main();
    TASK_CHECK(iso_quarantine_configure(3, 12345) == 0);

    uint64_t t = iso_quarantine_detach();
    TASK_CHECK(t != 0);

    iso_quarantine_stats s;
    TASK_CHECK(iso_quarantine_snapshot(t, &s) == 0);
    TASK_CHECK(s.pending_chunks == 0 && s.pending_bytes == 0);
    TASK_CHECK(s.max_chunks == 3 && s.max_bytes == 12345);

    /* Detach returned the calling thread to the unconfigured state */
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0);
    TASK_CHECK(s.max_chunks == 0 && s.pending_chunks == 0);

    TASK_CHECK(iso_quarantine_discard(t) == 0);
    TASK_CHECK(iso_quarantine_discard(t) == -1);
    TASK_CHECK(iso_quarantine_snapshot(t, &s) == -1);
    return 0;
}

static int case_adopt_appends_and_counts(void) {
    reset_main();

    /* Build a two chunk ticket */
    TASK_CHECK(iso_quarantine_configure(2, 1u << 20) == 0);
    void *a = iso_alloc(64), *b = iso_alloc(64);
    TASK_CHECK(a && b);
    size_t z = iso_chunksz(a);
    iso_free(a);
    iso_free(b);
    uint64_t t = iso_quarantine_detach();
    TASK_CHECK(t != 0);

    /* Target holds one chunk first */
    TASK_CHECK(iso_quarantine_configure(4, 1u << 20) == 0);
    void *c = iso_alloc(64);
    TASK_CHECK(c != NULL);
    iso_free(c);

    TASK_CHECK(iso_quarantine_adopt(t) == 0);

    iso_quarantine_stats s;
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0);
    TASK_CHECK(s.pending_chunks == 3);
    TASK_CHECK(s.pending_bytes == 3 * z);

    /* Ticket is consumed exactly once */
    TASK_CHECK(iso_quarantine_adopt(t) == -1);
    TASK_CHECK(iso_quarantine_snapshot(t, &s) == -1);

    iso_flush_caches();
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0 && s.released_flush == 3);
    return 0;
}

static int case_adopt_failure_retry(void) {
    reset_main();

    TASK_CHECK(iso_quarantine_configure(2, 1u << 20) == 0);
    void *p[2];

    for(int i = 0; i < 2; i++) {
        p[i] = iso_alloc(64);
        TASK_CHECK(p[i] != NULL);
        iso_free(p[i]);
    }

    iso_quarantine_stats ts0;
    uint64_t t = iso_quarantine_detach();
    TASK_CHECK(t != 0);
    TASK_CHECK(iso_quarantine_snapshot(t, &ts0) == 0 && ts0.pending_chunks == 2);
    const size_t ticket_bytes = ts0.pending_bytes;

    /* Count budget too small: atomic failure changes nothing */
    TASK_CHECK(iso_quarantine_configure(1, 1u << 20) == 0);
    TASK_CHECK(iso_quarantine_adopt(t) == -1);

    iso_quarantine_stats s;
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0 && s.pending_chunks == 0);
    TASK_CHECK(iso_quarantine_snapshot(t, &s) == 0 && s.pending_chunks == 2);

    /* Byte budget too small also fails */
    TASK_CHECK(iso_quarantine_configure(8, 1) == 0);
    TASK_CHECK(iso_quarantine_adopt(t) == -1);
    TASK_CHECK(iso_quarantine_snapshot(t, &s) == 0 && s.pending_chunks == 2);

    /* Loosen and retry succeeds, ticket is consumed */
    TASK_CHECK(iso_quarantine_configure(4, ticket_bytes) == 0);
    TASK_CHECK(iso_quarantine_adopt(t) == 0);
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0 && s.pending_chunks == 2);
    TASK_CHECK(s.pending_bytes == ticket_bytes);
    TASK_CHECK(iso_quarantine_adopt(t) == -1);

    iso_flush_caches();
    return 0;
}

static int case_invalid_tickets_and_out_copy(void) {
    reset_main();

    iso_quarantine_stats canary;
    memset(&canary, 0xAB, sizeof(canary));

    /* Snapshot failure must not write out */
    iso_quarantine_stats out = canary;
    TASK_CHECK(iso_quarantine_snapshot(0xDEADBEEFCAFEULL, &out) == -1);
    TASK_CHECK(memcmp(&out, &canary, sizeof(out)) == 0);

    TASK_CHECK(iso_quarantine_adopt(12345) == -1);
    TASK_CHECK(iso_quarantine_discard(12345) == -1);
    TASK_CHECK(iso_quarantine_snapshot(12345, NULL) == -1);

    /* ticket 0 is the live calling thread, never invalid */
    TASK_CHECK(iso_quarantine_snapshot(0, &out) == 0);
    return 0;
}

static int case_ticket_slots_limit(void) {
    reset_main();

    enum { N = 64 };
    uint64_t tickets[N];

    for(int i = 0; i < N; i++) {
        TASK_CHECK(iso_quarantine_configure(1, 4096) == 0);
        tickets[i] = iso_quarantine_detach();
        TASK_CHECK(tickets[i] != 0);
    }

    /* Table full: detach fails and the threads configuration persists */
    TASK_CHECK(iso_quarantine_configure(1, 4096) == 0);
    TASK_CHECK(iso_quarantine_detach() == 0);

    iso_quarantine_stats s;
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0 && s.max_chunks == 1);

    /* Consuming one slot lets a new detach through */
    TASK_CHECK(iso_quarantine_discard(tickets[0]) == 0);
    uint64_t extra = iso_quarantine_detach();
    TASK_CHECK(extra != 0);

    for(int i = 1; i < N; i++) {
        TASK_CHECK(iso_quarantine_discard(tickets[i]) == 0);
    }

    TASK_CHECK(iso_quarantine_discard(extra) == 0);
    return 0;
}

struct worker_ticket_arg {
    iso_alloc_zone_handle *zone;
    uint64_t ticket;
    int chunks;
};

static void *detach_worker(void *arg) {
    struct worker_ticket_arg *w = (struct worker_ticket_arg *) arg;

    if(iso_quarantine_configure(8, 1u << 20) != 0) {
        return (void *) 1;
    }

    for(int i = 0; i < w->chunks; i++) {
        void *p = iso_alloc_from_zone(w->zone);

        if(p == NULL) {
            return (void *) 2;
        }

        iso_free_from_zone(p, w->zone);
    }

    w->ticket = iso_quarantine_detach();

    if(w->ticket == 0) {
        return (void *) 3;
    }

    return NULL;
}

static int case_creator_thread_exit_adopt(void) {
    reset_main();

    iso_alloc_zone_handle *zone = iso_alloc_new_zone(128);
    TASK_CHECK(zone != NULL);

    struct worker_ticket_arg arg = {zone, 0, 2};
    pthread_t th;
    TASK_CHECK(pthread_create(&th, NULL, detach_worker, &arg) == 0);

    void *status = NULL;
    TASK_CHECK(pthread_join(th, &status) == 0);
    TASK_CHECK(status == NULL);
    TASK_CHECK(arg.ticket != 0);

    /* Creating thread is gone but the ticket and bitmap occupancy survive */
    iso_quarantine_stats s;
    TASK_CHECK(iso_quarantine_snapshot(arg.ticket, &s) == 0 && s.pending_chunks == 2);
    TASK_CHECK(iso_alloc_detect_zone_leaks(zone) == 2);

    TASK_CHECK(iso_quarantine_configure(4, 1u << 20) == 0);
    TASK_CHECK(iso_quarantine_adopt(arg.ticket) == 0);
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0 && s.pending_chunks == 2);
    TASK_CHECK(iso_alloc_detect_zone_leaks(zone) == 2);

    iso_flush_caches();
    TASK_CHECK(iso_alloc_detect_zone_leaks(zone) == 0);
    iso_alloc_destroy_zone(zone);
    return 0;
}

/* --------------------------------- lifecycle --------------------------------- */

static int case_private_zone_bitmap_lifecycle(void) {
    reset_main();

    iso_alloc_zone_handle *zone = iso_alloc_new_zone(64);
    TASK_CHECK(zone != NULL);

    void *p = iso_alloc_from_zone(zone);
    TASK_CHECK(p != NULL);
    TASK_CHECK(iso_alloc_detect_zone_leaks(zone) == 1);

    TASK_CHECK(iso_quarantine_configure(4, 1u << 20) == 0);
    iso_free_from_zone(p, zone);

    /* Still quarantined: bitmap bit stays set and detector confirms it */
    TASK_CHECK(iso_alloc_detect_zone_leaks(zone) == 1);

    uint64_t t = iso_quarantine_detach();
    TASK_CHECK(t != 0);
    TASK_CHECK(iso_alloc_detect_zone_leaks(zone) == 1);

    iso_quarantine_stats s;
    TASK_CHECK(iso_quarantine_snapshot(t, &s) == 0 && s.pending_chunks == 1);

    TASK_CHECK(iso_quarantine_discard(t) == 0);
    TASK_CHECK(iso_alloc_detect_zone_leaks(zone) == 0);

    iso_alloc_destroy_zone(zone);
    return 0;
}

static int case_two_private_zones_destroy_one(void) {
    reset_main();

    iso_alloc_zone_handle *a = iso_alloc_new_zone(64);
    iso_alloc_zone_handle *b = iso_alloc_new_zone(256);
    TASK_CHECK(a != NULL && b != NULL);

    TASK_CHECK(iso_quarantine_configure(8, 1u << 20) == 0);

    void *pa[2], *pb[2];

    for(int i = 0; i < 2; i++) {
        pa[i] = iso_alloc_from_zone(a);
        pb[i] = iso_alloc_from_zone(b);
        TASK_CHECK(pa[i] != NULL && pb[i] != NULL);
        iso_free_from_zone(pa[i], a);
        iso_free_from_zone(pb[i], b);
    }

    iso_quarantine_stats s;
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0 && s.pending_chunks == 4);
    TASK_CHECK(iso_alloc_detect_zone_leaks(a) == 2);
    TASK_CHECK(iso_alloc_detect_zone_leaks(b) == 2);

    /* Destroy a: only its two records leave the queue */
    iso_alloc_destroy_zone(a);

    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0);
    TASK_CHECK(s.pending_chunks == 2);
    TASK_CHECK(s.released_zone == 2);

    /* The other private zones chunks remain held and bitmap occupied */
    TASK_CHECK(iso_alloc_detect_zone_leaks(b) == 2);

    iso_flush_caches();
    TASK_CHECK(iso_quarantine_snapshot(0, &s) == 0 && s.pending_chunks == 0);
    TASK_CHECK(iso_alloc_detect_zone_leaks(b) == 0);

    iso_alloc_destroy_zone(b);
    return 0;
}

static int case_ticket_survives_unrelated_zone_destroy(void) {
    reset_main();

    iso_alloc_zone_handle *held = iso_alloc_new_zone(128);
    iso_alloc_zone_handle *unrelated = iso_alloc_new_zone(64);
    TASK_CHECK(held != NULL && unrelated != NULL);

    struct worker_ticket_arg arg = {held, 0, 1};
    pthread_t th;
    TASK_CHECK(pthread_create(&th, NULL, detach_worker, &arg) == 0);

    void *status = NULL;
    TASK_CHECK(pthread_join(th, &status) == 0);
    TASK_CHECK(status == NULL && arg.ticket != 0);

    TASK_CHECK(iso_alloc_detect_zone_leaks(held) == 1);

    /* Destroying an unrelated empty zone must not touch the ticket */
    iso_alloc_destroy_zone(unrelated);

    iso_quarantine_stats s;
    TASK_CHECK(iso_quarantine_snapshot(arg.ticket, &s) == 0 && s.pending_chunks == 1);
    TASK_CHECK(iso_alloc_detect_zone_leaks(held) == 1);

    /* Consume it through adoption on the configured main thread */
    TASK_CHECK(iso_quarantine_configure(4, 1u << 20) == 0);
    TASK_CHECK(iso_quarantine_adopt(arg.ticket) == 0);
    TASK_CHECK(iso_alloc_detect_zone_leaks(held) == 1);

    iso_flush_caches();
    TASK_CHECK(iso_alloc_detect_zone_leaks(held) == 0);
    iso_alloc_destroy_zone(held);
    return 0;
}

/* ---------------------------------- registry --------------------------------- */

const struct task_case task_cases[] = {
        {"budget", "config_validation", case_config_validation},
        {"budget", "count_byte_accounting", case_count_byte_accounting},
        {"budget", "count_eviction", case_count_eviction},
        {"budget", "max_chunks_capacity", case_max_chunks_capacity},
        {"budget", "budget_tightening", case_budget_tightening},
        {"budget", "oversize_immediate", case_oversize_immediate},
        {"budget", "zero_config_drains_disables", case_zero_config_drains_and_disables},
        {"budget", "null_zero_no_budget", case_null_and_zero_do_not_count},
        {"budget", "free_entry_points", case_free_entry_points},

        {"handoff", "detach_empty", case_detach_empty},
        {"handoff", "adopt_appends_counts", case_adopt_appends_and_counts},
        {"handoff", "adopt_failure_retry", case_adopt_failure_retry},
        {"handoff", "invalid_tickets_out_copy", case_invalid_tickets_and_out_copy},
        {"handoff", "ticket_slots_limit", case_ticket_slots_limit},
        {"handoff", "creator_thread_exit_adopt", case_creator_thread_exit_adopt},

        {"lifecycle", "private_zone_bitmap", case_private_zone_bitmap_lifecycle},
        {"lifecycle", "two_private_zones_destroy_one", case_two_private_zones_destroy_one},
        {"lifecycle", "ticket_survives_unrelated_destroy", case_ticket_survives_unrelated_zone_destroy},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
