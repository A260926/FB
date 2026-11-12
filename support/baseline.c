#include "xlsxwriter.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(int argc,char **argv) {
 assert(argc==2);char path[1024];
 for(int mode=0;mode<2;mode++) {
  snprintf(path,sizeof path,"%s/%d.xlsx",argv[1],mode);
  lxw_workbook_options o={0};o.constant_memory=mode;
  lxw_workbook *w=workbook_new_opt(path,&o);assert(w);
  lxw_worksheet *a=workbook_add_worksheet(w,"a"),*b=workbook_add_worksheet(w,"b");
  assert(!worksheet_write_string(a,0,0,"same",NULL));
  assert(!worksheet_write_string(a,1,0," other ",NULL));
  assert(!worksheet_write_string(b,0,0,"same",NULL));
  assert(w->sst->string_count==(mode?0:3));assert(w->sst->unique_count==(mode?0:2));
  assert(!workbook_close(w));FILE *f=fopen(path,"rb");assert(f);char sig[4];assert(fread(sig,1,4,f)==4);assert(!memcmp(sig,"PK\3\4",4));fclose(f);
 }
 puts("BUFFERED_SST_COUNT=3 UNIQUE=2 STREAMING_SST_COUNT=0 ZIP_OK");return 0;
}
