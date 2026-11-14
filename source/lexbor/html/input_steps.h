/*
 * Copyright (C) 2026 Alexander Borisov
 *
 * Author: Alexander Borisov <borisov@lexbor.com>
 */

#ifndef LEXBOR_HTML_INPUT_STEPS_H
#define LEXBOR_HTML_INPUT_STEPS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lexbor/html/interface.h"
#include "lexbor/dom/interfaces/element.h"


typedef enum {
    LXB_HTML_INPUT_ATTR_CHANGE = 0x00,
    LXB_HTML_INPUT_ATTR_APPEND,
    LXB_HTML_INPUT_ATTR_REMOVE,
    LXB_HTML_INPUT_ATTR_REPLACE
}
lxb_html_input_attr_action_t;


/*
 * Shared attribute step. Called by the HTML and style attribute dispatchers
 * for every element before the per-tag callback is selected.
 */
LXB_API lxb_status_t
lxb_html_input_steps_attr(lxb_dom_element_t *element,
                          lxb_dom_attr_id_t name,
                          lxb_html_input_attr_action_t action);

/*
 * Shared insertion/removal steps. Called by the HTML and style element
 * dispatchers for every node.
 */
LXB_API void
lxb_html_input_steps_insertion(lxb_dom_node_t *node);

LXB_API void
lxb_html_input_steps_removal(lxb_dom_node_t *node,
                             lxb_dom_node_t *old_parent);

/*
 * Reset the inputs owned by form to their checked content-attribute
 * defaults and clear their dirty checkedness.
 */
LXB_API lxb_status_t
lxb_html_input_steps_reset(lxb_html_form_element_t *form);

/*
 * Helpers used by the public input API.
 */
LXB_API bool
lxb_html_input_steps_checked(lxb_html_input_element_t *input);

LXB_API void
lxb_html_input_steps_set_checked(lxb_html_input_element_t *input,
                                 bool checked);

LXB_API lxb_html_form_element_t *
lxb_html_input_steps_form_owner(lxb_html_input_element_t *input);

LXB_API bool
lxb_html_input_steps_is_radio(lxb_html_input_element_t *input);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LEXBOR_HTML_INPUT_STEPS_H */
