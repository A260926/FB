#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <lexbor/html/html.h>
#include <lexbor/css/css.h>
#include <lexbor/selectors/selectors.h>
static lxb_status_t count_node(lxb_dom_node_t *n,lxb_css_selector_specificity_t s,void *ctx) {
 (void)s; assert(n->type==LXB_DOM_NODE_TYPE_ELEMENT); (*(size_t*)ctx)++; return LXB_STATUS_OK;
}
int main(void) {
 const char *html="<html lang='en'><body><article class='card' id='a'>A</article><article class='card' lang='fr' id='b'>B</article></body></html>";
 lxb_html_document_t *d=lxb_html_document_create(); assert(d);
 assert(lxb_html_document_parse(d,(const lxb_char_t*)html,strlen(html))==LXB_STATUS_OK);
 lxb_css_parser_t *p=lxb_css_parser_create(); assert(p); assert(lxb_css_parser_init(p,NULL)==LXB_STATUS_OK);
 lxb_selectors_t *s=lxb_selectors_create(); assert(s); assert(lxb_selectors_init(s)==LXB_STATUS_OK);
 const char *q=".card"; size_t count=0;
 lxb_css_selector_list_t *list=lxb_css_selectors_parse(p,(const lxb_char_t*)q,strlen(q)); assert(list && p->status==LXB_STATUS_OK);
 assert(lxb_selectors_find(s,lxb_dom_interface_node(d),list,count_node,&count)==LXB_STATUS_OK); assert(count==2);
 printf("BASELINE_CARDS count=%zu\n",count); lxb_css_parser_destroy(p,true); lxb_css_selector_list_destroy_memory(list);
 p=lxb_css_parser_create(); assert(p); assert(lxb_css_parser_init(p,NULL)==LXB_STATUS_OK); q=".card:lang(en)";
 list=lxb_css_selectors_parse(p,(const lxb_char_t*)q,strlen(q));
 printf("LANG_PARSE=%s\n",list && p->status==LXB_STATUS_OK ? "supported":"unsupported");
 lxb_selectors_destroy(s,true); lxb_css_parser_destroy(p,true);
 if(list) lxb_css_selector_list_destroy_memory(list); lxb_html_document_destroy(d);
 return 0;
}
