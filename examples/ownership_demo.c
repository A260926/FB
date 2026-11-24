/*
 * Ownership demonstration.
 *
 * Builds a short, fully legal ownership plan that shows the host-reference
 * rules in action and runs it through the shared replay interface:
 *
 *   - a new tracked object is created in a root slot,
 *   - an alias duplicates the responsibility,
 *   - the original root transfers its reference into the pending exception,
 *   - the exception is retrieved back into a root (transfer again),
 *   - every root is released and explicit GC finalizes the identity once.
 *
 * The replay chain performs the same plan twice in separate runtimes and
 * cleans up all roots, the holder and any pending exception itself. On
 * success this program prints OWNERSHIP_DEMO_OK.
 */
#include "fuzz/ownership_model.h"
#include "fuzz/ownership_replay.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static size_t emit(uint8_t *buf, size_t off, uint8_t op, uint8_t a, uint8_t b)
{
    buf[off++] = op;
    buf[off++] = a;
    buf[off++] = b;
    return off;
}

int main(void)
{
    uint8_t plan[21];
    size_t n = 0;
    struct qjs_ownership_report report;
    int rc;

    /* NEW 0: tracked identity 0 owned by root 0. */
    n = emit(plan, n, QJS_OWN_OP_NEW, 0, 0);
    /* DUP 0 -> 1: root 1 now shares the responsibility. */
    n = emit(plan, n, QJS_OWN_OP_DUP, 0, 1);
    /* THROW_TAKE 0: root 0 transfers its reference to the pending
     * exception; root 1 keeps the object alive. */
    n = emit(plan, n, QJS_OWN_OP_THROW_TAKE, 0, 0);
    /* CATCH 0: the pending exception transfers back into root 0. */
    n = emit(plan, n, QJS_OWN_OP_CATCH, 0, 0);
    /* DROP both aliases; the object becomes unreferenced. */
    n = emit(plan, n, QJS_OWN_OP_DROP, 1, 0);
    n = emit(plan, n, QJS_OWN_OP_DROP, 0, 0);
    /* GC: the single identity is finalized exactly once. */
    n = emit(plan, n, QJS_OWN_OP_GC, 0, 0);

    rc = qjs_ownership_run(plan, n, &report);
    assert(rc == 0);
    assert(report.steps == 7);
    assert(report.created == 1);
    assert(report.finalized == 1);
    assert(report.comparisons > 0);
    assert(report.exceptions == 1);

    printf("transfer demo: created=%zu finalized=%zu exceptions=%zu\n",
           report.created, report.finalized, report.exceptions);
    printf("OWNERSHIP_DEMO_OK\n");
    return 0;
}
