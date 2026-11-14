/*
 * Copyright (C) 2018 Alexander Borisov
 *
 * Author: Alexander Borisov <borisov@lexbor.com>
 */

#include "lexbor/html/interfaces/input_element.h"
#include "lexbor/html/interfaces/form_element.h"
#include "lexbor/html/interfaces/document.h"
#include "lexbor/html/input_steps.h"


lxb_html_input_element_t *
lxb_html_input_element_interface_create(lxb_html_document_t *document)
{
    lxb_html_input_element_t *element;

    element = lexbor_mraw_calloc(document->dom_document.mraw,
                                 sizeof(lxb_html_input_element_t));
    if (element == NULL) {
        return NULL;
    }

    lxb_dom_node_t *node = lxb_dom_interface_node(element);

    /*
     * checkedness and dirty checkedness default to false.
     * The parser appends the "checked" attribute through the DOM attribute
     * mutation steps, which sync the checkedness during parsing.
     */

    node->owner_document = lxb_html_document_original_ref(document);
    node->type = LXB_DOM_NODE_TYPE_ELEMENT;

    return element;
}

lxb_html_input_element_t *
lxb_html_input_element_interface_destroy(lxb_html_input_element_t *input_element)
{
    (void) lxb_dom_node_interface_destroy(lxb_dom_interface_node(input_element));
    return NULL;
}

bool
lxb_html_input_element_checked(lxb_html_input_element_t *input)
{
    if (input == NULL) {
        return false;
    }

    return lxb_html_input_steps_checked(input);
}

lxb_status_t
lxb_html_input_element_set_checked(lxb_html_input_element_t *input,
                                   bool checked)
{
    if (input == NULL) {
        return LXB_STATUS_ERROR_OBJECT_IS_NULL;
    }

    lxb_html_input_steps_set_checked(input, checked);

    return LXB_STATUS_OK;
}

lxb_html_form_element_t *
lxb_html_input_element_form_owner(lxb_html_input_element_t *input)
{
    if (input == NULL) {
        return NULL;
    }

    return lxb_html_input_steps_form_owner(input);
}
