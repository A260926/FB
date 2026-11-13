#ifndef TASK_MODEL_H
#define TASK_MODEL_H

#include <stddef.h>
#include "zlib.h"

/* ------------------------------------------------------------------ */
/* Deterministic pseudo random source. It has no relationship to the  */
/* library under test: the expected plaintext bytes and every chunking */
/* decision in the harness are drawn from this generator alone.        */
/* ------------------------------------------------------------------ */
typedef struct task_rng {
    unsigned long long s;
} task_rng;

void task_rng_seed(task_rng *r, unsigned long long seed);
unsigned long long task_rng_next(task_rng *r);
unsigned task_rng_below(task_rng *r, unsigned n); /* result in [0,n), n > 0 */

/* ------------------------------------------------------------------ */
/* Growable byte buffer used for every caller-owned store: plaintext   */
/* expectations, captured compressed prefixes, rebindable input stores. */
/* ------------------------------------------------------------------ */
typedef struct task_buf {
    unsigned char *p;
    size_t len;
    size_t cap;
} task_buf;

void task_buf_init(task_buf *b);
void task_buf_append(task_buf *b, const void *data, size_t n);
void task_buf_take(task_buf *dst, const task_buf *src); /* deep, independent copy */
int  task_buf_equal(const task_buf *a, const task_buf *b);
void task_buf_reset(task_buf *b);
void task_buf_free(task_buf *b);

/* Independent plaintext byte model: fills `out` with `n` expected bytes
 * chosen from a few pattern families. zlib is never consulted. */
void task_model_bytes(task_rng *r, task_buf *out, size_t n);

/* ------------------------------------------------------------------ */
/* Accounting allocator handed to every Init2 call. Both copy routines */
/* inherit these function pointers, so every library allocation in the */
/* whole suite is counted here.                                        */
/* ------------------------------------------------------------------ */
void task_acct_reset(void);
unsigned long task_acct_live(void);  /* outstanding allocations */
unsigned long task_acct_total(void); /* cumulative allocations */
voidpf task_zalloc(voidpf opaque, unsigned items, unsigned size);
void   task_zfree(voidpf opaque, voidpf address);

/* ------------------------------------------------------------------ */
/* External bookkeeping for one deflate branch. The library owns the   */
/* window/internal state; this structure owns the caller side:         */
/*   expect - plaintext bytes the model predicts for this branch       */
/*   prefix - compressed output already drained from the library       */
/*   store  - remaining caller-owned input store, with a cursor        */
/*   out    - distinct caller-owned output store                       */
/* Duplication after a copy is performed by the model with plain       */
/* memcpy; it never calls deflateCopy/inflateCopy to build hopes.      */
/* ------------------------------------------------------------------ */
typedef struct task_branch {
    task_buf expect;
    task_buf prefix;
    task_buf store;
    size_t cursor;
    unsigned char *out;
    size_t out_cap;
    int finished;
    int ended;
} task_branch;

void task_branch_init(task_branch *b);
void task_branch_duplicate(const task_branch *src, task_branch *dst);
void task_branch_bind_out(task_rng *r, task_branch *b, z_streamp z);
void task_branch_drain(task_branch *b, z_streamp z);
void task_branch_free(task_branch *b);

/* ------------------------------------------------------------------ */
/* Reference codec built strictly from the non-copy streaming API:     */
/* Init2 / (SetDictionary) / process / End. Used as the independent    */
/* decoder oracle. It never invokes deflateCopy or inflateCopy.        */
/* window_bits: 15 for zlib wrapping, -15 for raw deflate.             */
/* mid_flush: 0 = none, 2 = Z_SYNC_FLUSH, 3 = Z_FULL_FLUSH once.       */
/* chunk == 0 means "hand everything over in one call".                */
/* ------------------------------------------------------------------ */
int task_ref_deflate(const task_buf *plain, task_buf *compressed,
                     int window_bits, int level,
                     const unsigned char *dict, unsigned dict_len,
                     int mid_flush, unsigned chunk);
int task_ref_inflate(const task_buf *compressed, task_buf *plain,
                     int window_bits,
                     const unsigned char *dict, unsigned dict_len,
                     unsigned in_chunk, unsigned out_chunk);

#endif
