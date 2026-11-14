/*
 * Copyright (C) 2018 Alexander Borisov
 *
 * Author: Alexander Borisov <borisov@lexbor.com>
 */

#ifndef LEXBOR_HTML_INPUT_ELEMENT_H
#define LEXBOR_HTML_INPUT_ELEMENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lexbor/html/interface.h"
#include "lexbor/html/interfaces/element.h"


struct lxb_html_input_element {
    lxb_html_element_t element;

    lxb_html_form_element_t *form_owner;
    bool                    form_owner_set;
    bool                    checkedness;
    bool                    dirty_checkedness;
};


LXB_API lxb_html_input_element_t *
lxb_html_input_element_interface_create(lxb_html_document_t *document);

LXB_API lxb_html_input_element_t *
lxb_html_input_element_interface_destroy(lxb_html_input_element_t *input_element);

/*
 * Current checkedness of the input.
 *
 * This is the live state, not the value of the "checked" content attribute.
 * It is derived from the attribute at parse time and on attribute
 * addition/removal while the control has no dirty checkedness. Explicitly
 * setting it marks the control dirty.
 */
LXB_API bool
lxb_html_input_element_checked(lxb_html_input_element_t *input);

/*
 * Set the current checkedness.
 *
 * Always marks the checkedness dirty, even if the value does not change.
 * Setting a radio button to true unchecks the other checked radio buttons
 * of the same group. Does not modify the "checked" content attribute.
 */
LXB_API lxb_status_t
lxb_html_input_element_set_checked(lxb_html_input_element_t *input,
                                   bool checked);

/*
 * Form owner of the input, or NULL.
 *
 * The owner is resolved from the "form" attribute (by document order ID
 * lookup in the same tree) or, when the attribute is absent, from the
 * nearest ancestor form element.
 */
LXB_API lxb_html_form_element_t *
lxb_html_input_element_form_owner(lxb_html_input_element_t *input);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LEXBOR_HTML_INPUT_ELEMENT_H */
