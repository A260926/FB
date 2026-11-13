#ifndef TASK_SUPPORT_MODEL_H
#define TASK_SUPPORT_MODEL_H

/*
 * Independent DNS response model, wire-message generator and checking
 * oracle for the c-ares property tests.  Nothing in this module calls
 * into c-ares to produce an expected value and nothing derives the
 * model from a c-ares result.
 *
 * Grammar covered (RFC 1035 subset):
 *   - names are label arrays of [A-Za-z0-9-] labels, or the root;
 *   - a response is one question, 1..4 CNAME hops and a terminal
 *     A (IPv4) or AAAA (IPv6) record set;
 *   - three legal compression layouts are produced:
 *       L0 uncompressed, L1 suffix-only (whole-name pointers),
 *       L2 multi-level cross-record suffix compression;
 *   - every compression pointer targets an earlier name-label boundary.
 */

#include <stddef.h>

#define EM_MAX_LABELS   6       /* labels per non-root name        */
#define EM_MAX_LABLEN   14      /* generated label length limit    */
#define EM_MAX_CHAIN    4       /* CNAME hops, 1..4                */
#define EM_MAX_ADDRS    4       /* terminal address records        */
#define EM_CASES        500     /* fixed-seed property cases       */
#define EM_NAMEMAX      255     /* RFC 1035 wire name limit        */
#define EM_MSGMAX       2048    /* well under the 65535 DNS limit  */
#define EM_MAX_WNAMES   (1 + EM_MAX_CHAIN * 2 + EM_MAX_ADDRS + 1)

typedef enum { EM_LAYOUT_UNCOMPRESSED = 0,
               EM_LAYOUT_SUFFIX = 1,
               EM_LAYOUT_MULTI = 2 } em_layout;

typedef struct {
    unsigned char labels[EM_MAX_LABELS][EM_MAX_LABLEN];
    unsigned char nlens[EM_MAX_LABELS];
    int           nlabels;                /* 0 means the root name  */
    int           root;                   /* explicit root marker   */
} em_name;

typedef struct {
    unsigned char v[16];
    int           len;                    /* 4 for A, 16 for AAAA   */
} em_addr;

typedef struct {
    em_name qname;                        /* question name          */
    int     qtype;                        /* 1 = A, 28 = AAAA       */
    em_name cname[EM_MAX_CHAIN];          /* hop targets            */
    int     nhops;                        /* 1..4                   */
    em_addr addrs[EM_MAX_ADDRS];
    int     naddrs;
    em_name extra;                        /* irrelevant additional  */
    int     has_extra;
    unsigned seed;
} em_model;

/* One name as it appears in the emitted message. */
typedef struct {
    int      offset;              /* start offset inside the message  */
    int      enclen;              /* wire bytes the name occupies     */
    em_name  identity;            /* logical name it encodes          */
    int      ptrs;                /* number of indirections traversed */
} em_wname;

/* Result of laying one model out on the wire. */
typedef struct {
    unsigned char msg[EM_MSGMAX];
    int           msglen;
    em_wname      names[EM_MAX_WNAMES];
    int           nnames;
    /* Model address indices in the order the terminal RRs were emitted. */
    int           addr_order[EM_MAX_ADDRS];
    int           naddrs;
    /* Deepest pointer chain (number of nested indirections). */
    int           ptr_depth;
    /* 1 once the emitted message violates a size invariant. */
    int           oversized;
} em_wire;

/* Deterministic generator: model N (0..EM_CASES-1) from a fixed seed. */
em_model em_generate(unsigned seed, int n);

/*
 * Build a response message.
 *   order_variant  0 canonical, 1 swap first two addresses, 2 reverse
 *   case_variant   0 lower-case question, 1 all upper, 2 alternating
 *   extra_where    0 additional section, 1 answer front, 2 answer back
 * Returns 0 on success, -1 if a protocol size limit would be broken.
 */
int em_build(const em_model *m, em_layout layout, int order_variant,
             int case_variant, int extra_where, em_wire *out);

/* Permutation applied to terminal addresses by order_variant. */
int em_addr_perm(int order_variant, int idx, int n);

/* Full oracle: parse through the real c-ares API and check every
 * identity against the model.  Returns 0 on match, -1 on first
 * deviation (human-readable detail written into diff). */
int em_check_reply(const em_model *m, const em_wire *w, char *diff,
                   size_t diffsz);

/* Check ares_expand_name enclen and spelling against the recorded
 * per-name wire layout. */
int em_check_expand(const em_wire *w, char *diff, size_t diffsz);

/*
 * Predetermined logic-difference simulator: one byte of the first
 * terminal address is flipped in the expectation only, proving the
 * oracle reports a mismatch and keeps the very first case.
 */
int em_diff_simulator(void);

void em_render(const em_name *n, char *buf, size_t bufsz);
int  em_name_is_root(const em_name *n);
/* Full uncompressed wire length of the logical name (RFC 1035). */
int  em_name_wirelen(const em_name *n);

#endif
