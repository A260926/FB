/*
Copyright (c) 2026. The YARA Authors. All Rights Reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

//
// Independent correctness model for a strict subset of YARA hex strings.
//
// The model is self-contained: it never uses YARA's AST, atom extractor, RE
// bytecode or execution engine. It describes patterns as plain C structures,
// renders them into YARA rule text, and computes the expected match
// (offset, length) pairs directly from the structure of the pattern.
//
// Supported node kinds:
//   - literal byte            (e.g. 4A)
//   - any byte                (??)
//   - half-nibble wildcards   (e.g. A?, ?F)
//   - negated byte            (e.g. ~4A)
//   - negated half wildcard   (e.g. ~A?, ~?F); ~?? is deliberately unsupported
//   - concatenation
//   - fixed-width, non-empty alternation (every branch has the same width and
//     contains only fixed-width nodes, never jumps)
//   - bounded jumps [a-b], only inside the outer concatenation, at most two
//
// Limits enforced by the generator:
//   - at most HM_MAX_NODES nodes per pattern
//   - nesting depth at most HM_MAX_DEPTH
//   - jump end points in [0, HM_MAX_JUMP]
//   - generated data is at most HM_MAX_DATA bytes
//
#ifndef HEX_MODEL_H
#define HEX_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define HM_MAX_NODES 24
#define HM_MAX_DEPTH 3
#define HM_MAX_JUMP 202
#define HM_MAX_DATA 640
#define HM_MAX_JUMPS 2

// Node discriminants.
typedef enum {
  HM_BYTE = 1,       // literal or masked byte (mask == 0 means any)
  HM_NOT_BYTE,       // negated literal or masked byte (mask != 0)
  HM_ALT,            // fixed-width alternation of fixed-width node lists
  HM_CONCAT,         // concatenation (outer container; only one per pattern)
  HM_JUMP            // bounded any-byte jump
} hm_kind;

typedef struct hm_node hm_node;

struct hm_node {
  hm_kind kind;

  // HM_BYTE / HM_NOT_BYTE.
  uint8_t value;      // significant nibbles already zeroed where masked
  uint8_t mask;       // 0x00..0xFF, never 0x00 for HM_NOT_BYTE

  // HM_JUMP: inclusive range of consumed bytes.
  int jump_min;
  int jump_max;

  // HM_CONCAT / HM_ALT: children stored in a flat pre-allocated array.
  hm_node** children;
  int nchildren;
  int cap;

  // Fixed width in bytes for HM_BYTE/HM_NOT_BYTE/HM_ALT. -1 for HM_JUMP and
  // for HM_CONCAT patterns that contain jumps.
  int width;
};

// A generated pattern together with the benign input it is checked against.
typedef struct {
  hm_node* root;                 // always an HM_CONCAT
  uint8_t data[HM_MAX_DATA];
  int data_len;
  char description[64];          // short human label for fixed cases
} hm_case;

// Deterministic PRNG (xorshift64*), independent of libc rand().
typedef struct {
  uint64_t state;
} hm_rng;

uint64_t hm_rand(hm_rng* rng);
int hm_rand_range(hm_rng* rng, int lo, int hi);  // inclusive

// Memory ownership: every node is carved from a chunked arena so that whole
// patterns can be released with a single free. Chunks are linked and never
// reallocated, which keeps every pointer stored inside the arena (node and
// child arrays) stable for the arena's lifetime.
typedef struct hm_arena_chunk hm_arena_chunk;
struct hm_arena_chunk
{
  hm_arena_chunk* next;
  size_t used;
  size_t cap;
  uint8_t data[];
};

typedef struct
{
  hm_arena_chunk* head;
  hm_arena_chunk* cur;
} hm_arena;

void* hm_arena_alloc(hm_arena* a, size_t n);
void hm_arena_free(hm_arena* a);

hm_node* hm_new_byte(hm_arena* a, uint8_t value, uint8_t mask);
hm_node* hm_new_not(hm_arena* a, uint8_t value, uint8_t mask);
hm_node* hm_new_jump(hm_arena* a, int lo, int hi);
hm_node* hm_new_concat(hm_arena* a);
hm_node* hm_new_alt(hm_arena* a);
void hm_append(hm_node* parent, hm_node* child);

// Structural validation. Returns NULL when the pattern obeys every grammar
// restriction of the supported subset, otherwise a static reason string.
const char* hm_validate(const hm_node* root);

// Render the hex string body (without the surrounding rule), e.g.
// "4A (?? | ~?F) [2-3] 42". Writes at most cap bytes, always NUL terminated.
void hm_render_hex(const hm_node* root, char* out, size_t cap);

// Render a full rule using the #h >= 0 condition so that every full match is
// retained regardless of where it occurs.
void hm_render_rule(const hm_node* root, char* out, size_t cap);

// Print the full pattern structure (used by failure diagnostics).
void hm_dump(const hm_node* n, FILE* f, int indent);

// Append the same structural dump to a string buffer.
void hm_dump_str(const hm_node* n, char* out, size_t cap, int indent);

// One expected match. Offsets are relative to the start of the scanned buffer.
typedef struct {
  int64_t offset;
  int32_t length;
} hm_match;

// Compute expected matches straight from the pattern structure. For every
// matchable start position the shortest complete match length is reported;
// results are sorted by offset and overlapping starts are kept. On the
// restricted subset, derivations from the same start collapse into one result.
// Returns the number of matches (never truncates within HM_MAX_DATA inputs).
int hm_expected(const hm_node* root, const uint8_t* data, int data_len,
                hm_match* out, int out_cap);

// Parse a pattern back from the compact serialization produced by
// hm_serialize. Returns NULL on malformed input; rejection is distinct from a
// pattern that simply does not match.
hm_node* hm_deserialize(const uint8_t* buf, size_t len, hm_arena* a,
                        const char** reason);

// Compact one-byte-tag serialization of a pattern only (data is generated
// deterministically from the same bytes by the fuzzer entry point).
size_t hm_serialize(const hm_node* root, uint8_t* out, size_t cap);

// Generated cases.
//
// Build one structured pattern plus matching benign data. force_two_jumps
// requests a pattern with two long jumps (used to exercise the three-piece
// chain under threshold 2); long_near_boundary requests jump endpoints around
// 199..202.
void hm_generate(hm_rng* rng, hm_arena* a, hm_case* c, bool force_two_jumps,
                 bool long_near_boundary);

// Fill data with benign random bytes and plant one full derivation of root at
// a random start, with a minimal-length retry when the buffer is tight.
void hm_plant_random(const hm_node* root, uint8_t* data, int data_len,
                     hm_rng* rng);

// Fill a case's data buffer with deterministic benign bytes derived from a
// seed; embedding of pattern literals is done separately by the generator.
void hm_fill_data(hm_rng* rng, uint8_t* data, int len);

#endif
