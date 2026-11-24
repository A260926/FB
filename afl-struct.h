/*
  Structural fragment reduction session for afl-tmin
  ------------------------------------------------

  This header exposes the public session API used by afl-tmin's structural
  mode (-S).  The session owns a copy of the original input and of the
  dependency table; the caller never has to retain either.

  The dependency table contains one non-empty line per fragment:

      ID START END : NEEDS

  All values are decimal non-negative integers.  [START, END) is a half-open
  interval over the original input.  NEEDS is a comma-separated list of
  fragment IDs, or "-" for none.  Every non-empty interval must participate in
  an exact partition of the input.  Table line order does not matter,
  references must exist, and dependency cycles are permitted.
*/

#ifndef AFL_STRUCT_H
#define AFL_STRUCT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct afl_struct_session afl_struct_session;

/*
  Create a session.  Both the input and the table are copied.  Returns NULL
  for an invalid table or an invalid input/table combination.  Empty data
  together with an empty table creates a valid, already-empty session.
*/
afl_struct_session *afl_struct_create(const unsigned char *data, size_t len,
                                      const char *table);

/*
  Borrow the session's current accepted bytes.  The pointer is valid until the
  next afl_struct_step() call or afl_struct_destroy().  For an empty session
  the returned length is zero (the pointer itself may be NULL).
*/
const unsigned char *afl_struct_data(const afl_struct_session *s, size_t *len);

void afl_struct_destroy(afl_struct_session *s);

/*
  Advance the structural search by at most `budget` real candidate executions.

  observe() is invoked with a borrowed candidate buffer and must return 1 to
  accept the candidate, 0 to reject it, or -1 on an execution error.  On -1 the
  session does not advance its scan position and does not populate the result
  cache, so a subsequent call retries the same candidate.

  Cache hits do not consume budget.  Returns 0 when the search has completed,
  1 when it paused because the budget was exhausted, and -1 on an observe
  error.  A zero budget reports 0 for an already-empty graph and 1 when real
  (uncached) candidates remain.
*/
int afl_struct_step(afl_struct_session *s, size_t budget,
                    int (*observe)(const unsigned char *, size_t, void *),
                    void *context);

/*
  Checkpoint helpers used by afl-tmin -C.

  A state file records the original bytes, the canonical dependency-table
  identity, the active fragment set, the scan cursor, the complete result
  cache, and a caller-supplied metadata blob (target identity, execution
  options, start-coverage identity, ...).  Writes are performed through a
  temporary file followed by an atomic rename.
*/

#define AFL_STRUCT_STATE_OK        0
#define AFL_STRUCT_STATE_MISSING   1
#define AFL_STRUCT_STATE_MISMATCH  2
#define AFL_STRUCT_STATE_ERROR    -1

/*
  Persist the session.  Returns 0 on success, -1 on any system/write error.
  The previous state file is left untouched unless the atomic rename succeeds.
*/
int afl_struct_state_save(const afl_struct_session *s, const char *path,
                          const unsigned char *meta, size_t meta_len);

/*
  Restore a session.

   - AFL_STRUCT_STATE_MISSING: no file at `path`; *session_out is NULL and the
     caller should keep its freshly created session.
   - AFL_STRUCT_STATE_MISMATCH: the file exists but its version, integrity
     checksum, original bytes, canonical table identity, or internal structure
     does not match.  *session_out is NULL.
   - AFL_STRUCT_STATE_ERROR: a system error or the metadata blob does not fit.

  On AFL_STRUCT_STATE_OK, *session_out is a new session owned by the caller and
  the stored metadata is copied to meta_out (when meta_out is non-NULL).
*/
int afl_struct_state_load(const char *path, const unsigned char *data,
                          size_t len, const char *table,
                          unsigned char *meta_out, size_t meta_cap,
                          size_t *meta_len_out,
                          afl_struct_session **session_out);

#ifdef __cplusplus
}
#endif

#endif /* !AFL_STRUCT_H */
