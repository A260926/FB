#include "gumbo.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int tables, headers, cells, bodies;
static void walk(GumboNode *n) {
  if(n->type != GUMBO_NODE_ELEMENT && n->type != GUMBO_NODE_TEMPLATE) return;
  tables += n->v.element.tag == GUMBO_TAG_TABLE;
  bodies += n->v.element.tag == GUMBO_TAG_TBODY;
  headers += n->v.element.tag == GUMBO_TAG_TH;
  if(n->v.element.tag == GUMBO_TAG_TD) {
    ++cells;
    GumboAttribute *a=gumbo_get_attribute(&n->v.element.attributes,"headers");
    if(a) assert(strcmp(a->value,"region sales")==0);
  }
  for(unsigned i=0;i<n->v.element.children.length;i++) walk(n->v.element.children.data[i]);
}
int main(void) {
 const char *s="<table><tr><th id=region scope=row>北&amp;南<th id=sales>销售<tr><td headers='region sales'>7<td><table><tr><td>内表</table></table>";
 GumboOutput *o=gumbo_parse(s); assert(o && o->root); walk(o->root);
 assert(tables==2 && headers==2 && cells==3 && bodies==2);
 gumbo_destroy_output(&kGumboDefaultOptions,o);
 puts("GUMBO_DOM_OK tables=2 th=2 td=3 implied_tbody=2");
}
