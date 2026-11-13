// Contract tests for GumboOptions.fragment_context_node: parsing HTML
// fragments with a real GumboNode borrowed from a live host parse output as
// the fragment context.
//
// The test is deliberately self-contained (no gtest): it parses real host
// pages, pulls mount points out of the resulting trees, drives the new entry
// point, and checks parent/child relationships and namespaces with assert().
// On success it prints CONTEXT_NODE_OK; any failed assertion aborts with a
// non-zero status.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gumbo.h"

// Finds the first element with the given id, depth-first.
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

// Counts how many nodes in the subtree compare pointer-equal to target.
static int count_pointer_occurrences(GumboNode* node, const GumboNode* target) {
  int count = (node == target) ? 1 : 0;
  GumboVector* children = NULL;
  if (node->type == GUMBO_NODE_DOCUMENT) {
    children = &node->v.document.children;
  } else if (node->type == GUMBO_NODE_ELEMENT ||
             node->type == GUMBO_NODE_TEMPLATE) {
    children = &node->v.element.children;
  }
  if (children) {
    for (unsigned int i = 0; i < children->length; ++i) {
      count += count_pointer_occurrences(children->data[i], target);
    }
  }
  return count;
}

static bool is_descendant(const GumboNode* ancestor, const GumboNode* node) {
  for (const GumboNode* cur = node; cur != NULL; cur = cur->parent) {
    if (cur == ancestor) {
      return true;
    }
  }
  return false;
}

static GumboOutput* parse_fragment_with_context(
    const GumboNode* context, const char* fragment) {
  GumboOptions options = kGumboDefaultOptions;
  options.fragment_context_node = context;
  return gumbo_parse_with_options(&options, fragment, strlen(fragment));
}

// Snapshot of the parts of a host node that the fragment parse must not touch.
typedef struct {
  GumboNode* parent;
  size_t index_within_parent;
  unsigned int children_length;
  unsigned int attributes_length;
  unsigned int start_offset;
} NodeSnapshot;

static NodeSnapshot snapshot_node(const GumboNode* node) {
  NodeSnapshot snap;
  snap.parent = node->parent;
  snap.index_within_parent = node->index_within_parent;
  snap.children_length = node->v.element.children.length;
  snap.attributes_length = node->v.element.attributes.length;
  snap.start_offset = node->v.element.start_pos.offset;
  return snap;
}

static void assert_snapshot_unchanged(
    const GumboNode* node, const NodeSnapshot* snap) {
  assert(node->parent == snap->parent);
  assert(node->index_within_parent == snap->index_within_parent);
  assert(node->v.element.children.length == snap->children_length);
  assert(node->v.element.attributes.length == snap->attributes_length);
  assert(node->v.element.start_pos.offset == snap->start_offset);
}

// --- Foreign content: annotation-xml encoding drives the namespace ---------

static void test_annotation_xml_encoding(
    const char* encoding, GumboNamespaceEnum expected_namespace) {
  char host[256];
  if (encoding) {
    snprintf(host, sizeof(host),
        "<!doctype html><math><annotation-xml id=ax encoding=\"%s\">"
        "</annotation-xml></math>",
        encoding);
  } else {
    snprintf(host, sizeof(host),
        "<!doctype html><math><annotation-xml id=ax></annotation-xml></math>");
  }

  GumboOutput* host_output = gumbo_parse(host);
  GumboNode* ax = find_by_id(host_output->root, "ax");
  assert(ax != NULL);
  assert(ax->v.element.tag == GUMBO_TAG_ANNOTATION_XML);
  assert(ax->v.element.tag_namespace == GUMBO_NAMESPACE_MATHML);
  NodeSnapshot snap = snapshot_node(ax);

  GumboOutput* fragment =
      parse_fragment_with_context(ax, "<a id=a>x</a>");
  GumboNode* anchor = find_by_id(fragment->root, "a");
  assert(anchor != NULL);
  assert(anchor->v.element.tag_namespace == expected_namespace);
  // The borrowed context never enters the new tree, and no host ancestor or
  // sibling comes along with it.
  assert(count_pointer_occurrences(fragment->root, ax) == 0);
  assert_snapshot_unchanged(ax, &snap);

  gumbo_destroy_output(&kGumboDefaultOptions, fragment);
  // The host node must remain fully usable after the fragment output is gone.
  GumboAttribute* still_there =
      gumbo_get_attribute(&ax->v.element.attributes, "id");
  assert(still_there && strcmp(still_there->value, "ax") == 0);
  gumbo_destroy_output(&kGumboDefaultOptions, host_output);
}

// SVG foreignObject is an HTML integration point by tag alone; the node
// context path must preserve that pre-existing behavior.
static void test_svg_foreign_object(void) {
  GumboOutput* host_output =
      gumbo_parse("<!doctype html><svg><foreignObject id=fo>"
                  "</foreignObject></svg>");
  GumboNode* fo = find_by_id(host_output->root, "fo");
  assert(fo != NULL);
  assert(fo->v.element.tag_namespace == GUMBO_NAMESPACE_SVG);

  GumboOutput* fragment =
      parse_fragment_with_context(fo, "<div id=d>hi</div>");
  GumboNode* div = find_by_id(fragment->root, "d");
  assert(div != NULL);
  assert(div->v.element.tag_namespace == GUMBO_NAMESPACE_HTML);

  gumbo_destroy_output(&kGumboDefaultOptions, fragment);
  gumbo_destroy_output(&kGumboDefaultOptions, host_output);
}

// --- Form pointer -----------------------------------------------------------

// Runs the canonical nested-form fragment against a mount point and checks
// that the host form is only borrowed as the form pointer.
static void run_form_fragment(GumboNode* mount, const GumboNode* host_form) {
  NodeSnapshot mount_snap = snapshot_node(mount);
  NodeSnapshot form_snap = snapshot_node(host_form);

  GumboOutput* fragment = parse_fragment_with_context(mount,
      "<form id=inner><input id=x></form><form id=after></form>");

  // The inner <form> start tag is ignored because the form pointer is already
  // set; the input survives.
  assert(find_by_id(fragment->root, "inner") == NULL);
  GumboNode* input = find_by_id(fragment->root, "x");
  assert(input != NULL);
  assert(input->v.element.tag == GUMBO_TAG_INPUT);

  // The first </form> clears this parse's pointer, so the following form is
  // created normally.
  GumboNode* after = find_by_id(fragment->root, "after");
  assert(after != NULL);
  assert(after->v.element.tag == GUMBO_TAG_FORM);

  // Neither the host form nor the mount point are ever inserted into the
  // fragment tree.
  assert(count_pointer_occurrences(fragment->root, host_form) == 0);
  assert(count_pointer_occurrences(fragment->root, mount) == 0);

  assert_snapshot_unchanged(mount, &mount_snap);
  assert_snapshot_unchanged(host_form, &form_snap);

  gumbo_destroy_output(&kGumboDefaultOptions, fragment);
  assert_snapshot_unchanged(mount, &mount_snap);
  assert_snapshot_unchanged(host_form, &form_snap);
}

static void test_form_pointer_from_ancestor(void) {
  // The mount is several levels below the nearest form.
  GumboOutput* host_output = gumbo_parse(
      "<!doctype html><form id=hostform><section><article>"
      "<div id=mount></div></article></section></form>");
  GumboNode* mount = find_by_id(host_output->root, "mount");
  GumboNode* host_form = find_by_id(host_output->root, "hostform");
  assert(mount != NULL && host_form != NULL);
  run_form_fragment(mount, host_form);
  gumbo_destroy_output(&kGumboDefaultOptions, host_output);
}

static void test_form_pointer_from_context_itself(void) {
  // The context node is itself the form element.
  GumboOutput* host_output =
      gumbo_parse("<!doctype html><form id=hostform></form>");
  GumboNode* host_form = find_by_id(host_output->root, "hostform");
  assert(host_form != NULL);
  run_form_fragment(host_form, host_form);
  gumbo_destroy_output(&kGumboDefaultOptions, host_output);
}

static void test_no_host_form(void) {
  // Without a host form, fragment forms are established the old way.
  GumboOutput* host_output =
      gumbo_parse("<!doctype html><div id=mount></div>");
  GumboNode* mount = find_by_id(host_output->root, "mount");
  GumboOutput* fragment =
      parse_fragment_with_context(mount, "<form id=f></form>");
  GumboNode* form = find_by_id(fragment->root, "f");
  assert(form != NULL);
  assert(form->v.element.tag == GUMBO_TAG_FORM);
  gumbo_destroy_output(&kGumboDefaultOptions, fragment);
  gumbo_destroy_output(&kGumboDefaultOptions, host_output);
}

// --- Document quirks mode ---------------------------------------------------

static void test_quirks_mode(
    const char* host_page, GumboQuirksModeEnum expected_host_mode,
    bool table_inside_p) {
  GumboOutput* host_output = gumbo_parse(host_page);
  assert(host_output->document->v.document.doc_type_quirks_mode ==
         expected_host_mode);
  GumboNode* mount = find_by_id(host_output->root, "mount");
  assert(mount != NULL);

  // A doctype in the fragment must not re-infer the mode.
  GumboOutput* fragment = parse_fragment_with_context(mount,
      "<!doctype html><p id=p><table id=t><tr><td>x</td></tr></table>");
  assert(fragment->document->v.document.doc_type_quirks_mode ==
         expected_host_mode);

  GumboNode* p = find_by_id(fragment->root, "p");
  GumboNode* table = find_by_id(fragment->root, "t");
  assert(p != NULL && table != NULL);
  if (table_inside_p) {
    assert(table->parent == p);
  } else {
    assert(!is_descendant(p, table));
    assert(table->parent == p->parent);
  }

  gumbo_destroy_output(&kGumboDefaultOptions, fragment);
  // The host document is never modified, even though the fragment carried a
  // doctype of its own.
  assert(host_output->document->v.document.doc_type_quirks_mode ==
         expected_host_mode);
  gumbo_destroy_output(&kGumboDefaultOptions, host_output);
}

// --- Tokenizer state chosen from the real context type ----------------------

static void test_textarea_context(void) {
  GumboOutput* host_output =
      gumbo_parse("<!doctype html><textarea id=ta></textarea>");
  GumboNode* textarea = find_by_id(host_output->root, "ta");
  assert(textarea != NULL);
  assert(textarea->v.element.tag == GUMBO_TAG_TEXTAREA);

  // RCDATA means the markup is consumed as text.
  GumboOutput* fragment =
      parse_fragment_with_context(textarea, "<b id=b>x</b>");
  assert(find_by_id(fragment->root, "b") == NULL);
  GumboVector* root_children = &fragment->root->v.element.children;
  assert(root_children->length == 1);
  GumboNode* text = root_children->data[0];
  assert(text->type == GUMBO_NODE_TEXT ||
         text->type == GUMBO_NODE_WHITESPACE);
  assert(strcmp(text->v.text.text, "<b id=b>x</b>") == 0);

  gumbo_destroy_output(&kGumboDefaultOptions, fragment);
  gumbo_destroy_output(&kGumboDefaultOptions, host_output);
}

// --- Isolation between calls and destruction order --------------------------

static void test_call_isolation_and_destruction(void) {
  GumboOutput* form_host = gumbo_parse(
      "<!doctype html><form id=hostform><div id=mount></div></form>");
  GumboNode* form_mount = find_by_id(form_host->root, "mount");
  GumboNode* host_form = find_by_id(form_host->root, "hostform");

  // Two fragment outputs from the same host live at the same time; they share
  // no parser state and neither owns the borrowed nodes.
  GumboOutput* f1 = parse_fragment_with_context(form_mount,
      "<form id=inner><input id=x></form><form id=after></form>");
  GumboOutput* f2 = parse_fragment_with_context(form_mount,
      "<form id=inner><input id=y></form><form id=after2></form>");
  assert(find_by_id(f1->root, "inner") == NULL);
  assert(find_by_id(f1->root, "x") != NULL);
  assert(find_by_id(f1->root, "after") != NULL);
  assert(find_by_id(f2->root, "inner") == NULL);
  assert(find_by_id(f2->root, "y") != NULL);
  assert(find_by_id(f2->root, "after2") != NULL);

  // Destroy the first fragment output; the second and the host stay valid.
  gumbo_destroy_output(&kGumboDefaultOptions, f1);
  assert(find_by_id(f2->root, "y") != NULL);
  assert(host_form->v.element.attributes.length == 1);
  assert(form_mount->v.element.children.length == 0);

  // A different, form-less host must not inherit the borrowed form pointer.
  GumboOutput* plain_host =
      gumbo_parse("<!doctype html><div id=plain></div>");
  GumboNode* plain_mount = find_by_id(plain_host->root, "plain");
  GumboOutput* f3 =
      parse_fragment_with_context(plain_mount, "<form id=f3></form>");
  assert(find_by_id(f3->root, "f3") != NULL);
  gumbo_destroy_output(&kGumboDefaultOptions, f3);
  gumbo_destroy_output(&kGumboDefaultOptions, plain_host);

  gumbo_destroy_output(&kGumboDefaultOptions, f2);
  // Host still readable after every fragment output is gone.
  assert(find_by_id(form_host->root, "mount") == form_mount);
  gumbo_destroy_output(&kGumboDefaultOptions, form_host);

  // Quirks state must not carry between different hosts either.
  GumboOutput* quirks_host = gumbo_parse("<div id=mount></div>");
  GumboOutput* no_quirks_host =
      gumbo_parse("<!doctype html><div id=mount></div>");
  GumboNode* quirks_mount = find_by_id(quirks_host->root, "mount");
  GumboNode* no_quirks_mount = find_by_id(no_quirks_host->root, "mount");

  GumboOutput* q = parse_fragment_with_context(
      quirks_mount, "<p id=p><table id=t></table>");
  GumboOutput* nq = parse_fragment_with_context(
      no_quirks_mount, "<p id=p><table id=t></table>");
  assert(q->document->v.document.doc_type_quirks_mode ==
         GUMBO_DOCTYPE_QUIRKS);
  assert(nq->document->v.document.doc_type_quirks_mode ==
         GUMBO_DOCTYPE_NO_QUIRKS);
  assert(find_by_id(q->root, "t")->parent == find_by_id(q->root, "p"));
  assert(!is_descendant(find_by_id(nq->root, "p"),
             find_by_id(nq->root, "t")));

  // Reverse the destruction order relative to the earlier calls.
  gumbo_destroy_output(&kGumboDefaultOptions, nq);
  gumbo_destroy_output(&kGumboDefaultOptions, q);
  gumbo_destroy_output(&kGumboDefaultOptions, no_quirks_host);
  gumbo_destroy_output(&kGumboDefaultOptions, quirks_host);
}

// A real <script> node must select the script data tokenizer state.
static void test_script_context(void) {
  GumboOutput* host_output =
      gumbo_parse("<!doctype html><script id=s></script>");
  GumboNode* script = find_by_id(host_output->root, "s");
  assert(script != NULL);
  assert(script->v.element.tag == GUMBO_TAG_SCRIPT);

  GumboOutput* fragment =
      parse_fragment_with_context(script, "<b id=b>x</b>");
  assert(find_by_id(fragment->root, "b") == NULL);
  GumboVector* root_children = &fragment->root->v.element.children;
  assert(root_children->length == 1);
  GumboNode* text = root_children->data[0];
  assert(text->type == GUMBO_NODE_TEXT);
  assert(strcmp(text->v.text.text, "<b id=b>x</b>") == 0);

  gumbo_destroy_output(&kGumboDefaultOptions, fragment);
  gumbo_destroy_output(&kGumboDefaultOptions, host_output);
}

// max_errors and stop_on_first_error keep working through the node-context
// entry point.  A nested form inside a host form is a guaranteed parse error.
static void test_error_options_preserved(void) {
  GumboOutput* host_output = gumbo_parse(
      "<!doctype html><form id=f><div id=mount></div></form>");
  GumboNode* mount = find_by_id(host_output->root, "mount");
  const char* fragment = "<form id=inner></form><input id=x>";

  GumboOptions capped = kGumboDefaultOptions;
  capped.max_errors = 0;
  capped.fragment_context_node = mount;
  GumboOutput* capped_out =
      gumbo_parse_with_options(&capped, fragment, strlen(fragment));
  assert(capped_out->errors.length == 0);
  assert(capped_out->root != NULL);
  gumbo_destroy_output(&capped, capped_out);

  GumboOptions stop = kGumboDefaultOptions;
  stop.stop_on_first_error = true;
  stop.fragment_context_node = mount;
  GumboOutput* stop_out =
      gumbo_parse_with_options(&stop, fragment, strlen(fragment));
  assert(stop_out->root != NULL);
  assert(stop_out->errors.length >= 1);
  gumbo_destroy_output(&stop, stop_out);

  gumbo_destroy_output(&kGumboDefaultOptions, host_output);
}

// --- Node context takes precedence over the integer parameters --------------

static void test_node_precedence_over_tag_params(void) {
  GumboOutput* host_output = gumbo_parse(
      "<!doctype html><math><annotation-xml id=ax "
      "encoding=\"text/html\"></annotation-xml></math>");
  GumboNode* ax = find_by_id(host_output->root, "ax");
  assert(ax != NULL);

  // Deliberately contradictory tag/namespace parameters: the real node's
  // tag, namespace and attributes must win.
  GumboOptions options = kGumboDefaultOptions;
  options.fragment_context = GUMBO_TAG_DIV;
  options.fragment_namespace = GUMBO_NAMESPACE_HTML;
  options.fragment_context_node = ax;
  const char* fragment = "<a id=a>x</a>";
  GumboOutput* out =
      gumbo_parse_with_options(&options, fragment, strlen(fragment));
  GumboNode* anchor = find_by_id(out->root, "a");
  assert(anchor != NULL);
  assert(anchor->v.element.tag_namespace == GUMBO_NAMESPACE_HTML);
  gumbo_destroy_output(&options, out);
  gumbo_destroy_output(&kGumboDefaultOptions, host_output);
}

// A real <template> node must engage the template initial insertion mode.
static void test_template_context(void) {
  GumboOutput* host_output =
      gumbo_parse("<!doctype html><template id=t></template>");
  GumboNode* tmpl = find_by_id(host_output->root, "t");
  assert(tmpl != NULL);
  assert(tmpl->type == GUMBO_NODE_TEMPLATE);

  GumboOutput* fragment =
      parse_fragment_with_context(tmpl, "<div id=d></div>");
  GumboNode* div = find_by_id(fragment->root, "d");
  assert(div != NULL);
  assert(div->v.element.tag_namespace == GUMBO_NAMESPACE_HTML);
  assert(count_pointer_occurrences(fragment->root, tmpl) == 0);
  gumbo_destroy_output(&kGumboDefaultOptions, fragment);
  gumbo_destroy_output(&kGumboDefaultOptions, host_output);
}

// --- Custom allocator: no allocations outlive the fragment output -----------

typedef struct {
  size_t alloc_count;
  size_t free_count;
  size_t outstanding_bytes;
} AllocTracker;

static void* tracking_allocate(void* userdata, size_t size) {
  AllocTracker* tracker = (AllocTracker*) userdata;
  size_t* block = malloc(size + sizeof(size_t));
  assert(block != NULL);
  block[0] = size;
  tracker->alloc_count++;
  tracker->outstanding_bytes += size;
  return block + 1;
}

static void tracking_deallocate(void* userdata, void* ptr) {
  if (ptr == NULL) {
    return;
  }
  AllocTracker* tracker = (AllocTracker*) userdata;
  size_t* block = (size_t*) ptr - 1;
  tracker->free_count++;
  assert(tracker->outstanding_bytes >= block[0]);
  tracker->outstanding_bytes -= block[0];
  free(block);
}

static void test_custom_allocator_clean(void) {
  GumboOutput* host_output = gumbo_parse(
      "<!doctype html><form id=f><math><annotation-xml id=ax "
      "encoding=\"text/html\"></annotation-xml></math></form>");
  GumboNode* ax = find_by_id(host_output->root, "ax");
  assert(ax != NULL);

  AllocTracker tracker = {0, 0, 0};
  GumboOptions options = kGumboDefaultOptions;
  options.allocator = tracking_allocate;
  options.deallocator = tracking_deallocate;
  options.userdata = &tracker;
  options.fragment_context_node = ax;

  const char* fragment =
      "<form id=inner><a id=a>x</a></form><form id=after></form>";
  GumboOutput* fragment_output =
      gumbo_parse_with_options(&options, fragment, strlen(fragment));
  assert(tracker.alloc_count > 0);
  assert(find_by_id(fragment_output->root, "a") != NULL);
  assert(find_by_id(fragment_output->root, "after") != NULL);

  gumbo_destroy_output(&options, fragment_output);
  // Everything this call allocated came back through the same deallocator.
  assert(tracker.free_count == tracker.alloc_count);
  assert(tracker.outstanding_bytes == 0);

  // The host (parsed with the default allocator) is untouched and still
  // destroyable with no double-free of strings the fragment borrowed.
  assert(ax->v.element.attributes.length >= 1);
  gumbo_destroy_output(&kGumboDefaultOptions, host_output);
}

int main(void) {
  // annotation-xml: the two HTML encodings (one in upper case to exercise the
  // ASCII case-insensitive comparison), a non-HTML encoding, and no encoding.
  test_annotation_xml_encoding("text/html", GUMBO_NAMESPACE_HTML);
  test_annotation_xml_encoding(
      "APPLICATION/XHTML+XML", GUMBO_NAMESPACE_HTML);
  test_annotation_xml_encoding("application/xml", GUMBO_NAMESPACE_MATHML);
  test_annotation_xml_encoding(NULL, GUMBO_NAMESPACE_MATHML);
  test_svg_foreign_object();

  test_form_pointer_from_ancestor();
  test_form_pointer_from_context_itself();
  test_no_host_form();

  // Quirks: table stays inside <p>; no-quirks/limited-quirks: it becomes a
  // sibling.
  test_quirks_mode(
      "<div id=mount></div>", GUMBO_DOCTYPE_QUIRKS, true);
  test_quirks_mode(
      "<!doctype html><div id=mount></div>",
      GUMBO_DOCTYPE_NO_QUIRKS, false);
  test_quirks_mode(
      "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 Frameset//\" "
      "\"http://www.w3.org/TR/html4/frameset.dtd\"><div id=mount></div>",
      GUMBO_DOCTYPE_LIMITED_QUIRKS, false);

  test_textarea_context();
  test_script_context();
  test_template_context();
  test_error_options_preserved();
  test_node_precedence_over_tag_params();
  test_call_isolation_and_destruction();
  test_custom_allocator_clean();

  puts("CONTEXT_NODE_OK");
  return 0;
}
