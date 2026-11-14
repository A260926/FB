/*
 * Fixed regression cases with hand-computed expected matches. The hand list
 * is derived by manual calculation; the driver compares it against the
 * oracle, so a bug in the oracle cannot silently pass these cases.
 */

#include "hex_cases.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static hm_fixed* fx_begin(hm_fixed* f, const char* name) {
  memset(f, 0, sizeof(*f));
  hm_pattern_init(&f->c.pattern);
  f->name = name;
  f->c.name = name;
  return f;
}

static hm_node* fx_root(hm_fixed* f) {
  hm_node* root = hm_concat(&f->c.pattern);
  f->c.pattern.root = root;
  return root;
}

static void fx_data(hm_fixed* f, const uint8_t* d, int n) {
  assert(n <= HM_MAX_DATA);
  memcpy(f->c.data, d, (size_t) n);
  f->c.data_len = n;
}

static void fx_hand(hm_fixed* f, int start, int length) {
  assert(f->hand_count < HM_FIXED_HAND_MAX);
  f->hand[f->hand_count].start = start;
  f->hand[f->hand_count].length = length;
  f->hand_count++;
}

static hm_node* B(hm_fixed* f, uint8_t v, uint8_t m, bool neg) {
  return hm_byte(&f->c.pattern, v, m, neg);
}

// Builds one long-distance boundary case { 41 42 [g-g] 43 44 }.
static void fx_jump_boundary(hm_fixed* f, const char* name, int g) {
  fx_begin(f, name);
  hm_node* root = fx_root(f);
  hm_kid(root, B(f, 0x41, 0xFF, false));
  hm_kid(root, B(f, 0x42, 0xFF, false));
  hm_kid(root, hm_jump(&f->c.pattern, g, g));
  hm_kid(root, B(f, 0x43, 0xFF, false));
  hm_kid(root, B(f, 0x44, 0xFF, false));

  int total = g + 4;
  uint8_t d[HM_MAX_DATA];
  memset(d, 0, sizeof(d));
  d[0] = 0x41;
  d[1] = 0x42;
  d[g + 2] = 0x43;
  d[g + 3] = 0x44;
  fx_data(f, d, total);
  fx_hand(f, 0, total);  // single full-length match ending at the last byte
}

int hm_build_fixed(hm_fixed* out, int cap) {
  int n = 0;
  hm_fixed* f;
  hm_node* root;
  hm_node* alt;
  hm_node* branch;

#define NEED(k) assert(n + (k) <= cap)

  // F1: two plain literal occurrences.
  NEED(1);
  f = fx_begin(&out[n], "literal_basic");
  root = fx_root(f);
  hm_kid(root, B(f, 0x41, 0xFF, false));
  hm_kid(root, B(f, 0x42, 0xFF, false));
  {
    uint8_t d[] = {0x41, 0x42, 0x00, 0x41, 0x42};
    fx_data(f, d, (int) sizeof(d));
  }
  fx_hand(f, 0, 2);
  fx_hand(f, 3, 2);
  n++;

  // F2: overlapping starts through a bounded jump, shortest lengths.
  NEED(1);
  f = fx_begin(&out[n], "overlap_jump");
  root = fx_root(f);
  hm_kid(root, B(f, 0x41, 0xFF, false));
  hm_kid(root, hm_jump(&f->c.pattern, 2, 4));
  hm_kid(root, B(f, 0x42, 0xFF, false));
  {
    uint8_t d[] = {0x41, 0x41, 0x00, 0x42, 0x42};
    fx_data(f, d, (int) sizeof(d));
  }
  fx_hand(f, 0, 4);
  fx_hand(f, 1, 4);
  n++;

  // F3: alternation with nibble wildcard and masked negation.
  NEED(1);
  f = fx_begin(&out[n], "masked_alt");
  root = fx_root(f);
  hm_kid(root, B(f, 0x41, 0xFF, false));
  alt = hm_alt(&f->c.pattern);
  branch = hm_concat(&f->c.pattern);
  hm_kid(branch, B(f, 0x10, 0xF0, false));  // 1?
  hm_kid(alt, branch);
  branch = hm_concat(&f->c.pattern);
  hm_kid(branch, B(f, 0x0F, 0x0F, true));   // ~?F
  hm_kid(alt, branch);
  hm_kid(root, alt);
  hm_kid(root, B(f, 0x00, 0x00, false));    // ??
  hm_kid(root, B(f, 0x42, 0xFF, false));
  {
    uint8_t d[] = {0x41, 0x19, 0x5A, 0x42};
    fx_data(f, d, (int) sizeof(d));
  }
  fx_hand(f, 0, 4);
  n++;

  // F4: byte and nibble negation, only the inner start satisfies both.
  NEED(1);
  f = fx_begin(&out[n], "negation_pair");
  root = fx_root(f);
  hm_kid(root, B(f, 0x41, 0xFF, true));    // ~41
  hm_kid(root, B(f, 0x00, 0x0F, true));    // ~?0
  {
    uint8_t d[] = {0x41, 0x42, 0x43};
    fx_data(f, d, (int) sizeof(d));
  }
  fx_hand(f, 1, 2);
  n++;

  // F5: both masked-negation forms.
  NEED(1);
  f = fx_begin(&out[n], "negation_masked");
  root = fx_root(f);
  hm_kid(root, B(f, 0xF0, 0xF0, true));    // ~F?
  hm_kid(root, B(f, 0x0F, 0x0F, true));    // ~?F
  {
    uint8_t d[] = {0xFF, 0x0F, 0xF0, 0x00};
    fx_data(f, d, (int) sizeof(d));
  }
  fx_hand(f, 1, 2);
  n++;

  // F6: equal-width alternation branches, two disjoint occurrences.
  NEED(1);
  f = fx_begin(&out[n], "alt_equal_width");
  root = fx_root(f);
  alt = hm_alt(&f->c.pattern);
  branch = hm_concat(&f->c.pattern);
  hm_kid(branch, B(f, 0x41, 0xFF, false));
  hm_kid(branch, B(f, 0x42, 0xFF, false));
  hm_kid(alt, branch);
  branch = hm_concat(&f->c.pattern);
  hm_kid(branch, B(f, 0x43, 0xFF, false));
  hm_kid(branch, B(f, 0x44, 0xFF, false));
  hm_kid(alt, branch);
  hm_kid(root, alt);
  hm_kid(root, B(f, 0x45, 0xFF, false));
  {
    uint8_t d[] = {0x43, 0x44, 0x45, 0x41, 0x42, 0x45};
    fx_data(f, d, (int) sizeof(d));
  }
  fx_hand(f, 0, 3);
  fx_hand(f, 3, 3);
  n++;

  // F7: alternation with a wildcard suffix, spaced occurrences.
  NEED(1);
  f = fx_begin(&out[n], "alt_wildcard");
  root = fx_root(f);
  alt = hm_alt(&f->c.pattern);
  hm_kid(alt, B(f, 0x41, 0xFF, false));
  hm_kid(alt, B(f, 0x42, 0xFF, false));
  hm_kid(root, alt);
  hm_kid(root, B(f, 0x00, 0x00, false));  // ??
  {
    uint8_t d[] = {0x41, 0x00, 0x42, 0x00};
    fx_data(f, d, (int) sizeof(d));
  }
  fx_hand(f, 0, 2);
  fx_hand(f, 2, 2);
  n++;

  // F8: jump whose lower bound is zero (adjacent match wins, non-greedy).
  NEED(1);
  f = fx_begin(&out[n], "jump_zero");
  root = fx_root(f);
  hm_kid(root, B(f, 0x41, 0xFF, false));
  hm_kid(root, hm_jump(&f->c.pattern, 0, 2));
  hm_kid(root, B(f, 0x42, 0xFF, false));
  {
    uint8_t d[] = {0x41, 0x42, 0x42, 0x42};
    fx_data(f, d, (int) sizeof(d));
  }
  fx_hand(f, 0, 2);
  n++;

  // F9: exact-width jump.
  NEED(1);
  f = fx_begin(&out[n], "jump_exact");
  root = fx_root(f);
  hm_kid(root, B(f, 0x41, 0xFF, false));
  hm_kid(root, hm_jump(&f->c.pattern, 2, 2));
  hm_kid(root, B(f, 0x42, 0xFF, false));
  {
    uint8_t d[] = {0x41, 0x00, 0x00, 0x42};
    fx_data(f, d, (int) sizeof(d));
  }
  fx_hand(f, 0, 4);
  n++;

  // F10: the only successor sits one byte below the minimum gap, so the
  // pattern has no match. The failure is a relation failure, not a rejection.
  NEED(1);
  f = fx_begin(&out[n], "near_miss_gap");
  root = fx_root(f);
  hm_kid(root, B(f, 0x41, 0xFF, false));
  hm_kid(root, hm_jump(&f->c.pattern, 3, 4));
  hm_kid(root, B(f, 0x42, 0xFF, false));
  {
    // 42 at index 3 is gap 2, one below the minimum gap of 3.
    uint8_t d[] = {0x41, 0x00, 0x00, 0x42, 0x00};
    fx_data(f, d, (int) sizeof(d));
  }
  n++;  // hand list intentionally empty

  // F11: two heads share successors; the nearest successor for the second
  // head is one byte too close (gap 1 vs minimum 2), while both heads still
  // complete through another successor. Overlapping starts, non-greedy.
  NEED(1);
  f = fx_begin(&out[n], "shared_successor");
  root = fx_root(f);
  hm_kid(root, B(f, 0x41, 0xFF, false));
  hm_kid(root, hm_jump(&f->c.pattern, 2, 3));
  hm_kid(root, B(f, 0x5A, 0xFF, false));
  {
    // heads at 0 and 1; 5A at index 3 (gap 2 from head 0, gap 1 from head 1)
    // and index 4 (gap 3 from head 0, gap 2 from head 1).
    uint8_t d[] = {0x41, 0x41, 0x00, 0x5A, 0x5A};
    fx_data(f, d, (int) sizeof(d));
  }
  fx_hand(f, 0, 4);  // head 0 takes nearest legal 5A (index 3)
  fx_hand(f, 1, 4);  // head 1: index 3 illegal, index 4 legal
  n++;

  // F12: match ending exactly on the final input byte.
  NEED(1);
  f = fx_begin(&out[n], "ends_at_last_byte");
  root = fx_root(f);
  hm_kid(root, B(f, 0x41, 0xFF, false));
  hm_kid(root, B(f, 0x42, 0xFF, false));
  {
    uint8_t d[] = {0x00, 0x41, 0x42};
    fx_data(f, d, (int) sizeof(d));
  }
  fx_hand(f, 1, 2);
  n++;

  // F13: two short jumps; one string at threshold 200, three fragments at 2.
  NEED(1);
  f = fx_begin(&out[n], "chain_switch");
  root = fx_root(f);
  hm_kid(root, B(f, 0x41, 0xFF, false));
  hm_kid(root, B(f, 0x42, 0xFF, false));
  hm_kid(root, hm_jump(&f->c.pattern, 3, 4));
  hm_kid(root, B(f, 0x43, 0xFF, false));
  hm_kid(root, B(f, 0x44, 0xFF, false));
  hm_kid(root, hm_jump(&f->c.pattern, 3, 4));
  hm_kid(root, B(f, 0x45, 0xFF, false));
  hm_kid(root, B(f, 0x46, 0xFF, false));
  {
    uint8_t d[12];
    memset(d, 0, sizeof(d));
    d[0] = 0x41;
    d[1] = 0x42;
    d[5] = 0x43;
    d[6] = 0x44;
    d[10] = 0x45;
    d[11] = 0x46;
    fx_data(f, d, 12);
  }
  fx_hand(f, 0, 12);
  n++;

  // F14: two long jumps; three fragments in both builds.
  NEED(1);
  f = fx_begin(&out[n], "native_chain_long");
  root = fx_root(f);
  hm_kid(root, B(f, 0x41, 0xFF, false));
  hm_kid(root, B(f, 0x42, 0xFF, false));
  hm_kid(root, hm_jump(&f->c.pattern, 201, 202));
  hm_kid(root, B(f, 0x43, 0xFF, false));
  hm_kid(root, B(f, 0x44, 0xFF, false));
  hm_kid(root, hm_jump(&f->c.pattern, 201, 202));
  hm_kid(root, B(f, 0x45, 0xFF, false));
  hm_kid(root, B(f, 0x46, 0xFF, false));
  {
    static uint8_t d[408];
    memset(d, 0, sizeof(d));
    d[0] = 0x41;
    d[1] = 0x42;
    d[203] = 0x43;
    d[204] = 0x44;
    d[406] = 0x45;
    d[407] = 0x46;
    fx_data(f, d, 408);
  }
  fx_hand(f, 0, 408);
  n++;

  // F15-F18: real long-distance inputs straddling the 199..202 boundary.
  static const int gaps[] = {199, 200, 201, 202};
  for (int i = 0; i < 4; i++) {
    NEED(1);
    char name[32];
    snprintf(name, sizeof(name), "jump_boundary_%d", gaps[i]);
    fx_jump_boundary(&out[n], name, gaps[i]);
    n++;
  }

  // F19: alternation + masked negation + nibble wildcard combined with a
  // jump that splits at threshold 2.
  NEED(1);
  f = fx_begin(&out[n], "mask_neg_alt_chain");
  root = fx_root(f);
  alt = hm_alt(&f->c.pattern);
  hm_kid(alt, B(f, 0x41, 0xFF, false));          // 41
  hm_kid(alt, B(f, 0x00, 0x0F, true));           // ~?0
  hm_kid(root, alt);
  hm_kid(root, hm_jump(&f->c.pattern, 3, 4));
  hm_kid(root, B(f, 0x50, 0xF0, false));         // 5?
  hm_kid(root, B(f, 0x42, 0xFF, false));
  {
    uint8_t d[] = {0x44, 0x00, 0x00, 0x00, 0x5A, 0x42};
    fx_data(f, d, (int) sizeof(d));
  }
  fx_hand(f, 0, 6);
  n++;

  // F20: nibble wildcard and ?? without jumps.
  NEED(1);
  f = fx_begin(&out[n], "nibble_wildcards");
  root = fx_root(f);
  hm_kid(root, B(f, 0x40, 0xF0, false));         // 4?
  hm_kid(root, B(f, 0x00, 0x00, false));         // ??
  hm_kid(root, B(f, 0x03, 0x0F, false));         // ?3
  {
    uint8_t d[] = {0x45, 0x77, 0x23};
    fx_data(f, d, (int) sizeof(d));
  }
  fx_hand(f, 0, 3);
  n++;

  // F21: plain negation rejects the first byte, matches the second.
  NEED(1);
  f = fx_begin(&out[n], "plain_negation");
  root = fx_root(f);
  hm_kid(root, B(f, 0x41, 0xFF, true));          // ~41
  {
    uint8_t d[] = {0x41, 0x42};
    fx_data(f, d, (int) sizeof(d));
  }
  fx_hand(f, 1, 1);
  n++;

  // F22: PINNED scanner defect. The pattern has two adjacent jumps, one of
  // which ([202-202]) crosses the chaining threshold in both builds, so the
  // split leaves a tail fragment that itself starts with a variable jump
  // ([3-4]). Two overlapping heads (AA at 3 and 4) converge on the single DB
  // anchor at 210; the non-greedy relation admits both derivations, but the
  // chained scanner only publishes the later head. The model's hand[] answer
  // below is the correct one and is never altered to match the library;
  // buggy[] records what the library currently emits while the defect stands.
  //
  // Hand derivation (data length 211, AA at 3 and 4, DB at 210):
  //   start 3: head ends 4, +202 -> tail begins 206, gap 4 -> DB at 210,
  //            full length 211-3 = 208
  //   start 4: head ends 5, +202 -> tail begins 207, gap 3 -> DB at 210,
  //            full length 211-4 = 207
  NEED(1);
  f = fx_begin(&out[n], "pinned_chain_tail_leads_jump");
  root = fx_root(f);
  hm_kid(root, B(f, 0xAA, 0xFF, false));
  hm_kid(root, hm_jump(&f->c.pattern, 202, 202));
  hm_kid(root, hm_jump(&f->c.pattern, 3, 4));
  hm_kid(root, B(f, 0xDB, 0xFF, false));
  {
    static uint8_t d[211];
    memset(d, 0, sizeof(d));
    d[3] = 0xAA;
    d[4] = 0xAA;
    d[210] = 0xDB;
    fx_data(f, d, 211);
  }
  fx_hand(f, 3, 208);  // correct oracle answer
  fx_hand(f, 4, 207);
  f->known_diff = true;
  f->buggy[0].start = 4;  // what the library currently publishes
  f->buggy[0].length = 207;
  f->buggy_count = 1;
  n++;

#undef NEED
  return n;
}
