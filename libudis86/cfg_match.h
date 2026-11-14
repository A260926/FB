/* udis86 - libudis86/cfg_match.h
 *
 * Read-only x86-64 function structural matching.
 *
 * Two independently linked firmware images often contain the same function
 * with completely different byte encodings (layout, NOPs, register
 * allocation).  This entry point recovers the direct control-flow graph of
 * one function in each image and, under the explicit normalization rules
 * documented in task_support/match-contract.md, looks for:
 *
 *   - a one-to-one correspondence of retained instructions and blocks,
 *   - a bijection between the GPR families used across the whole function,
 *   - per-edge markers telling whether a conditional edge was obtained by
 *     inverting a Jcc and swapping its two successors.
 *
 * This is structural matching under fixed rules; it does not claim behavioral
 * equivalence of arbitrary programs.
 *
 * Inputs are never modified.  A successful result owns all of its data and
 * must be released with ud_match_result_free().
 */
#ifndef UD_CFG_MATCH_H
#define UD_CFG_MATCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Number of general purpose register families (RAX..R15). */
#define UD_MATCH_FAMILY_COUNT 16

enum ud_match_status
{
  /* A witness was found and published in the result object. */
  UD_MATCH_OK          = 0,
  /* Both inputs decode to supported code, but no structural witness exists. */
  UD_MATCH_NO_MATCH    = 1,
  /* Valid x86-64 code, but outside the supported matching boundary. */
  UD_MATCH_UNSUPPORTED = 2,
  /* Corrupt input: decode/truncation error, jump into an instruction,
   * control successor outside the image, malformed anchor table, ... */
  UD_MATCH_INVALID     = 3
};

/* One side of a function match: a read-only contiguous byte image, the
 * virtual base address of the first byte, and the entry virtual address. */
struct ud_function_bytes
{
  const uint8_t *data;
  size_t         size;
  uint64_t       base;
  uint64_t       entry;
};

/* A one-to-one reference-address anchor.  Only used for RIP-relative
 * effective addresses and direct CALL targets; never for plain immediates. */
struct ud_addr_anchor
{
  uint64_t left_va;
  uint64_t right_va;
};

/* Correspondence of two retained (non-transparent) instructions. */
struct ud_insn_correspondence
{
  uint64_t left_va;
  uint64_t right_va;
};

/* Correspondence of two basic blocks, identified by their entry VAs. */
struct ud_block_correspondence
{
  uint64_t left_entry_va;
  uint64_t right_entry_va;
};

/* A retained control-flow edge.  "swapped" is set only for the edges of an
 * inverted conditional branch, where the taken/fall-through roles exchange. */
struct ud_edge_correspondence
{
  uint64_t left_from;
  uint64_t left_to;
  uint64_t right_from;
  uint64_t right_to;
  int      swapped;
};

/* A GPR family pair.  Families are numbered 0..15 following the 64-bit view:
 * 0=RAX ... 4=RSP 5=RBP ... 15=R15. */
struct ud_register_correspondence
{
  unsigned left_family;
  unsigned right_family;
};

struct ud_match_result
{
  enum ud_match_status             status;
  struct ud_insn_correspondence   *insns;
  size_t                           insn_count;
  struct ud_block_correspondence  *blocks;
  size_t                           block_count;
  struct ud_edge_correspondence   *edges;
  size_t                           edge_count;
  struct ud_register_correspondence *registers;
  size_t                           register_count;
};

/* Matches two functions.
 *
 * Returns the match status.  On UD_MATCH_OK a heap-allocated result object is
 * published through *out_result and must be freed with
 * ud_match_result_free().  On every other status no witness is published and
 * *out_result is set to NULL.  Anchor tables may be NULL when anchor_count
 * is zero. */
enum ud_match_status
ud_match_functions64(const struct ud_function_bytes *left,
                     const struct ud_function_bytes *right,
                     const struct ud_addr_anchor *anchors,
                     size_t anchor_count,
                     struct ud_match_result **out_result);

/* Releases a result object.  NULL is accepted. */
void ud_match_result_free(struct ud_match_result *result);

/* Canonical family name ("RAX", "RSP", "R8", ...), NULL for bad indices. */
const char *ud_match_family_name(unsigned family);

#ifdef __cplusplus
}
#endif

#endif /* UD_CFG_MATCH_H */
