// Copyright 2026 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests for the table header association API.  Expected relationships are
// written out independently of the implementation under test.

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gumbo.h"
#include "table_headers.h"

static int g_cases = 0;

static bool is_el(const GumboNode* node, GumboTag tag) {
  return node != NULL && node->type == GUMBO_NODE_ELEMENT &&
         node->v.element.tag == tag;
}

static void collect_tables(const GumboNode* node, const GumboNode** out,
                           size_t* count, size_t cap) {
  if (node->type != GUMBO_NODE_ELEMENT &&
      node->type != GUMBO_NODE_TEMPLATE) {
    return;
  }
  if (node->v.element.tag == GUMBO_TAG_TABLE) {
    assert(*count < cap);
    out[(*count)++] = node;
  }
  const GumboVector* children = &node->v.element.children;
  for (unsigned int i = 0; i < children->length; ++i) {
    collect_tables(children->data[i], out, count, cap);
  }
}

// DFS within |scope| that never descends into a nested table.
static const GumboNode* find_id(const GumboNode* scope, const char* id) {
  if (scope->type == GUMBO_NODE_ELEMENT ||
      scope->type == GUMBO_NODE_TEMPLATE) {
    const GumboAttribute* attr =
        gumbo_get_attribute(&scope->v.element.attributes, "id");
    if (attr != NULL && strcmp(attr->value, id) == 0) return scope;
    const GumboVector* children = &scope->v.element.children;
    for (unsigned int i = 0; i < children->length; ++i) {
      const GumboNode* child = children->data[i];
      if (is_el(child, GUMBO_TAG_TABLE) && child != scope) continue;
      const GumboNode* found = find_id(child, id);
      if (found != NULL) return found;
    }
  }
  return NULL;
}

// Collects the source th/td cells of |table| in DOM order, skipping nested
// tables.  This is the independent reference list for entry ordering.
static void collect_cells(const GumboNode* node, const GumboNode* table,
                          const GumboNode** out, size_t* count, size_t cap) {
  if (node->type == GUMBO_NODE_ELEMENT ||
      node->type == GUMBO_NODE_TEMPLATE) {
    if (node != table && node->v.element.tag == GUMBO_TAG_TABLE) return;
    if (node->v.element.tag == GUMBO_TAG_TH ||
        node->v.element.tag == GUMBO_TAG_TD) {
      assert(*count < cap);
      out[(*count)++] = node;
    }
    const GumboVector* children = &node->v.element.children;
    for (unsigned int i = 0; i < children->length; ++i) {
      collect_cells(children->data[i], table, out, count, cap);
    }
  }
}

static const char* first_text(const GumboNode* node) {
  if (node->type == GUMBO_NODE_TEXT || node->type == GUMBO_NODE_WHITESPACE ||
      node->type == GUMBO_NODE_CDATA) {
    return node->v.text.text;
  }
  if (node->type != GUMBO_NODE_ELEMENT &&
      node->type != GUMBO_NODE_TEMPLATE) {
    return "";
  }
  const GumboVector* children = &node->v.element.children;
  for (unsigned int i = 0; i < children->length; ++i) {
    const char* text = first_text(children->data[i]);
    if (text[0] != '\0') return text;
  }
  return "";
}

static const GumboTableHeaderEntry* entry_for(const GumboTableHeaders* a,
                                              const GumboNode* cell) {
  for (size_t i = 0; i < a->cell_count; ++i) {
    if (a->cells[i].cell == cell) return &a->cells[i];
  }
  return NULL;
}

// Checks the exact header set, both by id and by node identity; the expected
// id list is written in the required DOM (tree) order.
static void expect_headers(const GumboTableHeaders* a, const GumboNode* table,
                           const GumboNode* cell,
                           const char* const* expected_ids,
                           size_t expected_count) {
  const GumboTableHeaderEntry* entry = entry_for(a, cell);
  assert(entry != NULL);
  assert(entry->header_count == expected_count);
  size_t previous_index = 0;
  const GumboNode* reference_cells[256];
  size_t reference_count = 0;
  collect_cells(table, table, reference_cells, &reference_count, 256);
  for (size_t i = 0; i < expected_count; ++i) {
    const GumboNode* header = entry->headers[i];
    assert(is_el(header, GUMBO_TAG_TH));
    const GumboAttribute* id =
        gumbo_get_attribute(&header->v.element.attributes, "id");
    assert(id != NULL);
    assert(strcmp(id->value, expected_ids[i]) == 0);
    assert(header == find_id(table, expected_ids[i]));
    // Stable tree-order output: each header's DOM index must increase.
    size_t index = 0;
    for (; index < reference_count; ++index) {
      if (reference_cells[index] == header) break;
    }
    assert(index < reference_count);
    if (i > 0) assert(index > previous_index);
    previous_index = index;
  }
}

static const char* cell_id(const GumboNode* cell) {
  const GumboAttribute* id =
      gumbo_get_attribute(&cell->v.element.attributes, "id");
  return id == NULL ? "?" : id->value;
}

static void expect_no_headers(const GumboTableHeaders* a,
                              const GumboNode* cell) {
  const GumboTableHeaderEntry* entry = entry_for(a, cell);
  assert(entry != NULL);
  if (entry->header_count != 0) {
    fprintf(stderr, "unexpected %zu headers for cell %s:\n",
            entry->header_count, cell_id(cell));
    for (size_t k = 0; k < entry->header_count; ++k) {
      fprintf(stderr, "  -> %s\n", first_text(entry->headers[k]));
    }
    abort();
  }
  assert(entry->headers == NULL);
}

// Scenario 1: ordinary two-column table, the positive control case.
static void test_basic_two_columns(void) {
  const char* html =
      "<table>"
      "<tr><th id='h1'>Name</th><th id='h2'>Score</th></tr>"
      "<tr><td id='r1c1'>A</td><td id='r1c2'>1</td></tr>"
      "<tr><td id='r2c1'>B</td><td id='r2c2'>2</td></tr>"
      "</table>";
  GumboOutput* output = gumbo_parse(html);
  const GumboNode* tables[4];
  size_t table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  assert(table_count == 1);
  const GumboNode* table = tables[0];

  // Non-table and NULL inputs fail rather than crash.
  assert(gumbo_table_headers_build(NULL) == NULL);
  assert(gumbo_table_headers_build(output->root) == NULL);

  GumboTableHeaders* assoc = gumbo_table_headers_build(table);
  assert(assoc != NULL);
  assert(assoc->cell_count == 6);

  const GumboNode* expected_cells[6];
  size_t expected_count = 0;
  collect_cells(table, table, expected_cells, &expected_count, 6);
  assert(expected_count == 6);
  for (size_t i = 0; i < 6; ++i) {
    assert(assoc->cells[i].cell == expected_cells[i]);
  }

  const char* h1[] = {"h1"};
  const char* h2[] = {"h2"};
  expect_headers(assoc, table, find_id(table, "r1c1"), h1, 1);
  expect_headers(assoc, table, find_id(table, "r1c2"), h2, 1);
  expect_headers(assoc, table, find_id(table, "r2c1"), h1, 1);
  expect_headers(assoc, table, find_id(table, "r2c2"), h2, 1);
  // The first header cell has nothing above or to its left.  The second one
  // picks up the first one through the spec's horizontal scan.
  expect_no_headers(assoc, find_id(table, "h1"));
  expect_headers(assoc, table, find_id(table, "h2"), h1, 1);

  gumbo_table_headers_destroy(assoc);
  gumbo_destroy_output(&kGumboDefaultOptions, output);
}

// Scenario 2: explicit headers attribute overrides scope-based association;
// unknown tokens and self references are ignored.
static void test_explicit_headers_overrides_scope(void) {
  const char* html =
      "<table>"
      "<tr><th id='a' scope='col'>A</th><th id='b' scope='col'>B</th></tr>"
      "<tr><th id='r' scope='row'>R</th>"
      "<td id='x' headers='b missing x'>x</td></tr>"
      "</table>";
  GumboOutput* output = gumbo_parse(html);
  const GumboNode* tables[4];
  size_t table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  GumboTableHeaders* assoc = gumbo_table_headers_build(tables[0]);
  assert(assoc != NULL);
  const char* expected[] = {"b"};
  expect_headers(assoc, tables[0], find_id(tables[0], "x"), expected, 1);
  gumbo_table_headers_destroy(assoc);
  gumbo_destroy_output(&kGumboDefaultOptions, output);
}

// Scenario 3: an empty (or whitespace-only) headers attribute means an empty
// set; automatic inference must not kick back in.
static void test_empty_headers_attribute(void) {
  const char* html =
      "<table>"
      "<tr><th id='e1'>E1</th><th id='e2'>E2</th></tr>"
      "<tr><td id='empty' headers=''>v</td>"
      "<td id='spaces' headers='   '>w</td></tr>"
      "</table>";
  GumboOutput* output = gumbo_parse(html);
  const GumboNode* tables[4];
  size_t table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  GumboTableHeaders* assoc = gumbo_table_headers_build(tables[0]);
  assert(assoc != NULL);
  expect_no_headers(assoc, find_id(tables[0], "empty"));
  expect_no_headers(assoc, find_id(tables[0], "spaces"));
  gumbo_table_headers_destroy(assoc);
  gumbo_destroy_output(&kGumboDefaultOptions, output);
}

// Scenario 4: header cells with identical text are distinct nodes and are
// associated independently.
static void test_duplicate_header_text(void) {
  const char* html =
      "<table>"
      "<tr><th id='x1'>Total</th><th id='x2'>Total</th></tr>"
      "<tr><td id='t1'>1</td><td id='t2'>2</td></tr>"
      "</table>";
  GumboOutput* output = gumbo_parse(html);
  const GumboNode* tables[4];
  size_t table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  const GumboNode* table = tables[0];
  GumboTableHeaders* assoc = gumbo_table_headers_build(table);
  assert(assoc != NULL);
  const GumboNode* first = find_id(table, "x1");
  const GumboNode* second = find_id(table, "x2");
  assert(first != second);
  assert(strcmp(first_text(first), "Total") == 0);
  assert(strcmp(first_text(second), "Total") == 0);
  const char* want_first[] = {"x1"};
  const char* want_second[] = {"x2"};
  expect_headers(assoc, table, find_id(table, "t1"), want_first, 1);
  expect_headers(assoc, table, find_id(table, "t2"), want_second, 1);
  gumbo_table_headers_destroy(assoc);
  gumbo_destroy_output(&kGumboDefaultOptions, output);
}

// Scenario 5: opaque row groups block headers from earlier groups once an
// intervening row group covers the principal cell's columns.
static void test_cross_group_headers(void) {
  const char* html =
      "<table>"
      "<thead><tr><th id='h'>C1</th><th id='h2'>C2</th></tr></thead>"
      "<tbody>"
      "<tr><td id='c1'>1</td><td id='c2'>2</td></tr>"
      "</tbody>"
      "<tbody>"
      "<tr><td id='c3'>3</td><td id='c4'>4</td></tr>"
      "</tbody>"
      "</table>";
  GumboOutput* output = gumbo_parse(html);
  const GumboNode* tables[4];
  size_t table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  const GumboNode* table = tables[0];
  GumboTableHeaders* assoc = gumbo_table_headers_build(table);
  assert(assoc != NULL);
  const char* h[] = {"h"};
  const char* h2[] = {"h2"};
  // First body row still reaches the thead headers through vertical scan.
  expect_headers(assoc, table, find_id(table, "c1"), h, 1);
  expect_headers(assoc, table, find_id(table, "c2"), h2, 1);
  // Second tbody: the first tbody's data cells block the thead headers.
  expect_no_headers(assoc, find_id(table, "c3"));
  expect_no_headers(assoc, find_id(table, "c4"));
  gumbo_table_headers_destroy(assoc);
  gumbo_destroy_output(&kGumboDefaultOptions, output);
}

// Scenario 6: a principal cell spanning multiple rows and columns scans every
// covered line, not just its top-left slot.
static void test_spans_multiple_scan_lines(void) {
  const char* html =
      "<table>"
      "<tr><th id='top'>T</th><th id='u'>U</th>"
      "<td id='big' rowspan='2' colspan='2'>big</td></tr>"
      "<tr><th id='side'>S</th><th id='side2'>S2</th></tr>"
      "</table>";
  GumboOutput* output = gumbo_parse(html);
  const GumboNode* tables[4];
  size_t table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  const GumboNode* table = tables[0];
  GumboTableHeaders* assoc = gumbo_table_headers_build(table);
  assert(assoc != NULL);
  const char* expected[] = {"top", "u", "side", "side2"};
  expect_headers(assoc, table, find_id(table, "big"), expected, 4);
  gumbo_table_headers_destroy(assoc);
  gumbo_destroy_output(&kGumboDefaultOptions, output);
}

// Scenario 7: a data cell splits the automatic header row into two blocks;
// it only blocks the scan line of its own column.
static void test_data_cell_separates_header_blocks(void) {
  const char* html =
      "<table>"
      "<tr><th id='k1'>K1</th><td id='sep'>sep</td><th id='k2'>K2</th></tr>"
      "<tr><td id='target' colspan='3'>x</td></tr>"
      "</table>";
  GumboOutput* output = gumbo_parse(html);
  const GumboNode* tables[4];
  size_t table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  const GumboNode* table = tables[0];
  GumboTableHeaders* assoc = gumbo_table_headers_build(table);
  assert(assoc != NULL);
  const char* expected[] = {"k1", "k2"};
  expect_headers(assoc, table, find_id(table, "target"), expected, 2);
  gumbo_table_headers_destroy(assoc);
  gumbo_destroy_output(&kGumboDefaultOptions, output);
}

// Scenario 8: a nested table reuses an ID; it must not leak into the outer
// table's ID resolution, and the inner table is built on its own.
static void test_nested_table_same_id(void) {
  const char* html =
      "<table>"
      "<tr><th id='dup'>Outer</th><td id='oc' headers='dup'>o</td></tr>"
      "<tr><td colspan='2'>"
      "<table>"
      "<tr><th id='dup'>Inner</th><td id='ic' headers='dup'>i</td></tr>"
      "</table>"
      "</td></tr>"
      "</table>";
  GumboOutput* output = gumbo_parse(html);
  const GumboNode* tables[4];
  size_t table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  assert(table_count == 2);
  const GumboNode* outer = tables[0];
  const GumboNode* inner = tables[1];

  GumboTableHeaders* outer_assoc = gumbo_table_headers_build(outer);
  GumboTableHeaders* inner_assoc = gumbo_table_headers_build(inner);
  assert(outer_assoc != NULL && inner_assoc != NULL);
  // Nested cells, rows and IDs do not contribute to the outer table.
  assert(outer_assoc->cell_count == 3);
  assert(inner_assoc->cell_count == 2);

  const GumboNode* outer_header = find_id(outer, "dup");
  const char* want_outer[] = {"dup"};
  expect_headers(outer_assoc, outer, find_id(outer, "oc"), want_outer, 1);
  const GumboTableHeaderEntry* outer_entry = entry_for(
      outer_assoc, find_id(outer, "oc"));
  assert(outer_entry->headers[0] == outer_header);

  const GumboNode* inner_header = find_id(inner, "dup");
  assert(inner_header != outer_header);
  const GumboTableHeaderEntry* inner_entry = entry_for(
      inner_assoc, find_id(inner, "ic"));
  assert(inner_entry != NULL);
  assert(inner_entry->header_count == 1);
  assert(inner_entry->headers[0] == inner_header);

  gumbo_table_headers_destroy(outer_assoc);
  gumbo_table_headers_destroy(inner_assoc);
  gumbo_destroy_output(&kGumboDefaultOptions, output);
}

// Scenario 9: overlapping slots are a table model error; the whole build must
// fail instead of returning a partial association.
static void test_model_conflict_returns_null(void) {
  const char* html =
      "<table>"
      "<tr><td>a</td><td id='b' rowspan='2'>b</td></tr>"
      "<tr><td colspan='2'>c</td></tr>"
      "</table>";
  GumboOutput* output = gumbo_parse(html);
  const GumboNode* tables[4];
  size_t table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  GumboTableHeaders* assoc = gumbo_table_headers_build(tables[0]);
  assert(assoc == NULL);
  gumbo_destroy_output(&kGumboDefaultOptions, output);
}

// Scenario 10: rowspan="0" reaches the last row of its row group.
static void test_rowspan_zero(void) {
  const char* html =
      "<table>"
      "<tbody>"
      "<tr><th id='z' rowspan='0'>Z</th><td id='d1'>1</td></tr>"
      "<tr><td id='d2'>2</td></tr>"
      "<tr><td id='d3'>3</td></tr>"
      "</tbody>"
      "</table>";
  GumboOutput* output = gumbo_parse(html);
  const GumboNode* tables[4];
  size_t table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  const GumboNode* table = tables[0];
  GumboTableHeaders* assoc = gumbo_table_headers_build(table);
  assert(assoc != NULL);
  const char* expected[] = {"z"};
  expect_headers(assoc, table, find_id(table, "d1"), expected, 1);
  expect_headers(assoc, table, find_id(table, "d2"), expected, 1);
  expect_headers(assoc, table, find_id(table, "d3"), expected, 1);
  gumbo_table_headers_destroy(assoc);
  gumbo_destroy_output(&kGumboDefaultOptions, output);
}

// Scenario 11: blank header cells (whitespace-only content) are removed from
// every computed header set.
static void test_blank_headers_removed(void) {
  const char* html =
      "<table>"
      "<tr><th id='blank'>   </th><th id='real'>R</th></tr>"
      "<tr><td id='x'>x</td><td id='y'>y</td></tr>"
      "</table>";
  GumboOutput* output = gumbo_parse(html);
  const GumboNode* tables[4];
  size_t table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  const GumboNode* table = tables[0];
  GumboTableHeaders* assoc = gumbo_table_headers_build(table);
  assert(assoc != NULL);
  expect_no_headers(assoc, find_id(table, "x"));
  const char* expected[] = {"real"};
  expect_headers(assoc, table, find_id(table, "y"), expected, 1);
  gumbo_table_headers_destroy(assoc);
  gumbo_destroy_output(&kGumboDefaultOptions, output);
}

// Scenario 12: the model orders rows thead -> tbody -> tfoot regardless of
// source order, while entries stay in source DOM order.
static void test_row_group_reordering(void) {
  const char* html =
      "<table>"
      "<tfoot><tr><td id='f'>foot</td></tr></tfoot>"
      "<thead><tr><th id='th1'>H</th></tr></thead>"
      "<tbody><tr><td id='b'>body</td></tr></tbody>"
      "</table>";
  GumboOutput* output = gumbo_parse(html);
  const GumboNode* tables[4];
  size_t table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  const GumboNode* table = tables[0];
  GumboTableHeaders* assoc = gumbo_table_headers_build(table);
  assert(assoc != NULL);
  const char* want_th[] = {"th1"};
  expect_headers(assoc, table, find_id(table, "b"), want_th, 1);
  // The tfoot cell is modelled after the tbody cell, whose data cell blocks
  // the upward scan.
  expect_no_headers(assoc, find_id(table, "f"));
  // Entry ordering follows the DOM, where the tfoot cell appears first.
  assert(assoc->cell_count == 3);
  assert(assoc->cells[0].cell == find_id(table, "f"));
  assert(assoc->cells[1].cell == find_id(table, "th1"));
  assert(assoc->cells[2].cell == find_id(table, "b"));
  gumbo_table_headers_destroy(assoc);
  gumbo_destroy_output(&kGumboDefaultOptions, output);
}

// Scenario 13: rowspan="0" ends at its own row group; it never covers the
// next group (a wrong extension to the table end would overlap and fail).
static void test_rowspan_zero_stops_at_group(void) {
  const char* html =
      "<table>"
      "<tbody>"
      "<tr><th id='z' rowspan='0'>Z</th><td id='d1'>1</td></tr>"
      "</tbody>"
      "<tbody>"
      "<tr><td id='spacer'>s</td><td id='d2'>2</td></tr>"
      "</tbody>"
      "</table>";
  GumboOutput* output = gumbo_parse(html);
  const GumboNode* tables[4];
  size_t table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  const GumboNode* table = tables[0];
  GumboTableHeaders* assoc = gumbo_table_headers_build(table);
  assert(assoc != NULL);
  // A wrong extension of rowspan="0" to the table end would overlap spacer
  // and make the whole build fail; success proves the group boundary held.
  const char* want_z[] = {"z"};
  expect_headers(assoc, table, find_id(table, "spacer"), want_z, 1);
  expect_no_headers(assoc, find_id(table, "d2"));
  gumbo_table_headers_destroy(assoc);
  gumbo_destroy_output(&kGumboDefaultOptions, output);
}

// Scenario 14: an opaque header block (a th of the exact same rectangle
// immediately to the left) is added and stops the horizontal scan, so the
// header further left is not reached.
static void test_opaque_header_block(void) {
  const char* html =
      "<table>"
      "<tr><th id='e'>E</th><th id='d' colspan='2'>D</th>"
      "<td id='x' colspan='2'>x</td></tr>"
      "</table>";
  GumboOutput* output = gumbo_parse(html);
  const GumboNode* tables[4];
  size_t table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  const GumboNode* table = tables[0];
  GumboTableHeaders* assoc = gumbo_table_headers_build(table);
  assert(assoc != NULL);
  const char* expected[] = {"d"};
  expect_headers(assoc, table, find_id(table, "x"), expected, 1);
  gumbo_table_headers_destroy(assoc);
  gumbo_destroy_output(&kGumboDefaultOptions, output);
}

// Scenario 15: size caps reject oversized models, including boundary checks;
// the limits are enforced before the large allocations.
static void test_size_caps(void) {
  enum { CAP = 4096 };
  char* html = malloc((size_t)(CAP + 8) * 32);
  assert(html != NULL);

  // 4097 rows exceeds the row cap.
  size_t pos = 0;
  pos += (size_t)sprintf(html + pos, "<table>");
  for (int i = 0; i < CAP + 1; ++i) {
    pos += (size_t)sprintf(html + pos, "<tr><td>x</td></tr>");
  }
  pos += (size_t)sprintf(html + pos, "</table>");
  GumboOutput* output = gumbo_parse(html);
  const GumboNode* tables[4];
  size_t table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  assert(gumbo_table_headers_build(tables[0]) == NULL);
  gumbo_destroy_output(&kGumboDefaultOptions, output);

  // Exactly 4096 rows is accepted.
  pos = 0;
  pos += (size_t)sprintf(html + pos, "<table>");
  for (int i = 0; i < CAP; ++i) {
    pos += (size_t)sprintf(html + pos, "<tr><td>x</td></tr>");
  }
  pos += (size_t)sprintf(html + pos, "</table>");
  output = gumbo_parse(html);
  table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  GumboTableHeaders* assoc = gumbo_table_headers_build(tables[0]);
  assert(assoc != NULL);
  assert(assoc->cell_count == CAP);
  gumbo_table_headers_destroy(assoc);
  gumbo_destroy_output(&kGumboDefaultOptions, output);

  // 4097 cells in one row exceeds the column cap (colspan itself is clamped
  // to 1000 by the spec, so the cap is exercised with plain cells).
  pos = 0;
  pos += (size_t)sprintf(html + pos, "<table><tr>");
  for (int i = 0; i < CAP + 1; ++i) {
    pos += (size_t)sprintf(html + pos, "<td>x</td>");
  }
  pos += (size_t)sprintf(html + pos, "</tr></table>");
  output = gumbo_parse(html);
  table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  assert(gumbo_table_headers_build(tables[0]) == NULL);
  gumbo_destroy_output(&kGumboDefaultOptions, output);

  // 1001 rows x 1000 columns exceeds the one-million-slot cap.
  pos = 0;
  pos += (size_t)sprintf(html + pos, "<table>");
  for (int i = 0; i < 1001; ++i) {
    pos += (size_t)sprintf(html + pos, "<tr><td colspan='1000'>x</td></tr>");
  }
  pos += (size_t)sprintf(html + pos, "</table>");
  output = gumbo_parse(html);
  table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  assert(gumbo_table_headers_build(tables[0]) == NULL);
  gumbo_destroy_output(&kGumboDefaultOptions, output);

  free(html);
}

// Scenario 16: duplicate IDs resolve to the first th in DOM order, and a
// referenced header's own headers attribute is not expanded.
static void test_duplicate_id_and_no_recursion(void) {
  const char* html =
      "<table><tr>"
      "<th id='dup' headers='chain'>First</th>"
      "<th id='chain'>Chain</th>"
      "<th id='dup'>Second</th>"
      "<td id='t' headers='dup'>t</td>"
      "</tr></table>";
  GumboOutput* output = gumbo_parse(html);
  const GumboNode* tables[4];
  size_t table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  const GumboNode* table = tables[0];
  GumboTableHeaders* assoc = gumbo_table_headers_build(table);
  assert(assoc != NULL);
  const GumboTableHeaderEntry* entry = entry_for(assoc, find_id(table, "t"));
  assert(entry != NULL);
  assert(entry->header_count == 1);
  assert(entry->headers[0] == find_id(table, "dup"));
  assert(strcmp(first_text(entry->headers[0]), "First") == 0);
  gumbo_table_headers_destroy(assoc);
  gumbo_destroy_output(&kGumboDefaultOptions, output);
}

// Scenario 17: explicit tokens pointing at blank headers or at non-th
// elements are ignored.
static void test_explicit_invalid_tokens(void) {
  const char* html =
      "<table><tr>"
      "<th id='bh'>   </th>"
      "<td id='d'>d</td>"
      "<td id='t' headers='bh d missing'>t</td>"
      "</tr></table>";
  GumboOutput* output = gumbo_parse(html);
  const GumboNode* tables[4];
  size_t table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  const GumboNode* table = tables[0];
  GumboTableHeaders* assoc = gumbo_table_headers_build(table);
  assert(assoc != NULL);
  expect_no_headers(assoc, find_id(table, "t"));
  gumbo_table_headers_destroy(assoc);
  gumbo_destroy_output(&kGumboDefaultOptions, output);
}

// Scenario 18: the column group rule keeps scanning past a blocking data
// cell, reaching a header that the plain horizontal scan cannot.
static void test_colgroup_association(void) {
  const char* html =
      "<table>"
      "<colgroup span='4'></colgroup>"
      "<tr><th id='gh'>GH</th><td id='sep'>sep</td>"
      "<th id='m'>M</th><td id='t'>t</td></tr>"
      "</table>";
  GumboOutput* output = gumbo_parse(html);
  const GumboNode* tables[4];
  size_t table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  const GumboNode* table = tables[0];
  GumboTableHeaders* assoc = gumbo_table_headers_build(table);
  assert(assoc != NULL);
  // Horizontal scan alone stops at sep and would only return m; the column
  // group scan continues past sep to gh.
  const char* expected_t[] = {"gh", "m"};
  expect_headers(assoc, table, find_id(table, "t"), expected_t, 2);
  gumbo_table_headers_destroy(assoc);
  gumbo_destroy_output(&kGumboDefaultOptions, output);
}

// Scenario 19: the same header reached through several rules is emitted once,
// deduplicated by node identity.
static void test_identity_dedup(void) {
  const char* html =
      "<table><tbody>"
      "<tr><th id='rh'>H</th><td id='a'>a</td></tr>"
      "<tr><td id='b'>b</td><td id='c'>c</td></tr>"
      "</tbody></table>";
  GumboOutput* output = gumbo_parse(html);
  const GumboNode* tables[4];
  size_t table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  const GumboNode* table = tables[0];
  GumboTableHeaders* assoc = gumbo_table_headers_build(table);
  assert(assoc != NULL);
  const char* expected[] = {"rh"};
  // Vertical scan and the row-group rule both reach rh; it appears once.
  expect_headers(assoc, table, find_id(table, "b"), expected, 1);
  gumbo_table_headers_destroy(assoc);
  gumbo_destroy_output(&kGumboDefaultOptions, output);
}

// Scenario 20: a table with no cells still builds an empty result.
static void test_empty_table(void) {
  GumboOutput* output = gumbo_parse("<table></table>");
  const GumboNode* tables[4];
  size_t table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  GumboTableHeaders* assoc = gumbo_table_headers_build(tables[0]);
  assert(assoc != NULL);
  assert(assoc->cell_count == 0);
  assert(assoc->cells != NULL);
  gumbo_table_headers_destroy(assoc);
  gumbo_destroy_output(&kGumboDefaultOptions, output);
}

// Scenario 21: explicit headers work on a th principal cell too, and override
// what automatic inference would produce.
static void test_explicit_headers_on_header_cell(void) {
  const char* html =
      "<table><tr>"
      "<th id='p' headers='q'>P</th>"
      "<th id='q'>Q</th>"
      "</tr></table>";
  GumboOutput* output = gumbo_parse(html);
  const GumboNode* tables[4];
  size_t table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  const GumboNode* table = tables[0];
  GumboTableHeaders* assoc = gumbo_table_headers_build(table);
  assert(assoc != NULL);
  const char* expected[] = {"q"};
  expect_headers(assoc, table, find_id(table, "p"), expected, 1);
  gumbo_table_headers_destroy(assoc);
  gumbo_destroy_output(&kGumboDefaultOptions, output);
}

// Scenario 22: the row group rule reaches a wide header even when the vertical
// scan line is blocked by an intervening data cell; coverage of the column is
// what matters, not ending exactly on it.
static void test_rowgroup_wide_header_coverage(void) {
  const char* html =
      "<table><tbody>"
      "<tr><th id='w' colspan='3'>W</th></tr>"
      "<tr><td>blocker</td><td>x</td><td>y</td></tr>"
      "<tr><td id='t'>t</td><td>u</td><td>v</td></tr>"
      "</tbody></table>";
  GumboOutput* output = gumbo_parse(html);
  const GumboNode* tables[4];
  size_t table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  const GumboNode* table = tables[0];
  GumboTableHeaders* assoc = gumbo_table_headers_build(table);
  assert(assoc != NULL);
  const char* expected[] = {"w"};
  expect_headers(assoc, table, find_id(table, "t"), expected, 1);
  gumbo_table_headers_destroy(assoc);
  gumbo_destroy_output(&kGumboDefaultOptions, output);
}

// Scenario 23: in the row group association step, a data cell in an upper row
// discards that row's contribution even though a header exists further up.
static void test_rowgroup_step_data_cell_block(void) {
  const char* html =
      "<table><tbody>"
      "<tr><th id='a'>A</th><td>sep</td></tr>"
      "<tr><th id='b'>B</th><td id='t'>t</td></tr>"
      "</tbody></table>";
  GumboOutput* output = gumbo_parse(html);
  const GumboNode* tables[4];
  size_t table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  const GumboNode* table = tables[0];
  GumboTableHeaders* assoc = gumbo_table_headers_build(table);
  assert(assoc != NULL);
  // The horizontal scan still reaches B in the same row; A above is blocked
  // both vertically and in the row group step by the intervening sep cell.
  const char* expected_t[] = {"b"};
  expect_headers(assoc, table, find_id(table, "t"), expected_t, 1);
  // The lower row header B still reaches A through its own vertical scan.
  const char* expected[] = {"a"};
  expect_headers(assoc, table, find_id(table, "b"), expected, 1);
  gumbo_table_headers_destroy(assoc);
  gumbo_destroy_output(&kGumboDefaultOptions, output);
}

// Scenario 24: colspan="0" clamps to 1000 per the table model rather than
// being treated as a single-column cell.
static void test_colspan_zero_clamps(void) {
  const char* html =
      "<table><tr><th id='h'>H</th>"
      "<td id='wide' colspan='0'>w</td></tr>"
      "<tr><td id='under' colspan='1000'>u</td></tr></table>";
  GumboOutput* output = gumbo_parse(html);
  const GumboNode* tables[4];
  size_t table_count = 0;
  collect_tables(output->root, tables, &table_count, 4);
  const GumboNode* table = tables[0];
  GumboTableHeaders* assoc = gumbo_table_headers_build(table);
  assert(assoc != NULL);
  assert(assoc->cell_count == 3);
  // The row-2 cell spans the same 1000 columns reached by the clamped
  // colspan=0 cell, so its vertical scan finds the header above.
  const char* expected[] = {"h"};
  expect_headers(assoc, table, find_id(table, "under"), expected, 1);
  gumbo_table_headers_destroy(assoc);
  gumbo_destroy_output(&kGumboDefaultOptions, output);
}

int main(void) {
  test_basic_two_columns();
  ++g_cases;
  test_explicit_headers_overrides_scope();
  ++g_cases;
  test_empty_headers_attribute();
  ++g_cases;
  test_duplicate_header_text();
  ++g_cases;
  test_cross_group_headers();
  ++g_cases;
  test_spans_multiple_scan_lines();
  ++g_cases;
  test_data_cell_separates_header_blocks();
  ++g_cases;
  test_nested_table_same_id();
  ++g_cases;
  test_model_conflict_returns_null();
  ++g_cases;
  test_rowspan_zero();
  ++g_cases;
  test_blank_headers_removed();
  ++g_cases;
  test_row_group_reordering();
  ++g_cases;
  test_rowspan_zero_stops_at_group();
  ++g_cases;
  test_opaque_header_block();
  ++g_cases;
  test_size_caps();
  ++g_cases;
  test_duplicate_id_and_no_recursion();
  ++g_cases;
  test_explicit_invalid_tokens();
  ++g_cases;
  test_colgroup_association();
  ++g_cases;
  test_identity_dedup();
  ++g_cases;
  test_empty_table();
  ++g_cases;
  test_explicit_headers_on_header_cell();
  ++g_cases;
  test_rowgroup_wide_header_coverage();
  ++g_cases;
  test_rowgroup_step_data_cell_block();
  ++g_cases;
  test_colspan_zero_clamps();
  ++g_cases;
  printf("HEADER_ASSOC_OK cases=%d\n", g_cases);
  return 0;
}
