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

#include "hex_model.h"

#include <stdlib.h>
#include <string.h>

//
// Deterministic PRNG (xorshift64*).
//
uint64_t hm_rand(hm_rng* rng)
{
  uint64_t x = rng->state;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  rng->state = x;
  return x * 0x2545F4914F6CDD1DULL;
}

int hm_rand_range(hm_rng* rng, int lo, int hi)
{
  if (hi <= lo)
    return lo;
  return lo + (int)(hm_rand(rng) % (uint64_t)(hi - lo + 1));
}

//
// Arena allocation. A whole pattern is carved from one arena and released
// with a single free. Chunks are linked and never reallocated, so node and
// child-array pointers handed out of the arena stay valid until the arena is
// freed; this keeps the fuzzer's deserialize path free of per-node cleanup.
//
#define HM_ARENA_CHUNK 4096

void* hm_arena_alloc(hm_arena* a, size_t n)
{
  if (a->cur == NULL || a->cur->used + n > a->cur->cap)
  {
    size_t cap = HM_ARENA_CHUNK;
    if (n > cap)
      cap = n;
    hm_arena_chunk* chunk = malloc(sizeof(hm_arena_chunk) + cap);
    if (chunk == NULL)
      return NULL;
    chunk->next = a->head;
    chunk->used = 0;
    chunk->cap = cap;
    a->head = chunk;
    a->cur = chunk;
  }
  void* p = a->cur->data + a->cur->used;
  a->cur->used += n;
  return p;
}

void hm_arena_free(hm_arena* a)
{
  hm_arena_chunk* chunk = a->head;
  while (chunk != NULL)
  {
    hm_arena_chunk* next = chunk->next;
    free(chunk);
    chunk = next;
  }
  a->head = NULL;
  a->cur = NULL;
}

static hm_node* alloc_node(hm_arena* a, hm_kind kind)
{
  hm_node* n = hm_arena_alloc(a, sizeof(hm_node));
  if (n == NULL)
    return NULL;
  memset(n, 0, sizeof(*n));
  n->kind = kind;
  n->width = -1;
  n->cap = HM_MAX_NODES + 1;
  n->children = hm_arena_alloc(a, sizeof(hm_node*) * (size_t)n->cap);
  if (n->children == NULL)
    return NULL;
  return n;
}

hm_node* hm_new_byte(hm_arena* a, uint8_t value, uint8_t mask)
{
  hm_node* n = alloc_node(a, HM_BYTE);
  if (n == NULL)
    return NULL;
  n->value = value & mask;  // masked-out nibbles are always stored as zero
  n->mask = mask;
  n->width = 1;
  return n;
}

hm_node* hm_new_not(hm_arena* a, uint8_t value, uint8_t mask)
{
  hm_node* n = alloc_node(a, HM_NOT_BYTE);
  if (n == NULL)
    return NULL;
  n->value = value & mask;
  n->mask = mask;  // mask == 0 (~??) is rejected by hm_validate
  n->width = 1;
  return n;
}

hm_node* hm_new_jump(hm_arena* a, int lo, int hi)
{
  hm_node* n = alloc_node(a, HM_JUMP);
  if (n == NULL)
    return NULL;
  n->jump_min = lo;
  n->jump_max = hi;
  return n;
}

hm_node* hm_new_concat(hm_arena* a)
{
  return alloc_node(a, HM_CONCAT);
}

hm_node* hm_new_alt(hm_arena* a)
{
  return alloc_node(a, HM_ALT);
}

void hm_append(hm_node* parent, hm_node* child)
{
  parent->children[parent->nchildren++] = child;
}

//
// Structural validation. A NULL return means the pattern is a legal member
// of the supported subset; anything else is a static reason string and the
// caller must treat the input as rejected, never as a non-matching pattern.
//
static int count_nodes(const hm_node* n)
{
  int total = 1;
  for (int i = 0; i < n->nchildren; i++)
    total += count_nodes(n->children[i]);
  return total;
}

static const char* validate_rec(const hm_node* n, int depth, bool under_alt,
                                int* jumps)
{
  // Depth counts nested composite nodes (concatenation / alternation). The
  // deepest legal shape is outer concat > alternation > branch concat, which
  // is three composite levels. Fixed-width leaves do not add a level.
  if ((n->kind == HM_CONCAT || n->kind == HM_ALT) && depth > HM_MAX_DEPTH)
    return "nesting depth exceeds HM_MAX_DEPTH";

  switch (n->kind)
  {
  case HM_BYTE:
    return NULL;

  case HM_NOT_BYTE:
    if (n->mask == 0x00)
      return "~?? is not a valid hex token";
    return NULL;

  case HM_JUMP:
    if (under_alt)
      return "jumps are not allowed inside alternation branches";
    // A direct child of the root concat is validated at depth 2.
    if (depth != 2)
      return "jumps may only appear directly in the outer concatenation";
    if (n->jump_min < 0 || n->jump_max < n->jump_min)
      return "invalid jump range";
    if (n->jump_max > HM_MAX_JUMP)
      return "jump endpoint exceeds HM_MAX_JUMP";
    (*jumps)++;
    if (*jumps > HM_MAX_JUMPS)
      return "more than HM_MAX_JUMPS jumps per pattern";
    return NULL;

  case HM_ALT:
  {
    if (n->nchildren < 2)
      return "alternation needs at least two branches";
    int w = -1;
    for (int i = 0; i < n->nchildren; i++)
    {
      const hm_node* branch = n->children[i];
      if (branch->kind != HM_CONCAT || branch->nchildren < 1)
        return "alternation branch must be a non-empty sequence";
      int bw = 0;
      for (int j = 0; j < branch->nchildren; j++)
      {
        const hm_node* leaf = branch->children[j];
        if (leaf->kind != HM_BYTE && leaf->kind != HM_NOT_BYTE)
          return "alternation branches may only contain fixed-width nodes";
        const char* r = validate_rec(leaf, depth, true, jumps);
        if (r != NULL)
          return r;
        bw += leaf->width;
      }
      if (w == -1)
        w = bw;
      else if (w != bw)
        return "alternation branches must have equal width";
    }
    return NULL;
  }

  case HM_CONCAT:
  {
    if (n->nchildren < 1)
      return "empty concatenation";
    for (int i = 0; i < n->nchildren; i++)
    {
      const char* r =
          validate_rec(n->children[i], depth + 1, under_alt, jumps);
      if (r != NULL)
        return r;
    }
    return NULL;
  }
  }

  return "unknown node kind";
}

const char* hm_validate(const hm_node* root)
{
  if (root == NULL || root->kind != HM_CONCAT)
    return "pattern root must be a concatenation";
  if (root->nchildren < 1)
    return "empty pattern";
  if (root->children[0]->kind == HM_JUMP ||
      root->children[root->nchildren - 1]->kind == HM_JUMP)
    return "hex strings may not start or end with a jump";
  if (count_nodes(root) > HM_MAX_NODES)
    return "pattern exceeds HM_MAX_NODES";
  int jumps = 0;
  return validate_rec(root, 1, false, &jumps);
}

//
// Rendering.
//
static void render_byte(const hm_node* n, char* out, size_t cap, int* pos)
{
  static const char hex[] = "0123456789ABCDEF";
  char tok[4];
  int k = 0;

  if (n->kind == HM_NOT_BYTE)
    tok[k++] = '~';

  uint8_t hi_mask = n->mask & 0xF0;
  uint8_t lo_mask = n->mask & 0x0F;

  tok[k++] = hi_mask ? hex[(n->value >> 4) & 0x0F] : '?';
  tok[k++] = lo_mask ? hex[n->value & 0x0F] : '?';
  tok[k] = '\0';

  *pos += snprintf(out + *pos, cap - (size_t)*pos, "%s", tok);
}

static void render_rec(const hm_node* n, char* out, size_t cap, int* pos)
{
  switch (n->kind)
  {
  case HM_BYTE:
  case HM_NOT_BYTE:
    render_byte(n, out, cap, pos);
    break;

  case HM_JUMP:
    // The grammar rejects the single-number spelling [0] ("invalid jump
    // length"); a zero-width jump must be written as the range [0-0]. The
    // range form is legal for every other endpoint as well.
    if (n->jump_min == 0 && n->jump_max == 0)
      *pos += snprintf(out + *pos, cap - (size_t)*pos, "[0-0]");
    else if (n->jump_min == n->jump_max)
      *pos += snprintf(out + *pos, cap - (size_t)*pos, "[%d]", n->jump_min);
    else
      *pos += snprintf(
          out + *pos, cap - (size_t)*pos, "[%d-%d]", n->jump_min,
          n->jump_max);
    break;

  case HM_ALT:
    *pos += snprintf(out + *pos, cap - (size_t)*pos, "( ");
    for (int i = 0; i < n->nchildren; i++)
    {
      if (i > 0)
        *pos += snprintf(out + *pos, cap - (size_t)*pos, " | ");
      const hm_node* branch = n->children[i];
      for (int j = 0; j < branch->nchildren; j++)
      {
        if (j > 0)
          *pos += snprintf(out + *pos, cap - (size_t)*pos, " ");
        render_rec(branch->children[j], out, cap, pos);
      }
    }
    *pos += snprintf(out + *pos, cap - (size_t)*pos, " )");
    break;

  case HM_CONCAT:
    for (int i = 0; i < n->nchildren; i++)
    {
      if (i > 0)
        *pos += snprintf(out + *pos, cap - (size_t)*pos, " ");
      render_rec(n->children[i], out, cap, pos);
    }
    break;
  }
}

void hm_render_hex(const hm_node* root, char* out, size_t cap)
{
  int pos = 0;
  render_rec(root, out, cap, &pos);
  if ((size_t)pos >= cap)
    pos = (int)cap - 1;
  out[pos] = '\0';
}

void hm_render_rule(const hm_node* root, char* out, size_t cap)
{
  char hex[512];
  hm_render_hex(root, hex, sizeof(hex));
  snprintf(
      out, cap, "rule hex_model_case { strings: $h = { %s } condition: #h >= 0 }",
      hex);
}

//
// Human-readable structural dump used in failure diagnostics.
//
void hm_dump(const hm_node* n, FILE* f, int indent)
{
  for (int i = 0; i < indent; i++)
    fputc(' ', f);

  switch (n->kind)
  {
  case HM_BYTE:
  case HM_NOT_BYTE:
    fprintf(
        f, "%s value=0x%02X mask=0x%02X width=%d\n",
        n->kind == HM_BYTE ? "BYTE" : "NOT_BYTE", n->value, n->mask, n->width);
    break;
  case HM_JUMP:
    fprintf(f, "JUMP [%d-%d]\n", n->jump_min, n->jump_max);
    break;
  case HM_ALT:
    fprintf(f, "ALT branches=%d\n", n->nchildren);
    for (int i = 0; i < n->nchildren; i++)
      hm_dump(n->children[i], f, indent + 2);
    break;
  case HM_CONCAT:
    fprintf(f, "CONCAT children=%d\n", n->nchildren);
    for (int i = 0; i < n->nchildren; i++)
      hm_dump(n->children[i], f, indent + 2);
    break;
  }
}

static void dump_str_indent(char* out, size_t cap, int* pos, int indent)
{
  for (int i = 0; i < indent && (size_t)*pos + 1 < cap; i++)
    *pos += snprintf(out + *pos, cap - (size_t)*pos, " ");
}

void hm_dump_str(const hm_node* n, char* out, size_t cap, int indent)
{
  int pos = (int)strlen(out);

  switch (n->kind)
  {
  case HM_BYTE:
  case HM_NOT_BYTE:
    dump_str_indent(out, cap, &pos, indent);
    pos += snprintf(
        out + pos, cap - (size_t)pos,
        "%s value=0x%02X mask=0x%02X width=%d\n",
        n->kind == HM_BYTE ? "BYTE" : "NOT_BYTE", n->value, n->mask,
        n->width);
    break;
  case HM_JUMP:
    dump_str_indent(out, cap, &pos, indent);
    pos += snprintf(
        out + pos, cap - (size_t)pos, "JUMP [%d-%d]\n", n->jump_min,
        n->jump_max);
    break;
  case HM_ALT:
    dump_str_indent(out, cap, &pos, indent);
    pos += snprintf(
        out + pos, cap - (size_t)pos, "ALT branches=%d\n", n->nchildren);
    for (int i = 0; i < n->nchildren; i++)
      hm_dump_str(n->children[i], out, cap, indent + 2);
    break;
  case HM_CONCAT:
    dump_str_indent(out, cap, &pos, indent);
    pos += snprintf(
        out + pos, cap - (size_t)pos, "CONCAT children=%d\n", n->nchildren);
    for (int i = 0; i < n->nchildren; i++)
      hm_dump_str(n->children[i], out, cap, indent + 2);
    break;
  }
}

//
// Reachability model. The outer concatenation is walked item by item; a
// bitset holds the input positions reachable after the items processed so
// far. Fixed-width items contribute a precomputed set of matching start
// positions, jumps dilate the reachable set by the inclusive byte range.
// Nothing here consults YARA internals: predicates are the mask/negation
// semantics straight from the hex grammar.
//
#define HM_WORDS ((HM_MAX_DATA + 1 + 63) / 64)

static void bs_zero(uint64_t bs[HM_WORDS])
{
  memset(bs, 0, sizeof(uint64_t) * HM_WORDS);
}

static void bs_set(uint64_t bs[HM_WORDS], int bit)
{
  bs[bit / 64] |= 1ULL << (bit % 64);
}

static bool bs_test(const uint64_t bs[HM_WORDS], int bit)
{
  return (bs[bit / 64] >> (bit % 64)) & 1ULL;
}

// out = in << n, dropping bits past HM_MAX_DATA.
static void bs_shift(const uint64_t in[HM_WORDS], int n,
                     uint64_t out[HM_WORDS])
{
  bs_zero(out);
  int word = n / 64;
  int bit = n % 64;
  for (int i = 0; i + word < HM_WORDS; i++)
  {
    uint64_t v = in[i] << bit;
    if (bit != 0 && i > 0)
      v |= in[i - 1] >> (64 - bit);
    out[i + word] |= v;
  }
}

// Keep bits 0..max_bit inclusive, clear every bit above it. Position
// data_len is a legal match end, so callers pass data_len (not data_len-1).
static void bs_truncate(uint64_t bs[HM_WORDS], int max_bit)
{
  int word = max_bit / 64;
  int bit = max_bit % 64;
  uint64_t mask = bit == 63 ? ~0ULL : ((1ULL << (bit + 1)) - 1);
  bs[word] &= mask;
  for (int w = word + 1; w < HM_WORDS; w++)
    bs[w] = 0;
}

// out = OR of (in << k) for k in [0, width]. The interval of shifts is split
// into dyadic blocks; P_k = OR of in<<0 .. in<<(2^k-1) is built by repeated
// doubling, so the whole dilation costs O(words * log width).
static void bs_dilate(const uint64_t in[HM_WORDS], int width,
                      uint64_t out[HM_WORDS])
{
  uint64_t powers[9][HM_WORDS];  // 2^8 = 256 > HM_MAX_JUMP + 1
  memcpy(powers[0], in, sizeof(powers[0]));

  int k = 0;
  while ((1 << (k + 1)) <= width + 1)
  {
    uint64_t shifted[HM_WORDS];
    bs_shift(powers[k], 1 << k, shifted);
    for (int w = 0; w < HM_WORDS; w++)
      powers[k + 1][w] = powers[k][w] | shifted[w];
    k++;
  }

  uint64_t acc[HM_WORDS];
  bs_zero(acc);

  int remaining = width + 1;
  int base = 0;
  while (remaining > 0)
  {
    int block = 1;
    int bk = 0;
    while (block * 2 <= remaining)
    {
      block *= 2;
      bk++;
    }
    uint64_t shifted[HM_WORDS];
    bs_shift(powers[bk], base, shifted);
    for (int w = 0; w < HM_WORDS; w++)
      acc[w] |= shifted[w];
    base += block;
    remaining -= block;
  }

  memcpy(out, acc, sizeof(acc));
}

static bool leaf_matches(const hm_node* leaf, const uint8_t* data, int pos)
{
  if (leaf->kind == HM_BYTE)
    return (data[pos] & leaf->mask) == leaf->value;
  return (data[pos] & leaf->mask) != leaf->value;
}

// Start-position bitset for any fixed-width item (leaf or alternation).
static void fixed_start_bitset(const hm_node* n, const uint8_t* data,
                               int data_len, uint64_t out[HM_WORDS])
{
  bs_zero(out);

  if (n->kind == HM_BYTE || n->kind == HM_NOT_BYTE)
  {
    for (int p = 0; p < data_len; p++)
      if (leaf_matches(n, data, p))
        bs_set(out, p);
    return;
  }

  // HM_ALT: union over the equal-width branches.
  int width = 0;
  for (int j = 0; j < n->children[0]->nchildren; j++)
    width += n->children[0]->children[j]->width;

  for (int p = 0; p + width <= data_len; p++)
  {
    for (int i = 0; i < n->nchildren; i++)
    {
      const hm_node* branch = n->children[i];
      int off = 0;
      bool ok = true;
      for (int j = 0; j < branch->nchildren; j++)
      {
        const hm_node* leaf = branch->children[j];
        if (!leaf_matches(leaf, data, p + off))
        {
          ok = false;
          break;
        }
        off += leaf->width;
      }
      if (ok)
      {
        bs_set(out, p);
        break;
      }
    }
  }
}

static int item_width(const hm_node* n)
{
  if (n->kind == HM_JUMP)
    return -1;
  if (n->kind == HM_ALT)
  {
    int w = 0;
    for (int j = 0; j < n->children[0]->nchildren; j++)
      w += n->children[0]->children[j]->width;
    return w;
  }
  return n->width;
}

int hm_expected(const hm_node* root, const uint8_t* data, int data_len,
                hm_match* out, int out_cap)
{
  const hm_node* items[HM_MAX_NODES + 1];
  uint64_t starts[HM_MAX_NODES + 1][HM_WORDS];
  int nitems = 0;

  for (int i = 0; i < root->nchildren; i++)
  {
    const hm_node* it = root->children[i];
    items[nitems] = it;
    if (it->kind != HM_JUMP)
      fixed_start_bitset(it, data, data_len, starts[nitems]);
    nitems++;
  }

  int nmatches = 0;

  for (int s = 0; s < data_len; s++)
  {
    uint64_t cur[HM_WORDS];
    bs_zero(cur);
    bs_set(cur, s);

    for (int i = 0; i < nitems; i++)
    {
      const hm_node* it = items[i];
      uint64_t nxt[HM_WORDS];
      bs_zero(nxt);

      if (it->kind == HM_JUMP)
      {
        uint64_t shifted[HM_WORDS];
        bs_shift(cur, it->jump_min, shifted);
        bs_dilate(shifted, it->jump_max - it->jump_min, nxt);
      }
      else
      {
        uint64_t eligible[HM_WORDS];
        for (int w = 0; w < HM_WORDS; w++)
          eligible[w] = cur[w] & starts[i][w];
        bs_shift(eligible, item_width(it), nxt);
      }

      // Positions past the end of the data can't start or finish a match;
      // position data_len itself is retained because a match may end exactly
      // on the last byte.
      for (int w = 0; w < HM_WORDS; w++)
        cur[w] = nxt[w];
      bs_truncate(cur, data_len);
    }

    int end = -1;
    for (int p = s; p <= data_len; p++)
    {
      if (bs_test(cur, p))
      {
        end = p;
        break;  // shortest complete derivation from start s
      }
    }

    if (end >= 0 && nmatches < out_cap)
    {
      out[nmatches].offset = s;
      out[nmatches].length = end - s;
      nmatches++;
    }
  }

  return nmatches;
}

//
// Compact serialization (pattern only). Tags:
//   1 BYTE       payload mask, value
//   2 NOT_BYTE   payload mask, value
//   3 JUMP       payload lo, hi
//   4 CONCAT     payload child_count, children
//   5 ALT        payload branch_count, branch concats
//
enum
{
  TAG_BYTE = 1,
  TAG_NOT = 2,
  TAG_JUMP = 3,
  TAG_CONCAT = 4,
  TAG_ALT = 5
};

static size_t serialize_rec(const hm_node* n, uint8_t* out, size_t cap,
                            size_t pos)
{
  size_t need;
  switch (n->kind)
  {
  case HM_BYTE:
  case HM_NOT_BYTE:
    need = 3;
    if (pos + need > cap)
      return 0;
    out[pos++] = n->kind == HM_BYTE ? TAG_BYTE : TAG_NOT;
    out[pos++] = n->mask;
    out[pos++] = n->value;
    return pos;
  case HM_JUMP:
    need = 3;
    if (pos + need > cap)
      return 0;
    out[pos++] = TAG_JUMP;
    out[pos++] = (uint8_t)n->jump_min;
    out[pos++] = (uint8_t)n->jump_max;
    return pos;
  case HM_CONCAT:
  case HM_ALT:
    need = 2;
    if (pos + need > cap)
      return 0;
    out[pos++] = n->kind == HM_CONCAT ? TAG_CONCAT : TAG_ALT;
    out[pos++] = (uint8_t)n->nchildren;
    for (int i = 0; i < n->nchildren; i++)
    {
      pos = serialize_rec(n->children[i], out, cap, pos);
      if (pos == 0)
        return 0;
    }
    return pos;
  }
  return 0;
}

size_t hm_serialize(const hm_node* root, uint8_t* out, size_t cap)
{
  return serialize_rec(root, out, cap, 0);
}

static hm_node* deserialize_rec(const uint8_t* buf, size_t len, size_t* pos,
                                hm_arena* a, const char** reason)
{
  if (*pos >= len)
  {
    *reason = "truncated pattern";
    return NULL;
  }
  uint8_t tag = buf[(*pos)++];

  switch (tag)
  {
  case TAG_BYTE:
  case TAG_NOT:
  {
    if (*pos + 2 > len)
    {
      *reason = "truncated byte node";
      return NULL;
    }
    uint8_t mask = buf[(*pos)++];
    uint8_t value = buf[(*pos)++];
    return tag == TAG_BYTE ? hm_new_byte(a, value, mask)
                           : hm_new_not(a, value, mask);
  }
  case TAG_JUMP:
  {
    if (*pos + 2 > len)
    {
      *reason = "truncated jump node";
      return NULL;
    }
    uint8_t lo = buf[(*pos)++];
    uint8_t hi = buf[(*pos)++];
    return hm_new_jump(a, lo, hi);
  }
  case TAG_CONCAT:
  case TAG_ALT:
  {
    if (*pos >= len)
    {
      *reason = "truncated container header";
      return NULL;
    }
    int count = buf[(*pos)++];
    if (count < 1 || count > HM_MAX_NODES)
    {
      *reason = "container child count out of range";
      return NULL;
    }
    hm_node* n = tag == TAG_CONCAT ? hm_new_concat(a) : hm_new_alt(a);
    if (n == NULL)
    {
      *reason = "allocation failure";
      return NULL;
    }
    for (int i = 0; i < count; i++)
    {
      hm_node* child = deserialize_rec(buf, len, pos, a, reason);
      if (child == NULL)
        return NULL;
      hm_append(n, child);
    }
    return n;
  }
  default:
    *reason = "unknown serialization tag";
    return NULL;
  }
}

hm_node* hm_deserialize(const uint8_t* buf, size_t len, hm_arena* a,
                        const char** reason)
{
  *reason = "malformed input";
  size_t pos = 0;
  hm_node* root = deserialize_rec(buf, len, &pos, a, reason);
  if (root == NULL)
    return NULL;
  if (pos != len)
  {
    *reason = "trailing bytes after pattern";
    return NULL;
  }
  const char* v = hm_validate(root);
  if (v != NULL)
  {
    *reason = v;
    return NULL;
  }
  return root;
}
