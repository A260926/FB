/*
 * Fixed, hand-computable regression cases. Every fixed case stores both the
 * independently derived oracle matches and a hand-written expected list, so
 * the oracle is itself checked against manual calculation before any scan.
 */

#ifndef HEX_CASES_H
#define HEX_CASES_H

#include "hex_model.h"

#define HM_FIXED_HAND_MAX 16

typedef struct {
  const char* name;
  hm_case c;
  hm_match hand[HM_FIXED_HAND_MAX];
  int hand_count;
  // When true this is the pinned regression for the documented scanner
  // defect. hand[] still holds the correct oracle answer. buggy[] holds the
  // matches the library currently publishes while the defect is present.
  bool known_diff;
  hm_match buggy[HM_FIXED_HAND_MAX];
  int buggy_count;
} hm_fixed;

// Builds all fixed cases into out[0..cap-1]. Returns the number written.
int hm_build_fixed(hm_fixed* out, int cap);

#endif  // HEX_CASES_H
