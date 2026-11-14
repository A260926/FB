/*
 * Copyright (C) 2026 Alexander Borisov
 *
 * Author: Alexander Borisov <borisov@lexbor.com>
 */

#include "lexbor/html/input_steps.h"
#include "lexbor/html/interfaces/input_element.h"
#include "lexbor/html/interfaces/form_element.h"
#include "lexbor/dom/interfaces/document.h"
#include "lexbor/dom/interfaces/element.h"
#include "lexbor/dom/interfaces/attr.h"
#include "lexbor/dom/interfaces/node.h"
#include "lexbor/core/str.h"

#include "lexbor/tag/const.h"
#include "lexbor/ns/const.h"


typedef struct {
    lxb_dom_node_t *target;
    bool           seen;
    bool           found;
}
lxb_html_input_later_t;

typedef struct {
    lxb_html_form_element_t *form;
}
lxb_html_input_reset_t;


/*
 * Basic tree helpers.
 */
static lxb_dom_node_t *
lxb_html_input_steps_tree_root(lxb_dom_node_t *node)
{
    while (node->parent != NULL) {
        node = node->parent;
    }

    return node;
}

static bool
lxb_html_input_steps_in_document_tree(lxb_dom_node_t *node)
{
    return lxb_html_input_steps_tree_root(node)->type
           == LXB_DOM_NODE_TYPE_DOCUMENT;
}

static bool
lxb_html_input_steps_is_input(lxb_dom_node_t *node)
{
    return node->type == LXB_DOM_NODE_TYPE_ELEMENT
           && node->ns == LXB_NS_HTML
           && node->local_name == LXB_TAG_INPUT;
}

static bool
lxb_html_input_steps_is_form(lxb_dom_node_t *node)
{
    return node->type == LXB_DOM_NODE_TYPE_ELEMENT
           && node->ns == LXB_NS_HTML
           && node->local_name == LXB_TAG_FORM;
}

static lxb_dom_attr_t *
lxb_html_input_steps_attr_by_name(lxb_dom_element_t *element,
                                  const char *name, size_t length)
{
    return lxb_dom_element_attr_by_name(element, (const lxb_char_t *) name,
                                        length);
}

/*
 * Check whether a mutation callback reports the given attribute name.
 *
 * "name", "value" and "form" are not in the built-in attribute table, so
 * their IDs are dynamic data pointers.  The attribute is looked up on the
 * element first (available for append/change); for removal the attribute is
 * already unlinked and the lookup falls back to the document attribute hash.
 */
static bool
lxb_html_input_steps_attr_id_is(lxb_dom_element_t *element,
                                lxb_dom_attr_id_t name,
                                const char *qname, size_t length,
                                lxb_dom_attr_t *attr)
{
    const lxb_dom_attr_data_t *data;

    if (attr != NULL && (name == attr->node.local_name
                         || name == attr->qualified_name))
    {
        return true;
    }

    data = lxb_dom_attr_data_by_local_name(element->node.owner_document->attrs,
                                           (const lxb_char_t *) qname, length);
    if (data == NULL) {
        return false;
    }

    return name == data->attr_id;
}

static bool
lxb_html_input_steps_has_checked(lxb_html_input_element_t *input)
{
    return lxb_dom_element_attr_by_id(&input->element.element,
                                      LXB_DOM_ATTR_CHECKED) != NULL;
}

bool
lxb_html_input_steps_is_radio(lxb_html_input_element_t *input)
{
    lxb_dom_attr_t *attr;
    const lxb_char_t *value;
    size_t length;

    attr = lxb_dom_element_attr_by_id(&input->element.element,
                                      LXB_DOM_ATTR_TYPE);
    if (attr == NULL) {
        return false;
    }

    value = lxb_dom_attr_value(attr, &length);
    if (value == NULL || length != 5) {
        return false;
    }

    return lexbor_str_data_ncasecmp(value, (const lxb_char_t *) "radio", 5);
}

static bool
lxb_html_input_steps_name(lxb_html_input_element_t *input,
                          const lxb_char_t **name, size_t *length)
{
    lxb_dom_attr_t *attr;

    attr = lxb_html_input_steps_attr_by_name(&input->element.element,
                                             "name", 4);
    if (attr == NULL) {
        return false;
    }

    *name = lxb_dom_attr_value(attr, length);
    if (*name == NULL || *length == 0) {
        return false;
    }

    return true;
}


/*
 * Form owner.
 *
 * The owner is cached on the input.  Mutation steps and reset keep the cache
 * up to date for the affected trees; the getter only resolves a single input
 * when the cache has never been filled (for example, for controls created by
 * the caller outside the mutation steps).
 */
static lxb_html_form_element_t *
lxb_html_input_steps_compute_owner(lxb_html_input_element_t *input)
{
    lxb_dom_node_t *node, *found;
    lxb_dom_attr_t *attr;
    const lxb_char_t *value;
    size_t length;

    node = lxb_dom_interface_node(input);

    attr = lxb_html_input_steps_attr_by_name(&input->element.element,
                                             "form", 4);

    if (attr != NULL) {
        value = lxb_dom_attr_value(attr, &length);

        if (value == NULL || length == 0) {
            return NULL;
        }

        /* Detached trees do not resolve form IDs. */
        if (!lxb_html_input_steps_in_document_tree(node)) {
            return NULL;
        }

        found = lxb_dom_node_by_id(lxb_html_input_steps_tree_root(node),
                                   value, length);

        if (found != NULL && lxb_html_input_steps_is_form(found)) {
            return lxb_html_interface_form(found);
        }

        return NULL;
    }

    node = node->parent;

    while (node != NULL) {
        if (lxb_html_input_steps_is_form(node)) {
            return lxb_html_interface_form(node);
        }

        node = node->parent;
    }

    return NULL;
}

static lxb_html_form_element_t *
lxb_html_input_steps_owner_refresh(lxb_html_input_element_t *input)
{
    input->form_owner = lxb_html_input_steps_compute_owner(input);
    input->form_owner_set = true;

    return input->form_owner;
}

static lxb_html_form_element_t *
lxb_html_input_steps_owner_get(lxb_html_input_element_t *input)
{
    if (!input->form_owner_set) {
        return lxb_html_input_steps_owner_refresh(input);
    }

    return input->form_owner;
}

static lexbor_action_t
lxb_html_input_steps_owner_walker(lxb_dom_node_t *node, void *ctx)
{
    (void) ctx;

    if (lxb_html_input_steps_is_input(node)) {
        (void) lxb_html_input_steps_owner_refresh(
                  lxb_html_interface_input(node));
    }

    return LEXBOR_ACTION_OK;
}

static void
lxb_html_input_steps_owners_refresh(lxb_dom_node_t *root)
{
    if (lxb_html_input_steps_is_input(root)) {
        (void) lxb_html_input_steps_owner_refresh(
                  lxb_html_interface_input(root));
    }

    lxb_dom_node_simple_walk(root, lxb_html_input_steps_owner_walker, NULL);
}

typedef struct {
    bool input;
    bool id;
    bool form;
    bool checked_radio;
}
lxb_html_input_steps_flags_t;

static void
lxb_html_input_steps_flags_apply(lxb_dom_node_t *node, void *ctx)
{
    lxb_html_input_steps_flags_t *flags = ctx;
    lxb_html_input_element_t *input;

    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        return;
    }

    if (lxb_dom_interface_element(node)->attr_id != NULL) {
        flags->id = true;
    }

    if (!lxb_html_input_steps_is_input(node)) {
        if (lxb_html_input_steps_is_form(node)) {
            flags->form = true;
        }

        return;
    }

    input = lxb_html_interface_input(node);
    flags->input = true;

    if (input->checkedness && lxb_html_input_steps_is_radio(input)) {
        flags->checked_radio = true;
    }
}

static lexbor_action_t
lxb_html_input_steps_flags_walker(lxb_dom_node_t *node, void *ctx)
{
    lxb_html_input_steps_flags_apply(node, ctx);
    return LEXBOR_ACTION_OK;
}

static lxb_html_input_steps_flags_t
lxb_html_input_steps_subtree_flags(lxb_dom_node_t *node)
{
    lxb_html_input_steps_flags_t flags;

    flags.input = false;
    flags.id = false;
    flags.form = false;
    flags.checked_radio = false;

    lxb_html_input_steps_flags_apply(node, &flags);

    lxb_dom_node_simple_walk(node, lxb_html_input_steps_flags_walker, &flags);

    return flags;
}

static bool
lxb_html_input_steps_tree_has_input(lxb_dom_node_t *root)
{
    lxb_html_input_steps_flags_t flags = lxb_html_input_steps_subtree_flags(root);
    return flags.input;
}



/*
 * Radio button group normalization.
 *
 * Two radio buttons are in the same group when they have the same non-empty
 * name, the same tree root, the same form owner and a radio type.  When a
 * group ends up with several checked members (parse, subtree insertion,
 * attribute-driven group change, reset), the last one in tree order wins.
 */
static bool
lxb_html_input_steps_same_group(lxb_html_input_element_t *first,
                                lxb_html_input_element_t *second)
{
    size_t flen, slen;
    const lxb_char_t *fname, *sname;
    lxb_dom_node_t *fnode, *snode;

    if (!lxb_html_input_steps_is_radio(first)
        || !lxb_html_input_steps_is_radio(second))
    {
        return false;
    }

    if (!lxb_html_input_steps_name(first, &fname, &flen)
        || !lxb_html_input_steps_name(second, &sname, &slen))
    {
        return false;
    }

    if (flen != slen || !lexbor_str_data_ncmp(fname, sname, flen)) {
        return false;
    }

    fnode = lxb_dom_interface_node(first);
    snode = lxb_dom_interface_node(second);

    if (lxb_html_input_steps_tree_root(fnode)
        != lxb_html_input_steps_tree_root(snode))
    {
        return false;
    }

    return lxb_html_input_steps_owner_get(first)
           == lxb_html_input_steps_owner_get(second);
}

static lexbor_action_t
lxb_html_input_steps_later_walker(lxb_dom_node_t *node, void *ctx)
{
    lxb_html_input_later_t *context = ctx;
    lxb_html_input_element_t *input;

    if (lxb_html_input_steps_is_input(node)) {
        input = lxb_html_interface_input(node);

        if (context->seen && input->checkedness
            && lxb_html_input_steps_same_group(
                    lxb_html_interface_input(context->target), input))
        {
            context->found = true;
            return LEXBOR_ACTION_STOP;
        }
    }

    if (node == context->target) {
        context->seen = true;
    }

    return LEXBOR_ACTION_OK;
}

static bool
lxb_html_input_steps_has_later(lxb_dom_node_t *root, lxb_dom_node_t *target)
{
    lxb_html_input_later_t context;

    context.target = target;
    context.seen = (root == target);
    context.found = false;

    lxb_dom_node_simple_walk(root, lxb_html_input_steps_later_walker,
                             &context);

    return context.found;
}

static void
lxb_html_input_steps_normalize_node(lxb_dom_node_t *node, lxb_dom_node_t *root)
{
    lxb_html_input_element_t *input;

    if (!lxb_html_input_steps_is_input(node)) {
        return;
    }

    input = lxb_html_interface_input(node);

    if (input->checkedness && lxb_html_input_steps_is_radio(input)
        && lxb_html_input_steps_has_later(root, node))
    {
        input->checkedness = false;
    }
}

static lexbor_action_t
lxb_html_input_steps_normalize_walker(lxb_dom_node_t *node, void *ctx)
{
    lxb_html_input_steps_normalize_node(node, ctx);
    return LEXBOR_ACTION_OK;
}

static void
lxb_html_input_steps_normalize(lxb_dom_node_t *root)
{
    lxb_html_input_steps_normalize_node(root, root);

    lxb_dom_node_simple_walk(root, lxb_html_input_steps_normalize_walker,
                             root);
}

static void
lxb_html_input_steps_normalize_tree(lxb_dom_node_t *node)
{
    lxb_html_input_steps_normalize(lxb_html_input_steps_tree_root(node));
}

/*
 * Uncheck the other checked radio buttons of the group.  Used when a control
 * explicitly becomes checked: the target always wins.
 */
static lexbor_action_t
lxb_html_input_steps_uncheck_walker(lxb_dom_node_t *node, void *ctx)
{
    lxb_html_input_element_t *target = ctx;
    lxb_html_input_element_t *input;

    if (!lxb_html_input_steps_is_input(node)) {
        return LEXBOR_ACTION_OK;
    }

    input = lxb_html_interface_input(node);

    if (input != target && input->checkedness
        && lxb_html_input_steps_same_group(target, input))
    {
        input->checkedness = false;
    }

    return LEXBOR_ACTION_OK;
}

static void
lxb_html_input_steps_uncheck_others(lxb_html_input_element_t *input)
{
    lxb_dom_node_t *root, *node;
    const lxb_char_t *name;
    size_t length;

    if (!lxb_html_input_steps_is_radio(input)) {
        return;
    }

    if (!lxb_html_input_steps_name(input, &name, &length)) {
        return;
    }

    (void) lxb_html_input_steps_owner_get(input);

    node = lxb_dom_interface_node(input);
    root = lxb_html_input_steps_tree_root(node);

    if (lxb_html_input_steps_is_input(root)) {
        (void) lxb_html_input_steps_uncheck_walker(root, input);
    }

    lxb_dom_node_simple_walk(root, lxb_html_input_steps_uncheck_walker,
                             input);
}

/*
 * Attribute handling.
 */
static void
lxb_html_input_steps_sync_checked(lxb_html_input_element_t *input)
{
    if (input->dirty_checkedness) {
        /* After an explicit set the attribute only changes the default. */
        return;
    }

    if (!lxb_html_input_steps_has_checked(input)) {
        input->checkedness = false;
        return;
    }

    input->checkedness = true;
    lxb_html_input_steps_uncheck_others(input);
}

lxb_status_t
lxb_html_input_steps_attr(lxb_dom_element_t *element,
                          lxb_dom_attr_id_t name,
                          lxb_html_input_attr_action_t action)
{
    lxb_dom_node_t *node = &element->node;
    lxb_dom_attr_t *attr;
    bool is_form_attr;

    (void) action;

    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        return LXB_STATUS_OK;
    }

    if (lxb_html_input_steps_is_input(node)) {
        lxb_html_input_element_t *input = lxb_html_interface_input(node);
        lxb_dom_node_t *root;

        if (name == LXB_DOM_ATTR_CHECKED) {
            lxb_html_input_steps_sync_checked(input);
            return LXB_STATUS_OK;
        }

        if (name == LXB_DOM_ATTR_ID) {
            /*
             * The input's own id may be the target of other inputs' form
             * attributes (only a form counts), so refresh the tree.
             */
            root = lxb_html_input_steps_tree_root(node);
            lxb_html_input_steps_owners_refresh(root);
            lxb_html_input_steps_normalize(root);
            return LXB_STATUS_OK;
        }

        attr = lxb_html_input_steps_attr_by_name(element, "form", 4);
        is_form_attr = lxb_html_input_steps_attr_id_is(element, name, "form",
                                                       4, attr);

        if (is_form_attr) {
            /* The input itself switches (or loses) its form ownership. */
            (void) lxb_html_input_steps_owner_refresh(input);
            lxb_html_input_steps_normalize_tree(node);
            return LXB_STATUS_OK;
        }

        if (name == LXB_DOM_ATTR_TYPE
            || lxb_html_input_steps_attr_id_is(element, name, "type", 4,
                        lxb_dom_element_attr_by_id(element, LXB_DOM_ATTR_TYPE))
            || lxb_html_input_steps_attr_id_is(element, name, "name", 4,
                        lxb_html_input_steps_attr_by_name(element, "name", 4)))
        {
            /* Group composition changed: make the new group exclusive. */
            lxb_html_input_steps_normalize_tree(node);
        }

        return LXB_STATUS_OK;
    }

    if (name == LXB_DOM_ATTR_ID) {
        /*
         * An id mutation may change the target of "form" attributes in this
         * tree, so the form owners of the affected tree are refreshed.  Trees
         * without inputs cannot be affected.
         */
        lxb_dom_node_t *root = lxb_html_input_steps_tree_root(node);

        if (lxb_html_input_steps_tree_has_input(root)) {
            lxb_html_input_steps_owners_refresh(root);
            lxb_html_input_steps_normalize(root);
        }
    }

    return LXB_STATUS_OK;
}


/*
 * Structural steps.
 */
void
lxb_html_input_steps_insertion(lxb_dom_node_t *node)
{
    lxb_html_input_steps_flags_t flags;

    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        return;
    }

    flags = lxb_html_input_steps_subtree_flags(node);

    if (!flags.input && !flags.id && !flags.form) {
        return;
    }

    if (flags.input || flags.form) {
        /* The inserted subtree's own inputs get new ancestors/owners. */
        lxb_html_input_steps_owners_refresh(node);
    }

    if (flags.id) {
        /*
         * A new element carrying an id can become the target of "form"
         * attributes and can change an earlier duplicate-id resolution.
         */
        lxb_dom_node_t *root = lxb_html_input_steps_tree_root(node);

        if (lxb_html_input_steps_tree_has_input(root)) {
            lxb_html_input_steps_owners_refresh(root);
            lxb_html_input_steps_normalize(root);
        }

        return;
    }

    if (flags.checked_radio) {
        /* The inserted checked member joins its new group immediately. */
        lxb_html_input_steps_normalize(
            lxb_html_input_steps_tree_root(node));
    }
}

void
lxb_html_input_steps_removal(lxb_dom_node_t *node,
                             lxb_dom_node_t *old_parent)
{
    lxb_html_input_steps_flags_t flags;
    lxb_dom_node_t *root;

    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        return;
    }

    flags = lxb_html_input_steps_subtree_flags(node);

    if (!flags.input && !flags.id && !flags.form) {
        return;
    }

    /*
     * The removed subtree may still find ancestor forms, but ID-based
     * association is disabled outside the document.
     */
    if (flags.input || flags.form) {
        lxb_html_input_steps_owners_refresh(node);
    }

    if (old_parent == NULL || !flags.id) {
        return;
    }

    /*
     * Dropping an id target can change the form owner (and therefore the
     * group composition) of inputs in the old tree.
     */
    root = lxb_html_input_steps_tree_root(old_parent);

    if (lxb_html_input_steps_tree_has_input(root)) {
        lxb_html_input_steps_owners_refresh(root);
        lxb_html_input_steps_normalize(root);
    }
}


/*
 * Reset.
 */
static lexbor_action_t
lxb_html_input_steps_reset_walker(lxb_dom_node_t *node, void *ctx)
{
    lxb_html_input_reset_t *context = ctx;
    lxb_html_input_element_t *input;

    if (!lxb_html_input_steps_is_input(node)) {
        return LEXBOR_ACTION_OK;
    }

    input = lxb_html_interface_input(node);

    if (lxb_html_input_steps_owner_get(input) != context->form) {
        return LEXBOR_ACTION_OK;
    }

    input->dirty_checkedness = false;
    input->checkedness = lxb_html_input_steps_has_checked(input);

    return LEXBOR_ACTION_OK;
}

lxb_status_t
lxb_html_input_steps_reset(lxb_html_form_element_t *form)
{
    lxb_dom_node_t *root;
    lxb_html_input_reset_t context;

    if (form == NULL) {
        return LXB_STATUS_ERROR_OBJECT_IS_NULL;
    }

    root = lxb_html_input_steps_tree_root(lxb_dom_interface_node(form));

    /* Make sure ownership decisions use the current tree. */
    lxb_html_input_steps_owners_refresh(root);

    context.form = form;

    if (lxb_html_input_steps_is_input(root)) {
        (void) lxb_html_input_steps_reset_walker(root, &context);
    }

    lxb_dom_node_simple_walk(root, lxb_html_input_steps_reset_walker,
                             &context);

    /* Restored defaults are normalized in tree order: the last one wins. */
    lxb_html_input_steps_normalize(root);

    return LXB_STATUS_OK;
}


/*
 * Public API support.
 */
bool
lxb_html_input_steps_checked(lxb_html_input_element_t *input)
{
    return input->checkedness;
}

void
lxb_html_input_steps_set_checked(lxb_html_input_element_t *input, bool checked)
{
    input->dirty_checkedness = true;

    if (checked) {
        input->checkedness = true;
        lxb_html_input_steps_uncheck_others(input);
    }
    else {
        input->checkedness = false;
    }
}

lxb_html_form_element_t *
lxb_html_input_steps_form_owner(lxb_html_input_element_t *input)
{
    return lxb_html_input_steps_owner_get(input);
}
