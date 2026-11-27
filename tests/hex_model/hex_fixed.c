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

#include "hex_fixed.h"

#include <string.h>

static void set_data(hm_case* c, const uint8_t* bytes, int len)
{
  memcpy(c->data, bytes, (size_t)len);
  c->data_len = len;
}

static void want(hm_fixed_case* f, int idx, int64_t off, int32_t len)
{
  f->want[idx].offset = off;
  f->want[idx].length = len;
  f->want_n = idx + 1;
}

static hm_node* leaf_byte(hm_arena* a, uint8_t v, uint8_t m)
{
  return hm_new_byte(a, v, m);
}

//
// Every expected pair below is computed by hand from the printed byte
// buffers; the driver additionally runs hm_expected over the same case and
// checks it reproduces this table, so the model itself is regression-tested
// rather than being treated as an oracle.
//
void hm_fixed_build(hm_arena* a, hm_fixed_case* out)
{
  hm_fixed_case* f;
  hm_node* root;
  hm_node* alt;
  hm_node* branch;

  // F01: two plain literal matches, overlapping-free, distinct starts.
  f = &out[0];
  root = hm_new_concat(a);
  hm_append(root, leaf_byte(a, 0x41, 0xFF));
  hm_append(root, leaf_byte(a, 0x42, 0xFF));
  f->c.root = root;
  uint8_t d01[] = {0x41, 0x42, 0x41, 0x42};
  set_data(&f->c, d01, (int)sizeof(d01));
  snprintf(f->c.description, sizeof(f->c.description), "F01-plain");
  want(f, 0, 0, 2);
  want(f, 1, 2, 2);
  f->expect_chain_parts_t2 = 0;
  f->expect_chain_parts_t200 = 0;

  // F02: half-nibble wildcard plus ??.
  f = &out[1];
  root = hm_new_concat(a);
  hm_append(root, leaf_byte(a, 0x40, 0xF0));  // 4?
  hm_append(root, leaf_byte(a, 0x00, 0x00));  // ??
  f->c.root = root;
  uint8_t d02[] = {0x41, 0x00, 0x4F, 0xFF, 0x30, 0x00, 0x40, 0x40};
  set_data(&f->c, d02, (int)sizeof(d02));
  snprintf(f->c.description, sizeof(f->c.description), "F02-wildcards");
  // 4? matches where the high nibble is 4: positions 0 (41), 2 (4F), 6 (40);
  // position 7 also starts with 4? but has no byte left for the trailing ??.
  want(f, 0, 0, 2);
  want(f, 1, 2, 2);
  want(f, 2, 6, 2);
  f->expect_chain_parts_t2 = 0;
  f->expect_chain_parts_t200 = 0;

  // F03: negated literal and negated half wildcard.
  f = &out[2];
  root = hm_new_concat(a);
  hm_append(root, hm_new_not(a, 0x41, 0xFF));  // ~41
  hm_append(root, hm_new_not(a, 0x00, 0x0F));  // ~?0
  f->c.root = root;
  uint8_t d03[] = {0x41, 0x01, 0x42, 0x00};
  set_data(&f->c, d03, (int)sizeof(d03));
  snprintf(f->c.description, sizeof(f->c.description), "F03-negation");
  want(f, 0, 1, 2);
  f->expect_chain_parts_t2 = 0;
  f->expect_chain_parts_t200 = 0;

  // F04: single-leaf alternation union followed by a literal.
  f = &out[3];
  root = hm_new_concat(a);
  alt = hm_new_alt(a);
  branch = hm_new_concat(a);
  hm_append(branch, leaf_byte(a, 0x41, 0xFF));
  hm_append(alt, branch);
  branch = hm_new_concat(a);
  hm_append(branch, leaf_byte(a, 0x42, 0xFF));
  hm_append(alt, branch);
  hm_append(root, alt);
  hm_append(root, leaf_byte(a, 0x43, 0xFF));
  f->c.root = root;
  uint8_t d04[] = {0x41, 0x43, 0x42, 0x43, 0x41, 0x44};
  set_data(&f->c, d04, (int)sizeof(d04));
  snprintf(f->c.description, sizeof(f->c.description), "F04-alt");
  want(f, 0, 0, 2);
  want(f, 1, 2, 2);
  f->expect_chain_parts_t2 = 0;
  f->expect_chain_parts_t200 = 0;

  // F05: equal-width two-leaf alternation branches plus trailing ??.
  f = &out[4];
  root = hm_new_concat(a);
  alt = hm_new_alt(a);
  branch = hm_new_concat(a);
  hm_append(branch, leaf_byte(a, 0x41, 0xFF));
  hm_append(branch, leaf_byte(a, 0x42, 0xFF));
  hm_append(alt, branch);
  branch = hm_new_concat(a);
  hm_append(branch, leaf_byte(a, 0x43, 0xFF));
  hm_append(branch, leaf_byte(a, 0x44, 0xFF));
  hm_append(alt, branch);
  hm_append(root, alt);
  hm_append(root, leaf_byte(a, 0x00, 0x00));
  f->c.root = root;
  uint8_t d05[] = {0x41, 0x42, 0x00, 0x43, 0x44, 0xFF, 0x41, 0x42, 0x43};
  set_data(&f->c, d05, (int)sizeof(d05));
  snprintf(f->c.description, sizeof(f->c.description), "F05-alt-wide");
  want(f, 0, 0, 3);
  want(f, 1, 3, 3);
  want(f, 2, 6, 3);
  f->expect_chain_parts_t2 = 0;
  f->expect_chain_parts_t200 = 0;

  // F06: ungreedy jump publishes the shortest derivation. The [1-3] range
  // splits into a chain under threshold 2 (end 3 > 2) but not under 200.
  f = &out[5];
  root = hm_new_concat(a);
  hm_append(root, leaf_byte(a, 0x41, 0xFF));
  hm_append(root, hm_new_jump(a, 1, 3));
  hm_append(root, leaf_byte(a, 0x42, 0xFF));
  f->c.root = root;
  uint8_t d06[] = {0x41, 0x00, 0x42, 0x42, 0x00, 0x00};
  set_data(&f->c, d06, (int)sizeof(d06));
  snprintf(f->c.description, sizeof(f->c.description), "F06-jump-ungreedy");
  want(f, 0, 0, 3);
  f->expect_chain_parts_t2 = 2;
  f->expect_chain_parts_t200 = 0;

  // F07: overlapping starts share tail bytes and report distinct lengths.
  // data = 41 41 42 00 42, jump [1-3]. Start 0 reaches the 42 at index 2
  // with gap 1 (length 3); start 1 cannot use gap 0, so it reaches the 42 at
  // index 4 with gap 2 (length 4). Both starts are kept.
  f = &out[6];
  root = hm_new_concat(a);
  hm_append(root, leaf_byte(a, 0x41, 0xFF));
  hm_append(root, hm_new_jump(a, 1, 3));
  hm_append(root, leaf_byte(a, 0x42, 0xFF));
  f->c.root = root;
  uint8_t d07[] = {0x41, 0x41, 0x42, 0x00, 0x42};
  set_data(&f->c, d07, (int)sizeof(d07));
  snprintf(f->c.description, sizeof(f->c.description), "F07-overlap");
  want(f, 0, 0, 3);
  want(f, 1, 1, 4);
  f->expect_chain_parts_t2 = 2;
  f->expect_chain_parts_t200 = 0;

  // F08: two candidate chain heads share one successor; one gap is just
  // below the legal range and only the other completes the chain.
  f = &out[7];
  root = hm_new_concat(a);
  hm_append(root, leaf_byte(a, 0x41, 0xFF));
  hm_append(root, leaf_byte(a, 0x42, 0xFF));
  hm_append(root, hm_new_jump(a, 199, 200));
  hm_append(root, leaf_byte(a, 0x43, 0xFF));
  hm_append(root, leaf_byte(a, 0x44, 0xFF));
  f->c.root = root;
  memset(f->c.data, 0, sizeof(f->c.data));
  f->c.data_len = 210;
  f->c.data[3] = 0x41;
  f->c.data[4] = 0x42;  // correct head, end=5, gap to tail = 200
  f->c.data[6] = 0x41;
  f->c.data[7] = 0x42;  // wrong head, end=8, gap to tail = 197 < 199
  f->c.data[205] = 0x43;
  f->c.data[206] = 0x44;  // shared successor
  snprintf(f->c.description, sizeof(f->c.description), "F08-shared-successor");
  want(f, 0, 3, 204);
  f->expect_chain_parts_t2 = 2;
  f->expect_chain_parts_t200 = 0;

  // F09: masked negation + alternation + long jump, match ends on the last
  // byte. Filler 0x1F fails both alternation branches, so only the planted
  // head can reach the single tail.
  f = &out[8];
  root = hm_new_concat(a);
  alt = hm_new_alt(a);
  branch = hm_new_concat(a);
  hm_append(branch, hm_new_not(a, 0x0F, 0x0F));  // ~?F
  hm_append(alt, branch);
  branch = hm_new_concat(a);
  hm_append(branch, leaf_byte(a, 0x41, 0xFF));
  hm_append(alt, branch);
  hm_append(root, alt);
  hm_append(root, hm_new_jump(a, 199, 200));
  hm_append(root, leaf_byte(a, 0x42, 0xFF));
  f->c.root = root;
  memset(f->c.data, 0x1F, sizeof(f->c.data));
  f->c.data_len = 204;
  f->c.data[0] = 0x41;   // branch-2 head, no reachable tail
  f->c.data[2] = 0x30;   // ~?F satisfied (low nibble 0 != F), valid head
  f->c.data[203] = 0x42; // last byte; head end=3, gap to tail = 200
  snprintf(f->c.description, sizeof(f->c.description), "F09-maskneg-alt-chain");
  want(f, 0, 2, 202);
  f->expect_chain_parts_t2 = 2;
  f->expect_chain_parts_t200 = 0;

  // F10: endpoint 202 forces a split even under the default threshold.
  f = &out[9];
  root = hm_new_concat(a);
  hm_append(root, leaf_byte(a, 0x41, 0xFF));
  hm_append(root, hm_new_jump(a, 201, 202));
  hm_append(root, leaf_byte(a, 0x42, 0xFF));
  f->c.root = root;
  memset(f->c.data, 0, sizeof(f->c.data));
  f->c.data_len = 204;
  f->c.data[0] = 0x41;
  f->c.data[203] = 0x42;  // head end=1, gap = 202, last byte
  snprintf(f->c.description, sizeof(f->c.description), "F10-boundary-202");
  want(f, 0, 0, 204);
  f->expect_chain_parts_t2 = 2;
  f->expect_chain_parts_t200 = 2;

  // F11: one head, tails planted at gaps 199/200/201/202; the shortest legal
  // gap (199) determines the published non-greedy length.
  f = &out[10];
  root = hm_new_concat(a);
  hm_append(root, leaf_byte(a, 0x41, 0xFF));
  hm_append(root, hm_new_jump(a, 199, 202));
  hm_append(root, leaf_byte(a, 0x42, 0xFF));
  f->c.root = root;
  memset(f->c.data, 0, sizeof(f->c.data));
  f->c.data_len = 204;
  f->c.data[0] = 0x41;
  f->c.data[200] = 0x42;  // gap 199 (head end=1)
  f->c.data[201] = 0x42;  // gap 200
  f->c.data[202] = 0x42;  // gap 201
  f->c.data[203] = 0x42;  // gap 202
  snprintf(f->c.description, sizeof(f->c.description), "F11-near-boundaries");
  want(f, 0, 0, 201);
  f->expect_chain_parts_t2 = 2;
  f->expect_chain_parts_t200 = 2;

  // F12: zero-width jump rendered as [0-0], which the grammar accepts while
  // the single-number spelling [0] is illegal. Behaves as direct adjacency.
  f = &out[11];
  root = hm_new_concat(a);
  hm_append(root, leaf_byte(a, 0x41, 0xFF));
  hm_append(root, hm_new_jump(a, 0, 0));
  hm_append(root, leaf_byte(a, 0x42, 0xFF));
  f->c.root = root;
  uint8_t d12[] = {0x41, 0x42, 0x41, 0x42, 0x41};
  set_data(&f->c, d12, (int)sizeof(d12));
  snprintf(f->c.description, sizeof(f->c.description), "F12-zero-jump");
  want(f, 0, 0, 2);
  want(f, 1, 2, 2);
  f->expect_chain_parts_t2 = 0;
  f->expect_chain_parts_t200 = 0;

  // F13: minimal regression for a real library limitation found by this
  // harness. Pattern AA [0-2] BB [2-5] CC; at threshold 2 the [2-5] jump
  // splits the string into a chain. The single head AA@1 reaches a short
  // middle BB@3 (gap 1) that cannot reach the only CC@10, and a long middle
  // BB@4 (gap 2) that can (gap 5). The short middle match is inserted first
  // and, when negatively confirmed, its removal also drops the head that the
  // long match still needs, so the split chain publishes nothing. The
  // unsplit RE engine (threshold 200) and the independent model both report
  // the single full match. The model is intentionally NOT adjusted; this
  // documents the divergence and fails loudly if the behaviour changes.
  f = &out[12];
  root = hm_new_concat(a);
  hm_append(root, leaf_byte(a, 0xAA, 0xFF));
  hm_append(root, hm_new_jump(a, 0, 2));
  hm_append(root, leaf_byte(a, 0xBB, 0xFF));
  hm_append(root, hm_new_jump(a, 2, 5));
  hm_append(root, leaf_byte(a, 0xCC, 0xFF));
  f->c.root = root;
  uint8_t d13[] = {0x11, 0xAA, 0x11, 0xBB, 0xBB,
                   0x11, 0x11, 0x11, 0x11, 0x11, 0xCC};
  set_data(&f->c, d13, (int)sizeof(d13));
  snprintf(f->c.description, sizeof(f->c.description),
           "F13-chain-shadow-known-divergence");
  want(f, 0, 1, 10);  // AA@1, long middle BB@4, gap 5, CC@10
  f->expect_chain_parts_t2 = 2;
  f->expect_chain_parts_t200 = 0;
  f->known_divergence_t2 = true;
}

int hm_fixed_rejection_checks(void)
{
  return 8;
}
