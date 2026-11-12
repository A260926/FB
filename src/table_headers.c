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
// Implementation of the table processing model and the algorithm for
// assigning header cells:
// https://html.spec.whatwg.org/multipage/tables.html#processing-model-1

#include "table_headers.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Spec-mandated clamp values for the span attributes.
#define MAX_COLSPAN 1000
#define MAX_ROWSPAN 65534

typedef struct {
  const GumboNode* node;
  int x;       // anchor column
  int y;       // anchor row
  int w;       // width in columns
  int h;       // height in rows
  int group;   // row group index, -1 if the tr is a direct child of table
  bool is_th;
  bool blank;  // header cell whose text content is only whitespace
} ModelCell;

typedef struct {
  ModelCell* data;
  size_t length;
  size_t capacity;
} CellVec;

typedef struct {
  int start_row;
  int end_row;  // exclusive
} RowGroup;

typedef struct {
  int start_col;
  int end_col;  // exclusive
} ColGroup;

typedef struct {
  // rows[y][x] holds the cell index + 1, or 0 for a free slot.
  int** rows;
  int row_count;
  int col_count;
} Grid;

static bool is_element(const GumboNode* node, GumboTag tag) {
  return node != NULL && node->type == GUMBO_NODE_ELEMENT &&
         node->v.element.tag == tag;
}

// Parses a non-negative integer per the rules used by colspan/rowspan/span.
// Returns -1 if the attribute is absent or cannot be parsed; clamps to |cap|.
static int parse_non_negative(const GumboNode* node, const char* attr,
                              int cap) {
  const GumboAttribute* a =
      gumbo_get_attribute(&node->v.element.attributes, attr);
  if (a == NULL) return -1;
  const char* p = a->value;
  while (*p != '\0' && isspace((unsigned char)*p)) ++p;
  if (!isdigit((unsigned char)*p)) return -1;
  long value = 0;
  do {
    int digit = *p - '0';
    if (value > (cap - digit) / 10L) return cap;
    value = value * 10 + digit;
    ++p;
  } while (isdigit((unsigned char)*p));
  return (int)value;
}

static bool subtree_is_blank(const GumboNode* node) {
  if (node->type == GUMBO_NODE_TEXT || node->type == GUMBO_NODE_CDATA) {
    for (const char* p = node->v.text.text; *p != '\0'; ++p) {
      if (!isspace((unsigned char)*p)) return false;
    }
    return true;
  }
  if (node->type != GUMBO_NODE_ELEMENT &&
      node->type != GUMBO_NODE_TEMPLATE) {
    return true;
  }
  const GumboVector* children = &node->v.element.children;
  for (unsigned int i = 0; i < children->length; ++i) {
    if (!subtree_is_blank(children->data[i])) return false;
  }
  return true;
}

static bool cellvec_push(CellVec* vec, ModelCell cell, bool* ok) {
  // Every cell occupies at least one slot, so this bounds all backing
  // allocations before they can grow past the slot cap.
  if (vec->length >= GUMBO_TABLE_HEADERS_MAX_SLOTS) {
    *ok = false;
    return false;
  }
  if (vec->length == vec->capacity) {
    size_t new_capacity = vec->capacity == 0 ? 16 : vec->capacity * 2;
    ModelCell* grown = realloc(vec->data, new_capacity * sizeof(ModelCell));
    if (grown == NULL) {
      *ok = false;
      return false;
    }
    vec->data = grown;
    vec->capacity = new_capacity;
  }
  vec->data[vec->length++] = cell;
  return true;
}

// Grows every row so that the grid has at least |need| columns.  The size caps
// are enforced before any reallocation.
static bool grid_grow(Grid* grid, int need, int total_rows) {
  if (need > GUMBO_TABLE_HEADERS_MAX_COLUMNS) return false;
  if ((long)total_rows * (long)need > GUMBO_TABLE_HEADERS_MAX_SLOTS) {
    return false;
  }
  if (need <= grid->col_count) return true;
  for (int y = 0; y < total_rows; ++y) {
    int* grown;
    if (grid->rows[y] == NULL) {
      grown = calloc((size_t)need, sizeof(int));
    } else {
      grown = realloc(grid->rows[y], (size_t)need * sizeof(int));
      if (grown != NULL) {
        memset(grown + grid->col_count, 0,
               (size_t)(need - grid->col_count) * sizeof(int));
      }
    }
    if (grown == NULL) return false;
    grid->rows[y] = grown;
  }
  grid->col_count = need;
  return true;
}

static void collect_row(const GumboNode* tr, int group, CellVec* cells,
                        int* row_count, bool* ok);

static void collect_group(const GumboNode* group_node, CellVec* cells,
                          RowGroup** groups, size_t* group_count,
                          size_t* group_capacity, int* row_count, bool* ok) {
  if (*group_count == *group_capacity) {
    size_t new_capacity = *group_capacity == 0 ? 4 : *group_capacity * 2;
    RowGroup* grown = realloc(*groups, new_capacity * sizeof(RowGroup));
    if (grown == NULL) {
      *ok = false;
      return;
    }
    *groups = grown;
    *group_capacity = new_capacity;
  }
  int group = (int)*group_count;
  (*groups)[group].start_row = *row_count;
  ++*group_count;

  const GumboVector* children = &group_node->v.element.children;
  for (unsigned int i = 0; i < children->length; ++i) {
    const GumboNode* child = children->data[i];
    if (is_element(child, GUMBO_TAG_TR)) {
      collect_row(child, group, cells, row_count, ok);
      if (!*ok) return;
    }
  }
  (*groups)[group].end_row = *row_count;
}

static void collect_row(const GumboNode* tr, int group, CellVec* cells,
                        int* row_count, bool* ok) {
  int y = *row_count;
  if (y >= GUMBO_TABLE_HEADERS_MAX_ROWS) {
    *ok = false;
    return;
  }
  ++*row_count;
  const GumboVector* children = &tr->v.element.children;
  for (unsigned int i = 0; i < children->length; ++i) {
    const GumboNode* child = children->data[i];
    if (!is_element(child, GUMBO_TAG_TH) && !is_element(child, GUMBO_TAG_TD)) {
      continue;
    }
    int colspan = parse_non_negative(child, "colspan", MAX_COLSPAN);
    if (colspan < 0) {
      colspan = 1;  // Absent or unparsable defaults to 1.
    } else if (colspan == 0) {
      colspan = MAX_COLSPAN;  // Spec clamps zero to 1000, not 1.
    }
    int rowspan = parse_non_negative(child, "rowspan", MAX_ROWSPAN);
    // Absent or unparsable defaults to 1; rowspan="0" stays zero here.
    if (rowspan < 0) rowspan = 1;
    ModelCell cell;
    cell.node = child;
    cell.x = 0;
    cell.y = y;
    cell.w = colspan;
    cell.h = rowspan;
    cell.group = group;
    cell.is_th = child->v.element.tag == GUMBO_TAG_TH;
    cell.blank = subtree_is_blank(child);
    if (!cellvec_push(cells, cell, ok)) return;
  }
}

// Walks the direct row containers of |table|.  Nested tables are skipped, so
// they contribute no rows or cells to the outer model.
//
// Row order follows the table model rather than source order: all thead rows
// come first, then tbody rows and bare trs in tree order, then all tfoot
// rows last.
static void collect_cells_in(const GumboNode* table, CellVec* cells,
                             RowGroup** groups, size_t* group_count,
                             size_t* group_capacity, int* row_count,
                             bool* ok) {
  static const GumboTag pass_tags[3] = {GUMBO_TAG_THEAD, GUMBO_TAG_TBODY,
                                        GUMBO_TAG_TFOOT};
  const GumboVector* children = &table->v.element.children;
  for (int pass = 0; pass < 3 && *ok; ++pass) {
    for (unsigned int i = 0; i < children->length; ++i) {
      const GumboNode* child = children->data[i];
      if (child->type != GUMBO_NODE_ELEMENT) continue;
      GumboTag tag = child->v.element.tag;
      if (tag == pass_tags[pass]) {
        collect_group(child, cells, groups, group_count, group_capacity,
                      row_count, ok);
        if (!*ok) return;
      } else if (pass == 1 && tag == GUMBO_TAG_TR) {
        // The parser normally implies a tbody for bare trs, but handle them
        // explicitly as group-less rows just in case.
        collect_row(child, -1, cells, row_count, ok);
        if (!*ok) return;
      }
      // caption/colgroup/col/nested-table/... carry no rows for this table.
    }
  }
}

// Builds the column-group column ranges from colgroup/col children of table.
static bool build_colgroups(const GumboNode* table, ColGroup** out_groups,
                            size_t* out_count) {
  size_t capacity = 0;
  size_t count = 0;
  ColGroup* groups = NULL;
  int column = 0;
  const GumboVector* children = &table->v.element.children;
  for (unsigned int i = 0; i < children->length; ++i) {
    const GumboNode* child = children->data[i];
    if (!is_element(child, GUMBO_TAG_COLGROUP)) continue;
    int start = column;
    const GumboVector* cg_children = &child->v.element.children;
    bool has_col = false;
    for (unsigned int j = 0; j < cg_children->length; ++j) {
      const GumboNode* col = cg_children->data[j];
      if (!is_element(col, GUMBO_TAG_COL)) continue;
      has_col = true;
      int span = parse_non_negative(col, "span", MAX_COLSPAN);
      if (span < 0) {
        span = 1;
      } else if (span == 0) {
        span = MAX_COLSPAN;
      }
      column += span;
      if (column > GUMBO_TABLE_HEADERS_MAX_COLUMNS) {
        free(groups);
        return false;
      }
    }
    if (!has_col) {
      int span = parse_non_negative(child, "span", MAX_COLSPAN);
      if (span < 0) {
        span = 1;
      } else if (span == 0) {
        span = MAX_COLSPAN;
      }
      column += span;
      if (column > GUMBO_TABLE_HEADERS_MAX_COLUMNS) {
        free(groups);
        return false;
      }
    }
    if (count == capacity) {
      size_t new_capacity = capacity == 0 ? 4 : capacity * 2;
      ColGroup* grown = realloc(groups, new_capacity * sizeof(ColGroup));
      if (grown == NULL) {
        free(groups);
        return false;
      }
      groups = grown;
      capacity = new_capacity;
    }
    groups[count].start_col = start;
    groups[count].end_col = column;
    ++count;
  }
  *out_groups = groups;
  *out_count = count;
  return true;
}

// Places every cell on the grid, resolving anchors, spans (including
// rowspan="0") and detecting overlap table model errors.
static bool place_cells(Grid* grid, CellVec* cells, int total_rows) {
  grid->row_count = total_rows;
  grid->col_count = 0;
  grid->rows = calloc((size_t)total_rows, sizeof(int*));
  if (total_rows > 0 && grid->rows == NULL) return false;

  size_t cursor = 0;
  for (int y = 0; y < total_rows; ++y) {
    int x = 0;
    while (cursor < cells->length && cells->data[cursor].y == y) {
      ModelCell* cell = &cells->data[cursor++];
      while (x < grid->col_count && grid->rows[y][x] != 0) ++x;
      cell->x = x;

      int height = cell->h;  // rowspan="0" was already resolved.
      if (y + height > total_rows) {
        // A rowspan extending past the last row is truncated.
        height = total_rows - y;
      }
      cell->h = height;

      int need = x + cell->w;
      if (!grid_grow(grid, need, total_rows)) return false;
      for (int yy = y; yy < y + height; ++yy) {
        for (int xx = x; xx < x + cell->w; ++xx) {
          if (grid->rows[yy][xx] != 0) return false;  // overlap error
          grid->rows[yy][xx] = (int)cursor;           // cell index + 1
        }
      }
    }
  }
  return true;
}

// Expands rowspan="0" cells before placement: the height reaches the last row
// of the containing row group, or of the whole table for group-less rows.
static void resolve_rowspan_zero(CellVec* cells, const RowGroup* groups,
                                 size_t group_count, int total_rows) {
  for (size_t i = 0; i < cells->length; ++i) {
    ModelCell* cell = &cells->data[i];
    if (cell->h != 0) continue;
    int limit = total_rows;
    if (cell->group >= 0 && (size_t)cell->group < group_count) {
      limit = groups[cell->group].end_row;
    }
    int height = limit - cell->y;
    if (height < 1) height = 1;
    cell->h = height;
  }
}

static int grid_cell_at(const Grid* grid, int x, int y) {
  if (x < 0 || y < 0 || x >= grid->col_count || y >= grid->row_count ||
      grid->rows[y] == NULL) {
    return -1;
  }
  int slot = grid->rows[y][x];
  return slot == 0 ? -1 : slot - 1;
}

static void add_marker(unsigned char* markers, const CellVec* cells, int idx,
                       const ModelCell* subject) {
  if (idx < 0) return;
  const ModelCell* candidate = &cells->data[idx];
  if (candidate == subject || !candidate->is_th || candidate->blank) return;
  markers[idx] = 1;
}

// The automatic "algorithm for assigning header cells" for one source cell.
// |scratch| is a caller-owned buffer of the same size as |markers|, used for
// per-step blocking rules that need to discard headers added within the step.
static void assign_automatic(const ModelCell* subject, const Grid* grid,
                             const CellVec* cells, const RowGroup* groups,
                             const ColGroup* colgroups,
                             size_t colgroup_count, unsigned char* markers,
                             unsigned char* scratch, int* hits) {
  const int topx = subject->x;
  const int topy = subject->y;
  const int w = subject->w;
  const int h = subject->h;

  // 1. Horizontal scan, once for every row the principal cell covers.
  for (int y = topy; y < topy + h; ++y) {
    for (int x = topx - 1; x >= 0; --x) {
      int idx = grid_cell_at(grid, x, y);
      if (idx < 0) continue;
      const ModelCell* candidate = &cells->data[idx];
      if (candidate == subject) continue;
      if (!candidate->is_th) break;  // A data cell blocks the whole line.
      // A header block of the exact same rectangle immediately to the left
      // blocks further scanning of this line.
      if (candidate->w == w && candidate->h == h &&
          candidate->x == topx - w && candidate->y == topy) {
        add_marker(markers, cells, idx, subject);
        break;
      }
      add_marker(markers, cells, idx, subject);
    }
  }

  // 2. Vertical scan, once for every column the principal cell covers.
  for (int x = topx; x < topx + w; ++x) {
    for (int y = topy - 1; y >= 0; --y) {
      int idx = grid_cell_at(grid, x, y);
      if (idx < 0) continue;
      const ModelCell* candidate = &cells->data[idx];
      if (candidate == subject) continue;
      if (!candidate->is_th) break;  // A data cell blocks the whole line.
      // A header block of the exact same rectangle immediately above blocks
      // further scanning of this line.
      bool header_block = candidate->w == w && candidate->h == h &&
                          candidate->x == topx && candidate->y == topy - h;
      add_marker(markers, cells, idx, subject);
      if (header_block) break;
    }
  }

  // A spanning header cell is itself a header block, so the row group and
  // column group steps do not apply to it.
  bool principal_spans = subject->is_th && (w > 1 || h > 1);

  // 3. Row group headers: th cells in rows above within the same row group
  // that cover one of the principal cell's columns.  A data cell in an upper
  // row discards the headers contributed by that row in this step only.
  if (subject->group >= 0 && !principal_spans) {
    const RowGroup* rg = &groups[subject->group];
    memset(scratch, 0, cells->length);
    for (int y = topy - 1; y >= rg->start_row; --y) {
      size_t hit_count = 0;
      bool blocked = false;
      for (int x = topx; x < topx + w; ++x) {
        int idx = grid_cell_at(grid, x, y);
        if (idx < 0) continue;
        const ModelCell* candidate = &cells->data[idx];
        if (!candidate->is_th) {
          blocked = true;
          break;
        }
        if (candidate->x <= x && x < candidate->x + candidate->w) {
          hits[hit_count++] = idx;
        }
      }
      if (!blocked) {
        for (size_t k = 0; k < hit_count; ++k) scratch[hits[k]] = 1;
      }
    }
    for (size_t j = 0; j < cells->length; ++j) {
      if (scratch[j]) add_marker(markers, cells, (int)j, subject);
    }
  }

  // 4. Column group headers: th cells to the left within the principal cell's
  // column group that cover one of the rows it spans.  A data cell in a left
  // column discards the headers contributed by that column in this step.
  if (!principal_spans) {
    const ColGroup* cg = NULL;
    for (size_t i = 0; i < colgroup_count; ++i) {
      if (colgroups[i].start_col <= topx && topx < colgroups[i].end_col) {
        cg = &colgroups[i];
        break;
      }
    }
    if (cg != NULL) {
      int left_bound = topx == cg->start_col ? 0 : cg->start_col;
      memset(scratch, 0, cells->length);
      for (int x = topx - 1; x >= left_bound; --x) {
        size_t hit_count = 0;
        bool blocked = false;
        for (int y = topy; y < topy + h; ++y) {
          int idx = grid_cell_at(grid, x, y);
          if (idx < 0) continue;
          const ModelCell* candidate = &cells->data[idx];
          if (!candidate->is_th) {
            blocked = true;
            break;
          }
          if (candidate->y <= y && y < candidate->y + candidate->h) {
            hits[hit_count++] = idx;
          }
        }
        if (!blocked) {
          for (size_t k = 0; k < hit_count; ++k) scratch[hits[k]] = 1;
        }
      }
      for (size_t j = 0; j < cells->length; ++j) {
        if (scratch[j]) add_marker(markers, cells, (int)j, subject);
      }
    }
  }

  // 5. Opaque row group: the nearest preceding row group that covers any of
  // the principal cell's columns contributes its headers; earlier groups are
  // not consulted.
  if (subject->group >= 0) {
    for (int g = subject->group - 1; g >= 0; --g) {
      bool any_cell = false;
      for (int y = groups[g].start_row; y < groups[g].end_row; ++y) {
        for (int x = topx; x < topx + w; ++x) {
          int idx = grid_cell_at(grid, x, y);
          if (idx < 0) continue;
          any_cell = true;
          add_marker(markers, cells, idx, subject);
        }
      }
      if (any_cell) break;
    }
  }
}

// Resolves one headers-attribute token against this table's th cells.
// Duplicate IDs resolve to the first th in DOM order; non-th, missing and
// self references are ignored.  |dom_order| lists model cell indices in
// source DOM order.
static int resolve_id_token(const CellVec* cells, const size_t* dom_order,
                            const char* token, size_t token_len,
                            const ModelCell* subject) {
  for (size_t k = 0; k < cells->length; ++k) {
    const ModelCell* candidate = &cells->data[dom_order[k]];
    if (!candidate->is_th || candidate == subject) continue;
    const GumboAttribute* id =
        gumbo_get_attribute(&candidate->node->v.element.attributes, "id");
    if (id == NULL || strlen(id->value) != token_len) continue;
    if (memcmp(id->value, token, token_len) == 0) return (int)dom_order[k];
  }
  return -1;
}

static void assign_explicit(const ModelCell* subject, const CellVec* cells,
                            const size_t* dom_order,
                            const char* headers_attr,
                            unsigned char* markers) {
  // Referenced header cells are not expanded through their own headers
  // attribute; tokens are resolved one level only.
  const char* p = headers_attr;
  while (*p != '\0') {
    while (isspace((unsigned char)*p)) ++p;
    const char* start = p;
    while (*p != '\0' && !isspace((unsigned char)*p)) ++p;
    size_t len = (size_t)(p - start);
    if (len > 0) {
      int idx = resolve_id_token(cells, dom_order, start, len, subject);
      if (idx >= 0) add_marker(markers, cells, idx, subject);
    }
  }
}

// Appends source th/td nodes in DOM order, never descending into a nested
// table (which has its own independent model).
static void collect_dom_cells(const GumboNode* node, const GumboNode* table,
                              const GumboNode** out, size_t* count) {
  if (node->type != GUMBO_NODE_ELEMENT &&
      node->type != GUMBO_NODE_TEMPLATE) {
    return;
  }
  if (node != table && node->v.element.tag == GUMBO_TAG_TABLE) return;
  if (node->v.element.tag == GUMBO_TAG_TH ||
      node->v.element.tag == GUMBO_TAG_TD) {
    out[(*count)++] = node;
  }
  const GumboVector* children = &node->v.element.children;
  for (unsigned int i = 0; i < children->length; ++i) {
    collect_dom_cells(children->data[i], table, out, count);
  }
}

typedef struct {
  const GumboNode* node;
  size_t model_index;
} NodeIndex;

static int node_index_cmp(const void* a, const void* b) {
  uintptr_t x = (uintptr_t)((const NodeIndex*)a)->node;
  uintptr_t y = (uintptr_t)((const NodeIndex*)b)->node;
  return x < y ? -1 : x > y ? 1 : 0;
}

static int node_search_cmp(const void* key, const void* element) {
  uintptr_t x = (uintptr_t)key;
  uintptr_t y = (uintptr_t)((const NodeIndex*)element)->node;
  return x < y ? -1 : x > y ? 1 : 0;
}

static void free_grid(Grid* grid) {
  for (int y = 0; y < grid->row_count; ++y) free(grid->rows[y]);
  free(grid->rows);
}

GumboTableHeaders* gumbo_table_headers_build(const GumboNode* table) {
  if (!is_element(table, GUMBO_TAG_TABLE)) return NULL;

  CellVec cells = {NULL, 0, 0};
  RowGroup* groups = NULL;
  size_t group_count = 0;
  size_t group_capacity = 0;
  ColGroup* colgroups = NULL;
  size_t colgroup_count = 0;
  int row_count = 0;
  bool ok = true;

  collect_cells_in(table, &cells, &groups, &group_count, &group_capacity,
                   &row_count, &ok);
  if (!ok) goto fail;

  if (!build_colgroups(table, &colgroups, &colgroup_count)) goto fail;

  resolve_rowspan_zero(&cells, groups, group_count, row_count);

  Grid grid;
  memset(&grid, 0, sizeof(grid));
  if (!place_cells(&grid, &cells, row_count)) {
    free_grid(&grid);
    goto fail;
  }

  size_t n = cells.length;

  // Source DOM order of this table's cells; nested-table cells are excluded.
  const GumboNode** dom_nodes = malloc((n == 0 ? 1 : n) * sizeof(GumboNode*));
  size_t* dom_order = malloc((n == 0 ? 1 : n) * sizeof(size_t));
  NodeIndex* by_node = malloc((n == 0 ? 1 : n) * sizeof(NodeIndex));
  GumboTableHeaders* result = malloc(sizeof(GumboTableHeaders));
  GumboTableHeaderEntry* entries =
      malloc((n == 0 ? 1 : n) * sizeof(GumboTableHeaderEntry));
  unsigned char* markers = malloc(n == 0 ? 1 : n);
  unsigned char* scratch = malloc(n == 0 ? 1 : n);
  int* hits = malloc((n == 0 ? 1 : n) * sizeof(int));
  if (dom_nodes == NULL || dom_order == NULL || by_node == NULL ||
      result == NULL || entries == NULL || markers == NULL ||
      scratch == NULL || hits == NULL) {
    free(dom_nodes);
    free(dom_order);
    free(by_node);
    free(result);
    free(entries);
    free(markers);
    free(scratch);
    free(hits);
    free_grid(&grid);
    goto fail;
  }

  size_t dom_count = 0;
  collect_dom_cells(table, table, dom_nodes, &dom_count);
  if (dom_count != n) {
    // Internal bookkeeping mismatch; refuse rather than return half a result.
    free(dom_nodes);
    free(dom_order);
    free(by_node);
    free(result);
    free(entries);
    free(markers);
    free(scratch);
    free(hits);
    free_grid(&grid);
    goto fail;
  }
  for (size_t i = 0; i < n; ++i) {
    by_node[i].node = cells.data[i].node;
    by_node[i].model_index = i;
  }
  qsort(by_node, n, sizeof(NodeIndex), node_index_cmp);
  for (size_t k = 0; k < n; ++k) {
    const NodeIndex* hit =
        bsearch(dom_nodes[k], by_node, n, sizeof(NodeIndex),
                node_search_cmp);
    if (hit == NULL) {
      // The parser only places th/td inside tr, so every DOM cell should be
      // present in the model; a mismatch means an internal inconsistency.
      free(dom_nodes);
      free(dom_order);
      free(by_node);
      free(result);
      free(entries);
      free(markers);
      free(scratch);
      free(hits);
      free_grid(&grid);
      goto fail;
    }
    dom_order[k] = hit->model_index;
  }

  // Compute associations against the model, one source cell at a time.
  for (size_t k = 0; k < n; ++k) {
    size_t i = dom_order[k];
    const ModelCell* subject = &cells.data[i];
    memset(markers, 0, n);
    const GumboAttribute* headers =
        gumbo_get_attribute(&subject->node->v.element.attributes, "headers");
    if (headers != NULL) {
      // An empty attribute intentionally yields an empty header set; there is
      // no fallback to automatic inference.
      assign_explicit(subject, &cells, dom_order, headers->value, markers);
    } else {
      assign_automatic(subject, &grid, &cells, groups, colgroups,
                       colgroup_count, markers, scratch, hits);
    }

    // Emit the marked headers in source DOM order.  add_marker already dropped
    // the principal cell itself and blank headers; the DOM-order walk fixes
    // the output ordering.
    size_t count = 0;
    for (size_t j = 0; j < n; ++j) {
      if (markers[dom_order[j]]) ++count;
    }
    const GumboNode** array = NULL;
    if (count > 0) {
      array = malloc(count * sizeof(const GumboNode*));
      if (array == NULL) {
        for (size_t done = 0; done < k; ++done) {
          free((void*)entries[done].headers);
        }
        free(dom_nodes);
        free(dom_order);
        free(by_node);
        free(entries);
        free(markers);
        free(scratch);
        free(hits);
        free(result);
        free_grid(&grid);
        goto fail;
      }
      size_t out = 0;
      for (size_t j = 0; j < n; ++j) {
        if (markers[dom_order[j]]) {
          array[out++] = cells.data[dom_order[j]].node;
        }
      }
    }
    entries[k].cell = subject->node;
    entries[k].headers = array;
    entries[k].header_count = count;
  }

  free(markers);
  free(scratch);
  free(hits);
  free(dom_nodes);
  free(dom_order);
  free(by_node);
  free_grid(&grid);
  free(groups);
  free(colgroups);
  free(cells.data);

  result->cells = entries;
  result->cell_count = n;
  return result;

fail:
  free(groups);
  free(colgroups);
  free(cells.data);
  return NULL;
}

void gumbo_table_headers_destroy(GumboTableHeaders* result) {
  if (result == NULL) return;
  if (result->cells != NULL) {
    for (size_t i = 0; i < result->cell_count; ++i) {
      // The headers pointer is const-qualified in the public API but the
      // backing array is owned by this result.
      free((void*)result->cells[i].headers);
    }
    free(result->cells);
  }
  free(result);
}
