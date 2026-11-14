/*
 * Copyright (C) 2018 Alexander Borisov
 *
 * Author: Alexander Borisov <borisov@lexbor.com>
 */

#ifndef LEXBOR_HTML_FORM_ELEMENT_H
#define LEXBOR_HTML_FORM_ELEMENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lexbor/html/interface.h"
#include "lexbor/html/interfaces/element.h"


struct lxb_html_form_element {
    lxb_html_element_t element;
};


LXB_API lxb_html_form_element_t *
lxb_html_form_element_interface_create(lxb_html_document_t *document);

LXB_API lxb_html_form_element_t *
lxb_html_form_element_interface_destroy(lxb_html_form_element_t *form_element);

/*
 * Reset the form.
 *
 * Every input whose form owner is this form (including inputs associated
 * through the "form" attribute, outside the form subtree) has its dirty
 * checkedness cleared and its current checkedness restored from the
 * current "checked" content attribute default. Radio button groups are
 * then normalized in tree order.
 */
LXB_API lxb_status_t
lxb_html_form_element_reset(lxb_html_form_element_t *form_element);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LEXBOR_HTML_FORM_ELEMENT_H */
