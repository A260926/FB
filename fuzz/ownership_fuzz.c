/*
 * libFuzzer entry point for ownership-model fuzzing.
 *
 * Invalid or truncated plans are rejected in a controlled way before the
 * engine is touched (qjs_ownership_run returns 1) and never count as
 * findings. Only a genuine model/engine divergence (return 2) is reported
 * as a failure. The deterministic driver shares the exact same replay
 * chain through qjs_ownership_run.
 */
#include "ownership_model.h"
#include "ownership_replay.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct qjs_ownership_report report;
    int rc;

    /* The plan format rejects anything larger than 1024 three-byte
     * records, but check the bound here too so oversized inputs never
     * reach the validator's fixed-size storage. */
    if (size > QJS_OWN_PLAN_MAX_SIZE)
        return 0;

    rc = qjs_ownership_run(data, size, &report);
    if (rc == 1)
        return 0; /* controlled rejection of an invalid whole plan */
    if (rc == 2)
        return 2; /* first divergence already printed by the replay chain */
    return 0;
}
