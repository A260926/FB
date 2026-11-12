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
// Demonstrates gumbo_table_headers_build() on a table with multi-level
// headers.  Build (from the repository root) with:
//
//   cc -std=c99 -Isrc src/*.c examples/table_headers_demo.c -o demo
//   ./demo

#include <stdio.h>

#include "gumbo.h"
#include "table_headers.h"

static const GumboNode* find_first_table(const GumboNode* node) {
  if (node->type != GUMBO_NODE_ELEMENT &&
      node->type != GUMBO_NODE_TEMPLATE) {
    return NULL;
  }
  if (node->v.element.tag == GUMBO_TAG_TABLE) return node;
  const GumboVector* children = &node->v.element.children;
  for (unsigned int i = 0; i < children->length; ++i) {
    const GumboNode* found = find_first_table(children->data[i]);
    if (found != NULL) return found;
  }
  return NULL;
}

static const char* cell_text(const GumboNode* node) {
  const GumboVector* children = &node->v.element.children;
  for (unsigned int i = 0; i < children->length; ++i) {
    const GumboNode* child = children->data[i];
    if (child->type == GUMBO_NODE_TEXT ||
        child->type == GUMBO_NODE_WHITESPACE) {
      return child->v.text.text;
    }
  }
  return "";
}

int main(void) {
  // Two header levels: a spanning "Sales" header above the quarterly columns,
  // plus row headers in the first column.
  const char* html =
      "<table>"
      "  <thead>"
      "    <tr><th rowspan='2'>Region</th>"
      "        <th colspan='2'>Sales</th></tr>"
      "    <tr><th>Q1</th><th>Q2</th></tr>"
      "  </thead>"
      "  <tbody>"
      "    <tr><th>North</th><td>10</td><td>20</td></tr>"
      "    <tr><th>South</th><td>30</td><td>40</td></tr>"
      "  </tbody>"
      "</table>";

  GumboOutput* output = gumbo_parse(html);
  const GumboNode* table = find_first_table(output->root);

  GumboTableHeaders* assoc = gumbo_table_headers_build(table);
  if (assoc == NULL) {
    fprintf(stderr, "table model error or size limit exceeded\n");
    gumbo_destroy_output(&kGumboDefaultOptions, output);
    return 1;
  }

  for (size_t i = 0; i < assoc->cell_count; ++i) {
    const GumboTableHeaderEntry* entry = &assoc->cells[i];
    printf("cell <%s> headers:",
           entry->cell->v.element.tag == GUMBO_TAG_TH ? "th" : "td");
    for (size_t j = 0; j < entry->header_count; ++j) {
      printf(" [%s]", cell_text(entry->headers[j]));
    }
    printf(" -> %s\n", cell_text(entry->cell));
  }

  // Destroy the association first, then the parse tree it borrowed from.
  gumbo_table_headers_destroy(assoc);
  gumbo_destroy_output(&kGumboDefaultOptions, output);
  return 0;
}
