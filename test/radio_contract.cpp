/*
 * Copyright (C) 2026 Alexander Borisov
 *
 * Author: Alexander Borisov <borisov@lexbor.com>
 */

/*
 * Contract tests for the input checkedness state, radio button group
 * exclusivity, form ownership and form reset.
 *
 * The support harness includes this file after defining unit_cases() and
 * UNIT_CHECK, and registers the returned cases.
 */

#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>
#include <lexbor/style/style.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include "contract.hpp"


/*
 * The replay example is built in-process for these contract tests.  Its
 * command line main() is disabled; the tests call lxb_form_replay_run().
 */
#define LXB_FORM_REPLAY_NO_MAIN
#include "../examples/form_replay.c"

namespace {

struct Document {
    lxb_html_document_t *document;

    explicit Document(const char *html)
    {
        document = lxb_html_document_create();
        UNIT_CHECK(document != NULL);

        lxb_status_t status = lxb_html_document_parse(
            document, reinterpret_cast<const lxb_char_t *>(html),
            std::strlen(html));
        UNIT_CHECK(status == LXB_STATUS_OK);
    }

    Document(const char *html, bool with_style)
    {
        document = lxb_html_document_create();
        UNIT_CHECK(document != NULL);

        UNIT_CHECK(lxb_style_init(document) == LXB_STATUS_OK);

        lxb_status_t status = lxb_html_document_parse(
            document, reinterpret_cast<const lxb_char_t *>(html),
            std::strlen(html));
        UNIT_CHECK(status == LXB_STATUS_OK);
    }

    ~Document()
    {
        if (document != NULL) {
            lxb_html_document_destroy(document);
        }
    }

    Document(const Document &) = delete;
    Document &operator=(const Document &) = delete;
};

lxb_dom_node_t *
node_by_id(lxb_html_document_t *document, const char *id)
{
    return lxb_dom_node_by_id(lxb_dom_interface_node(document),
                              reinterpret_cast<const lxb_char_t *>(id),
                              std::strlen(id));
}

lxb_html_input_element_t *
input_by_id(lxb_html_document_t *document, const char *id)
{
    lxb_dom_node_t *node = node_by_id(document, id);
    UNIT_CHECK(node != NULL);
    UNIT_CHECK(node->type == LXB_DOM_NODE_TYPE_ELEMENT);
    UNIT_CHECK(node->local_name == LXB_TAG_INPUT);

    return lxb_html_interface_input(node);
}

lxb_html_form_element_t *
form_by_id(lxb_html_document_t *document, const char *id)
{
    lxb_dom_node_t *node = node_by_id(document, id);
    UNIT_CHECK(node != NULL);
    UNIT_CHECK(node->type == LXB_DOM_NODE_TYPE_ELEMENT);
    UNIT_CHECK(node->local_name == LXB_TAG_FORM);

    return lxb_html_interface_form(node);
}

bool
checked(lxb_html_input_element_t *input)
{
    return lxb_html_input_element_checked(input);
}

void
select_input(lxb_html_input_element_t *input)
{
    UNIT_CHECK(lxb_html_input_element_set_checked(input, true)
               == LXB_STATUS_OK);
    UNIT_CHECK(lxb_html_input_element_checked(input) == true);
}

void
unselect_input(lxb_html_input_element_t *input)
{
    UNIT_CHECK(lxb_html_input_element_set_checked(input, false)
               == LXB_STATUS_OK);
    UNIT_CHECK(lxb_html_input_element_checked(input) == false);
}

bool
has_attribute(lxb_dom_node_t *node, const char *name)
{
    return lxb_dom_element_has_attribute(
        lxb_dom_interface_element(node),
        reinterpret_cast<const lxb_char_t *>(name), std::strlen(name));
}

void
set_attribute(lxb_dom_node_t *node, const char *name, const char *value)
{
    UNIT_CHECK(lxb_dom_element_set_attribute(
        lxb_dom_interface_element(node),
        reinterpret_cast<const lxb_char_t *>(name), std::strlen(name),
        reinterpret_cast<const lxb_char_t *>(value), std::strlen(value))
        != NULL);
}

void
remove_attribute(lxb_dom_node_t *node, const char *name)
{
    UNIT_CHECK(lxb_dom_element_remove_attribute(
        lxb_dom_interface_element(node),
        reinterpret_cast<const lxb_char_t *>(name), std::strlen(name))
        == LXB_STATUS_OK);
}

/* ------------------------------------------------------------------ */
/* checked                                                            */
/* ------------------------------------------------------------------ */

void
checked_parse_and_defaults()
{
    Document doc("<form id=f>"
                 "<input type=radio name=g id=a checked>"
                 "<input type=radio name=g id=b checked>"
                 "<input type=checkbox name=g id=c checked>"
                 "</form>");

    lxb_html_input_element_t *a = input_by_id(doc.document, "a");
    lxb_html_input_element_t *b = input_by_id(doc.document, "b");
    lxb_html_input_element_t *c = input_by_id(doc.document, "c");

    /*
     * Two checked radio buttons in one group: the parsed attribute default
     * is still visible on both, but the live state is mutually exclusive and
     * the later one in tree order wins.
     */
    UNIT_CHECK(has_attribute(node_by_id(doc.document, "a"), "checked"));
    UNIT_CHECK(has_attribute(node_by_id(doc.document, "b"), "checked"));
    UNIT_CHECK(checked(a) == false);
    UNIT_CHECK(checked(b) == true);
    UNIT_CHECK(checked(c) == true);
}

void
checked_setter_marks_dirty()
{
    Document doc("<form id=f><input type=radio name=g id=a></form>");

    lxb_html_input_element_t *a = input_by_id(doc.document, "a");
    lxb_dom_node_t *node = node_by_id(doc.document, "a");

    /* A clean control follows additions/removals of the checked attribute. */
    UNIT_CHECK(checked(a) == false);
    set_attribute(node, "checked", "");
    UNIT_CHECK(checked(a) == true);
    remove_attribute(node, "checked");
    UNIT_CHECK(checked(a) == false);

    /* The setter writes only the live state, never the content attribute. */
    select_input(a);
    UNIT_CHECK(has_attribute(node, "checked") == false);
    UNIT_CHECK(checked(a) == true);

    /*
     * After an explicit set the control is dirty: the attribute only changes
     * the default, not the current value.
     */
    set_attribute(node, "checked", "");
    UNIT_CHECK(checked(a) == true);
    remove_attribute(node, "checked");
    UNIT_CHECK(checked(a) == true);

    /* Setting the same value still marks the control dirty. */
    unselect_input(a);
    set_attribute(node, "checked", "");
    UNIT_CHECK(checked(a) == false);

    /* ... and the default is still readable from the DOM attribute. */
    UNIT_CHECK(has_attribute(node, "checked"));
    remove_attribute(node, "checked");
    UNIT_CHECK(has_attribute(node, "checked") == false);
    UNIT_CHECK(checked(a) == false);
}

void
checked_group_exclusivity()
{
    Document doc("<form id=f>"
                 "<input type=radio name=g id=a checked>"
                 "<input type=radio name=g id=b>"
                 "<input type=radio name=other id=c checked>"
                 "<input type=checkbox name=g id=d checked>"
                 "<input type=radio id=e checked>"
                 "<input type=radio id=f2 checked>"
                 "</form>");

    lxb_html_input_element_t *a = input_by_id(doc.document, "a");
    lxb_html_input_element_t *b = input_by_id(doc.document, "b");
    lxb_html_input_element_t *c = input_by_id(doc.document, "c");
    lxb_html_input_element_t *d = input_by_id(doc.document, "d");
    lxb_html_input_element_t *e = input_by_id(doc.document, "e");
    lxb_html_input_element_t *f2 = input_by_id(doc.document, "f2");

    UNIT_CHECK(checked(a) == true);
    UNIT_CHECK(checked(b) == false);

    select_input(b);
    UNIT_CHECK(checked(a) == false);
    UNIT_CHECK(checked(b) == true);

    /* The attribute default of the unselected member is untouched. */
    UNIT_CHECK(has_attribute(node_by_id(doc.document, "a"), "checked"));

    /* A different name is a different group. */
    UNIT_CHECK(checked(c) == true);
    select_input(c);
    UNIT_CHECK(checked(b) == true);

    /* Checkboxes do not take part in radio group exclusivity. */
    UNIT_CHECK(checked(d) == true);
    select_input(a);
    UNIT_CHECK(checked(d) == true);

    /* Radio buttons without a name are never mutually exclusive. */
    UNIT_CHECK(checked(e) == true);
    UNIT_CHECK(checked(f2) == true);
    select_input(e);
    UNIT_CHECK(checked(f2) == true);
}

void
checked_type_switch_and_case()
{
    Document doc("<form id=f>"
                 "<input TYPE='RaDiO' name=g id=a checked>"
                 "<input type=radio name=g id=b>"
                 "</form>");

    lxb_html_input_element_t *a = input_by_id(doc.document, "a");
    lxb_html_input_element_t *b = input_by_id(doc.document, "b");

    /* type is matched ASCII case-insensitively. */
    select_input(b);
    UNIT_CHECK(checked(a) == false);

    select_input(a);

    /* Leaving radio keeps the current state. */
    set_attribute(node_by_id(doc.document, "a"), "type", "text");
    UNIT_CHECK(checked(a) == true);

    /* Coming back to radio, the current state takes part in the group. */
    set_attribute(node_by_id(doc.document, "a"), "type", "radio");
    UNIT_CHECK(checked(a) == true);
    UNIT_CHECK(checked(b) == false);

    /* A non-radio control never clears other radio buttons. */
    select_input(b);
    set_attribute(node_by_id(doc.document, "a"), "type", "text");
    set_attribute(node_by_id(doc.document, "a"), "type", "radio");
    UNIT_CHECK(checked(b) == true);
    UNIT_CHECK(checked(a) == false);
}

void
checked_setter_ignores_disabled()
{
    Document doc("<form id=f>"
                 "<input type=radio name=g id=a disabled>"
                 "<input type=radio name=g id=b disabled checked>"
                 "</form>");

    lxb_html_input_element_t *a = input_by_id(doc.document, "a");
    lxb_html_input_element_t *b = input_by_id(doc.document, "b");

    UNIT_CHECK(checked(b) == true);
    select_input(a);
    UNIT_CHECK(checked(a) == true);
    UNIT_CHECK(checked(b) == false);

    lxb_html_form_element_t *form = form_by_id(doc.document, "f");
    UNIT_CHECK(lxb_html_form_element_reset(form) == LXB_STATUS_OK);

    /* Reset does not skip disabled controls. */
    UNIT_CHECK(checked(a) == false);
    UNIT_CHECK(checked(b) == true);
}

void
checked_style_document_steps()
{
    Document doc("<style>input{color:#000}</style>"
                 "<form id=f>"
                 "<input type=radio name=g id=a checked>"
                 "<input type=radio name=g id=b checked>"
                 "</form>", true);

    lxb_html_input_element_t *a = input_by_id(doc.document, "a");
    lxb_html_input_element_t *b = input_by_id(doc.document, "b");

    /* Parser steps run through the style-enabled dispatch chain too. */
    UNIT_CHECK(checked(a) == false);
    UNIT_CHECK(checked(b) == true);

    select_input(a);
    UNIT_CHECK(checked(a) == true);
    UNIT_CHECK(checked(b) == false);

    set_attribute(node_by_id(doc.document, "b"), "name", "h");
    select_input(b);
    UNIT_CHECK(checked(a) == true);
    UNIT_CHECK(checked(b) == true);
}

void
checked_attribute_value_path_parity()
{
    /*
     * Changing an attribute through lxb_dom_attr_set_value and through
     * lxb_dom_element_set_attribute must produce the same state.
     */
    Document doc("<form id=f>"
                 "<input type=radio name=g id=a checked>"
                 "<input type=radio name=g id=b>"
                 "</form>");

    lxb_html_input_element_t *a = input_by_id(doc.document, "a");
    lxb_html_input_element_t *b = input_by_id(doc.document, "b");
    lxb_dom_node_t *b_node = node_by_id(doc.document, "b");

    lxb_dom_attr_t *attr = lxb_dom_element_attr_by_name(
        lxb_dom_interface_element(b_node),
        reinterpret_cast<const lxb_char_t *>("name"), 4);

    UNIT_CHECK(attr != NULL);
    UNIT_CHECK(lxb_dom_attr_set_value(attr,
                                      reinterpret_cast<const lxb_char_t *>("h"),
                                      1) == LXB_STATUS_OK);
    UNIT_CHECK(lxb_html_input_element_checked(a) == true);
    UNIT_CHECK(lxb_html_input_element_checked(b) == false);

    /* Same-value change through the element path keeps the group intact. */
    set_attribute(b_node, "name", "h");
    UNIT_CHECK(lxb_html_input_element_checked(a) == true);
    UNIT_CHECK(lxb_html_input_element_checked(b) == false);

    /* Printing a radio's type via the value path leaves the state alone. */
    lxb_dom_attr_t *type = lxb_dom_element_attr_by_name(
        lxb_dom_interface_element(node_by_id(doc.document, "a")),
        reinterpret_cast<const lxb_char_t *>("type"), 4);
    UNIT_CHECK(type != NULL);
    UNIT_CHECK(lxb_dom_attr_set_value(type,
                                      reinterpret_cast<const lxb_char_t *>("radio"),
                                      5) == LXB_STATUS_OK);
    UNIT_CHECK(lxb_html_input_element_checked(a) == true);
}

/* ------------------------------------------------------------------ */
/* ownership                                                          */
/* ------------------------------------------------------------------ */

void
ownership_ancestor_and_attribute()
{
    Document doc("<div id=outer>"
                 "<form id=f>"
                 "<input type=radio name=g id=inside>"
                 "<div id=box><input type=radio name=g id=deep></div>"
                 "</form>"
                 "<input type=radio name=g id=outside form=f>"
                 "</div>"
                 "<input type=radio name=g id=orphan>");

    lxb_html_form_element_t *form = form_by_id(doc.document, "f");

    UNIT_CHECK(lxb_html_input_element_form_owner(
                   input_by_id(doc.document, "inside")) == form);
    UNIT_CHECK(lxb_html_input_element_form_owner(
                   input_by_id(doc.document, "deep")) == form);
    UNIT_CHECK(lxb_html_input_element_form_owner(
                   input_by_id(doc.document, "outside")) == form);
    UNIT_CHECK(lxb_html_input_element_form_owner(
                   input_by_id(doc.document, "orphan")) == NULL);
}

void
ownership_explicit_target_missing_and_empty()
{
    Document doc("<form id=f></form>"
                 "<input type=radio name=g id=a form=missing>"
                 "<input type=radio name=g id=b form=''>");

    UNIT_CHECK(lxb_html_input_element_form_owner(
                   input_by_id(doc.document, "a")) == NULL);
    UNIT_CHECK(lxb_html_input_element_form_owner(
                   input_by_id(doc.document, "b")) == NULL);
}

void
ownership_ignores_non_form_id()
{
    Document doc("<div id=f></div>"
                 "<form id=f2>"
                 "<input type=radio name=g id=a form=f>"
                 "</form>");

    /* The first element with the id is a div: no association, no fallback. */
    UNIT_CHECK(lxb_html_input_element_form_owner(
                   input_by_id(doc.document, "a")) == NULL);

    set_attribute(node_by_id(doc.document, "a"), "form", "f2");
    UNIT_CHECK(lxb_html_input_element_form_owner(
                   input_by_id(doc.document, "a"))
               == form_by_id(doc.document, "f2"));
}

void
ownership_form_id_change()
{
    Document doc("<form id=f>"
                 "<input type=radio name=g id=a form=f>"
                 "</form>");

    lxb_html_input_element_t *a = input_by_id(doc.document, "a");
    lxb_dom_node_t *form_node = node_by_id(doc.document, "f");

    UNIT_CHECK(lxb_html_input_element_form_owner(a)
               == form_by_id(doc.document, "f"));

    /* Renaming the form leaves the association unresolved. */
    set_attribute(form_node, "id", "renamed");
    UNIT_CHECK(lxb_html_input_element_form_owner(a) == NULL);

    /* Renaming it back restores the association. */
    set_attribute(form_node, "id", "f");
    UNIT_CHECK(lxb_html_input_element_form_owner(a)
               == form_by_id(doc.document, "f"));
}

void
ownership_two_forms_same_named_groups()
{
    Document doc("<form id=f1>"
                 "<input type=radio name=g id=a checked>"
                 "<input type=radio name=g id=b>"
                 "</form>"
                 "<form id=f2>"
                 "<input type=radio name=g id=c checked>"
                 "</form>");

    lxb_html_input_element_t *a = input_by_id(doc.document, "a");
    lxb_html_input_element_t *c = input_by_id(doc.document, "c");

    lxb_html_form_element_t *f1 = form_by_id(doc.document, "f1");
    lxb_html_form_element_t *f2 = form_by_id(doc.document, "f2");

    UNIT_CHECK(lxb_html_input_element_form_owner(a) == f1);
    UNIT_CHECK(lxb_html_input_element_form_owner(c) == f2);

    /* Same name, different form owner: different groups. */
    select_input(c);
    UNIT_CHECK(checked(a) == true);
    UNIT_CHECK(checked(c) == true);

    /* Moving c into f1 joins the same group and clears a. */
    UNIT_CHECK(lxb_dom_node_append_child(lxb_dom_interface_node(f1),
                                         node_by_id(doc.document, "c"))
               == LXB_DOM_EXCEPTION_OK);
    UNIT_CHECK(lxb_html_input_element_form_owner(c) == f1);
    UNIT_CHECK(checked(a) == false);
    UNIT_CHECK(checked(c) == true);
}

void
ownership_moving_subtree_with_checked()
{
    Document doc("<form id=f1>"
                 "<div id=box>"
                 "<input type=radio name=g id=a checked>"
                 "<input type=radio name=g id=b checked>"
                 "</div>"
                 "</form>"
                 "<form id=f2>"
                 "<input type=radio name=g id=c>"
                 "</form>");

    lxb_html_input_element_t *a = input_by_id(doc.document, "a");
    lxb_html_input_element_t *b = input_by_id(doc.document, "b");
    lxb_html_input_element_t *c = input_by_id(doc.document, "c");

    lxb_html_form_element_t *f2 = form_by_id(doc.document, "f2");

    /* Before the move, the later checked member of the group already won. */
    UNIT_CHECK(checked(a) == false);
    UNIT_CHECK(checked(b) == true);

    UNIT_CHECK(lxb_dom_node_append_child(lxb_dom_interface_node(f2),
                                         node_by_id(doc.document, "box"))
               == LXB_DOM_EXCEPTION_OK);

    UNIT_CHECK(lxb_html_input_element_form_owner(a) == f2);
    UNIT_CHECK(lxb_html_input_element_form_owner(b) == f2);
    UNIT_CHECK(lxb_html_input_element_form_owner(c) == f2);

    /*
     * The merged group is exclusive.  The box was appended after c, so in
     * the resulting tree order b is the last checked member and wins.
     */
    UNIT_CHECK(checked(a) == false);
    UNIT_CHECK(checked(b) == true);
    UNIT_CHECK(checked(c) == false);
}

void
ownership_detached_tree_isolation()
{
    Document doc("<form id=f>"
                 "<input type=radio name=g id=a checked>"
                 "</form>"
                 "<input type=radio name=g id=b checked>");

    lxb_html_input_element_t *a = input_by_id(doc.document, "a");
    lxb_html_input_element_t *b = input_by_id(doc.document, "b");

    lxb_html_form_element_t *form = form_by_id(doc.document, "f");

    /*
     * a is inside the form, b is an unowned orphan in the same document:
     * different owners, so both may stay checked.
     */
    UNIT_CHECK(lxb_html_input_element_form_owner(b) == NULL);
    UNIT_CHECK(checked(a) == true);
    UNIT_CHECK(checked(b) == true);

    /* Detach the form subtree: the ancestor form is still found locally. */
    lxb_dom_node_t *form_node = node_by_id(doc.document, "f");
    lxb_dom_node_remove(form_node);

    UNIT_CHECK(lxb_html_input_element_form_owner(a) == form);
    UNIT_CHECK(checked(a) == true);
}

void
ownership_form_attribute_in_detached_tree()
{
    Document doc("<form id=f></form>"
                 "<div id=hold>"
                 "<input type=radio name=g id=a form=f checked>"
                 "</div>");

    lxb_html_input_element_t *a = input_by_id(doc.document, "a");

    /* Inside the document, the form attribute associates the input. */
    UNIT_CHECK(lxb_html_input_element_form_owner(a)
               == form_by_id(doc.document, "f"));

    /* Once detached, form ID lookup is disabled in the new tree. */
    lxb_dom_node_t *hold = node_by_id(doc.document, "hold");
    lxb_dom_node_remove(hold);

    UNIT_CHECK(lxb_html_input_element_form_owner(a) == NULL);
    UNIT_CHECK(checked(a) == true);
}

void
ownership_separate_tree_roots()
{
    Document doc("<form id=f>"
                 "<div id=r1>"
                 "<input type=radio name=g id=a checked>"
                 "<input type=radio name=g id=b checked>"
                 "</div>"
                 "<div id=r2>"
                 "<input type=radio name=g id=c checked>"
                 "</div>"
                 "</form>");

    lxb_html_input_element_t *a = input_by_id(doc.document, "a");
    lxb_html_input_element_t *b = input_by_id(doc.document, "b");
    lxb_html_input_element_t *c = input_by_id(doc.document, "c");

    /*
     * Initially all three share one tree root, one form owner and one name:
     * the checked control last in tree order wins.
     */
    UNIT_CHECK(checked(a) == false);
    UNIT_CHECK(checked(b) == false);
    UNIT_CHECK(checked(c) == true);

    /* Detach both subtrees: separate roots must not uncheck each other. */
    lxb_dom_node_t *r1 = node_by_id(doc.document, "r1");
    lxb_dom_node_t *r2 = node_by_id(doc.document, "r2");
    lxb_dom_node_remove(r1);
    lxb_dom_node_remove(r2);

    UNIT_CHECK(lxb_html_input_element_form_owner(a) == NULL);
    UNIT_CHECK(lxb_html_input_element_form_owner(c) == NULL);
    UNIT_CHECK(checked(a) == false);
    UNIT_CHECK(checked(b) == false);
    UNIT_CHECK(checked(c) == true);

    /* Selecting a control in one detached tree leaves the other tree alone. */
    select_input(a);
    UNIT_CHECK(checked(a) == true);
    UNIT_CHECK(checked(b) == false);
    UNIT_CHECK(checked(c) == true);
    select_input(b);
    UNIT_CHECK(checked(a) == false);
    UNIT_CHECK(checked(b) == true);
    UNIT_CHECK(checked(c) == true);

    /*
     * Joining the roots makes them one group again: the control last in
     * tree order wins (the r1 subtree is appended after c).
     */
    UNIT_CHECK(lxb_dom_node_append_child(r2, r1) == LXB_DOM_EXCEPTION_OK);
    UNIT_CHECK(lxb_html_input_element_form_owner(a) == NULL);
    UNIT_CHECK(checked(b) == true);
    UNIT_CHECK(checked(c) == false);
}

void
ownership_plain_subtree_insertion()
{
    /*
     * The inserted subtree root carries no id, but it contains checked
     * radio buttons.  The target tree must become exclusive on insertion.
     */
    Document doc("<form id=f>"
                 "<input type=radio name=g id=a checked>"
                 "<div id=sink></div>"
                 "</form>");

    lxb_html_input_element_t *a = input_by_id(doc.document, "a");
    lxb_dom_node_t *sink = node_by_id(doc.document, "sink");

    UNIT_CHECK(checked(a) == true);

    Document source("<form id=f>"
                    "<div><span><input type=radio name=g id=b checked>"
                    "</span></div></form>");

    /* Import the wrapper subtree through the DOM entry point. */
    lxb_dom_node_t *wrapper = node_by_id(source.document, "f")->first_child;

    UNIT_CHECK(wrapper != NULL);
    UNIT_CHECK(wrapper->local_name == LXB_TAG_DIV);

    lxb_dom_node_t *clone = lxb_dom_node_clone(wrapper, true);
    UNIT_CHECK(clone != NULL);
    UNIT_CHECK(lxb_dom_node_append_child(sink, clone)
               == LXB_DOM_EXCEPTION_OK);

    lxb_html_input_element_t *b = input_by_id(doc.document, "b");

    UNIT_CHECK(lxb_html_input_element_form_owner(b)
               == form_by_id(doc.document, "f"));

    /* The newcomer is later in tree order and wins. */
    UNIT_CHECK(checked(a) == false);
    UNIT_CHECK(checked(b) == true);
}

/* ------------------------------------------------------------------ */
/* reset                                                              */
/* ------------------------------------------------------------------ */

void
reset_restores_defaults_and_clears_dirty()
{
    Document doc("<form id=f>"
                 "<input type=radio name=g id=a checked>"
                 "<input type=radio name=g id=b>"
                 "</form>");

    lxb_html_input_element_t *a = input_by_id(doc.document, "a");
    lxb_html_input_element_t *b = input_by_id(doc.document, "b");
    lxb_html_form_element_t *form = form_by_id(doc.document, "f");

    select_input(b);
    UNIT_CHECK(checked(a) == false);
    UNIT_CHECK(checked(b) == true);

    UNIT_CHECK(lxb_html_form_element_reset(form) == LXB_STATUS_OK);

    UNIT_CHECK(checked(a) == true);
    UNIT_CHECK(checked(b) == false);

    /* Dirty flags are cleared: attributes drive the value again. */
    remove_attribute(node_by_id(doc.document, "a"), "checked");
    UNIT_CHECK(checked(a) == false);

    set_attribute(node_by_id(doc.document, "a"), "checked", "");
    UNIT_CHECK(checked(a) == true);
    UNIT_CHECK(checked(b) == false);

    /* Changing a default after reset participates in the group again. */
    set_attribute(node_by_id(doc.document, "b"), "checked", "");
    UNIT_CHECK(checked(a) == false);
    UNIT_CHECK(checked(b) == true);
}

void
reset_only_owned_inputs()
{
    Document doc("<form id=f>"
                 "<input type=radio name=g id=a checked>"
                 "</form>"
                 "<input type=radio name=g id=other checked>"
                 "<form id=g>"
                 "<input type=radio name=h id=inner>"
                 "</form>");

    lxb_html_input_element_t *a = input_by_id(doc.document, "a");
    lxb_html_input_element_t *other = input_by_id(doc.document, "other");
    lxb_html_input_element_t *inner = input_by_id(doc.document, "inner");

    select_input(other);
    select_input(inner);

    UNIT_CHECK(lxb_html_form_element_reset(form_by_id(doc.document, "f"))
               == LXB_STATUS_OK);

    UNIT_CHECK(checked(a) == true);
    UNIT_CHECK(checked(other) == true);
    UNIT_CHECK(checked(inner) == true);
}

void
reset_external_and_tree_order()
{
    Document doc("<form id=f>"
                 "<input type=radio name=g id=a checked>"
                 "<div id=box>"
                 "<input type=radio name=g id=b checked>"
                 "</div>"
                 "</form>"
                 "<input type=radio name=g id=ext checked form=f>");

    lxb_html_input_element_t *a = input_by_id(doc.document, "a");
    lxb_html_input_element_t *b = input_by_id(doc.document, "b");
    lxb_html_input_element_t *ext = input_by_id(doc.document, "ext");

    lxb_html_form_element_t *form = form_by_id(doc.document, "f");

    /* After parsing, the last checked member of the group owns the state. */
    UNIT_CHECK(checked(a) == false);
    UNIT_CHECK(checked(b) == false);
    UNIT_CHECK(checked(ext) == true);

    UNIT_CHECK(lxb_html_form_element_reset(form) == LXB_STATUS_OK);

    /*
     * Restored defaults are normalized in tree order including the external
     * control associated through its form attribute: the last one wins.
     */
    UNIT_CHECK(checked(a) == false);
    UNIT_CHECK(checked(b) == false);
    UNIT_CHECK(checked(ext) == true);
}

void
reset_after_attribute_default_change()
{
    Document doc("<form id=f>"
                 "<input type=radio name=g id=a checked>"
                 "<input type=radio name=g id=b>"
                 "</form>");

    lxb_html_input_element_t *a = input_by_id(doc.document, "a");
    lxb_html_input_element_t *b = input_by_id(doc.document, "b");

    lxb_html_form_element_t *form = form_by_id(doc.document, "f");

    /* Dirty b by an explicit set, then change defaults in the DOM. */
    select_input(b);
    remove_attribute(node_by_id(doc.document, "a"), "checked");
    set_attribute(node_by_id(doc.document, "b"), "checked", "");

    /* While dirty, b ignores the attribute. */
    UNIT_CHECK(checked(b) == true);
    UNIT_CHECK(checked(a) == false);

    UNIT_CHECK(lxb_html_form_element_reset(form) == LXB_STATUS_OK);

    /* Reset restores the current attribute defaults, not the parse state. */
    UNIT_CHECK(checked(a) == false);
    UNIT_CHECK(checked(b) == true);

    /* And afterwards attributes drive the value again. */
    remove_attribute(node_by_id(doc.document, "b"), "checked");
    UNIT_CHECK(checked(b) == false);
    set_attribute(node_by_id(doc.document, "a"), "checked", "");
    UNIT_CHECK(checked(a) == true);
}

void
reset_style_document()
{
    Document doc("<style>form{display:block}</style>"
                 "<form id=f>"
                 "<input type=radio name=g id=a checked>"
                 "<input type=radio name=g id=b>"
                 "</form>", true);

    lxb_html_input_element_t *a = input_by_id(doc.document, "a");
    lxb_html_input_element_t *b = input_by_id(doc.document, "b");

    select_input(b);
    UNIT_CHECK(checked(a) == false);

    UNIT_CHECK(lxb_html_form_element_reset(form_by_id(doc.document, "f"))
               == LXB_STATUS_OK);

    UNIT_CHECK(checked(a) == true);
    UNIT_CHECK(checked(b) == false);
}

/* ------------------------------------------------------------------ */
/* replay example                                                     */
/* ------------------------------------------------------------------ */

std::string
replay(const char *actions, int *status)
{
    std::FILE *out = tmpfile();
    UNIT_CHECK(out != NULL);

    *status = lxb_form_replay_run(
        "test/files/lexbor/html/form_replay/form_a.html",
        "test/files/lexbor/html/form_replay/form_b.html", actions, out);

    std::rewind(out);

    std::string result;
    char buffer[256];

    while (std::fgets(buffer, (int) sizeof(buffer), out) != NULL) {
        result += buffer;
    }

    std::fclose(out);

    return result;
}

void
replay_is_repeatable()
{
    static const char actions[] =
        "select A ext1\n"
        "move A holder alpha\n"
        "reset A alpha\n"
        "move B b4 gamma\n"
        "reset B gamma\n";

    int first_status, second_status;
    std::string first = replay(actions, &first_status);
    std::string second = replay(actions, &second_status);

    UNIT_CHECK(first_status == 0);
    UNIT_CHECK(second_status == 0);
    UNIT_CHECK(first == second);

    /* Independently written expected record block. */
    static const std::string expected =
        "ENTRY alpha color=red\n"
        "ENTRY alpha size=l\n"
        "ENTRY beta color=red\n"
        "ENTRY gamma color=green\n"
        "REPLAY_OK\n";

    UNIT_CHECK(first == expected);
}

void
replay_external_and_reset_records()
{
    int selected_status, reset_status;

    /* Ext1 is outside the alpha subtree but belongs to it via form="alpha". */
    std::string selected = replay("select A ext1\n", &selected_status);

    UNIT_CHECK(selected_status == 0);
    UNIT_CHECK(selected.find("ENTRY alpha color=blue\n") != std::string::npos);
    UNIT_CHECK(selected.find("ENTRY alpha color=red\n") == std::string::npos);
    UNIT_CHECK(selected.find("ENTRY beta color=red\n") != std::string::npos);

    /* Reset restores alpha's checked content-attribute defaults. */
    std::string reset = replay("select A ext1\nreset A alpha\n", &reset_status);

    UNIT_CHECK(reset_status == 0);
    UNIT_CHECK(reset.find("ENTRY alpha color=red\n") != std::string::npos);
    UNIT_CHECK(reset.find("ENTRY alpha color=blue\n") == std::string::npos);
    UNIT_CHECK(reset.find("ENTRY beta color=red\n") != std::string::npos);
}

void
replay_move_keeps_single_entry()
{
    static const char actions[] =
        "select A ext1\n"
        "move A holder alpha\n"
        "move A a1 sink\n";

    int status;
    std::string result = replay(actions, &status);

    UNIT_CHECK(status == 0);

    /* alpha keeps exactly one checked color record after the subtree merge. */
    size_t first = result.find("ENTRY alpha color=");
    UNIT_CHECK(first != std::string::npos);
    UNIT_CHECK(result.find("ENTRY alpha color=", first + 1)
               == std::string::npos);

    /* The merged color group's selected control is ext1. */
    UNIT_CHECK(result.find("ENTRY alpha color=blue\n") != std::string::npos);

    /* The moved-out a1 is gone from alpha; its size group entry remains. */
    UNIT_CHECK(result.find("ENTRY alpha color=red\n") == std::string::npos);
    UNIT_CHECK(result.find("ENTRY alpha size=l\n") != std::string::npos);
}

void
replay_unselect_and_reset()
{
    static const char actions[] =
        "select A a2\n"
        "reset A alpha\n";

    int status;
    std::string result = replay(actions, &status);

    UNIT_CHECK(status == 0);

    /* After reset the alpha group is back to its recorded default. */
    UNIT_CHECK(result.find("ENTRY alpha color=red\n") != std::string::npos);
    UNIT_CHECK(result.find("ENTRY alpha color=green\n") == std::string::npos);
    UNIT_CHECK(result.find("ENTRY alpha size=l\n") == std::string::npos);
}

} /* namespace */


std::vector<uf::Case>
unit_cases()
{
    return {
        {"checked.parse_and_defaults", "checked", checked_parse_and_defaults},
        {"checked.setter_marks_dirty", "checked", checked_setter_marks_dirty},
        {"checked.group_exclusivity", "checked", checked_group_exclusivity},
        {"checked.type_switch_and_case", "checked", checked_type_switch_and_case},
        {"checked.setter_ignores_disabled", "checked", checked_setter_ignores_disabled},
        {"checked.style_document_steps", "checked", checked_style_document_steps},
        {"checked.attribute_value_path_parity", "checked", checked_attribute_value_path_parity},
        {"ownership.ancestor_and_attribute", "ownership", ownership_ancestor_and_attribute},
        {"ownership.explicit_target_and_empty", "ownership", ownership_explicit_target_missing_and_empty},
        {"ownership.ignores_non_form_id", "ownership", ownership_ignores_non_form_id},
        {"ownership.form_id_change", "ownership", ownership_form_id_change},
        {"ownership.two_forms_same_named_groups", "ownership", ownership_two_forms_same_named_groups},
        {"ownership.moving_subtree_with_checked", "ownership", ownership_moving_subtree_with_checked},
        {"ownership.detached_tree_isolation", "ownership", ownership_detached_tree_isolation},
        {"ownership.form_attribute_in_detached_tree", "ownership", ownership_form_attribute_in_detached_tree},
        {"ownership.separate_tree_roots", "ownership", ownership_separate_tree_roots},
        {"ownership.plain_subtree_insertion", "ownership", ownership_plain_subtree_insertion},
        {"reset.restores_defaults_and_clears_dirty", "reset", reset_restores_defaults_and_clears_dirty},
        {"reset.only_owned_inputs", "reset", reset_only_owned_inputs},
        {"reset.external_and_tree_order", "reset", reset_external_and_tree_order},
        {"reset.after_attribute_default_change", "reset", reset_after_attribute_default_change},
        {"reset.style_document", "reset", reset_style_document},
        {"reset.replay_is_repeatable", "reset", replay_is_repeatable},
        {"reset.replay_external_and_reset_records", "reset", replay_external_and_reset_records},
        {"reset.replay_move_keeps_single_entry", "reset", replay_move_keeps_single_entry},
        {"reset.replay_unselect_and_reset", "reset", replay_unselect_and_reset},
    };
}
