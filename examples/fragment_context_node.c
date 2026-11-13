// Collection-side example for GumboOptions.fragment_context_node.
//
// The collector first saves a complete host page and parses it.  An interface
// later returns an HTML fragment that was mounted at a particular node of that
// page; parsing the fragment with the real mount node as context reproduces
// the foreign-content, form and quirks-mode state of the mount point.
//
// Borrowing / lifetime rules (also enforced by the implementation):
//
//   1. The context node, the host GumboOutput that owns it, and the host
//      page's original input buffer must all stay alive for the duration of
//      the gumbo_parse_with_options() call.  The node is only borrowed: it is
//      never inserted into the fragment tree, never modified, and is not freed
//      when the fragment output is destroyed.  Everything the parser needs to
//      keep (tag, namespace, attributes) is copied through the call's own
//      allocator, so the returned fragment output holds no host memory and the
//      host may in principle be torn down as soon as the call returns; keeping
//      it alive until after the fragment output is destroyed, as shown below,
//      is the simplest safe ordering.
//   2. The fragment input buffer must outlive the fragment GumboOutput, just
//      like an ordinary gumbo_parse() call: source positions and original tag
//      text point into it.
//   3. Destroy each output with the options used to create it.  Destroying the
//      fragment output never frees host nodes or host strings, and destroying
//      the host afterwards encounters no double-free.
//
// Build:
//   cc -std=c99 -Isrc examples/fragment_context_node.c .build/libgumbo.a -o ex

#include <stdio.h>
#include <string.h>

#include "gumbo.h"

// Returns the first element in the subtree carrying the given id, or NULL.
static GumboNode* find_by_id(GumboNode* node, const char* id) {
  GumboVector* children = NULL;
  if (node->type == GUMBO_NODE_DOCUMENT) {
    children = &node->v.document.children;
  } else if (node->type == GUMBO_NODE_ELEMENT ||
             node->type == GUMBO_NODE_TEMPLATE) {
    GumboAttribute* attr =
        gumbo_get_attribute(&node->v.element.attributes, "id");
    if (attr && strcmp(attr->value, id) == 0) {
      return node;
    }
    children = &node->v.element.children;
  }
  if (children) {
    for (unsigned int i = 0; i < children->length; ++i) {
      GumboNode* found = find_by_id(children->data[i], id);
      if (found) {
        return found;
      }
    }
  }
  return NULL;
}

static void print_indented(const GumboNode* node, unsigned depth) {
  if (!(node->type == GUMBO_NODE_ELEMENT ||
          node->type == GUMBO_NODE_TEMPLATE)) {
    return;
  }
  for (unsigned i = 0; i < depth; ++i) {
    fputs("  ", stdout);
  }
  const char* namespace_name =
      node->v.element.tag_namespace == GUMBO_NAMESPACE_HTML ? "html" :
      node->v.element.tag_namespace == GUMBO_NAMESPACE_SVG ? "svg" : "mathml";
  printf("<%s ns=%s>\n",
      gumbo_normalized_tagname(node->v.element.tag), namespace_name);
  for (unsigned i = 0; i < node->v.element.children.length; ++i) {
    print_indented(node->v.element.children.data[i], depth + 1);
  }
}

int main(void) {
  // The saved complete page.  In the collector this is a retained buffer; it
  // must remain valid while host_output and any fragment parse are in use.
  const char* host_page =
      "<!doctype html>"
      "<form id=outer>"
      "<div id=mount></div>"
      "</form>";
  GumboOutput* host_output = gumbo_parse(host_page);
  GumboNode* mount = find_by_id(host_output->root, "mount");
  if (mount == NULL) {
    fputs("mount point not found\n", stderr);
    gumbo_destroy_output(&kGumboDefaultOptions, host_output);
    return 1;
  }

  // An HTML fragment captured for that mount point.  This buffer must outlive
  // fragment_output.  The outer <form> start tag is ignored because the mount
  // sits inside the host form, exactly as in a browser fragment parse.
  const char* fragment =
      "<form id=inner><input id=x name=q></form>"
      "<a id=link href='/p'>next</a>";

  GumboOptions options = kGumboDefaultOptions;
  options.fragment_context_node = mount;  // borrow, not ownership transfer
  GumboOutput* fragment_output =
      gumbo_parse_with_options(&options, fragment, strlen(fragment));

  printf("fragment document quirks mode: %d (host no-quirks is %d)\n",
      fragment_output->document->v.document.doc_type_quirks_mode,
      GUMBO_DOCTYPE_NO_QUIRKS);
  puts("fragment tree under the synthetic <html> root:");
  print_indented(fragment_output->root, 0);

  // Tear down the fragment output first.  This frees only memory allocated by
  // the fragment call; the borrowed mount node and its strings are untouched.
  gumbo_destroy_output(&options, fragment_output);
  // The host tree is still intact and can serve more fragment parses before
  // it is itself destroyed.
  gumbo_destroy_output(&kGumboDefaultOptions, host_output);
  return 0;
}
