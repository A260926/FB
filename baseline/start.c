#include "katana.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
 const char *css="background-image: url(hero.png); color:red; --asset: url(a.png); background:var(--asset, url(fallback.png));";
 KatanaOutput *o=katana_parse(css,strlen(css),KatanaParserModeDeclarationList);
 assert(o && o->declarations);
 printf("declarations=%u errors=%u\n",o->declarations->length,o->errors.length);
 for(unsigned i=0;i<o->declarations->length;i++){
 KatanaDeclaration *d=o->declarations->data[i];printf("property=%s value=%s count=%u\n",d->property,d->string,d->values->length);}
 assert(o->declarations->length>=2);
 katana_destroy_output(o);puts("KATANA_DECL_BASELINE_OK");
}
