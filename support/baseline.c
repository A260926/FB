#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){
 const char *html="<form id=f><input type=radio name=g checked><input type=radio name=g checked></form><input type=checkbox>";
 lxb_html_document_t *d=lxb_html_document_create();assert(d);
 assert(lxb_html_document_parse(d,(const lxb_char_t*)html,strlen(html))==LXB_STATUS_OK);
 lxb_dom_collection_t *c=lxb_dom_collection_make(&d->dom_document,8);assert(c);
 assert(lxb_dom_elements_by_tag_name(lxb_dom_interface_element(d),c,(const lxb_char_t*)"input",5)==LXB_STATUS_OK);
 assert(lxb_dom_collection_length(c)==3);
 lxb_dom_element_t *a=lxb_dom_collection_element(c,0),*b=lxb_dom_collection_element(c,1);
 assert(lxb_dom_element_has_attribute(a,(const lxb_char_t*)"checked",7));
 assert(lxb_dom_element_has_attribute(b,(const lxb_char_t*)"checked",7));
 assert(lxb_dom_element_set_attribute(b,(const lxb_char_t*)"name",4,(const lxb_char_t*)"other",5));
 size_t n;const lxb_char_t *v=lxb_dom_element_get_attribute(b,(const lxb_char_t*)"name",4,&n);assert(n==5&&!memcmp(v,"other",5));
 puts("DOM_INPUT_PARSE_AND_MUTATION_OK");
 lxb_dom_collection_destroy(c,true);lxb_html_document_destroy(d);
}
