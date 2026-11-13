#ifndef TASK_HARNESS_H
#define TASK_HARNESS_H

#include "task_model.h"

#define TASK_MAX_ACTIONS 64
#define TASK_MAX_STREAMS 4
#define TASK_MAX_PLAIN   (64u * 1024u)
#define TASK_PROGRAMS    500

/* Action grammar of a legal deflate lifecycle. Every action is legal under
 * the real API preconditions: INIT precedes any use, dictionaries are applied
 * before the first deflate() call, copies only target free slots, every
 * stream is finished and ended exactly once. */
enum {
    A_INIT = 0,   /* create stream in `slot`, apply dictionary if flagged */
    A_FEED,       /* append `len` model bytes, consume them fully */
    A_PARTFEED,   /* append `len` bytes but leave part of the input store
                     unconsumed, so the next DCOPY sees partial input */
    A_FLUSH,      /* SYNC_FLUSH or FULL_FLUSH (arg selects) */
    A_DCOPY,      /* deflateCopy(slot=dst, source=slot), then rebind stores */
    A_FINISH,     /* Z_FINISH until Z_STREAM_END, capture the full stream */
    A_END         /* deflateEnd and release the slot */
};

#define TASK_ARG_SYNC 2
#define TASK_ARG_FULL 3

typedef struct task_action {
    unsigned char kind;
    unsigned char slot;
    unsigned char dst;
    unsigned char arg;
    unsigned len;
    size_t src_off;
} task_action;

typedef struct task_program {
    int tag;
    int window_bits;       /* 15 = zlib wrapper, -15 = raw deflate */
    int level;
    int use_dict;
    task_buf dict;
    task_buf source;       /* independent model plaintext for this program */
    task_action act[TASK_MAX_ACTIONS];
    unsigned nact;
    unsigned plain_total;
    /* captures are keyed by action index (a slot may host several stream
     * generations over one program, so a slot key would alias) */
    task_buf cap_act[TASK_MAX_ACTIONS];
    int has_cap_act[TASK_MAX_ACTIONS];
} task_program;

/* Real path coverage, accumulated while interpreting programs. */
typedef struct task_cov {
    unsigned long programs;
    unsigned long raw_programs;
    unsigned long zlib_programs;
    unsigned long dict_paths;
    unsigned long sync_flushes;
    unsigned long full_flushes;
    unsigned long deflate_copies;
    unsigned long inflate_copies;
    unsigned long copy_partial_input;
    unsigned long copy_partial_output;
    unsigned long finish_original_first;
    unsigned long branch_forks;
    unsigned long inflate_dict_paths;
    unsigned long active_peak;
} task_cov;

extern task_cov task_coverage;
extern int task_fault_inject; /* predetermined differential simulator switch */

void task_cov_reset(void);
void task_program_free(task_program *p);

/* Build the fixed seed corpus: pinned programs that guarantee every required
 * path, followed by deterministic seeded programs up to TASK_PROGRAMS. */
void task_generate(task_program *prog, unsigned index);

/* Interpret one program against the real deflate/deflateCopy API. When
 * capture is non-zero the finished compressed streams are stored in the
 * program for replay comparison; otherwise they are compared against those
 * captures. Returns zero on success; a failed TASK_CHECK exits the process. */
void task_run_deflate(task_program *p, int capture);

/* inflateCopy scenario: partially consumed normal input, constrained output,
 * copy, rebind, finish original first, then continue the copy. */
void task_run_inflate_copy(task_rng *r, int raw, int use_dict);

#endif
