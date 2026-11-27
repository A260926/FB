/*
 * model.h - independent reference model for the zlib replay property tests.
 *
 * This header and model.c deliberately know nothing about zlib: model.c does
 * not include zlib.h (enforced by a compile time check in model.c).  The model
 * owns:
 *   - the deterministic PRNG and every byte of "original" plaintext,
 *   - the bounded action grammar / lifecycle state machine,
 *   - the expected plaintext partition attached to every stream,
 *   - an independent Adler-32 implementation,
 *   - replay/coverage bookkeeping.
 * The zlib-facing engine in target.c drives the real library API and asks the
 * model what the answer should be; the model never calls a zlib copy routine.
 */
#ifndef TASK_MODEL_H
#define TASK_MODEL_H

#include <stddef.h>
#include <stdio.h>

#define TASK_MAX_ACTIONS 64
#define TASK_MAX_STREAMS 4
#define TASK_MAX_INPUT   65536u  /* 64 KiB total plaintext per program */
#define TASK_SEED_COUNT  500
#define TASK_DICT_MAX    4

/* Growable byte buffer owned by the caller (the model or the test engine). */
typedef struct {
    unsigned char *p;
    size_t len;
    size_t cap;
} Buf;

void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_reset(Buf *b);
void  buf_put(Buf *b, const void *data, size_t n);
void  buf_put_byte(Buf *b, unsigned char c);
int   buf_equal(const Buf *a, const Buf *b);

/* Deterministic splitmix64 PRNG; no global state, seedable per program. */
typedef struct { unsigned long long s; } Rng;
void              rng_seed(Rng *r, unsigned long long seed);
unsigned long long rng_next(Rng *r);
unsigned          rng_between(Rng *r, unsigned lo, unsigned hi); /* inclusive */
unsigned char     rng_byte(Rng *r);

/* Independent Adler-32 (RFC 1950), implemented without zlib. */
unsigned long model_adler32(const unsigned char *p, size_t n);

/* Independent 64-bit FNV-1a hash for replay byte comparison, no zlib use. */
unsigned long long model_fnv64(const unsigned char *p, size_t n);

/* Action opcodes of the bounded lifecycle grammar. */
enum {
    OP_NOP = 0,
    OP_DINIT,    /* deflateInit2 (+ immediate dictionary) */
    OP_FEED,     /* hand plaintext bytes to a deflate stream */
    OP_FLUSH,    /* SYNC_FLUSH or FULL_FLUSH */
    OP_DCOPY,    /* deflateCopy and caller-side re-bind */
    OP_FINISH,   /* drain with Z_FINISH */
    OP_DEND,     /* deflateEnd */
    OP_IINIT,    /* inflateInit2 bound to a finished deflate stream */
    OP_IFEED,    /* feed a bounded chunk of compressed bytes */
    OP_ICOPY,    /* inflateCopy and caller-side re-bind */
    OP_IFINISH,  /* drive an inflate stream to Z_STREAM_END */
    OP_IEND,     /* inflateEnd */
    OP__LIMIT
};

/*
 * Operand conventions:
 *   DINIT   a=signed windowBits b=level c=memLevel d=strategy e=dict index(-1)
 *   FEED    a=slot                       data=plaintext bytes
 *   FLUSH   a=slot b=flush (Z_SYNC_FLUSH/Z_FULL_FLUSH numeric value)
 *   DCOPY   a=src slot b=dst slot d=source output id outid=forked output id
 *           e=shared prefix length; the model copies the plaintext prefix
 *           itself (no zlib copy call)
 *   FINISH  a=slot outid=compressed output id
 *   DEND    a=slot
 *   IINIT   a=slot b=source deflate id   c=dict index(-1)
 *   IFEED   a=slot b=max compressed bytes per call c=output buffer capacity
 *   ICOPY   a=src slot b=dst slot
 *   IFINISH a=slot
 *   IEND    a=slot
 */
typedef struct {
    int op;
    int a, b, c, d, e;
    int outid; /* FINISH: compressed output id; DCOPY: e=shared prefix length */
    Buf data;
} Action;

typedef struct {
    int seed;
    Action act[TASK_MAX_ACTIONS];
    int nact;
    Buf dict[TASK_DICT_MAX];
    int ndict;
    Buf expect[TASK_MAX_ACTIONS]; /* expected full plaintext per output id */
    int wrapper[TASK_MAX_ACTIONS];/* signed windowBits chosen at DINIT */
    int outdict[TASK_MAX_ACTIONS];/* dictionary index per output, or -1 */
    int noutputs;
    size_t total_input;
} Program;

void program_init(Program *pg);
void program_free(Program *pg);
void program_generate(Program *pg, int seed);

/* Pure state-machine check of every API precondition used by the grammar. */
int program_legal(const Program *pg);

/* Real coverage counters, updated by the zlib engine, printed in the report. */
typedef struct {
    unsigned long programs;
    unsigned long replays;
    unsigned long def_streams;
    unsigned long inf_streams;
    unsigned long dcopy;            /* deflateCopy calls */
    unsigned long icopy;            /* inflateCopy calls */
    unsigned long dcopy_partial_in; /* copy with unconsumed caller input */
    unsigned long dcopy_partial_out;/* copy with uncollected output bytes */
    unsigned long icopy_partial_in;
    unsigned long icopy_partial_out;
    unsigned long end_original_first; /* inflate: original finished before copy */
    unsigned long sync_flush;
    unsigned long full_flush;
    unsigned long dict_zlib_need;   /* Z_NEED_DICT resolved with model dict */
    unsigned long dict_raw;         /* raw inflate primed with model dict */
    unsigned long raw_programs;
    unsigned long zlib_programs;
    unsigned long branch_suffix;
    unsigned long end_ok;
    unsigned long alloc_peak;
} Coverage;

void coverage_zero(Coverage *c);
void coverage_print(const Coverage *c, FILE *f);

#endif
