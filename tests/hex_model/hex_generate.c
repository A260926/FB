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

#include <string.h>

//
// Structured-pattern generator and benign-data producer. Patterns are built
// first; data is rendered afterwards by planting full derivations (and
// occasional near misses) into a deterministic random byte buffer.
//

#define HM_GEN_MIN(a, b) ((a) < (b) ? (a) : (b))
#define HM_GEN_MAX(a, b) ((a) > (b) ? (a) : (b))

void hm_fill_data(hm_rng* rng, uint8_t* data, int len)
{
  for (int i = 0; i < len; i++)
    data[i] = (uint8_t)hm_rand(rng);
}

static hm_node* random_leaf(hm_rng* rng, hm_arena* a)
{
  static const uint8_t nibble_masks[4] = {0xF0, 0x0F, 0xFF, 0xFF};
  int kind = hm_rand_range(rng, 0, 6);
  uint8_t value = (uint8_t)hm_rand(rng);
  uint8_t mask;

  switch (kind)
  {
  case 0:  // full wildcard
    return hm_new_byte(a, 0, 0x00);
  case 1:  // half-nibble wildcard, high nibble significant
    return hm_new_byte(a, value, 0xF0);
  case 2:  // half-nibble wildcard, low nibble significant
    return hm_new_byte(a, value, 0x0F);
  case 3:  // plain literal
    return hm_new_byte(a, value, 0xFF);
  case 4:  // negated literal
    return hm_new_not(a, value, 0xFF);
  default:  // negated half wildcard, mask deliberately non-zero
    mask = nibble_masks[hm_rand_range(rng, 0, 3)];
    return hm_new_not(a, value, mask);
  }
}

// Fixed-width alternation: every branch gets the same number of leaves.
static hm_node* random_alt(hm_rng* rng, hm_arena* a)
{
  hm_node* alt = hm_new_alt(a);
  int branches = hm_rand_range(rng, 2, 3);
  int leaves = hm_rand_range(rng, 1, 2);

  for (int i = 0; i < branches; i++)
  {
    hm_node* branch = hm_new_concat(a);
    for (int j = 0; j < leaves; j++)
      hm_append(branch, random_leaf(rng, a));
    hm_append(alt, branch);
  }
  return alt;
}

static hm_node* random_fixed_item(hm_rng* rng, hm_arena* a)
{
  if (hm_rand_range(rng, 0, 3) == 0)
    return random_alt(rng, a);
  return random_leaf(rng, a);
}

static void append_fixed_segment(hm_rng* rng, hm_arena* a, hm_node* root,
                                 int max_items)
{
  int items = hm_rand_range(rng, 1, max_items);
  for (int i = 0; i < items; i++)
    hm_append(root, random_fixed_item(rng, a));
}

// Pick a jump range according to the requested distance profile.
static void jump_profile(hm_rng* rng, bool long_near_boundary, int* lo,
                         int* hi)
{
  if (long_near_boundary)
  {
    // Endpoints clustered on 199/200/201/202 to straddle the default
    // YR_STRING_CHAINING_THRESHOLD of 200.
    int lows[4] = {198, 199, 200, 201};
    *lo = lows[hm_rand_range(rng, 0, 3)];
    *hi = hm_rand_range(rng, *lo, HM_GEN_MIN(*lo + 2, HM_MAX_JUMP));
    return;
  }

  int profile = hm_rand_range(rng, 0, 4);
  if (profile == 0)
  {
    *lo = 0;
    *hi = hm_rand_range(rng, 1, 6);
  }
  else if (profile <= 3)
  {
    *lo = hm_rand_range(rng, 0, 10);
    *hi = hm_rand_range(rng, *lo, *lo + 8);
  }
  else
  {
    *lo = hm_rand_range(rng, 20, 120);
    *hi = hm_rand_range(rng, *lo, HM_GEN_MIN(*lo + 40, HM_MAX_JUMP));
  }
}

//
// Plant one derivation of the pattern into data starting at *pos. Jumps
// consume arbitrary in-range bytes; alternation branches and negated/masked
// leaves are satisfied explicitly. When satisfy is false one fixed leaf is
// forced to violate its predicate (a near miss). With minimal=true every jump
// consumes exactly jump_min, which guarantees the shortest derivation fits.
//
static bool plant_rec(const hm_node* n, uint8_t* data, int data_len, int* pos,
                      hm_rng* rng, bool satisfy, bool minimal)
{
  switch (n->kind)
  {
  case HM_BYTE:
    if (*pos >= data_len)
      return false;
    if (!satisfy && n->mask != 0x00)
    {
      // Flip a significant nibble so the masked comparison must fail.
      uint8_t flip = (n->mask & 0x0F) ? 0x01 : 0x10;
      data[(*pos)++] = n->value ^ flip;
      return true;
    }
    if (n->mask == 0x00)
      data[(*pos)++] = (uint8_t)hm_rand(rng);
    else
    {
      data[*pos] = (uint8_t)((data[*pos] & ~n->mask) | n->value);
      (*pos)++;
    }
    return true;

  case HM_NOT_BYTE:
    if (*pos >= data_len)
      return false;
    if (!satisfy)
      data[(*pos)++] = n->value;  // exact equality violates negation
    else
    {
      uint8_t flip = (n->mask & 0x0F) ? 0x01 : 0x10;
      data[(*pos)++] = n->value ^ flip;
    }
    return true;

  case HM_JUMP:
  {
    int consume = minimal
                      ? n->jump_min
                      : hm_rand_range(rng, n->jump_min, n->jump_max);
    if (*pos + consume > data_len)
      return false;
    for (int i = 0; i < consume; i++)
      data[(*pos)++] = (uint8_t)hm_rand(rng);
    return true;
  }

  case HM_ALT:
  {
    const hm_node* branch =
        n->children[hm_rand_range(rng, 0, n->nchildren - 1)];
    return plant_rec(branch, data, data_len, pos, rng, satisfy, minimal);
  }

  case HM_CONCAT:
    for (int i = 0; i < n->nchildren; i++)
      if (!plant_rec(
              n->children[i], data, data_len, pos, rng, satisfy, minimal))
        return false;
    return true;
  }

  return false;
}

static int pattern_min_length(const hm_node* n)
{
  switch (n->kind)
  {
  case HM_BYTE:
  case HM_NOT_BYTE:
    return 1;
  case HM_JUMP:
    return n->jump_min;
  case HM_ALT:
  {
    int w = 0;
    for (int j = 0; j < n->children[0]->nchildren; j++)
      w += n->children[0]->children[j]->width;
    return w;
  }
  case HM_CONCAT:
  {
    int total = 0;
    for (int i = 0; i < n->nchildren; i++)
      total += pattern_min_length(n->children[i]);
    return total;
  }
  }
  return 0;
}

static int pattern_max_length(const hm_node* n)
{
  switch (n->kind)
  {
  case HM_BYTE:
  case HM_NOT_BYTE:
    return 1;
  case HM_JUMP:
    return n->jump_max;
  case HM_ALT:
    return pattern_min_length(n);  // branches are equal width
  case HM_CONCAT:
  {
    int total = 0;
    for (int i = 0; i < n->nchildren; i++)
      total += pattern_max_length(n->children[i]);
    return total;
  }
  }
  return 0;
}

static bool pattern_has_alt(const hm_node* n)
{
  if (n->kind == HM_ALT)
    return true;
  for (int i = 0; i < n->nchildren; i++)
    if (pattern_has_alt(n->children[i]))
      return true;
  return false;
}

static bool plant_derivation(const hm_node* root, uint8_t* data, int data_len,
                             int start, hm_rng* rng, bool satisfy)
{
  int pos = start;
  if (plant_rec(root, data, data_len, &pos, rng, satisfy, false))
    return true;
  // Retry with the shortest possible derivation so a tight buffer still gets
  // a guaranteed plant.
  pos = start;
  return plant_rec(root, data, data_len, &pos, rng, satisfy, true);
}

void hm_plant_random(const hm_node* root, uint8_t* data, int data_len,
                     hm_rng* rng)
{
  int min_len = pattern_min_length(root);
  if (min_len > data_len)
    return;
  int start = hm_rand_range(rng, 0, data_len - min_len);
  plant_derivation(root, data, data_len, start, rng, true);
}

void hm_generate(hm_rng* rng, hm_arena* a, hm_case* c, bool force_two_jumps,
                 bool long_near_boundary)
{
  for (int attempt = 0; attempt < 16; attempt++)
  {
    hm_arena scratch = {0};
    hm_node* root = hm_new_concat(&scratch);
    if (root == NULL)
      continue;

    int jumps_wanted;
    if (force_two_jumps)
      jumps_wanted = 2;
    else if (long_near_boundary)
      jumps_wanted = 1;
    else
      jumps_wanted = hm_rand_range(rng, 0, HM_MAX_JUMPS);

    int jumps_placed = 0;
    append_fixed_segment(rng, &scratch, root, 3);

    while (jumps_placed < jumps_wanted)
    {
      int lo, hi;
      if (force_two_jumps)
      {
        // Both endpoints stay at or below 200 so the default build keeps one
        // unsplit string while threshold 2 splits at every jump.
        hi = hm_rand_range(rng, 185, 200);
        lo = hm_rand_range(rng, 180, hi);
      }
      else
      {
        jump_profile(rng, long_near_boundary, &lo, &hi);
      }

      hm_append(root, hm_new_jump(&scratch, lo, hi));
      jumps_placed++;
      append_fixed_segment(rng, &scratch, root, 3);
    }

    if (hm_validate(root) != NULL)
    {
      hm_arena_free(&scratch);
      continue;
    }

    int max_len = pattern_max_length(root);
    int data_len =
        hm_rand_range(rng, HM_GEN_MAX(max_len + 8, 48), HM_MAX_DATA);

    // Deep-copy via serialization, which also re-checks the structure.
    uint8_t packed[256];
    size_t plen = hm_serialize(root, packed, sizeof(packed));
    const char* reason;
    hm_node* copy = hm_deserialize(packed, plen, a, &reason);
    hm_arena_free(&scratch);
    if (plen == 0 || copy == NULL)
      continue;

    c->root = copy;
    hm_fill_data(rng, c->data, data_len);
    c->data_len = data_len;
    snprintf(c->description, sizeof(c->description), "gen");

    // Plant one or two full derivations at distinct starts; random placement
    // also produces overlapping starts in many cases.
    int plants = hm_rand_range(rng, 1, 2);
    for (int p = 0; p < plants; p++)
    {
      int start = hm_rand_range(rng, 0, HM_GEN_MAX(0, data_len - max_len - 1));
      plant_derivation(c->root, c->data, data_len, start, rng, true);
    }

    // Near-miss plant: violate one fixed leaf. Skipped for alternation
    // patterns where another branch could still satisfy.
    if (!pattern_has_alt(c->root) && max_len + 2 <= data_len)
    {
      int start = hm_rand_range(rng, 0, data_len - max_len - 1);
      plant_derivation(c->root, c->data, data_len, start, rng, false);
    }

    return;
  }

  // Defensive fallback (the attempts above are expected to always succeed).
  hm_node* root = hm_new_concat(a);
  hm_append(root, hm_new_byte(a, 0x41, 0xFF));
  hm_append(root, hm_new_jump(a, 1, 3));
  hm_append(root, hm_new_byte(a, 0x42, 0xFF));
  c->root = root;
  c->data_len = 16;
  hm_fill_data(rng, c->data, c->data_len);
  c->data[0] = 0x41;
  c->data[5] = 0x42;
  snprintf(c->description, sizeof(c->description), "gen-fallback");
}
