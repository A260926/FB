#include "gumbo.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static GumboNode* find(GumboNode* n,const char* id) {
  GumboVector* c=NULL;
  if(n->type==GUMBO_NODE_DOCUMENT)c=&n->v.document.children;
  else if(n->type==GUMBO_NODE_ELEMENT || n->type==GUMBO_NODE_TEMPLATE){
    GumboAttribute* a=gumbo_get_attribute(&n->v.element.attributes,"id");
    if(a && !strcmp(a->value,id))return n;
    c=&n->v.element.children;
  }
  if(c)for(unsigned i=0;i<c->length;i++){GumboNode* x=find(c->data[i],id);if(x)return x;}
  return NULL;
}
int main(void){
  GumboOutput* o=gumbo_parse("<!doctype html><p id=p>A&amp;B</p><table><tr><td id=c>7</td></tr></table>");
  GumboNode* n=find(o->root,"p");assert(n && n->v.element.children.length==1);
  assert(!strcmp(((GumboNode*)n->v.element.children.data[0])->v.text.text,"A&B"));
  assert(find(o->root,"c"));gumbo_destroy_output(&kGumboDefaultOptions,o);
  GumboOptions x=kGumboDefaultOptions;x.fragment_context=GUMBO_TAG_ANNOTATION_XML;x.fragment_namespace=GUMBO_NAMESPACE_MATHML;
  const char* s="<a id=a>x</a>";o=gumbo_parse_with_options(&x,s,strlen(s));n=find(o->root,"a");assert(n);
  assert(n->v.element.tag_namespace==GUMBO_NAMESPACE_MATHML);
  printf("LEGACY_FRAGMENT namespace=%d\n",n->v.element.tag_namespace);gumbo_destroy_output(&x,o);
  x=kGumboDefaultOptions;x.fragment_context=GUMBO_TAG_DIV;
  s="<form id=f><input></form>";o=gumbo_parse_with_options(&x,s,strlen(s));assert(find(o->root,"f"));gumbo_destroy_output(&x,o);
  puts("GUMBO_BASELINE_OK entities table foreign-fragment form");return 0;
}
