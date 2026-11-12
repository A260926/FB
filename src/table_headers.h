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
// Table header association for Gumbo parse trees.
//
// This module implements the table model and the "algorithm for assigning
// header cells" from the HTML Standard:
// https://html.spec.whatwg.org/multipage/tables.html#processing-model-1
//
// It works purely on the Gumbo DOM: the parse tree is not modified, no layout
// is performed, and CSS is not consulted.  Coordinates are derived from the
// parsed nodes (row groups, colgroups, colspan, rowspan, including
// rowspan="0"), never guessed from source tag positions.

#ifndef GUMBO_TABLE_HEADERS_H_
#define GUMBO_TABLE_HEADERS_H_

#include <stddef.h>

#include "gumbo.h"

#ifdef __cplusplus
extern "C" {
#endif

// Hard size caps for a single table model.  If the model would exceed any of
// these, gumbo_table_headers_build() returns NULL.  The checks happen before
// the corresponding allocations, so hostile input cannot trigger a huge
// allocation.
#define GUMBO_TABLE_HEADERS_MAX_ROWS 4096
#define GUMBO_TABLE_HEADERS_MAX_COLUMNS 4096
#define GUMBO_TABLE_HEADERS_MAX_SLOTS 1000000

// One source cell (th or td) together with the header cells assigned to it.
//
// All node pointers are borrowed from the GumboOutput tree: this structure
// never owns nodes, and destroying it does not destroy any node.
typedef struct {
  // The source th/td node, in tree order.
  const GumboNode* cell;

  // Header cells assigned to |cell|, sorted by document (tree) order of the
  // th nodes within this table.  Distinct th nodes with identical text content
  // are kept as distinct entries.  NULL when header_count is zero; otherwise
  // points into storage owned by the enclosing GumboTableHeaders result.
  const GumboNode* const* headers;

  // Number of entries in |headers|; may be zero.
  size_t header_count;
} GumboTableHeaderEntry;

// The full header association for one table element.
//
// Ownership: the result owns |cells| and every |headers| array (and their
// backing storage), but none of the GumboNode pointers.
typedef struct {
  // One entry per source th/td of this table, in source DOM order.  Cells
  // belonging to nested tables are not included.
  GumboTableHeaderEntry* cells;

  // Number of entries in |cells|.
  size_t cell_count;
} GumboTableHeaders;

// Builds the table model for |table| and assigns header cells to every cell.
//
// |table| must be an element node whose tag is GUMBO_TAG_TABLE.  Nested tables
// contribute no rows, cells, IDs or headers to the outer table; each table is
// built independently.
//
// Returns a heap-allocated result on success, or NULL if:
//   * |table| is NULL or not an HTML table element;
//   * the table model contains an error, e.g. cells that would overlap
//     (the whole result is rejected -- partial associations are never
//     returned);
//   * the model would exceed GUMBO_TABLE_HEADERS_MAX_ROWS / MAX_COLUMNS /
//     MAX_SLOTS.
//
// The returned pointer must be released with gumbo_table_headers_destroy().
GumboTableHeaders* gumbo_table_headers_build(const GumboNode* table);

// Frees a result returned by gumbo_table_headers_build(), including all entry
// and header arrays.  Does nothing if |result| is NULL.  The borrowed
// GumboNode pointers are not touched.
//
// Release order: call gumbo_table_headers_destroy() while the GumboOutput
// tree that the nodes were borrowed from is still alive, and only afterwards
// call gumbo_destroy_output().  Using any node pointer from a destroyed result
// after gumbo_destroy_output() is a dangling-pointer bug.
void gumbo_table_headers_destroy(GumboTableHeaders* result);

// Multi-level header example:
//
//   const char* html =
//       "<table>"
//       "  <thead>"
//       "    <tr><th rowspan=2 id=region>Region</th>"
//       "        <th colspan=2 id=sales>Sales</th></tr>"
//       "    <tr><th id=q1>Q1</th><th id=q2>Q2</th></tr>"
//       "  </thead>"
//       "  <tbody>"
//       "    <tr><th id=north scope=row>North</th>"
//       "       <td>10</td><td>20</td></tr>"
//       "  </tbody>"
//       "</table>";
//   GumboOutput* output = gumbo_parse(html);
//   const GumboNode* table = find_first_table(output->root);
//   GumboTableHeaders* assoc = gumbo_table_headers_build(table);
//   if (assoc != NULL) {
//     for (size_t i = 0; i < assoc->cell_count; ++i) {
//       const GumboTableHeaderEntry* e = &assoc->cells[i];
//       for (size_t j = 0; j < e->header_count; ++j) {
//         // e->headers[j] is a th node borrowed from output.
//       }
//     }
//     gumbo_table_headers_destroy(assoc);  // first
//   }
//   gumbo_destroy_output(&kGumboDefaultOptions, output);  // then the tree

#ifdef __cplusplus
}
#endif

#endif  // GUMBO_TABLE_HEADERS_H_
