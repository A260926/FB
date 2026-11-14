/*
 * Independent correctness regression for YARA hex strings.
 *
 * The pattern model, the expected-match oracle and the YARA scan adapter
 * live in tests/hex_model/. The oracle computes reachability between input
 * positions directly from the pattern nodes; it never looks at YARA's AST,
 * atom tables, RE bytecode or the scanner.
 *
 * Supported pattern surface (everything else is explicitly rejected):
 *   - byte tokens: literal bytes, ??, nibble wildcards (x? / ?x)
 *   - negation: ~xx, ~x?, ~?x  (never ~??)
 *   - concatenation
 *   - non-empty, fixed-width alternation branches
 *   - bounded jumps [n] / [n-m], only inside the outer concatenation,
 *     at most two of them, endpoints in [0, 202]
 *   - at most 24 nodes, nesting depth at most 3, data at most 640 bytes
 *
 * A hex string may not start or end with a jump.
 */

#ifndef HEX_MODEL_H
#define HEX_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HM_MAX_NODES 24
#define HM_MAX_DEPTH 3
#define HM_MAX_DATA 640
#define HM_MAX_JUMP_END 202
#define HM_MAX_JUMPS 2
#define HM_MAX_ALT 4
#define HM_POOL_SIZE 128
#define HM_HEX_MAX 512
#define HM_RULE_MAX (HM_HEX_MAX + 128)
#define HM_DUMP_MAX 1024
#define HM_ERR_MAX 256

typedef enum {
  HM_BYTE = 1,  // one masked byte, optionally negated
  HM_ALT,       // alternation of fixed-width branches
  HM_CONCAT,    // ordered sequence; jumps are only legal here
  HM_JUMP       // [lo-hi] consumption of arbitrary bytes
} hm_type;

typedef struct hm_node {
  hm_type type;

  // HM_BYTE
  uint8_t value;  // significant bits already positioned in the byte
  uint8_t mask;   // 0xFF, 0xF0, 0x0F or 0x00 (0x00 means ??)
  bool neg;       // ~ operator; never combined with mask == 0x00

  // HM_JUMP
  int lo;
  int hi;

  // HM_ALT / HM_CONCAT: kids[0 .. n-1]
  int n;
  struct hm_node* kids[HM_MAX_NODES];
} hm_node;

typedef struct {
  hm_node pool[HM_POOL_SIZE];
  int pool_used;
  hm_node* root;
} hm_pattern;

typedef struct {
  int start;
  int length;
} hm_match;

typedef struct {
  uint64_t state;
} hm_rng;

typedef struct {
  const char* name;
  hm_pattern pattern;
  uint8_t data[HM_MAX_DATA];
  int data_len;
  hm_match expected[HM_MAX_DATA];
  int expected_count;
  char hex[HM_HEX_MAX];
  char rule[HM_RULE_MAX];
} hm_case;

// ---- node pool ---------------------------------------------------------

void hm_pattern_init(hm_pattern* p);
hm_node* hm_new_node(hm_pattern* p, hm_type type);

// Constructors used by the fixed set, the generator and the fuzzer entry.
hm_node* hm_byte(hm_pattern* p, uint8_t value, uint8_t mask, bool neg);
hm_node* hm_jump(hm_pattern* p, int lo, int hi);
hm_node* hm_concat(hm_pattern* p);
hm_node* hm_alt(hm_pattern* p);
void hm_kid(hm_node* parent, hm_node* child);

// Structural validation against the supported surface. A rejected pattern is
// an unsupported input; it must never be presented to the scanner or counted
// as a match failure. Fills err[] when it returns false.
bool hm_validate(const hm_pattern* p, char* err, size_t err_size);

// ---- rendering ----------------------------------------------------------

// Renders the pattern body, e.g. "41 ( 1? | ~?F ) [2-3] 42".
void hm_render_hex(const hm_pattern* p, char* out, size_t out_size);

// Renders the full YARA rule text used by the scan adapter.
void hm_render_rule(const hm_pattern* p, char* out, size_t out_size);

// Multi-line structural dump for replayable failure reports.
void hm_dump(const hm_pattern* p, char* out, size_t out_size);

// ---- deterministic generator -------------------------------------------

void hm_rng_seed(hm_rng* r, uint64_t seed);
uint64_t hm_rand64(hm_rng* r);
int hm_rand_range(hm_rng* r, int lo, int hi);  // inclusive

// Generates a pattern that is valid by construction.
void hm_generate_pattern(hm_rng* r, hm_pattern* p);

// Fills data[] around a planted derivation of the pattern. The pattern is
// guaranteed to match at least once; surrounding bytes are random.
int hm_plant_data(hm_rng* r, const hm_pattern* p, uint8_t* data, int max_len);

// Fills data_len random benign bytes with no planting.
int hm_random_data(hm_rng* r, uint8_t* data, int max_len);

// ---- reference oracle ---------------------------------------------------

// All end positions reachable from input position start. Sorted, unique.
// Returned value is the number of entries written to ends[].
int hm_reachable(const hm_node* node, const uint8_t* data, int data_len,
                 int start, int* ends, int ends_cap);

// Shortest complete match for every start position that can reach the end of
// the pattern, sorted by start. Overlapping starts are kept. With the
// restricted surface here every start has at most one derivation length.
int hm_expected_matches(const hm_pattern* p, const uint8_t* data, int data_len,
                        hm_match* out, int out_cap);

// Builds the rendered text and expected matches of a case.
void hm_case_prepare(hm_case* c, const char* name);

// ---- explicit rejection probes ------------------------------------------

// Builds deliberately unsupported patterns into p; returns the number added.
int hm_build_invalid(hm_pattern* p, int index);
int hm_invalid_count(void);

#endif  // HEX_MODEL_H
