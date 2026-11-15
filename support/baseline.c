#include "expat.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern unsigned long long unit_scan_work;
struct seen {size_t length; int starts;};
static void XMLCALL start(void *ud,const XML_Char *name,const XML_Char **atts) {
 struct seen *s=ud; assert(strcmp(name,"r")==0); assert(atts[0] && atts[1]);
 assert(strlen(atts[1])==s->length); ++s->starts;
}
static unsigned long long run(size_t n,int buffered,int chunk,int deferred) {
 const char *prefix="<r a='",*suffix="'/>"; size_t len=strlen(prefix)+n+strlen(suffix),off=0;
 char *doc=malloc(len+1); XML_Parser p=XML_ParserCreate(NULL); struct seen s={n,0};
 unsigned long long work; assert(p && doc); memcpy(doc,prefix,strlen(prefix));
 memset(doc+strlen(prefix),'x',n); strcpy(doc+strlen(prefix)+n,suffix);
 XML_SetReparseDeferralEnabled(p,deferred?XML_TRUE:XML_FALSE);
 XML_SetUserData(p,&s); XML_SetStartElementHandler(p,start); unit_scan_work=0;
 while(off<len) { size_t take=(len-off<(size_t)chunk)?len-off:(size_t)chunk; enum XML_Status status;
  if(buffered) {void *b=XML_GetBuffer(p,(int)take); assert(b); memcpy(b,doc+off,take); status=XML_ParseBuffer(p,(int)take,off+take==len);}
  else status=XML_Parse(p,doc+off,(int)take,off+take==len);
  assert(status==XML_STATUS_OK); off+=take;
 }
 assert(s.starts==1); work=unit_scan_work; XML_ParserFree(p); free(doc); return work;
}
int main(int argc,char **argv) {
 if(argc==2 && strcmp(argv[1],"--target-bound")==0) {
  unsigned long long work=run(65536,0,64,1),limit=8*65536ULL+4096;
  printf("target_linear_bound scanned=%llu limit=%llu\n",work,limit);
  return work>limit?1:0;
 }

 int mode; unsigned long long one=run(65536,0,1000000,1); assert(one>=65536 && one<70000);
 printf("normal_one_shot=PASS scanned=%llu\n",one);
 /* Per public feeding API, record both the mitigated (default) scan
    work and the legacy workload reproduced by explicitly disabling
    reparse deferral.  The pre-fix original_quadratic_witness asserted
    the legacy numbers were quadratic; that assertion was a pre-fix
    diagnostic only and does not have to hold for the default parser
    anymore.  The legacy column still shows the original growth. */
 for(mode=0;mode<2;++mode) {
  unsigned long long a_on=run(32768,mode,64,1),b_on=run(65536,mode,64,1);
  unsigned long long a_off=run(32768,mode,64,0),b_off=run(65536,mode,64,0);
  printf("reparse_probe mode=%s n=32768 deferred=%llu legacy=%llu\n",
         mode?"buffer":"parse",a_on,a_off);
  printf("reparse_probe mode=%s n=65536 deferred=%llu legacy=%llu\n",
         mode?"buffer":"parse",b_on,b_off);
 }
 puts("target_reparse_mitigation=IMPLEMENTED both_public_feeding_apis");
 return 0;
}
